#include "sceneitem.h"

#include "shortcut/daemonlauncher.h"

#include "sandboxreport.h"

#include "sceneengine.h"

#include "media/mediahub.h"
#include "bridgeclient.h"

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

SceneItem::SceneItem(QQuickItem *parent)
    : BridgeItem(parent)
{
    setHostName(u"scene engine"_s);
    setFailureNoun(u"scene"_s);
    m_waitTimer.setSingleShot(true);
    m_waitTimer.setInterval(kVerdictWaitMs);
    setParkAfterMs(kParkAfterMs);
    connect(&m_waitTimer, &QTimer::timeout, this, &SceneItem::endVerdictWait);
    connect(&SceneCheckRunner::shared(), &SceneCheckRunner::verdictReady, this, &SceneItem::onVerdict);
}

// The usershortcut properties of the item, read by the host from its project.json; the engine is told the name of the application a user
// chose and gets its icon, and a click on the icon is a request the host judges (shortcut/shortcutitem.h).
shortcut::ShortcutItem *SceneItem::shortcuts() const
{
    if (!m_shortcuts && !m_shortcutsNone && !m_itemDir.isEmpty()) {
        auto item = std::make_unique<shortcut::ShortcutItem>(m_itemDir, shortcut::systemShortcutEnvironment(), &shortcut::ShortcutHub::shared().limiter());
        if (item->keys().isEmpty()) {
            m_shortcutsNone = true; // most items: no usershortcut property, nothing more to do
            return nullptr;
        }
        auto *self = const_cast<SceneItem *>(this);
        connect(item.get(), &shortcut::ShortcutItem::launchApproved, self, [self](const QString &dir, const QString &key) {
            shortcut::requestLaunch(self->m_outputName, self->m_desktop, dir, key);
        });
        item->setVisibleProbe([self] { return self->isVisible() && !self->paused(); });
        connect(self, &BridgeItem::buttonPressed, item.get(), &shortcut::ShortcutItem::noteClick);
        m_shortcuts = std::move(item);
        m_shortcuts->assignmentsChanged(properties());
    }
    return m_shortcuts.get();
}

void SceneItem::resetShortcuts()
{
    m_shortcuts.reset();
    m_shortcutsNone = false;
}

QVariantMap SceneItem::propertiesForEngine(const QVariantMap &properties) const
{
    if (const shortcut::ShortcutItem *s = shortcuts())
        return s->engineProperties(properties);
    return properties;
}

void SceneItem::propertiesApplied(const QVariantMap &properties)
{
    if (shortcut::ShortcutItem *s = shortcuts())
        s->assignmentsChanged(properties);
}

void SceneItem::clientReady(BridgeClient *client)
{
    if (shortcut::ShortcutItem *s = shortcuts())
        s->attach(client); // the application icons of the shortcuts, and their requests
    // only the Vulkan engine speaks the media messages (a producer that announced less never gets them), and it gets them only when its
    // scene says it uses them and the settings allow it (media/mediahub.h)
    media::MediaHub::shared().attach(client);
}

// scene-<output>.log, whichever engine runs (the log is about the output, the engine is named in its lines).
QString SceneItem::logName(const BridgeLaunch &) const
{
    return u"scene-"_s + (m_outputName.isEmpty() ? u"murasaki"_s : m_outputName);
}

// Whether the launch should wait: `auto`, both engines installed, nothing known about the item yet. Starts the check (the
// runner runs one per key) and the timer the first time it is asked.
bool SceneItem::waitingForVerdict() const
{
    return false;
}

void SceneItem::onVerdict(const QString &itemDir)
{
    if (itemDir != m_itemDir)
        return;
    if (m_waiting && !m_waitOver) {
        endVerdictWait(); // the answer is here: launch the right engine
        return;
    }
    // Late: the item started before the verdict was back. A "yes" is offered once as an upgrade.
    if (m_choice.pending && !m_upgradeOffered && !m_choice.key.isEmpty()) {
        if (const auto verdict = SceneCheckCache::shared().verdict(m_choice.key); verdict && verdict->ok) {
            m_upgradeOffered = true;
            emit engineUpgradeAvailable();
        }
    }
}

void SceneItem::endVerdictWait()
{
    m_waitTimer.stop();
    if (m_waitOver)
        return;
    m_waitOver = true;
    m_waiting = false;
    if (isCompleted())
        restartHost();
}

void SceneItem::setDesktop(const QString &desktop)
{
    if (desktop == m_desktop)
        return;
    m_desktop = desktop;
    emit desktopChanged(); // (nothing restarts: it only says which assignment a launch is judged against)
}

void SceneItem::setItemDir(const QString &dir)
{
    if (dir == m_itemDir)
        return;
    m_itemDir = dir;
    resetShortcuts();
    m_waiting = false;
    m_waitOver = false;
    m_waitTimer.stop();
    m_upgradeOffered = false;
    emit itemDirChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setAssetsDir(const QString &dir)
{
    if (dir == m_assetsDir)
        return;
    m_assetsDir = dir;
    emit assetsDirChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setOutputName(const QString &name)
{
    if (name == m_outputName)
        return;
    m_outputName = name;
    emit outputNameChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setSceneEngine(const QString &mode)
{
    const QString value = isValidSceneEngine(mode) ? mode : QString(kDefaultSceneEngine);
    if (value == m_sceneEngine)
        return;
    m_sceneEngine = value;
    emit sceneEngineChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setSandbox(bool sandbox)
{
    if (sandbox == m_sandbox)
        return;
    m_sandbox = sandbox;
    SceneCheckRunner::shared().setSandbox(sandbox);
    emit sandboxChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setRequireSandbox(bool require)
{
    if (require == m_requireSandbox)
        return;
    m_requireSandbox = require;
    SceneCheckRunner::shared().setRequireSandbox(require);
    emit requireSandboxChanged();
    if (isCompleted())
        restartHost();
}

void SceneItem::setRenderScale(double scale)
{
    const double clamped = scale <= 0.0 ? 0.0 : std::clamp(scale, kMinSceneRenderScale, kMaxSceneRenderScale); // 0: automatic
    if (clamped == m_renderScale)
        return;
    m_renderScale = clamped;
    emit renderScaleChanged();
    if (isCompleted())
        restartHost();
}

bool SceneItem::canStart() const
{
    return !m_itemDir.isEmpty() && !waitingForVerdict();
}

QString SceneItem::configurationError() const
{
    return sceneOutputNameValid(m_outputName) ? QString() : u"invalid output name"_s;
}

BridgeLaunch SceneItem::buildLaunch(int bridgeFd, const QSize &size) const
{
    BridgeLaunch result;
    result.carriesState = true; // everything but pause is on the command line
    SceneEngineRequest request;
    request.mode = sceneEngineModeFromString(m_sceneEngine);
    request.itemDir = m_itemDir;
    request.assetsDir = m_assetsDir;
    request.vulkanProgram = findVulkanSceneEngine();
    request.cache = &SceneCheckCache::shared();
    request.runner = &SceneCheckRunner::shared(); // a missing verdict is fetched in the background
    request.span = spanEnabled();
    m_choice = chooseSceneEngine(request);
    if (m_choice.label != m_engineLabel) {
        m_engineLabel = m_choice.label;
        // The surface is a const-correct caller of buildLaunch(); the notification goes out from the event loop.
        QMetaObject::invokeMethod(const_cast<SceneItem *>(this), &SceneItem::engineLabelChanged, Qt::QueuedConnection);
    }
    qInfo("murasaki: scene %s: engine %s", qPrintable(m_itemDir), qPrintable(m_choice.label));
    if (!m_choice.refusal.isEmpty()) {
        result.error = m_choice.refusal;
        return result;
    }
    if (m_choice.program.isEmpty()) {
        result.error = u"scene engine not installed"_s;
        return result;
    }
    SceneLaunch launch;
    launch.bridgeFd = bridgeFd;
    launch.size = bufferSize(size);
    result.size = launch.size;
    launch.outputName = m_outputName;
    launch.itemDir = m_itemDir;
    launch.assetsDir = m_assetsDir;
    launch.fill = fill();
    launch.fps = fps();
    launch.volume = audible() ? static_cast<int>(std::lround(volume() * 128.0)) : 0;
    launch.properties = propertiesForEngine(properties());
    launch.renderScale = m_choice.vulkan ? m_renderScale : 0.0;
    result.program = m_choice.program;
    result.arguments = sceneEngineArguments(launch);
    if (m_sandbox || m_requireSandbox) {
        const QString helper = findSandboxHelper();
        if (helper.isEmpty()) {
            if (m_requireSandbox) {
                result.error = u"the sandbox is required, but murasaki-sandbox is not installed"_s;
                return result;
            }
            qWarning("murasaki: murasaki-sandbox not found; the scene engine runs without a sandbox");
        } else {
            const SandboxReport report = querySandboxReport(helper);
            qInfo("murasaki: scene sandbox: %s", qPrintable(report.summary()));
            if (m_requireSandbox && !report.confines()) {
                result.error = u"the sandbox is required, but this system can not provide it: "_s + report.summary();
                return result;
            }
            const SandboxedCommand command = sandboxSceneEngine(helper, m_itemDir, m_assetsDir, result.program,
                                                                result.arguments, m_requireSandbox);
            result.program = command.program;
            result.arguments = command.arguments;
        }
    }
    return result;
}

QSize SceneItem::bufferSize(const QSize &pixels) const
{
    return pixels;
}

// The Vulkan engine gets one restart on an item; its second crash is recorded in the ledger (chooseSceneEngine reads it).
bool SceneItem::handleHostLost(const QString &)
{
    if (!m_choice.vulkan || m_choice.key.isEmpty())
        return false;
    const int crashes = SceneCheckCache::shared().recordCrash(m_itemDir, m_choice.key);
    Q_UNUSED(crashes);
    return false;
}

} // namespace murasaki
