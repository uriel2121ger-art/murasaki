#include "bwrap.h"

#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>

namespace murasaki::sandbox {

namespace {

bool isExecutableFile(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

bool startsWith(const std::string &text, const char *prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

std::vector<std::string> globbed(const char *pattern)
{
    std::vector<std::string> found;
    glob_t result{};
    if (::glob(pattern, 0, nullptr, &result) == 0) {
        for (size_t i = 0; i < result.gl_pathc; ++i)
            found.emplace_back(result.gl_pathv[i]);
    }
    ::globfree(&result);
    return found;
}

size_t depth(const std::string &path)
{
    return static_cast<size_t>(std::count(path.begin(), path.end(), '/'));
}

} // namespace

std::string findBwrap(const std::string &preference)
{
    if (preference == "none")
        return {};
    if (!preference.empty())
        return isExecutableFile(preference) ? preference : std::string();
    const char *path = std::getenv("PATH");
    const std::string dirs = path ? path : "/usr/bin:/bin";
    size_t pos = 0;
    while (pos <= dirs.size()) {
        const size_t next = dirs.find(':', pos);
        const std::string dir = dirs.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!dir.empty() && dir[0] == '/') {
            const std::string candidate = dir + "/bwrap";
            if (isExecutableFile(candidate))
                return candidate;
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return {};
}

bool bwrapWorks(const std::string &bwrap)
{
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
        }
        ::execl(bwrap.c_str(), "bwrap", "--unshare-net", "--unshare-pid", "--unshare-ipc", "--ro-bind", "/", "/", "--",
                "/usr/bin/true", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR)
            return false;
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::vector<std::pair<std::string, std::string>> keptEnvironment(char **environment)
{
    static const char *const exact[] = {"PATH", "HOME", "USER", "LOGNAME", "LANG", "LANGUAGE", "TZ", "LD_LIBRARY_PATH", "XDG_CACHE_HOME",
                                        "XDG_CONFIG_HOME", "XDG_DATA_DIRS", "XDG_RUNTIME_DIR", "vblank_mode"};
    static const char *const prefixes[] = {"LC_", "MESA_", "__GL_", "__NV_", "VK_", "RADV_", "ACO_", "AMD_", "DRI_", "LIBGL_",
                                           "GALLIUM_", "MURASAKI_", "SDL_", "PIPEWIRE_", "PULSE_", "ENABLE_", "DISABLE_"};
    std::vector<std::pair<std::string, std::string>> kept;
    for (char **entry = environment; entry && *entry; ++entry) {
        const std::string text = *entry;
        const size_t eq = text.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string name = text.substr(0, eq);
        bool keep = std::find(std::begin(exact), std::end(exact), name) != std::end(exact);
        for (const char *prefix : prefixes)
            keep = keep || startsWith(name, prefix);
        if (keep)
            kept.emplace_back(name, text.substr(eq + 1));
    }
    return kept;
}

bool isProcfsPath(const std::string &path)
{
    return path == "/proc" || startsWith(path, "/proc/");
}

std::vector<std::string> bwrapCommand(const Policy &policy, const BwrapInput &input,
                                      const std::vector<std::pair<std::string, std::string>> &environment)
{
    std::vector<std::string> args{input.bwrap, "--unshare-net", "--unshare-pid", "--unshare-ipc", "--die-with-parent", "--new-session",
                                  "--clearenv", "--cap-drop", "ALL", "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"};
    for (const auto &[name, value] : environment) {
        args.insert(args.end(), {"--setenv", name, value});
    }

    // The runtime directory: only the audio sockets live in it. There is no bus socket and no Wayland socket.
    const std::string &run = input.runtimeDir;
    if (!run.empty()) {
        args.insert(args.end(), {"--perms", "0700", "--tmpfs", run});
        args.insert(args.end(), {"--bind-try", run + "/pipewire-0", run + "/pipewire-0"});
        args.insert(args.end(), {"--bind-try", run + "/pulse", run + "/pulse"});
        // A PipeWire socket the user named (PIPEWIRE_REMOTE=pipewire-1) lives in the same directory.
        for (const auto &[name, value] : environment)
            if (name == "PIPEWIRE_REMOTE" && !value.empty() && value.find('/') == std::string::npos && value != "pipewire-0")
                args.insert(args.end(), {"--bind-try", run + "/" + value, run + "/" + value});
    }

    // Shallow paths first, so that a path inside another never ends up hidden by it.
    std::vector<std::pair<std::string, bool>> paths; // path, writable
    std::set<std::string> seen;
    for (const std::string &p : policy.readOnly)
        if (seen.insert(p).second)
            paths.emplace_back(p, false);
    for (const std::string &p : policy.readWrite)
        if (seen.insert(p).second)
            paths.emplace_back(p, true);
    // A policy path that is itself a symlink is recreated as one, so what it points at must be there too (an item
    // library linked to another disk). Targets already inside another policy path (/lib -> usr/lib) need nothing.
    const size_t declared = paths.size();
    for (size_t i = 0; i < declared; ++i) {
        const std::string &path = paths[i].first;
        struct stat st {};
        char real[PATH_MAX];
        if (isProcfsPath(path) || ::lstat(path.c_str(), &st) != 0 || !S_ISLNK(st.st_mode) || !::realpath(path.c_str(), real))
            continue;
        const std::string target = real;
        const bool covered = std::any_of(paths.begin(), paths.begin() + static_cast<std::ptrdiff_t>(declared), [&](const auto &other) {
            return other.first != path && (target == other.first || startsWith(target, (other.first + "/").c_str()));
        });
        if (!covered && seen.insert(target).second)
            paths.emplace_back(target, paths[i].second);
    }
    std::stable_sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) { return depth(a.first) < depth(b.first); });

    for (const auto &[path, writable] : paths) {
        if (isProcfsPath(path))
            continue; // the new procfs above
        if (!run.empty() && (path == run || startsWith(path, (run + "/").c_str())))
            continue; // the private runtime directory above
        if (path == "/dev/dri") {
            for (const std::string &node : globbed("/dev/dri/renderD*")) // render nodes only: no display control
                args.insert(args.end(), {"--dev-bind-try", node, node});
            continue;
        }
        if (startsWith(path, "/dev/")) {
            if (path == "/dev/null" || path == "/dev/zero" || path == "/dev/urandom" || path == "/dev/random")
                continue; // part of --dev
            args.insert(args.end(), {"--dev-bind-try", path, path});
            continue;
        }
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode)) {
            char target[4096];
            const ssize_t n = ::readlink(path.c_str(), target, sizeof(target) - 1);
            if (n > 0)
                args.insert(args.end(), {"--symlink", std::string(target, static_cast<size_t>(n)), path});
            continue;
        }
        args.insert(args.end(), {writable ? "--bind-try" : "--ro-bind-try", path, path});
    }

    // This program again, to apply Landlock inside the namespace.
    args.insert(args.end(), {"--ro-bind", input.helper, "/.murasaki-sandbox", "--", "/.murasaki-sandbox"});
    args.insert(args.end(), input.innerArguments.begin(), input.innerArguments.end());
    return args;
}

} // namespace murasaki::sandbox
