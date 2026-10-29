#include "systemcontrol.h"

#include "sandboxreport.h"

#include "autostart.h"
#include "buildinfo.h"
#include "constants.h"
#include "plasmadbus.h"
#include "sceneengine.h"
#include "sceneenginelookup.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

SystemControl::Launcher &launcherStorage()
{
    static SystemControl::Launcher launcher;
    return launcher;
}

SystemControl::AsyncRunner &runnerStorage()
{
    static SystemControl::AsyncRunner runner;
    return runner;
}

SystemControl::PlasmaRunner &plasmaRunnerStorage()
{
    static SystemControl::PlasmaRunner runner;
    return runner;
}

// The real Plasma runner: plasmashell and the daemon are asked over D-Bus, which blocks, so on a thread of its own.
// (QDBusConnection::sessionBus() gives every thread a connection of its own.)
void runPlasmaOnThread(SystemControl::PlasmaOperation operation, std::function<void(const plasma::Result &)> done)
{
    std::thread([operation, done = std::move(done)] {
        plasma::DBusBackend backend;
        const plasma::Options options;
        plasma::Result result;
        switch (operation) {
        case SystemControl::PlasmaOperation::Enable: result = plasma::enable(backend, options); break;
        case SystemControl::PlasmaOperation::Disable: result = plasma::disable(backend, options); break;
        case SystemControl::PlasmaOperation::Status: result = plasma::status(backend, options); break;
        }
        done(result);
    }).detach();
}

int g_queryTimeoutMs = 5000;
int g_changeTimeoutMs = 30000; // enable/disable --now also starts or stops the unit

// systemctl through QProcess, answered from the event loop. `owner` bounds the lifetime of the process.
void runSystemctl(QObject *owner, const QStringList &arguments, int timeoutMs, std::function<void(int)> done)
{
    if (const SystemControl::AsyncRunner &custom = runnerStorage()) {
        custom(arguments, timeoutMs, std::move(done));
        return;
    }
    auto *process = new QProcess(owner);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    // Whichever of finished / error / timeout comes first answers; the others find `answered` set.
    auto answered = std::make_shared<bool>(false);
    auto finish = [process, answered, done = std::move(done)](int code) {
        if (*answered)
            return;
        *answered = true;
        process->disconnect();
        process->deleteLater();
        done(code);
    };
    QObject::connect(process, &QProcess::finished, process, [process, finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1);
    });
    QObject::connect(timer, &QTimer::timeout, process, [process, finish] {
        process->kill();
        finish(-1);
    });
    timer->start(timeoutMs);
    process->start(u"systemctl"_s, arguments);
}

} // namespace

SystemControl::SystemControl(QObject *parent)
    : QObject(parent)
{
    connect(&SceneCheckRunner::shared(), &SceneCheckRunner::verdictReady, this, [this] {
        ++m_sceneEngineRevision;
        emit sceneEngineRevisionChanged();
    });
}

QString SystemControl::appVersion() const
{
    return versionString();
}

int SystemControl::minWebFpsLimit() const
{
    return kMinWebFpsLimit;
}

int SystemControl::maxWebFpsLimit() const
{
    return kMaxWebFpsLimit;
}

int SystemControl::defaultWebFpsLimit() const
{
    return kDefaultWebFpsLimit;
}

QString SystemControl::localFile(const QUrl &url) const
{
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

bool SystemControl::sceneEngineAvailable() const
{
    return !findVulkanSceneEngine().isEmpty();
}

QString SystemControl::sandboxStatus() const
{
    const QString helper = findSandboxHelper();
    return helper.isEmpty() ? QString() : querySandboxReport(helper).summary();
}

QString SystemControl::sceneEngineLabel(const QString &mode, const QString &itemDir, const QString &assetsDir) const
{
    if (itemDir.isEmpty())
        return {};
    SceneEngineRequest request;
    request.mode = sceneEngineModeFromString(mode);
    request.itemDir = itemDir;
    request.assetsDir = assetsDir;
    request.vulkanProgram = findVulkanSceneEngine();
    request.cache = &SceneCheckCache::shared();
    request.runner = &SceneCheckRunner::shared();
    const SceneEngineChoice choice = chooseSceneEngine(request);
    return choice.program.isEmpty() ? QString() : choice.label; // empty: no engine installed (the page says so)
}

bool SystemControl::defaultPauseWhenCovered() const
{
    return kDefaultPauseWhenCovered;
}

SystemControl::Launcher SystemControl::setLauncher(Launcher launcher)
{
    return std::exchange(launcherStorage(), std::move(launcher));
}

void SystemControl::setTimeouts(int queryMs, int changeMs)
{
    g_queryTimeoutMs = queryMs;
    g_changeTimeoutMs = changeMs;
}

SystemControl::AsyncRunner SystemControl::setAsyncRunner(AsyncRunner runner)
{
    return std::exchange(runnerStorage(), std::move(runner));
}

void SystemControl::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit autostartBusyChanged();
}

void SystemControl::refreshAutostart()
{
    if (m_busy)
        return;
    setBusy(true);
    runSystemctl(this, autostart::queryEnabledArguments(), g_queryTimeoutMs,
                 [self = QPointer<SystemControl>(this)](int code) {
                     if (!self)
                         return;
                     self->setBusy(false);
                     const bool enabled = code == 0;
                     if (enabled != self->m_autostartEnabled) {
                         self->m_autostartEnabled = enabled;
                         emit self->autostartEnabledChanged();
                     }
                 });
}

void SystemControl::setAutostart(bool enabled)
{
    if (m_busy) {
        emit autostartEnabledChanged(); // let a bound switch fall back
        return;
    }
    setBusy(true);
    const QString verb = autostart::changeEnabledVerb(enabled);
    runSystemctl(this, autostart::changeEnabledArguments(enabled), g_changeTimeoutMs,
                 [self = QPointer<SystemControl>(this), enabled, verb](int code) {
                     if (!self)
                         return;
                     self->setBusy(false);
                     if (code == 0)
                         self->m_autostartEnabled = enabled;
                     else
                         emit self->error(tr("Could not change autostart: %1")
                                              .arg(code < 0 ? tr("cannot run systemctl")
                                                            : tr("systemctl %1 failed (exit code %2)").arg(verb).arg(code)));
                     emit self->autostartEnabledChanged();
                 });
}

bool SystemControl::plasmaSupported() const
{
    return plasma::isPlasmaSession();
}

SystemControl::PlasmaRunner SystemControl::setPlasmaRunner(PlasmaRunner runner)
{
    return std::exchange(plasmaRunnerStorage(), std::move(runner));
}

void SystemControl::runPlasma(PlasmaOperation operation)
{
    setPlasmaBusy(true);
    auto answer = [self = QPointer<SystemControl>(this), operation](const plasma::Result &result) {
        // Back on the GUI thread, whichever thread the runner answers from.
        QMetaObject::invokeMethod(
            qApp,
            [self, operation, result] {
                if (!self)
                    return;
                self->setPlasmaBusy(false);
                if (result.ok)
                    self->m_plasmaEnabled = result.active;
                else if (operation != PlasmaOperation::Status)
                    emit self->error(tr("Could not change the Plasma wallpaper: %1").arg(result.error));
                emit self->plasmaEnabledChanged();
            },
            Qt::QueuedConnection);
    };
    if (const PlasmaRunner &custom = plasmaRunnerStorage())
        custom(operation, std::move(answer));
    else
        runPlasmaOnThread(operation, std::move(answer));
}

void SystemControl::setPlasmaBusy(bool busy)
{
    if (busy == m_plasmaBusy)
        return;
    m_plasmaBusy = busy;
    emit plasmaBusyChanged();
}

void SystemControl::refreshPlasma()
{
    if (m_plasmaBusy || !plasmaSupported())
        return;
    runPlasma(PlasmaOperation::Status);
}

void SystemControl::setPlasma(bool enabled)
{
    if (m_plasmaBusy || !plasmaSupported()) {
        emit plasmaEnabledChanged(); // let a bound switch fall back
        return;
    }
    runPlasma(enabled ? PlasmaOperation::Enable : PlasmaOperation::Disable);
}

bool SystemControl::startSurface()
{
    const QString program = u"systemctl"_s;
    const QStringList arguments = autostart::startArguments();
    const Launcher &custom = launcherStorage();
    const bool started = custom ? custom(program, arguments) : QProcess::startDetached(program, arguments);
    if (!started)
        emit error(tr("Could not start the wallpaper service"));
    return started;
}

} // namespace murasaki
