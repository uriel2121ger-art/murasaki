#include "ptyprocess.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace murasaki {

PtyProcess::PtyProcess(QObject *parent)
    : QObject(parent)
{
    m_killTimer.setSingleShot(true);
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_group > 0)
            ::kill(-m_group, SIGKILL);
        m_process.kill();
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readAvailable(); // the last of the output
        closeMaster();
        // Helpers the leader left behind are not wanted either: ask them to go, and force it after the grace period.
        if (!groupGone()) {
            ::kill(-m_group, SIGTERM);
            m_killTimer.start(2000);
        } else {
            m_killTimer.stop();
        }
        emit finished(code, status);
    });
}

PtyProcess::~PtyProcess()
{
    m_process.disconnect(this);
    if (m_group > 0)
        ::kill(-m_group, SIGKILL);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    closeMaster();
}

bool PtyProcess::start(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment)
{
    if (m_process.state() != QProcess::NotRunning)
        return false;
    int master = -1;
    int slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
        return false;
    termios attributes{};
    if (tcgetattr(slave, &attributes) == 0) {
        attributes.c_lflag &= ~tcflag_t(ECHO | ECHONL); // a typed password must not come back in the output
        tcsetattr(slave, TCSANOW, &attributes);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    m_process.setProgram(program);
    m_process.setArguments(arguments);
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    m_process.setChildProcessModifier([master, slave] {
        // Only async-signal-safe calls: this runs in the forked child.
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        close(master);
    });
    m_process.start();
    const bool started = m_process.waitForStarted(5000);
    close(slave);
    if (!started) {
        close(master);
        return false;
    }
    m_master = master;
    m_group = pid_t(m_process.processId()); // setsid() in the child made it the leader of a group of its own
    m_notifier = std::make_unique<QSocketNotifier>(m_master, QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &PtyProcess::readAvailable);
    return true;
}

void PtyProcess::readAvailable()
{
    if (m_master < 0)
        return;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(m_master, buffer, sizeof(buffer));
        if (n > 0) {
            emit output(QByteArray(buffer, int(n)));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        if (m_notifier) // end of the stream (EIO once the child closed its side)
            m_notifier->setEnabled(false);
        return;
    }
}

void PtyProcess::closeMaster()
{
    m_notifier.reset();
    if (m_master >= 0) {
        close(m_master);
        m_master = -1;
    }
}

bool PtyProcess::write(const QByteArray &data)
{
    if (m_master < 0)
        return false;
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(m_master, data.constData() + done, size_t(data.size() - done));
        if (n > 0) {
            done += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) { // the pty buffer is full: wait for it to drain
            pollfd waiting{m_master, POLLOUT, 0};
            if (::poll(&waiting, 1, 2000) > 0)
                continue;
        }
        return false;
    }
    return true;
}

void PtyProcess::terminate(int killDelayMs)
{
    if (m_group <= 0 || groupGone())
        return;
    ::kill(-m_group, SIGTERM);
    m_killTimer.start(killDelayMs);
}

bool PtyProcess::groupGone() const
{
    if (m_group <= 0)
        return true;
    // kill(2) with signal 0 only checks: the group is gone when no process of it exists (a zombie counts as there
    // until its parent, this process, has reaped it, which QProcess does when it finishes).
    return ::kill(-m_group, 0) != 0 && errno == ESRCH;
}

} // namespace murasaki
