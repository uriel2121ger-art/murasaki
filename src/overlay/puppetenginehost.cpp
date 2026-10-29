#include "puppetenginehost.h"

#include "crashledger.h"
#include "puppet/exitcodes.h"
#include "puppetnames.h"

#include "bridge/channel.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaMethod>
#include <QThread>

#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <csignal>

using namespace Qt::StringLiterals;
namespace puppet = murasaki::puppet;

namespace murasaki {

namespace {

QString name(std::string_view n)
{
    return QString::fromUtf8(n.data(), qsizetype(n.size()));
}

} // namespace

PuppetEngineHost::PuppetEngineHost(Config config, CrashLedger *ledger, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_ledger(ledger)
{
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, [this] { launch(); });
}

PuppetEngineHost::~PuppetEngineHost()
{
    stop();
}

QString PuppetEngineHost::modelPath() const
{
    return m_itemDir + u'/' + m_file;
}

int PuppetEngineHost::crashes() const
{
    return m_ledger ? m_ledger->crashes(modelPath()) : 0;
}

QStringList PuppetEngineHost::command(const Config &config, const QString &itemDir, const QString &file, QSize size, int bridgeFd)
{
    QStringList engine = {config.enginePath, u"--engine"_s, u"--item"_s, itemDir, u"--file"_s, file, u"--murasaki-bridge-fd"_s, QString::number(bridgeFd), u"--murasaki-size"_s,
                          u"%1x%2"_s.arg(size.width()).arg(size.height()), u"--fps"_s, QString::number(config.fps), u"--seed"_s, QString::number(config.seed)};
    if (config.sandboxPath.isEmpty())
        return engine;
    QStringList out = {config.sandboxPath, u"--profile"_s, u"puppet"_s, u"--item"_s, itemDir};
    if (config.requireSandbox)
        out << u"--require"_s;
    if (!config.corePath.isEmpty())
        out << u"--core"_s << config.corePath;
    out << u"--"_s << engine;
    return out;
}

void PuppetEngineHost::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void PuppetEngineHost::start(const QString &petId, const QString &itemDir, const QString &file, QSize size)
{
    stop();
    // Never two engines at once (two GPU processes, two sockets): one still leaving after the grace period is killed now.
    for (const QPointer<QProcess> &old : std::as_const(m_dying)) {
        if (old && old->state() != QProcess::NotRunning) {
            old->kill();
            old->waitForFinished(1000);
        }
    }
    m_dying.clear();
    m_petId = petId;
    m_itemDir = itemDir;
    m_file = file;
    m_size = size;
    m_stopping = false;
    // (PR_SET_PDEATHSIG fires when the forking thread dies: see the header)
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || thread() != QThread::currentThread()) {
        setState(State::Refused);
        emit degraded(u"start from the main thread"_s);
        return;
    }
    if (m_ledger && m_ledger->blocked(modelPath())) {
        setState(State::Blocked);
        emit degraded(name(puppet::kBlockedAfterCrashes));
        return;
    }
    if (m_config.sandboxPath.isEmpty() && m_config.requireSandbox) {
        setState(State::Refused);
        emit degraded(u"sandbox unavailable"_s);
        return;
    }
    launch();
}

void PuppetEngineHost::launch()
{
    if (m_stopping)
        return;
    auto pair = bridge::makeSocketPair();
    if (!pair) {
        setState(State::Refused);
        emit degraded(u"cannot create the bridge socket"_s);
        return;
    }
    const int childFd = pair->second.get();
    const QStringList command = PuppetEngineHost::command(m_config, m_itemDir, m_file, m_size, childFd);
    m_process = std::make_unique<QProcess>();
    m_process->setProgram(command.first());
    m_process->setArguments(command.mid(1));
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    const pid_t parentPid = ::getpid();
    m_process->setChildProcessModifier([childFd, parentPid] {
        const int flags = ::fcntl(childFd, F_GETFD);
        if (flags >= 0)
            ::fcntl(childFd, F_SETFD, flags & ~FD_CLOEXEC); // (only async-signal-safe calls here: this runs in the forked child)
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parentPid)
            ::_exit(1);
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, &PuppetEngineHost::onError);
    connect(m_process.get(), &QProcess::finished, this, &PuppetEngineHost::onFinished);
    if (m_ledger) {
        m_ledger->markOpening(m_petId, modelPath());
        m_marked = true;
    }
    ++m_launches;
    m_alive.invalidate();
    // A descriptor nobody takes would leak (and keep the engine waiting for a consumer that is not there): without a receiver it is closed with `pair`.
    if (isSignalConnected(QMetaMethod::fromSignal(&PuppetEngineHost::consumerFd)))
        emit consumerFd(pair->first.release());
    setState(State::Running);
    m_process->start(); // (returns at once: `started` or `errorOccurred` follows)
}

void PuppetEngineHost::onError(QProcess::ProcessError error)
{
    if (m_stopping || error != QProcess::FailedToStart)
        return; // (a crash is reported by `finished`)
    // the program could not be started at all: not the engine's crash, and nothing to retry
    if (m_ledger && m_marked)
        m_ledger->clearOpening(m_petId);
    m_marked = false;
    setState(State::Refused);
    emit degraded(u"cannot start the engine"_s);
}

void PuppetEngineHost::firstFrameShown()
{
    if (m_state == State::Running && !m_alive.isValid())
        m_alive.start(); // "it ran" starts at the first picture: an engine stuck loading for half an hour has not run well
    if (m_ledger && m_marked)
        m_ledger->clearOpening(m_petId);
    m_marked = false;
}

void PuppetEngineHost::stop()
{
    m_stopping = true;
    m_restart.stop();
    if (m_process) {
        QProcess *process = m_process.release();
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning) {
            delete process;
        } else {
            // It is asked to end and left to do it: the QProcess stays (a child of the host, so the host's destruction kills what is still there) until it finishes, and a timer kills it if the
            // grace period passes first.
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
    if (m_ledger && m_marked)
        m_ledger->clearOpening(m_petId);
    m_marked = false;
    if (m_state != State::Blocked && m_state != State::Refused && m_state != State::NoGpu)
        setState(State::Idle);
}

void PuppetEngineHost::onFinished(int code, QProcess::ExitStatus status)
{
    if (m_stopping)
        return;
    if (m_ledger && m_marked)
        m_ledger->clearOpening(m_petId);
    m_marked = false;
    switch (puppet::classifyExit(status != QProcess::NormalExit, code)) {
    case puppet::ExitClass::Clean:
        setState(State::Idle);
        return;
    case puppet::ExitClass::ModelRefused:
        setState(State::Refused);
        emit degraded(u"model refused"_s);
        return;
    case puppet::ExitClass::NoGpu:
        setState(State::NoGpu);
        emit degraded(name(puppet::kNoGpu));
        return;
    case puppet::ExitClass::SandboxUnavailable:
        setState(State::Refused);
        emit degraded(u"sandbox unavailable"_s);
        return;
    case puppet::ExitClass::Crash:
        break;
    }
    if (m_ledger) // (an engine that had run a long time before it died does not count toward the block: the ledger starts the count again)
        m_ledger->recordCrash(modelPath(), m_alive.isValid() && m_alive.elapsed() >= m_config.longRunMs);
    if (m_ledger && m_ledger->blocked(modelPath())) {
        setState(State::Blocked);
        emit degraded(name(puppet::kBlockedAfterCrashes));
        return;
    }
    setState(State::Restarting);
    emit degraded(name(puppet::kEngineRestarted));
    m_restart.start(m_config.restartDelayMs);
}

} // namespace murasaki
