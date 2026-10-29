#include "screenmatchhost.h"

#include "crashledger.h"
#include "screenmatch/exitcodes.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QThread>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace murasaki {

ScreenMatchHost::ScreenMatchHost(Config config, CrashLedger *ledger, QObject *parent) : QObject(parent), m_config(std::move(config)), m_ledger(ledger) {}

ScreenMatchHost::~ScreenMatchHost()
{
    stop();
    for (const QPointer<QProcess> &p : std::as_const(m_dying)) {
        if (p && p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
}

int ScreenMatchHost::crashes() const
{
    return m_ledger ? m_ledger->crashes(m_config.helperPath) : 0;
}

void ScreenMatchHost::resetCrashes()
{
    if (m_ledger)
        m_ledger->forget(m_config.helperPath);
    if (m_state == State::Blocked)
        setState(State::Idle);
}

QStringList ScreenMatchHost::command(const Config &config, int channelFd, int pipewireFd, const QList<unsigned> &nodes)
{
    QStringList ids;
    for (const unsigned n : nodes)
        ids << QString::number(n);
    QStringList helper = {config.helperPath, u"--chan-fd"_s, QString::number(channelFd), u"--pw-fd"_s, QString::number(pipewireFd), u"--nodes"_s, ids.join(u',')};
    if (config.allowDmaBuf)
        helper << u"--buffers"_s << u"any"_s;
    if (config.allowCapture)
        helper << u"--allow-capture"_s;
    if (config.sandboxPath.isEmpty())
        return helper;
    QStringList out = {config.sandboxPath, u"--profile"_s, u"screenmatch"_s};
    if (config.requireSandbox)
        out << u"--require"_s;
    out << u"--"_s << helper;
    return out;
}

void ScreenMatchHost::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void ScreenMatchHost::start(int pipewireFd, const QList<unsigned> &nodes)
{
    stop();
    for (const QPointer<QProcess> &old : std::as_const(m_dying)) { // never two helpers at once
        if (old && old->state() != QProcess::NotRunning) {
            old->kill();
            old->waitForFinished(1000);
        }
    }
    m_dying.clear();
    m_stopping = false;
    m_lines = {};
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || thread() != QThread::currentThread()) {
        setState(State::Refused);
        emit degraded(u"start from the main thread"_s);
        return;
    }
    if (m_ledger && m_ledger->blocked(m_config.helperPath)) {
        setState(State::Blocked);
        emit degraded(u"screen: helper keeps crashing, turned off"_s);
        return;
    }
    if (m_config.sandboxPath.isEmpty() && m_config.requireSandbox) {
        setState(State::Refused);
        emit degraded(u"sandbox unavailable"_s);
        return;
    }
    if (nodes.isEmpty() || nodes.size() > screenmatch::kMaxStreams || pipewireFd < 0) {
        setState(State::Refused);
        emit degraded(u"screen: no stream to watch"_s);
        return;
    }
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        setState(State::Refused);
        emit degraded(u"cannot create the channel"_s);
        return;
    }
    const int childChannel = pair[1];
    const int childPipewire = ::fcntl(pipewireFd, F_DUPFD_CLOEXEC, 3);
    if (childPipewire < 0) {
        ::close(pair[0]);
        ::close(pair[1]);
        setState(State::Refused);
        emit degraded(u"cannot pass the PipeWire descriptor"_s);
        return;
    }
    m_channel = new QLocalSocket(this);
    if (!m_channel->setSocketDescriptor(pair[0], QLocalSocket::ConnectedState, QIODevice::ReadWrite)) {
        ::close(pair[0]);
        ::close(pair[1]);
        ::close(childPipewire);
        delete m_channel;
        setState(State::Refused);
        emit degraded(u"cannot use the channel"_s);
        return;
    }
    connect(m_channel, &QLocalSocket::readyRead, this, &ScreenMatchHost::onReadable);

    const QStringList command = ScreenMatchHost::command(m_config, childChannel, childPipewire, nodes);
    m_process = std::make_unique<QProcess>();
    m_process->setProgram(command.first());
    m_process->setArguments(command.mid(1));
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    const pid_t parentPid = ::getpid();
    m_process->setChildProcessModifier([childChannel, childPipewire, parentPid] {
        for (const int fd : {childChannel, childPipewire}) {
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0)
                ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC); // (only async-signal-safe calls here: this runs in the forked child)
        }
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parentPid)
            ::_exit(1);
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, &ScreenMatchHost::onError);
    connect(m_process.get(), &QProcess::finished, this, &ScreenMatchHost::onFinished);
    // The child has its own copies after the fork: ours of its two ends are closed once it started (or failed to). Connected BEFORE start(), which may report either at once, and to the
    // process itself, so that stop()'s disconnect from this host does not drop them.
    m_childFds = std::make_shared<ChildFds>();
    m_childFds->fd[0] = childChannel;
    m_childFds->fd[1] = childPipewire;
    const std::shared_ptr<ChildFds> fds = m_childFds;
    connect(m_process.get(), &QProcess::started, m_process.get(), [fds] { fds->close(); });
    connect(m_process.get(), &QProcess::errorOccurred, m_process.get(), [fds](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fds->close();
    });
    ++m_launches;
    m_alive.restart();
    setState(State::Running);
    m_process->start();
}

void ScreenMatchHost::send(const screenmatch::Command &c)
{
    if (m_state != State::Running || !m_channel)
        return;
    m_channel->write(screenmatch::formatCommand(c) + '\n');
}

void ScreenMatchHost::onReadable()
{
    if (!m_channel)
        return;
    const QByteArray bytes = m_channel->readAll();
    for (const QByteArray &line : m_lines.feed(bytes.constData(), bytes.size())) {
        QString why;
        const auto r = screenmatch::parseReply(line, &why);
        if (!r) {
            ++m_badLines; // (never echoed)
            continue;
        }
        emit reply(*r);
    }
}

void ScreenMatchHost::onError(QProcess::ProcessError error)
{
    if (m_stopping || error != QProcess::FailedToStart)
        return; // (a crash is reported by `finished`)
    setState(State::Refused);
    emit degraded(u"cannot start the screen matching helper"_s);
}

void ScreenMatchHost::ChildFds::close()
{
    for (int &f : fd) {
        if (f >= 0)
            ::close(f);
        f = -1;
    }
}

void ScreenMatchHost::stop()
{
    m_stopping = true;
    if (m_childFds) { // (stopped before the process reported its start: the descriptors are still ours)
        m_childFds->close();
        m_childFds.reset();
    }
    if (m_channel) {
        m_channel->write(screenmatch::formatCommand({.type = screenmatch::CommandType::Quit}) + '\n');
        m_channel->flush();
        m_channel->deleteLater();
        m_channel.clear();
    }
    if (m_process) {
        QProcess *process = m_process.release();
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning) {
            delete process;
        } else {
            process->setParent(this);
            m_dying.append(process);
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            QTimer::singleShot(m_config.stopGraceMs, process, [process] {
                if (process->state() != QProcess::NotRunning)
                    process->kill();
            });
            process->terminate();
        }
    }
    if (m_state == State::Running)
        setState(State::Idle);
}

void ScreenMatchHost::onFinished(int code, QProcess::ExitStatus status)
{
    if (m_stopping)
        return;
    if (m_channel) {
        onReadable(); // (what it said last)
        m_channel->deleteLater();
        m_channel.clear();
    }
    switch (screenmatch::classifyExit(status != QProcess::NormalExit, code)) {
    case screenmatch::ExitClass::Clean:
        setState(State::Idle);
        return;
    case screenmatch::ExitClass::NoPipeWire:
        setState(State::NoPipeWire);
        emit degraded(u"screen: no PipeWire"_s);
        return;
    case screenmatch::ExitClass::StreamLost:
        setState(State::StreamLost);
        emit degraded(u"screen: stopped by the desktop"_s);
        return;
    case screenmatch::ExitClass::ChannelStalled:
        setState(State::Idle);
        emit degraded(u"screen: the application did not read the helper, stopped"_s);
        return;
    case screenmatch::ExitClass::SandboxUnavailable:
        setState(State::Refused);
        emit degraded(u"sandbox unavailable"_s);
        return;
    case screenmatch::ExitClass::Crash:
        break;
    }
    if (m_ledger)
        m_ledger->recordCrash(m_config.helperPath, m_alive.isValid() && m_alive.elapsed() >= m_config.longRunMs);
    if (m_ledger && m_ledger->blocked(m_config.helperPath)) {
        setState(State::Blocked);
        emit degraded(u"screen: helper keeps crashing, turned off"_s);
        return;
    }
    setState(State::Crashed);
    emit degraded(u"screen: helper crashed"_s);
}

} // namespace murasaki
