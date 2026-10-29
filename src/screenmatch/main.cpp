// murasaki-screenmatch: compares regions of the screen with reference pictures and tells the daemon how well they match.
//
//   murasaki-screenmatch --chan-fd N --pw-fd N --nodes ID[,ID...] [--buffers shm|any] [--allow-capture]
//   murasaki-screenmatch --selftest       (for the sandbox tests: loads the filter, tries what it must not do, prints what happened)
//
// It runs behind `murasaki-sandbox --profile screenmatch`: no files, no network, no programs. It has two descriptors: the daemon's channel (a socketpair, the line protocol of protocol.h) and the
// PipeWire remote the daemon opened through the portal. Frames come from the second and only scores (and, when asked and allowed, one explicit capture) go out on the first. There is no option that
// names a file or a frame source other than PipeWire.

#include "exitcodes.h"
#include "protocol.h"
#include "pwsource.h"
#include "session.h"
#include "seccomp.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace murasaki::screenmatch;

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool writeAll(int fd, const QByteArray &bytes)
{
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (n > 0) {
            done += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, 5000) <= 0) // the daemon does not read: give up rather than grow without bound
                return false;
            continue;
        }
        return false;
    }
    return true;
}

bool send(int fd, const QList<QByteArray> &lines)
{
    QByteArray all;
    for (const QByteArray &l : lines)
        all += l + '\n';
    return all.isEmpty() || writeAll(fd, all);
}

// No core dump and no ptrace by a process of the same user: the helper's memory holds screen pixels (the shared memory of the frames is part of a dump).
void protectMemory()
{
    ::prctl(PR_SET_DUMPABLE, 0);
    const rlimit none{0, 0};
    ::setrlimit(RLIMIT_CORE, &none);
}

bool isSocket(int fd)
{
    struct stat st {};
    return ::fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}

// What the sandbox tests look at: each line is `name result errno`. Every attempt is one the helper must never be able to make.
int runSelftest()
{
    const auto report = [](const char *name, long result) { std::printf("%s %ld %d\n", name, result, result < 0 ? errno : 0); };
    {
        rlimit core{1, 1};
        ::getrlimit(RLIMIT_CORE, &core);
        report("core-limit", long(core.rlim_cur));
        report("dumpable", ::prctl(PR_GET_DUMPABLE));
    }
    errno = 0;
    report("socket", ::syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0));
    report("connect", ::syscall(SYS_connect, 3, nullptr, 0));
    report("bind", ::syscall(SYS_bind, 3, nullptr, 0));
    report("listen", ::syscall(SYS_listen, 3, 1));
    report("accept4", ::syscall(SYS_accept4, 3, nullptr, nullptr, 0));
    char program[] = "/bin/true";
    char *args[] = {program, nullptr};
    report("execve", ::syscall(SYS_execve, program, args, nullptr));
    int pair[2];
    report("socketpair", ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    report("getpid", ::getpid() > 0 ? 0 : -1);
    errno = 0;
    const int home = ::open("/etc/hostname", O_RDONLY);
    report("open-etc", home);
    const int tmp = ::open("/tmp/murasaki-screenmatch-selftest", O_WRONLY | O_CREAT, 0600);
    report("open-write", tmp);
    if (tmp >= 0)
        ::unlink("/tmp/murasaki-screenmatch-selftest"); // (only reached without the sandbox: it leaves nothing behind)
    const int usr = ::open("/usr/bin/true", O_RDONLY);
    report("open-usr", usr);
    std::fflush(stdout);
    return kExitOk;
}

int usage(const char *why)
{
    std::fprintf(stderr, "murasaki-screenmatch: %s\n", why);
    return kExitUsage;
}

bool number(const char *text, long low, long high, long *out)
{
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || v < low || v > high)
        return false;
    *out = v;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    bool selftest = false;
    long chanFd = -1, pwFd = -1;
    std::vector<unsigned> nodes;
    bool allowDmaBuf = false, allowCapture = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--allow-capture") {
            allowCapture = true;
        } else if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "--chan-fd" || arg == "--pw-fd" || arg == "--nodes" || arg == "--buffers") {
            if (i + 1 >= argc)
                return usage("missing value");
            const char *value = argv[++i];
            if (arg == "--chan-fd" && !number(value, 3, 1023, &chanFd))
                return usage("--chan-fd needs a descriptor number");
            if (arg == "--pw-fd" && !number(value, 3, 1023, &pwFd))
                return usage("--pw-fd needs a descriptor number");
            if (arg == "--buffers") {
                if (std::string(value) == "any")
                    allowDmaBuf = true;
                else if (std::string(value) != "shm")
                    return usage("--buffers is shm or any");
            }
            if (arg == "--nodes") {
                std::string list = value;
                size_t pos = 0;
                while (pos <= list.size()) {
                    const size_t next = list.find(',', pos);
                    long id = 0;
                    if (!number(list.substr(pos, next == std::string::npos ? std::string::npos : next - pos).c_str(), 1, 0x7ffffff0, &id) || int(nodes.size()) >= kMaxStreams)
                        return usage("--nodes needs 1 to 8 node ids");
                    nodes.push_back(unsigned(id));
                    if (next == std::string::npos)
                        break;
                    pos = next + 1;
                }
            }
        } else {
            return usage("unknown option");
        }
    }
    protectMemory(); // (before anything of the screen can be in memory, and so before the filter, which does not need to allow prctl for it)
    // First thing, before any frame or any line: the syscalls this process never needs are refused (EPERM). The filter also refuses the exec that started us, which is why it is loaded here and
    // not by the sandbox wrapper.
    std::string seccompError;
    if (!murasaki::sandbox::applyScreenmatchSeccomp(seccompError)) {
        std::fprintf(stderr, "murasaki-screenmatch: no syscall filter: %s\n", seccompError.c_str());
        return kExitSandboxUnavailable;
    }
    if (selftest)
        return runSelftest();
    if (chanFd < 0 || pwFd < 0 || nodes.empty() || chanFd == pwFd)
        return usage("needs --chan-fd, --pw-fd and --nodes");
    // The two descriptors are sockets of the daemon's making and nothing else is open: a wrong number would otherwise be read or written as if it were the channel.
    if (!isSocket(int(chanFd)) || !isSocket(int(pwFd)))
        return usage("--chan-fd and --pw-fd are not sockets");
    for (int fd = 3; fd < 1024; ++fd)
        if (fd != chanFd && fd != pwFd)
            ::close(fd); // (the daemon marks its own CLOEXEC; this is the second lock)
    // Gone with the daemon: no orphan keeps watching the screen for nobody.
    const pid_t parent = ::getppid();
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent)
        return kExitFailure;
    ::signal(SIGPIPE, SIG_IGN);
    ::fcntl(int(chanFd), F_SETFL, ::fcntl(int(chanFd), F_GETFL) | O_NONBLOCK);

    QString error;
    auto source = PipeWireFrameSource::create(int(pwFd), nodes, murasaki::screenmatch::kMaxRate, allowDmaBuf, &error);
    if (!source) {
        std::fprintf(stderr, "murasaki-screenmatch: %s\n", error.toLocal8Bit().constData());
        return kExitNoPipeWire;
    }
    SessionOptions options;
    options.captureAllowed = allowCapture;
    Session session(source.get(), options);
    LineBuffer lines;
    int overlongSeen = 0;
    if (!send(int(chanFd), {formatReply({.type = ReplyType::Hello, .version = kProtocolVersion})}))
        return kExitFailure;

    qint64 nextTick = 0;
    char buffer[4096];
    while (!session.quitRequested()) {
        const qint64 now = nowMs();
        int timeout = 200;
        if (session.running())
            timeout = int(std::max<qint64>(0, std::min<qint64>(nextTick - now, 200)));
        pollfd fds[2] = {{int(chanFd), POLLIN, 0}, {source->pollFd(), POLLIN, 0}};
        const int ready = ::poll(fds, 2, timeout);
        if (ready < 0 && errno != EINTR)
            return kExitFailure;
        source->iterate();
        if (source->failed()) {
            send(int(chanFd), {formatReply({.type = ReplyType::State, .word = QStringLiteral("stopped")})});
            return kExitStreamLost;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            const ssize_t n = ::read(int(chanFd), buffer, sizeof(buffer));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
                return kExitOk; // the daemon closed the channel
            if (n > 0) {
                QList<QByteArray> reply;
                for (const QByteArray &line : lines.feed(buffer, n)) {
                    reply += session.handle(line, nowMs());
                    if (session.quitRequested())
                        break;
                }
                while (overlongSeen < lines.overlong()) {
                    ++overlongSeen;
                    reply.append(formatReply({.type = ReplyType::Err, .word = QStringLiteral("line"), .reason = QStringLiteral("too-long")}));
                }
                if (!send(int(chanFd), reply))
                    return kExitChannelStalled;
                if (session.running() && nextTick == 0)
                    nextTick = nowMs();
            }
        }
        if (session.running() && nowMs() >= nextTick) {
            if (!send(int(chanFd), session.tick(nowMs())))
                return kExitChannelStalled;
            nextTick = nowMs() + 1000 / session.ticksPerSecond();
        } else if (!session.running()) {
            nextTick = 0;
        }
    }
    return kExitOk;
}
