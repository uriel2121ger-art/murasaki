#include "surfacecontroller.h"

#include "assignment.h"
#include "canvas.h"
#include "constants.h"
#include "media/mediahub.h"
#include "shortcut/shortcutitem.h"
#ifdef MURASAKI_HAVE_FOREIGN_TOPLEVEL
#include "foreigntoplevelmonitor.h"
#ifdef MURASAKI_HAVE_IDLE_NOTIFY
#include "waylandidlemonitor.h"
#endif
#endif
#include "hyprlandmonitor.h"
#include "heaptuning.h"
#include "webprofilefactory.h"
#include "networkgate.h"
#include "kwinmonitor.h"
#include "outputsurface.h"
#include "playbackpolicy.h"
#include "sceneenginelookup.h"
#include "spansync.h"
#include "surfaceservice.h"
#include "swaymonitor.h"
#ifdef MURASAKI_HAVE_X11
#include "x11monitor.h"
#endif

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDBusError>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QQuickWebEngineProfile>
#include <QScreen>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {
constexpr int kFirstRegisterBackoffMs = 1000;
constexpr int kMaxRegisterBackoffMs = 30000;
} // namespace

SurfaceController::SurfaceController(SurfaceOptions options, QObject *parent)
    : QObject(parent)
    , m_windowed(options.windowed)
    , m_hostOutput(std::move(options.hostOutput))
    , m_sinkFactory(std::move(options.sinkFactory))
    , m_screenName(std::move(options.screenName))
    , m_webProfiles(new WebProfileFactory(NetworkGate::instance(), this)) // one off-the-record profile per shown web item
    , m_service(new SurfaceService(this))
    , m_monitorFactory(std::move(options.monitorFactory))
    , m_powerBus(std::move(options.powerBus))
{
    m_noFrameTimeoutMs = options.noFrameTimeoutMs;
    m_idleFactory = std::move(options.idleFactory);
    m_heapTrim.setSingleShot(true);
    m_heapTrim.setInterval(3000); // after the old wallpaper has faded out and its engine is gone
    connect(&m_heapTrim, &QTimer::timeout, this, [] { trimHeap(); });
    m_engine.addImportPath(u"qrc:/qt/qml"_s);
    if (!m_sinkFactory) {
        m_sinkFactory = [this](const SinkRequest &request) -> std::unique_ptr<OutputSink> {
            if (!request.screen)
                return nullptr;
            auto surface = std::make_unique<OutputSurface>(request.screen, request.engine, request.webProfile, m_windowed,
                                                          m_compositor.layer);
            surface->show();
            return surface;
        };
    }
    m_service->outputsProvider = [this] { return outputsJson(); };
    m_service->desktopsProvider = [this] { return desktopsJson(); };
    m_service->hostStateProvider = [this](const QString &name) { return hostStateJson(name); };
    connect(m_service, &SurfaceService::retryRequested, this, &SurfaceController::retryOutput);
    connect(m_service, &SurfaceService::hostsChanged, this, &SurfaceController::onHostsChanged);
    connect(m_service, &SurfaceService::kwinCursorReported, this, &SurfaceController::onCursorReported);
    m_cursorClock.start();
    m_spanSync.setInterval(kSpanSyncIntervalMs);
    connect(&m_spanSync, &QTimer::timeout, this, &SurfaceController::syncSpanVideos);
    m_spanSync.start();
    m_cursorFlush.setSingleShot(true);
    connect(&m_cursorFlush, &QTimer::timeout, this, [this] {
        dispatchCursor(m_cursorRouter.takePending(cursorScreens(), m_cursorClock.elapsed()));
        armCursorFlush();
    });
    connect(&m_audio, &AudioCapture::spectrumChanged, this, &SurfaceController::pushSpectrum);
}

SurfaceController::~SurfaceController()
{
    if (m_hostRegistered) { // the registration would also go with the bus name; this is just earlier
        QDBusMessage call = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                           QString(kSurfaceDBusInterface), u"UnregisterHost"_s);
        call.setArguments({m_hostOutput});
        QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
    }
    if (!isHost()) { // free the name for the next policy server (a restart, or the next test)
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterService(QString(kSurfaceDBusService));
        bus.unregisterObject(QString(kSurfaceDBusPath));
    }
    for (auto &[name, output] : m_outputs)
        output.sink.reset(); // before m_engine, which the views use
    // Views are deleted later by the event loop; flush that before the engine and profile go away.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

bool SurfaceController::start()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("murasaki-surface: cannot connect to the D-Bus session bus");
        return false;
    }
    // A host process leaves the Surface1 name to the policy server (the main murasaki-surface).
    if (!isHost()
        && (!bus.registerObject(QString(kSurfaceDBusPath), m_service, QDBusConnection::ExportScriptableSlots
                                                          | QDBusConnection::ExportScriptableSignals)
            || !bus.registerService(QString(kSurfaceDBusService)))) {
        qCritical("murasaki-surface: %s is already taken; is another murasaki-surface running?",
                  qPrintable(QString(kSurfaceDBusService)));
        return false;
    }

    m_monitor = isHost() ? std::make_unique<NullMonitor>()
        : m_monitorFactory ? m_monitorFactory() : createMonitor();
    connect(m_monitor.get(), &SessionMonitor::stateChanged, this, &SurfaceController::onSessionChanged);
    // The pause rules' inputs. Both services are optional: their absence is the quiet state "no game, on mains". A
    // host takes the verdict from the policy server instead.
    if (!isHost()) {
        m_gameMode = std::make_unique<GameModeWatcher>(bus, this);
        connect(m_gameMode.get(), &GameModeWatcher::changed, this, &SurfaceController::applyPlayback);
        m_gameMode->start();
        m_power = std::make_unique<PowerWatcher>(m_powerBus.value_or(QDBusConnection::systemBus()), this);
        connect(m_power.get(), &PowerWatcher::changed, this, &SurfaceController::applyPlayback);
        m_power->start();
    }

    if (!bus.connect(QString(kDBusService), QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s,
                     this, SLOT(refreshAll())))
        qWarning("murasaki-surface: cannot subscribe to AssignmentsChanged; changes show up after a restart");
    if (!bus.connect(QString(kDBusService), QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s,
                     this, SLOT(loadSettings())))
        qWarning("murasaki-surface: cannot subscribe to SettingsChanged; settings apply after a restart");
    // Wallpapers stay as they are while murasakid is away; reload everything when it comes back.
    m_daemonWatcher = new QDBusServiceWatcher(QString(kDBusService), bus,
                                              QDBusServiceWatcher::WatchForRegistration, this);
    // The assets dir decides whether scenes can play, so it is read before anything is resolved again.
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceRegistered, this, &SurfaceController::loadSettings);
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceRegistered, this, &SurfaceController::onDaemonRegistered);
    m_assetsWait.setSingleShot(true);
    m_assetsWait.setInterval(1000);
    connect(&m_assetsWait, &QTimer::timeout, this, [this] { finishAssetsRequest(m_assetsRequest, {}, false); });
    // Windows wait for the presenter setting; a daemon that does not answer in time means the default.
    m_presenterKnown = isHost();
    m_presenterWait.setSingleShot(true);
    m_presenterWait.setInterval(1000);
    connect(&m_presenterWait, &QTimer::timeout, this, [this] { applySettings(m_webFpsLimit, m_pauseWhenCovered, m_presenter, m_sceneEngine); });
    if (!m_presenterKnown)
        m_presenterWait.start();
    loadSettings();
    loadAssetsDir();

    if (isHost()) {
        checkDaemonVersion();
        if (!addOutput(m_hostOutput, nullptr))
            return false;
        m_registerRetry.setSingleShot(true);
        connect(&m_registerRetry, &QTimer::timeout, this, &SurfaceController::registerWithPolicyServer);
        // The policy server is the main murasaki-surface; it may start before or after this host, or restart.
        m_policyWatcher = new QDBusServiceWatcher(QString(kSurfaceDBusService), bus,
                                                  QDBusServiceWatcher::WatchForRegistration
                                                      | QDBusServiceWatcher::WatchForUnregistration,
                                                  this);
        connect(m_policyWatcher, &QDBusServiceWatcher::serviceRegistered, this, &SurfaceController::registerWithPolicyServer);
        connect(m_policyWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &SurfaceController::onPolicyServerGone);
        if (!bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostStateChanged"_s, this, SLOT(onHostStateChanged(QString))))
            qWarning("murasaki-surface: cannot subscribe to HostStateChanged; playback stays as it is");
        if (!bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostCursor"_s, this, SLOT(onHostCursor(QString,double,double,double,double)))
            || !bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                            u"HostCursorLeft"_s, this, SLOT(onHostCursorLeft(QString))))
            qWarning("murasaki-surface: cannot subscribe to HostCursor; the wallpaper only sees the mouse the plugin gets");
        if (!bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostPointer"_s, this, SLOT(onHostPointer(QString,double,double,double,double,uint,int,int))))
            qWarning("murasaki-surface: cannot subscribe to HostPointer; clicks of the interaction mode will not reach this wallpaper");
        if (!bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostCommand"_s, this, SLOT(onHostCommand(QString,QString))))
            qWarning("murasaki-surface: cannot subscribe to HostCommand; \"retry\" in the GUI will not reach this wallpaper");
        if (bus.interface()->isServiceRegistered(QString(kSurfaceDBusService)))
            registerWithPolicyServer();
        else
            onPolicyServerGone();
    } else {
        const auto screens = QGuiApplication::screens();
        for (QScreen *screen : screens)
            addScreen(screen);
        connect(qGuiApp, &QGuiApplication::screenAdded, this, &SurfaceController::addScreen);
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, &SurfaceController::removeScreen);
    }
    // Windows exist (black) from the start; what they show waits for the monitor's first report so the "*"
    // wallpaper does not flash on a non-default desktop.
    m_startupWait.setSingleShot(true);
    m_startupWait.setInterval(300);
    connect(&m_startupWait, &QTimer::timeout, this, &SurfaceController::resolveOutputs);
    if (!isHost()) {
        m_idle = createIdleSource();
        if (m_idle) {
            connect(m_idle.get(), &IdleSource::changed, this, [this] { publishSessionFacts(m_monitor ? m_monitor->state() : SessionState(), m_factsQuiet); });
            m_idle->start();
        }
    }
    m_monitor->start();
    if (dynamic_cast<NullMonitor *>(m_monitor.get()))
        resolveOutputs();
    else
        m_startupWait.start();
    return true;
}

// Whether the user is away: from the compositor's ext-idle-notify on Wayland (the daemon's idle triggers prefer it to logind's hint, which Plasma never sets). Nothing on
// a platform without it, and nothing in a test that does not give one.
std::unique_ptr<IdleSource> SurfaceController::createIdleSource()
{
    if (m_idleFactory)
        return m_idleFactory();
#ifdef MURASAKI_HAVE_IDLE_NOTIFY
    if (QGuiApplication::platformName().startsWith(u"wayland"_s))
        return std::make_unique<WaylandIdleMonitor>(kIdleNotifyTimeoutMs);
#endif
    return nullptr;
}

std::unique_ptr<SessionMonitor> SurfaceController::createMonitor()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const bool kwinOnBus = QDBusConnection::sessionBus().interface()->isServiceRegistered(u"org.kde.KWin"_s);
    QSet<QString> globals;
#ifdef MURASAKI_HAVE_FOREIGN_TOPLEVEL
    if (!env.value(u"WAYLAND_DISPLAY"_s).isEmpty() || !env.value(u"XDG_RUNTIME_DIR"_s).isEmpty())
        globals = probeWaylandGlobals();
#endif
    const bool gnomeShellOnBus = QDBusConnection::sessionBus().interface()->isServiceRegistered(u"org.gnome.Shell"_s);
    m_compositor = selectCompositorProfile(env, kwinOnBus, globals, gnomeShellOnBus,
                                           QGuiApplication::platformName().startsWith(u"xcb"_s));
    qInfo("murasaki-surface: %s", qPrintable(m_compositor.summary));

    std::unique_ptr<SessionMonitor> monitor;
    switch (m_compositor.monitor) {
    case MonitorKind::KWin: {
        auto kwin = std::make_unique<KWinMonitor>();
        connect(m_service, &SurfaceService::kwinStateReported, kwin.get(), &KWinMonitor::report);
        monitor = std::move(kwin);
        break;
    }
    case MonitorKind::Sway:
        monitor = std::make_unique<SwayMonitor>(env.value(u"SWAYSOCK"_s));
        break;
    case MonitorKind::Hyprland:
        if (const QString dir = HyprlandMonitor::socketDir(env); !dir.isEmpty())
            monitor = std::make_unique<HyprlandMonitor>(dir);
        else
            qWarning("murasaki-surface: Hyprland socket not found; wallpapers will not pause or follow workspaces");
        break;
    case MonitorKind::GnomeShell: {
        // The Murasaki extension inside the shell reports the windows; without it nothing arrives and the wallpapers
        // simply never pause.
        auto shell = std::make_unique<ShellMonitor>();
        connect(m_service, &SurfaceService::shellStateReported, shell.get(), &ShellMonitor::report);
        monitor = std::move(shell);
        break;
    }
    case MonitorKind::X11Ewmh:
#ifdef MURASAKI_HAVE_X11
        // The EWMH state of the window manager, whichever desktop environment it belongs to.
        monitor = std::make_unique<X11Monitor>();
#endif
        break;
    case MonitorKind::ForeignToplevel:
#ifdef MURASAKI_HAVE_FOREIGN_TOPLEVEL
        monitor = std::make_unique<ForeignToplevelMonitor>();
#endif
        break;
    case MonitorKind::None:
        qInfo("murasaki-surface: no supported compositor monitor; wallpapers will not pause or follow desktops");
        break;
    }
    if (!monitor)
        return std::make_unique<NullMonitor>();
#ifdef MURASAKI_HAVE_FOREIGN_TOPLEVEL
    // Sway and Hyprland know what covers an output, not which application it is: the pause rules get that from the
    // foreign-toplevel protocol.
    if (m_compositor.appsFromToplevels)
        return std::make_unique<MergedMonitor>(std::move(monitor), std::make_unique<ForeignToplevelMonitor>(QString(), true));
#endif
    return monitor;
}

QString SurfaceController::nameOf(QScreen *screen) const
{
    return m_screenName ? m_screenName(screen) : screen->name();
}

void SurfaceController::addScreen(QScreen *screen)
{
    addOutput(nameOf(screen), screen);
}

bool SurfaceController::addOutput(const QString &name, QScreen *screen)
{
    if (m_outputs.count(name))
        return true;
    Output &output = m_outputs[name];
    output.screen = screen;
    if (screen)
        connect(screen, &QScreen::geometryChanged, this, &SurfaceController::updateSpans);
    if (!createSink(name)) {
        m_outputs.erase(name);
        return false;
    }
    if (m_resolving)
        refresh(name);
    m_service->notifyOutputsChanged();
    updateSpans(); // the canvas of the other outputs just changed
    return true;
}

// Whether this process draws the output itself: not when the Plasma plugin does.
bool SurfaceController::wantsWindow(const QString &name) const
{
    if (isHost())
        return true;
    if (!m_presenterKnown || m_presenter == kPresenterPlasma)
        return false;
    return m_presenter == kPresenterLayerShell || !m_service->isHosted(name);
}

bool SurfaceController::createSink(const QString &name)
{
    Output &output = m_outputs.at(name);
    if (output.sink || !wantsWindow(name))
        return true;
    std::unique_ptr<OutputSink> sink = m_sinkFactory({name, output.screen, &m_engine, m_webProfiles});
    if (!sink) {
        qWarning("murasaki-surface: cannot create the output for %s", qPrintable(name));
        return false;
    }
    output.sink = std::move(sink);
    if (!isHost())
        qInfo("murasaki-surface: window for %s", qPrintable(name));
    output.sink->setWantsAudioListener([this] { updateAudioCapture(); });
    output.sink->setHealthListener([this, name](const SinkHealthEvent &event) { onHealthEvent(name, event); });
    output.sink->setFrameRateLimit(frameRateLimit());
    if (!output.content.kind.isEmpty()) { // content resolved before the window existed
        output.sink->setContent(contentMap(name, output.content, {}));
        output.sink->setInteractive(output.content.kind == u"web" || output.content.kind == u"scene");
    }
    return true;
}

void SurfaceController::syncSink(const QString &name)
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end())
        return;
    if (wantsWindow(name)) {
        createSink(name);
    } else if (it->second.sink) {
        std::unique_ptr<OutputSink> sink = std::move(it->second.sink);
        sink.reset(); // frees the player or page of an output the plugin draws
        qInfo("murasaki-surface: no window for %s any more (hosted or presenter %s)", qPrintable(name),
              qPrintable(m_presenter));
    }
}

void SurfaceController::onHostsChanged(const QString &output)
{
    syncSink(output);
    applyPlayback();
    updateCursorReporting();
    // A host that arrives while the cursor already rests gets its position without waiting for a movement.
    if (m_cursorReporting && m_haveCursor) {
        if (const auto hit = mapCursor(cursorScreens(), m_lastCursor); hit && hit->output == output)
            m_service->sendHostCursor(output, hit->local, hit->size);
    }
}

// ---- cursor (policy server side) ------------------------------------------------------------------------------

// The Plasma desktop above a wallpaper takes the pointer events, so the host never sees the mouse. KWin does: its
// script reports the cursor only while a visible host can use it, and the position goes to the host of the output
// under it (never to the others).
void SurfaceController::updateCursorReporting()
{
    const bool wanted = !isHost() && m_service->hasVisibleHost();
    if (wanted == m_cursorReporting)
        return;
    m_cursorReporting = wanted;
    if (m_monitor)
        m_monitor->setCursorReporting(wanted);
    if (!wanted) {
        m_cursorFlush.stop();
        m_cursorRouter.reset();
        m_haveCursor = false;
    }
}

QList<CursorScreen> SurfaceController::cursorScreens() const
{
    QList<CursorScreen> screens;
    for (const auto &[name, output] : m_outputs) {
        if (output.screen)
            screens.append({name, output.screen->geometry()});
    }
    return screens;
}

void SurfaceController::onCursorReported(const QPointF &global)
{
    if (!m_cursorReporting)
        return;
    m_lastCursor = global;
    m_haveCursor = true;
    dispatchCursor(m_cursorRouter.update(cursorScreens(), global, m_cursorClock.elapsed()));
    armCursorFlush();
}

void SurfaceController::armCursorFlush()
{
    const int delay = m_cursorRouter.pendingDelayMs(m_cursorClock.elapsed());
    if (delay >= 0 && !m_cursorFlush.isActive())
        m_cursorFlush.start(std::max(delay, 1));
}

void SurfaceController::dispatchCursor(const QList<CursorMessage> &messages)
{
    for (const CursorMessage &message : messages) {
        if (message.kind == CursorMessage::Kind::Move)
            m_service->sendHostCursor(message.output, message.local, message.size);
        else
            m_service->sendHostCursorLeft(message.output);
    }
}

// ---- cursor (host side) ---------------------------------------------------------------------------------------

void SurfaceController::onHostCursor(const QString &output, double x, double y, double width, double height)
{
    if (!isHost() || output != m_hostOutput || !(width > 0) || !(height > 0))
        return;
    const auto it = m_outputs.find(output);
    if (it != m_outputs.end() && it->second.sink)
        it->second.sink->setCursorFraction(x / width, y / height);
}

// The overlay's interaction mode: clicks and wheel above the desktop, for the wallpaper of this host's output.
void SurfaceController::onHostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY)
{
    if (!isHost() || output != m_hostOutput || !(width > 0) || !(height > 0))
        return;
    const auto it = m_outputs.find(output);
    if (it != m_outputs.end() && it->second.sink)
        it->second.sink->injectPointerFraction(x / width, y / height, buttons, wheelX, wheelY);
}

// The host keeps the last position: moving the pointer back to the centre would jump a parallax scene.
void SurfaceController::onHostCursorLeft(const QString &output)
{
    if (isHost() && output == m_hostOutput && qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG"))
        qInfo("murasaki.host: cursor left %s; keeping its last position", qPrintable(output));
}

void SurfaceController::removeScreen(QScreen *screen)
{
    const auto it = m_outputs.find(nameOf(screen));
    if (it == m_outputs.end())
        return;
    std::unique_ptr<OutputSink> sink = std::move(it->second.sink);
    m_outputs.erase(it); // before the delete: nothing may reach a dying sink through the map
    m_webProfiles->release(nameOf(screen));
    sink.reset();
    updateReadableRoots();
    updateSpans(); // the canvas of the remaining outputs just changed
    applyPlayback(); // audio ownership may move to another output
}

// Reads the daemon's settings. A daemon without Settings() (older than API 3) means the defaults; any other
// failure (daemon away, transient error) keeps the values already in force.
void SurfaceController::loadSettings()
{
    const quint64 request = ++m_settingsRequest;
    const QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                             QString(kDBusInterface), u"Settings"_s);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (request != m_settingsRequest)
            return;
        if (reply.isError()) {
            if (reply.error().type() == QDBusError::UnknownMethod)
                applySettings(kDefaultWebFpsLimit, kDefaultPauseWhenCovered, QString(kDefaultPresenter), QString(kDefaultSceneEngine));
            else
                applySettings(m_webFpsLimit, m_pauseWhenCovered, m_presenter, m_sceneEngine); // keeps the values, ends the wait
            if (reply.error().type() == QDBusError::UnknownMethod) {
                applySandboxSetting(kDefaultSandboxEngines, kDefaultRequireSandbox);
                applyRenderScale(kDefaultSceneRenderScale);
                // a daemon without Settings() has none of ours: the defaults
                media::MediaHub::shared().setRemoteArt(kDefaultMediaRemoteArt);
                media::MediaHub::shared().setShare(kDefaultMediaShare);
                shortcut::ShortcutHub::shared().setLaunchAllowed(kDefaultShortcutsLaunch);
            } else if (!std::exchange(m_settingsRetried, true)) {
                // a transient failure: once more shortly (the media settings count as off until they are read)
                QTimer::singleShot(5000, this, &SurfaceController::loadSettings);
            }
            return;
        }
        const QJsonObject values = QJsonDocument::fromJson(reply.value().toUtf8()).object();
        const int fps = values.value(u"webFpsLimit"_s).toInt(kDefaultWebFpsLimit);
        const QString presenter = values.value(u"presenter"_s).toString();
        const QString sceneEngine = values.value(u"sceneEngine"_s).toString(); // absent from an older daemon: the default
        // The pause rules are absent from a daemon older than revision 7: the defaults.
        PauseRules rules;
        rules.pauseOnGame = values.value(u"pauseOnGame"_s).toBool(kDefaultPauseOnGame);
        rules.battery = batteryModeFromString(values.value(u"batteryMode"_s).toString());
        for (const QJsonValue &app : values.value(u"pauseApps"_s).toArray()) {
            if (app.isString())
                rules.apps.append(app.toString());
        }
        applySettings(std::clamp(fps, kMinWebFpsLimit, kMaxWebFpsLimit),
                      values.value(u"pauseWhenCovered"_s).toBool(kDefaultPauseWhenCovered),
                      isValidPresenter(presenter) ? presenter : QString(kDefaultPresenter),
                      isValidSceneEngine(sceneEngine) ? sceneEngine : QString(kDefaultSceneEngine));
        const double renderScale = values.value(u"sceneRenderScale"_s).toDouble(kDefaultSceneRenderScale); // absent from an older daemon: the default
        applyRenderScale(renderScale <= 0.0 ? 0.0 : std::clamp(renderScale, kMinSceneRenderScale, kMaxSceneRenderScale)); // 0: automatic
        applySandboxSetting(values.value(u"sandboxEngines"_s).toBool(kDefaultSandboxEngines),
                            values.value(u"requireSandbox"_s).toBool(kDefaultRequireSandbox)); // absent: the defaults
        // (absent from an older daemon: the defaults; a settings call that failed leaves both unknown, which the hub reads as "no")
        media::MediaHub::shared().setRemoteArt(values.value(u"mediaRemoteArt"_s).toBool(kDefaultMediaRemoteArt)); // the host downloads the https cover
        media::MediaHub::shared().setShare(values.value(u"mediaShare"_s).toBool(kDefaultMediaShare));          // what plays reaches the scenes
        shortcut::ShortcutHub::shared().setLaunchAllowed(values.value(u"shortcutsLaunch"_s).toBool(kDefaultShortcutsLaunch)); // a click may start the application the user chose
        if (std::exchange(m_pauseRules, rules) != rules)
            applyPlayback();
    });
}

// Scenes need Wallpaper Engine's assets directory, which the daemon evaluates when it starts. Resolving waits
// for the answer (a failed or slow one counts as "unchanged"), so a scene never flashes its preview first. A
// changed value changes the scenes' content keys, so everything is resolved again.
void SurfaceController::loadAssetsDir()
{
    const quint64 request = ++m_assetsRequest;
    m_assetsKnown = false;
    m_assetsWait.start();
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                       u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    call.setArguments({QString(kDBusInterface), u"WallpaperEngineAssetsDir"_s});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        finishAssetsRequest(request, reply.isError() ? QString() : reply.value().variant().toString(),
                            !reply.isError());
    });
}

void SurfaceController::finishAssetsRequest(quint64 request, const QString &dir, bool ok)
{
    if (request != m_assetsRequest)
        return;
    // The wait gives up after a second, but the answer of a daemon that was busy starting still counts when it comes: its name was
    // on the bus all along, so no serviceRegistered would ask again, and the scenes would stay previews ("assets not found").
    if (m_assetsKnown && !ok)
        return;
    m_assetsKnown = true;
    m_assetsWait.stop();
    const bool changed = ok && dir != m_assetsDir;
    if (ok)
        m_assetsDir = dir;
    if (changed || m_refreshPending) {
        m_refreshPending = false;
        refreshAll();
    }
}

// Wallpapers stay as they are while murasakid is away; reload everything when it comes back, after its assets
// dir is known again.
void SurfaceController::onDaemonRegistered()
{
    if (isHost())
        checkDaemonVersion();
    m_refreshPending = true;
    loadAssetsDir();
}

// A host speaks the API revision it was built with; an older daemon (not yet restarted after an update) would
// misread it. A daemon that is not there yet is checked again when it registers.
void SurfaceController::checkDaemonVersion()
{
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                       u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    call.setArguments({QString(kDBusInterface), u"ApiVersion"_s});
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *w;
        if (reply.isError())
            return; // no daemon (yet) or no such property; a registering daemon triggers another check
        const quint32 version = reply.value().variant().toUInt();
        if (version >= kMinDBusApiVersion)
            return;
        qCritical("murasaki-surface: murasakid speaks API revision %u but %u is needed; update and restart murasakid",
                  version, unsigned(kMinDBusApiVersion));
        emit incompatibleDaemon(version);
    });
}

// Scenes on screen start again at the other resolution (the content key of a scene includes it).
void SurfaceController::applyRenderScale(double scale)
{
    if (std::exchange(m_sceneRenderScale, scale) == scale)
        return;
    for (const auto &[name, output] : m_outputs) {
        if (output.sink && output.content.kind == u"scene")
            output.sink->setContent(contentMap(name, output.content, {}));
    }
}

void SurfaceController::applySandboxSetting(bool sandbox, bool require)
{
    const bool changedSandbox = std::exchange(m_sandboxEngines, sandbox) != sandbox;
    const bool changedRequire = std::exchange(m_requireSandbox, require) != require;
    if (!changedSandbox && !changedRequire)
        return;
    // Scenes on screen start again outside or inside the sandbox (the content key of a scene includes it).
    for (const auto &[name, output] : m_outputs) {
        if (output.sink && output.content.kind == u"scene")
            output.sink->setContent(contentMap(name, output.content, {}));
    }
}

void SurfaceController::applySettings(int webFpsLimit, bool pauseWhenCovered, const QString &presenter,
                                      const QString &sceneEngine)
{
    m_presenterWait.stop();
    m_webFpsLimit = webFpsLimit;
    if (std::exchange(m_sceneEngine, sceneEngine) != sceneEngine) {
        // Scenes already on screen start again under the new setting (the content key of a scene includes the engine).
        for (const auto &[name, output] : m_outputs) {
            if (output.sink && output.content.kind == u"scene")
                output.sink->setContent(contentMap(name, output.content, {}));
        }
    }
    for (const auto &[name, output] : m_outputs) {
        if (output.sink)
            output.sink->setFrameRateLimit(frameRateLimit());
    }
    const bool presenterChanged = !m_presenterKnown || m_presenter != presenter;
    m_presenterKnown = true;
    m_presenter = presenter;
    if (presenterChanged && !isHost()) {
        std::vector<QString> names;
        for (const auto &[name, output] : m_outputs)
            names.push_back(name);
        for (const QString &name : names)
            syncSink(name);
    }
    if (std::exchange(m_pauseWhenCovered, pauseWhenCovered) != pauseWhenCovered || presenterChanged)
        applyPlayback();
    else
        m_service->notifyHostStates();
}

// A host follows the policy server's frame rate; otherwise it is the daemon's webFpsLimit.
int SurfaceController::frameRateLimit() const
{
    return m_hostPolicy ? m_hostPolicy->frameRateLimit : m_webFpsLimit;
}

QString SurfaceController::desktopFor(const QString &name) const
{
    if (isHost())
        return m_hostPolicy ? m_hostPolicy->desktop : QString(kWildcard);
    return m_monitor ? m_monitor->state().forOutput(name).desktop : QString(kWildcard);
}

void SurfaceController::resolveOutputs()
{
    m_startupWait.stop();
    if (std::exchange(m_resolving, true))
        return;
    refreshAll();
}

void SurfaceController::refreshAll()
{
    if (!m_resolving)
        return; // resolveOutputs() will load everything
    for (const auto &[name, output] : m_outputs)
        refresh(name);
}

void SurfaceController::refresh(const QString &name, bool isRetry)
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end())
        return;
    if (!m_assetsKnown) {
        m_refreshPending = true; // finishAssetsRequest() resumes
        return;
    }
    const QString desktop = desktopFor(name);
    const quint64 request = ++m_nextRequest;
    it->second.desktop = desktop;
    it->second.request = request;
    it->second.pending = true;

    QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                       QString(kDBusInterface), u"ResolveAssignment"_s);
    call.setArguments({name, desktop});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, name, request, desktop, isRetry](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        const auto it = m_outputs.find(name);
        if (it == m_outputs.end() || it->second.request != request)
            return; // the screen left, or a newer request superseded this one
        it->second.pending = false;
        if (reply.isError()) {
            qWarning("murasaki-surface: cannot ask murasakid about %s: %s", qPrintable(name),
                     qPrintable(reply.error().message()));
            // The serviceRegistered path covers a daemon that was away; this covers a transient failure.
            if (!isRetry)
                QTimer::singleShot(2000, this, [this, name] { refresh(name, true); });
            applyPlayback();
            return;
        }
        const WallpaperContent content =
            WallpaperContent::fromResolveReply(QJsonDocument::fromJson(reply.value().toUtf8()).object(), m_assetsDir);
        if (!content.itemId.isEmpty() && content.kind == u"none")
            qWarning("murasaki-surface: %s is assigned to %s but is not in the library", qPrintable(content.itemId),
                     qPrintable(name));
        if (content.reason != it->second.reason) {
            it->second.reason = content.reason;
            if (!content.reason.isEmpty())
                qWarning("murasaki-surface: %s shows its preview: %s", qPrintable(content.itemId),
                         qPrintable(content.reason));
        }
        // Same page or scene, other property values: the surface pushes the difference into the running one.
        const WallpaperContent &previous = it->second.content;
        QJsonObject changedProperties;
        if (previous.key == content.key && previous.kind == content.kind) {
            if (content.kind == u"web")
                changedProperties = changedUserProperties(previous.userProperties, content.userProperties);
            else if (content.kind == u"scene")
                changedProperties = changedUserProperties(previous.sceneProperties, content.sceneProperties);
        }
        it->second.content = content;
        it->second.shownDesktop = desktop;
        updateReadableRoots(); // before the new page starts loading
        if (it->second.sink) {
            it->second.sink->setContent(contentMap(name, content, changedProperties));
            it->second.sink->setInteractive(content.kind == u"web" || content.kind == u"scene");
        }
        applyPlayback();
    });
}

void SurfaceController::onSessionChanged()
{
    if (!m_resolving) {
        resolveOutputs(); // the first report arrived
        return;
    }
    const SessionState state = m_monitor->state();
    for (const auto &[name, output] : m_outputs) {
        if (state.forOutput(name).desktop != output.desktop)
            refresh(name);
    }
    applyPlayback();
}

void SurfaceController::updateReadableRoots()
{
    QStringList roots;
    for (const auto &[name, output] : m_outputs)
        roots += output.content.readableRoots;
    roots.removeDuplicates();
    m_webProfiles->setRoots(roots);
}

void SurfaceController::setExternalPause(bool paused)
{
    if (std::exchange(m_externalPause, paused) == paused)
        return;
    sendHostVisible();
    applyPlayback();
}

PauseFacts SurfaceController::pauseFacts() const
{
    PauseFacts facts;
    facts.gameRunning = m_gameMode && m_gameMode->active();
    facts.onBattery = m_power && m_power->onBattery();
    if (m_monitor) {
        const SessionState state = m_monitor->state();
        facts.activeApp = state.activeApp;
        facts.fullscreenApps = state.fullscreenApps;
        facts.windowsKnown = m_monitor->knowsOcclusion();
    }
    return facts;
}

bool SurfaceController::pausedByRule(const Output &output) const
{
    return m_pauseVerdict.pauseAll && (!m_pauseVerdict.coveredOnly || output.occluded);
}

void SurfaceController::applyPlayback()
{
    if (!isHost())
        m_pauseVerdict = evaluatePauseRules(m_pauseRules, pauseFacts());
    const SessionState state = m_monitor ? m_monitor->state() : SessionState();
    QList<PlaybackInput> inputs;
    for (const auto &[name, output] : m_outputs) {
        // A hosted output whose plugin hides it takes no part in who owns the audio.
        const bool hidden = !m_service->hostVisible(name);
        const bool showing = output.content.kind != u"none" && !output.content.kind.isEmpty() && !hidden;
        // An output whose content is being swapped still shows (and sounds) its previous content, so it keeps
        // its previous occlusion too: the new desktop's windows must not pause the old desktop's wallpaper.
        const bool occluded = output.pending ? output.occluded : state.forOutput(name).occluded;
        inputs.append({name, showing ? output.content.itemId : QString(), output.content.muted, occluded});
    }
    const QHash<QString, PlaybackDecision> decisions = decidePlayback(inputs, m_pauseWhenCovered);
    for (auto &[name, output] : m_outputs) {
        if (output.pending)
            continue; // keep the previous decision until the reply lands
        output.occluded = state.forOutput(name).occluded;
        output.decision = decisions.value(name);
        if (isHost() && m_hostPolicy)
            output.decision = {m_hostPolicy->paused, m_hostPolicy->audible};
        if (pausedByRule(output)) // a game (where it covers the screen), the battery or an excluded application: it stops and is silent
            output.decision = {true, false};
        if (m_externalPause || !m_service->hostVisible(name))
            output.decision = {true, false};
        output.paused = output.decision.paused;
        output.fpsCap = isHost() ? (m_hostPolicy ? m_hostPolicy->fpsCap : 0) : m_pauseVerdict.fpsCap;
        if (output.sink) {
            output.sink->setPlayback(output.decision.paused, output.decision.audible);
            output.sink->setPlaybackCap(output.fpsCap);
        }
    }
    updateAudioCapture();
    if (!isHost()) { // the policy server decides for the pets too
        const bool game = m_pauseVerdict.pauseAll && (m_pauseVerdict.reason == PauseReason::Game || m_pauseVerdict.reason == PauseReason::App);
        m_service->setPetAudioMuted(game || !pauseFacts().fullscreenApps.isEmpty());
        publishSessionFacts(state, game);
    }
    m_service->notifyOutputsChanged();
    m_service->notifyHostStates();
}

// What the pets' rules (and the automations of a later phase) read: per output whether a fullscreen window is there and which application, the open applications, the
// game and battery facts. A session with no compositor monitor says `known: false` and nothing else is claimed (docs/pets/session-facts.md).
void SurfaceController::publishSessionFacts(const SessionState &state, bool quiet)
{
    m_factsQuiet = quiet;
    QStringList names;
    for (const auto &[name, output] : m_outputs)
        names.append(name);
    for (auto it = state.outputs.cbegin(); it != state.outputs.cend(); ++it) {
        if (!names.contains(it.key()))
            names.append(it.key());
    }
    names.sort();
    const bool known = m_monitor && m_compositor.monitor != MonitorKind::None;
    QJsonObject outputs, publicOutputs;
    bool anyFullscreen = false;
    for (const QString &name : std::as_const(names)) {
        const OutputSession session = state.forOutput(name);
        anyFullscreen = anyFullscreen || session.fullscreen;
        outputs.insert(name, QJsonObject{{u"fullscreen"_s, session.fullscreen}, {u"app"_s, session.fullscreenApp}, {u"occluded"_s, session.occluded}});
        publicOutputs.insert(name, QJsonObject{{u"fullscreen"_s, session.fullscreen}, {u"occluded"_s, session.occluded}});
    }
    const PauseFacts facts = pauseFacts();
    const bool idleKnown = m_idle && m_idle->known();
    QJsonObject json{{u"outputs"_s, outputs},
                           {u"anyFullscreen"_s, anyFullscreen},
                           {u"activeApp"_s, state.activeApp},
                           {u"openApps"_s, QJsonArray::fromStringList(state.openApps)},
                           {u"gameRunning"_s, facts.gameRunning},
                           {u"onBattery"_s, facts.onBattery},
                           {u"quiet"_s, quiet},
                           {u"known"_s, known},
                           {u"monitor"_s, m_compositor.summary.left(80)}};
    if (idleKnown) { // (only when the compositor tells; the daemon uses logind's hint otherwise)
        json.insert(u"idleKnown"_s, true);
        json.insert(u"idle"_s, m_idle->idle());
        json.insert(u"idleAfterMs"_s, m_idle->afterMs());
    }
    // What any process of the session may read: where a window is fullscreen or covers an output, and nothing that names an application.
    const QJsonObject publicJson{{u"outputs"_s, publicOutputs}, {u"anyFullscreen"_s, anyFullscreen}, {u"known"_s, known}};
    QJsonObject publicFullscreen;
    for (auto it = publicOutputs.begin(); it != publicOutputs.end(); ++it)
        publicFullscreen.insert(it.key(), QJsonObject{{u"fullscreen"_s, it.value().toObject().value(u"fullscreen"_s)}});
    const auto text = [](const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
    m_service->setSessionFacts({text(json), text(outputFullscreenJson(state, names)), text(publicJson), text(publicFullscreen)});
}

// Only a web item that is running and listening gets the spectrum; covered or paused outputs get nothing.
bool SurfaceController::wantsSpectrum(const Output &output) const
{
    return output.sink && outputWantsSpectrum(output.content.kind, output.paused, output.sink->wantsAudio());
}

// The PipeWire stream exists only while some output can use it.
void SurfaceController::updateAudioCapture()
{
    QList<SpectrumInput> inputs;
    for (const auto &[name, output] : m_outputs)
        inputs.append({output.content.kind, output.paused, output.sink && output.sink->wantsAudio()});
    m_audio.setActive(anyWantsSpectrum(inputs));
}

void SurfaceController::pushSpectrum(const QList<float> &spectrum)
{
    for (const auto &[name, output] : m_outputs) {
        if (wantsSpectrum(output))
            output.sink->setAudioSpectrum(spectrum);
    }
}

QVariantMap SurfaceController::contentMap(const QString &name, const WallpaperContent &content, const QJsonObject &changedProperties)
{
    // The engine is part of what a scene is, so changing it replaces the delegate (the crossfade starts the new one)
    // instead of patching the running one.
    const bool scene = content.kind == u"scene";
    // What the user asked for an item ("retry", or an engine of its own) belongs to that item.
    QString engine = m_sceneEngine;
    int retries = 0;
    const auto own = m_outputs.find(name);
    if (own != m_outputs.end()) {
        Output &output = own->second;
        if (output.overrideItem != content.itemId) {
            output.overrideItem.clear();
            output.engineOverride.clear();
            output.retries = 0;
        }
        if (!output.engineOverride.isEmpty())
            engine = output.engineOverride;
        retries = output.retries;
    }
    QVariantMap map{
        {u"key"_s, (scene ? content.key + (m_sandboxEngines ? QString() : u"|nosandbox"_s) + (m_requireSandbox ? u"|requiresandbox"_s : QString()) + u"|engine:"_s + engine
                         + (m_sceneRenderScale > 0.0 ? u"|scale:"_s + QString::number(m_sceneRenderScale, 'g', 4) : QString())
                         : content.key) + (retries > 0 ? u"|retry:"_s + QString::number(retries) : QString())},
        {u"noFrameTimeoutMs"_s, m_noFrameTimeoutMs},
        {u"sceneEngine"_s, engine},
        {u"sandboxEngines"_s, m_sandboxEngines},
        {u"requireSandbox"_s, m_requireSandbox},
        {u"sceneRenderScale"_s, m_sceneRenderScale},
        {u"kind"_s, content.kind},
        {u"itemId"_s, content.itemId},
        {u"entryFile"_s, content.entryFile},
        {u"fill"_s, content.fill},
        {u"script"_s, content.script},
        {u"networkAllowed"_s, content.networkAllowed},
        {u"properties"_s,
         (content.kind == u"scene" ? content.sceneProperties : content.userProperties).toVariantMap()},
        {u"itemDir"_s, content.contentDir},
        {u"desktop"_s, m_outputs.count(name) ? m_outputs.at(name).shownDesktop : desktopFor(name)}, // what the assignment was asked for (a shortcut launch is judged for it)
        {u"assetsDir"_s, content.assetsDir},
        {u"changedProperties"_s, changedProperties.toVariantMap()},
        {u"previewUrl"_s,
         content.previewPath.isEmpty() ? QString() : QUrl::fromLocalFile(content.previewPath).toString()},
    };
    if (content.span) {
        const QVariantMap span = spanFor(name);
        map.insert(u"span"_s, span);
        const auto it = m_outputs.find(name);
        if (it != m_outputs.end())
            it->second.lastSpan = span;
    }
    if (content.kind == u"web") {
        // A profile of its own for what this output shows; it carries the item's permission (see WebProfileFactory).
        const auto web = m_webProfiles->acquire(name, content.key, content.contentDir, content.networkAllowed);
        const QUrl url = itemEntryUrl(web.itemToken, content.contentDir, content.entryFile);
        if (url.isValid()) {
            map.insert(u"contentUrl"_s, url.toString(QUrl::FullyEncoded));
            map.insert(u"webProfile"_s, QVariant::fromValue(static_cast<QObject *>(web.profile)));
            map.insert(u"webLease"_s, QVariant::fromValue(static_cast<QObject *>(web.lease)));
            map.insert(u"proxyToken"_s, web.proxyToken);
            map.insert(u"networkAllowed"_s, web.networkAllowed); // false when the gate could not start
        } else {
            map.insert(u"kind"_s, u"preview"_s);
            m_webProfiles->release(name);
        }
    } else {
        m_webProfiles->release(name);
    }
    if (own != m_outputs.end()) {
        if (own->second.health.key() != map.value(u"key"_s).toString())
            own->second.contentClock.restart();
        if (own->second.health.begin(map.value(u"key"_s).toString()))
            publishHealth(name);
    } // new content: whatever failed before is history
    m_heapTrim.start(); // restarts on every switch: a storm trims once, when it ends
    return map;
}

QVariantMap SurfaceController::spanViewport(const QString &name) const
{
    QList<SpanOutput> outputs;
    for (const auto &[outputName, output] : m_outputs) {
        if (output.screen)
            outputs.append({outputName, QRectF(output.screen->geometry())});
    }
    const SpanLayout layout = spanLayout(outputs);
    const auto it = layout.viewports.constFind(name);
    if (!layout.isValid() || it == layout.viewports.cend())
        return {};
    return QVariantMap{{u"canvasW"_s, layout.canvas.width()}, {u"canvasH"_s, layout.canvas.height()},
                       {u"x"_s, it->x()},                     {u"y"_s, it->y()},
                       {u"w"_s, it->width()},                 {u"h"_s, it->height()}};
}

QVariantMap SurfaceController::spanFor(const QString &name) const
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end() || !it->second.content.span)
        return {};
    // A host does not see the other screens: the policy server computed the rectangle for it.
    if (isHost())
        return m_hostPolicy ? m_hostPolicy->span : QVariantMap();
    return spanViewport(name);
}

void SurfaceController::updateSpans()
{
    for (auto &[name, output] : m_outputs) {
        if (!output.content.span)
            continue;
        const QVariantMap span = spanFor(name);
        if (span == output.lastSpan)
            continue;
        output.lastSpan = span;
        if (output.sink)
            output.sink->setContent(contentMap(name, output.content, {}));
    }
    m_service->notifyHostStates(); // a host's rectangle is part of its state
}

void SurfaceController::syncSpanVideos()
{
    QList<SpanPlayer> players;
    QList<OutputSink *> sinks;
    for (const auto &[name, output] : m_outputs) { // the map is ordered by name: the same output leads every round
        if (!output.sink || !output.content.span || output.content.kind != u"video" || output.paused)
            continue;
        players.append({name, output.sink->videoPosition().value_or(-1)});
        sinks.append(output.sink.get());
    }
    if (players.size() < 2)
        return;
    for (const SpanCorrection &fix : spanVideoCorrections(players)) {
        for (qsizetype i = 0; i < players.size(); ++i) {
            if (players.at(i).output == fix.output)
                sinks.at(i)->seekVideo(fix.seconds);
        }
    }
}

// ---- health ----------------------------------------------------------------------------------------------------

QJsonObject SurfaceController::failureJson(const QString &name, const Output &output) const
{
    if (m_service->isHosted(name)) // the host process draws it and says so
        return m_service->hostFailure(name);
    const auto &failure = output.health.failure();
    if (!failure)
        return {};
    return QJsonObject{{u"kind"_s, failureKindName(failure->kind)},
                       {u"reason"_s, failure->reason},
                       {u"engine"_s, failure->engine},
                       {u"canTryOther"_s, failure->canTryOther}};
}


void SurfaceController::onHealthEvent(const QString &name, const SinkHealthEvent &event)
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end())
        return;
    Output &output = it->second;
    bool changed = false;
    if (event.event == u"frame") {
        if (event.key == output.health.key() && output.contentClock.isValid() && qEnvironmentVariableIsSet("MURASAKI_MEDIA_LOG"))
            qInfo("murasaki.media: first frame of %s (%s) on %s after %lld ms", qPrintable(output.content.itemId),
                  qPrintable(output.content.kind), qPrintable(name), static_cast<long long>(output.contentClock.elapsed()));
        changed = output.health.frame(event.key);
    } else if (event.event == u"failed") {
        bool canTryOther = false;
        const OutputFailure failure{classifyFailure(event.reason), event.reason, event.engine, canTryOther};
        changed = output.health.report(event.key, failure);
        if (changed) // once per failure: the same report again is not logged
            qWarning("murasaki-surface: %s is not shown on %s (%s): %s", qPrintable(output.content.itemId),
                     qPrintable(name), qPrintable(failureKindName(failure.kind)), qPrintable(failure.reason));
    }
    if (changed)
        publishHealth(name);
}

void SurfaceController::publishHealth(const QString &name)
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end())
        return;
    if (isHost()) {
        const auto &failure = it->second.health.failure();
        const QString json = failure ? QString::fromUtf8(QJsonDocument(QJsonObject{{u"kind"_s, failureKindName(failure->kind)},
                                                                                  {u"reason"_s, failure->reason},
                                                                                  {u"engine"_s, failure->engine},
                                                                                  {u"canTryOther"_s, failure->canTryOther}})
                                                              .toJson(QJsonDocument::Compact))
                                     : QString();
        QDBusMessage call = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                           QString(kSurfaceDBusInterface), u"ReportOutputHealth"_s);
        call.setArguments({name, json});
        QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
        return;
    }
    m_service->notifyOutputsChanged();
}

void SurfaceController::retryOutput(const QString &name)
{
    if (m_service->isHosted(name)) {
        m_service->sendHostCommand(name, u"retry"_s);
        return;
    }
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end() || !it->second.sink || it->second.content.kind.isEmpty() || it->second.content.kind == u"none")
        return;
    Output &output = it->second;
    output.overrideItem = output.content.itemId;
    ++output.retries; // another key: the delegate is made again, and a fresh engine process with it
    output.sink->setContent(contentMap(name, output.content, {}));
    applyPlayback();
}


void SurfaceController::onHostCommand(const QString &output, const QString &command)
{
    if (!isHost() || output != m_hostOutput)
        return;
    if (command == u"retry")
        retryOutput(output);
}

QJsonArray SurfaceController::outputsJson() const
{
    const SessionState state = m_monitor ? m_monitor->state() : SessionState();
    QJsonArray array;
    for (const auto &[name, output] : m_outputs) {
        const bool showing = output.content.kind != u"none" && !output.content.kind.isEmpty();
        const QJsonObject failure = showing ? failureJson(name, output) : QJsonObject();
        array.append(QJsonObject{
            {u"name"_s, name},
            {u"desktop"_s, output.shownDesktop},
            {u"occluded"_s, output.pending ? output.occluded : state.forOutput(name).occluded},
            {u"itemId"_s, output.content.itemId},
            {u"playing"_s, showing && !output.paused},
            {u"paused"_s, output.paused},
            {u"audible"_s, output.decision.audible},
            {u"hosted"_s, m_service->isHosted(name)},
            // The content takes the pointer (a web page or a scene): the overlay's interaction mode has something to pass clicks to.
            {u"interactive"_s, showing && (output.content.kind == u"web" || output.content.kind == u"scene")},
            // Why a rule paused it ("game", "battery", "app"; the app rule that matched in pauseDetail), and the cap it
            // plays under on battery; both empty or 0 normally.
            {u"pauseReason"_s, pausedByRule(output) ? pauseReasonToString(m_pauseVerdict.reason) : QString()},
            {u"pauseDetail"_s, pausedByRule(output) ? m_pauseVerdict.detail : QString()},
            {u"fpsCap"_s, output.fpsCap},
            // Why nothing is shown although something was asked for: {kind, reason, engine, canTryOther}, or null.
            {u"failure"_s, failure.isEmpty() ? QJsonValue() : QJsonValue(failure)},
        });
    }
    return array;
}

QJsonObject SurfaceController::hostStateJson(const QString &name) const
{
    const auto it = m_outputs.find(name);
    if (it == m_outputs.end()) // not a monitor of this process: nothing to coordinate with, so play
        return QJsonObject{{u"desktop"_s, QString(kWildcard)}, {u"paused"_s, false}, {u"audible"_s, true},
                           {u"frameRateLimit"_s, m_webFpsLimit}};
    QJsonObject state{{u"desktop"_s, desktopFor(name)},
                      {u"paused"_s, it->second.paused},
                      {u"audible"_s, it->second.decision.audible},
                      {u"frameRateLimit"_s, m_webFpsLimit},
                      {u"fpsCap"_s, m_pauseVerdict.fpsCap}};
    if (it->second.content.span) // the host cannot see the other screens
        state.insert(u"span"_s, QJsonObject::fromVariantMap(spanViewport(name)));
    return state;
}

// ---- host side ----------------------------------------------------------------------------------------------

void SurfaceController::registerWithPolicyServer()
{
    m_registerRetry.stop();
    const quint64 request = ++m_registerRequest;
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                       QString(kSurfaceDBusInterface), u"RegisterHost"_s);
    call.setArguments({m_hostOutput});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply = *w;
        if (request != m_registerRequest)
            return; // the policy server went away or a newer registration superseded this one
        if (reply.isError()) {
            // The server is there but said no (an output already hosted, a limit, bad arguments): say why, play
            // unconditionally meanwhile and ask again with growing pauses, as the other host may be on its way out.
            qWarning("murasaki-surface: the policy server refused RegisterHost for %s: %s (retrying in %d s)",
                     qPrintable(m_hostOutput), qPrintable(reply.error().message()), m_registerBackoff / 1000);
            fallBackToPlaying();
            m_registerRetry.start(m_registerBackoff);
            m_registerBackoff = std::min(m_registerBackoff * 2, kMaxRegisterBackoffMs);
            return;
        }
        m_registerBackoff = kFirstRegisterBackoffMs;
        m_hostRegistered = true;
        m_loggedNoPolicy = false;
        qInfo("murasaki-surface: registered %s with the policy server", qPrintable(m_hostOutput));
        sendHostVisible();
        fetchHostState();
    });
}

// No policy server (yet): the output plays, on the wildcard desktop, as in the first host release.
void SurfaceController::onPolicyServerGone()
{
    m_registerRetry.stop();
    m_registerBackoff = kFirstRegisterBackoffMs;
    if (!std::exchange(m_loggedNoPolicy, true))
        qInfo("murasaki-surface: no policy server on the bus; %s plays unconditionally until one appears",
              qPrintable(m_hostOutput));
    fallBackToPlaying();
}

void SurfaceController::fallBackToPlaying()
{
    ++m_registerRequest; // calls in flight are void
    ++m_hostRequest;
    m_hostRegistered = false;
    if (!m_hostPolicy)
        return;
    m_hostPolicy.reset();
    for (const auto &[name, output] : m_outputs) {
        if (output.sink)
            output.sink->setFrameRateLimit(frameRateLimit());
    }
    if (!m_outputs.empty() && m_outputs.begin()->second.desktop != desktopFor(m_hostOutput))
        refresh(m_hostOutput);
    applyPlayback();
}

void SurfaceController::onHostStateChanged(const QString &output)
{
    if (isHost() && output == m_hostOutput && m_hostRegistered)
        fetchHostState();
}

void SurfaceController::fetchHostState()
{
    const quint64 request = ++m_hostRequest;
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                       QString(kSurfaceDBusInterface), u"HostState"_s);
    call.setArguments({m_hostOutput});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (request != m_hostRequest || reply.isError())
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(reply.value().toUtf8());
        if (doc.isObject())
            applyHostState(doc.object());
    });
}

void SurfaceController::applyHostState(const QJsonObject &state)
{
    HostPolicy policy;
    policy.desktop = state.value(u"desktop"_s).toString(QString(kWildcard));
    if (policy.desktop.isEmpty())
        policy.desktop = QString(kWildcard);
    policy.paused = state.value(u"paused"_s).toBool(false);
    policy.audible = state.value(u"audible"_s).toBool(true);
    policy.frameRateLimit = std::clamp(state.value(u"frameRateLimit"_s).toInt(m_webFpsLimit), kMinWebFpsLimit, kMaxWebFpsLimit);
    policy.fpsCap = std::clamp(state.value(u"fpsCap"_s).toInt(0), 0, kMaxWebFpsLimit);
    policy.span = state.value(u"span"_s).toObject().toVariantMap();
    const bool spanChanged = !m_hostPolicy || m_hostPolicy->span != policy.span;
    m_hostPolicy = policy;
    const auto it = m_outputs.find(m_hostOutput);
    if (it == m_outputs.end())
        return;
    if (spanChanged && it->second.content.span)
        updateSpans();
    if (it->second.sink)
        it->second.sink->setFrameRateLimit(frameRateLimit());
    if (it->second.desktop != policy.desktop)
        refresh(m_hostOutput); // the content of the new desktop; playback follows its reply
    applyPlayback();
}

// The plugin's containment visibility (the bridge Pause) is one more pause input of the policy server.
void SurfaceController::sendHostVisible()
{
    if (!isHost() || !m_hostRegistered)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                       QString(kSurfaceDBusInterface), u"SetHostVisible"_s);
    call.setArguments({m_hostOutput, !m_externalPause});
    QDBusConnection::sessionBus().asyncCall(call);
}

QJsonArray SurfaceController::desktopsJson() const
{
    QJsonArray array;
    if (m_monitor) {
        for (const DesktopInfo &desktop : m_monitor->state().desktops)
            array.append(desktop.toJson());
    }
    return array;
}

} // namespace murasaki
