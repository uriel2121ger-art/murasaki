#include "buildinfo.h"
#include "buspropertywatcher.h"
#include "constants.h"
#include "crashledger.h"
#include "filelog.h"
#include "compositorprofile.h"
#include "globalshortcut.h"
#include "hyprlandpointersource.h"
#include "interactcontroller.h"
#include "interactwindow.h"
#include "kwinpointersource.h"
#include "pointercapability.h"
#include "pollingpointersource.h"
#include "poolmaskloader.h"
#include "spritemask.h"
#include "overlaycontroller.h"
#include "overlayservice.h"
#include "triggershortcuts.h"
#include "petaudiocontroller.h"
#include "surfacemutewatcher.h"
#include "petcheck.h"
#include "petwindow.h"
#include "sessionwatch.h"
#ifdef MURASAKI_HAVE_X11
#include "x11util.h"
#endif
#include "shellwindows.h"
#include "puppetitem.h"
#include "spriteitem.h"

#include <QCommandLineParser>
#include <QCursor>
#include <QDir>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QtQml/qqml.h>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <QStandardPaths>
#include <QtQml/qqmlextensionplugin.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <clocale>
#include <csignal>
#include <functional>
#include <memory>
#include <optional>

#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

Q_IMPORT_QML_PLUGIN(Murasaki_RenderPlugin)

namespace {

int g_signalPipe[2] = {-1, -1};

// Async-signal-safe: only write(2) to the self-pipe; the Qt side does the real work.
void onTerminateSignal(int)
{
    const char byte = 1;
    const int saved = errno;
    [[maybe_unused]] const ssize_t written = ::write(g_signalPipe[1], &byte, 1);
    errno = saved;
}

bool installQuitSignals()
{
    if (::pipe2(g_signalPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;
    struct sigaction action {};
    action.sa_handler = onTerminateSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return ::sigaction(SIGTERM, &action, nullptr) == 0 && ::sigaction(SIGINT, &action, nullptr) == 0;
}

// The memory the pets may use for decoded frames: MURASAKI_FRAME_CACHE_MIB, 128 MiB by default, 0 turns the cache off.
qint64 frameCacheBudgetBytes()
{
    bool ok = false;
    const int mib = qEnvironmentVariableIntValue("MURASAKI_FRAME_CACHE_MIB", &ok);
    return qint64(ok && mib >= 0 ? std::min(mib, kMaxFrameCacheMiB) : kDefaultFrameCacheMiB) << 20;
}

// The memory the hit masks of all the pets may take: MURASAKI_MASK_CACHE_MIB, 8 MiB by default.
qint64 maskBudgetBytes()
{
    bool ok = false;
    const int mib = qEnvironmentVariableIntValue("MURASAKI_MASK_CACHE_MIB", &ok);
    return ok && mib >= 0 ? qint64(std::min(mib, 1024)) << 20 : qint64(kMaskBudgetBytes);
}

} // namespace

int main(int argc, char *argv[])
{
    // `--check ITEM` needs no session, window or GPU: a plain core application, before the GUI one starts. Only the
    // exact form runs it; any other use of the option (`--check=ITEM`, extra arguments, no item) is an error and
    // never starts the overlay.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check") != 0 && std::strncmp(argv[i], "--check=", 8) != 0)
            continue;
        if (argc == 3 && i == 1 && std::strcmp(argv[1], "--check") == 0 && argv[2][0] != '-') {
            QCoreApplication core(argc, argv);
            return runPetCheck(QString::fromLocal8Bit(argv[2]));
        }
        std::fputs("murasaki-overlay: usage: murasaki-overlay --check ITEM (nothing else on the command line)\n", stderr);
        return 2;
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL); // VideoItem renders through libmpv's OpenGL API
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C"); // libmpv refuses to start with any other numeric locale
    QGuiApplication::setApplicationName(u"murasaki-overlay"_s);
    QGuiApplication::setApplicationVersion(versionString());
    QGuiApplication::setDesktopFileName(QString::fromLatin1(kShellOverlayAppId)); // (the GNOME extension trusts pet windows by this id)

    if (installQuitSignals()) {
        auto *notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char buffer[16];
            while (::read(g_signalPipe[0], buffer, sizeof buffer) > 0) {
            }
            QCoreApplication::quit();
        });
    } else {
        qWarning("murasaki-overlay: cannot install signal handlers");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Murasaki overlay: draws the desktop pets above all windows."_s);
    parser.addHelpOption();
    parser.addVersionOption();
    // Handled before the application starts; listed here for --help.
    parser.addOption(QCommandLineOption(u"check"_s, u"Print whether the overlay can play ITEM (an Anima Engine item folder, an animation file or anima:<id>) as one JSON line, and exit."_s, u"item"_s));
    parser.process(app);
    murasaki::installFileLogging(u"overlay"_s);

#ifdef MURASAKI_HAVE_X11
    const bool x11Session = QGuiApplication::platformName().startsWith(u"xcb"_s);
#else
    const bool x11Session = false;
#endif
    if (!x11Session && !QGuiApplication::platformName().startsWith(u"wayland"_s)) {
        qCritical("murasaki-overlay needs a Wayland session with layer-shell or an X11 session (found \"%s\")",
                  qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("murasaki-overlay: cannot connect to the D-Bus session bus");
        return 1;
    }

    // libmpv decodes in this process, so a poison file takes every pet down; the ledger remembers what was being
    // opened when the process died and keeps such a file from being opened again after three crashes.
    const QString stateHome = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    // A relative XDG_STATE_HOME is invalid and must be ignored (XDG base directory spec).
    CrashLedger crashLedger((QDir::isAbsolutePath(stateHome) ? stateHome : QDir::homePath() + u"/.local/state"_s)
                            + u"/murasaki/overlay"_s);
    crashLedger.load();

    qmlRegisterType<SpriteItem>("Murasaki.Sprite", 1, 0, "SpriteItem");
    qmlRegisterType<PuppetItem>("Murasaki.Puppet", 1, 0, "PuppetItem");
    QQmlEngine engine; // outlives the controller, which owns the windows
    SessionBusBackend daemon(kDBusService, kDBusPath, kDBusInterface,
                             {u"PetsChanged"_s, u"TranscodeChanged"_s, u"PetsArrangeChanged"_s, u"ItemsChanged"_s, u"SettingsChanged"_s, u"PetsHiddenChanged"_s, u"WallpaperInteractChanged"_s, u"PetRuntimeChanged"_s, u"PetsMutedChanged"_s, u"TriggersChanged"_s});
    // (Declared before the controller, which uses them until it is destroyed.)
    PoolMaskLoader maskLoader(defaultMaskCacheRoot());
    std::unique_ptr<PointerSource> pointerSource;
    OverlayController controller(
        &daemon, [&engine](const QString &output, const QString &petId) -> std::unique_ptr<PetSurface> {
            for (QScreen *screen : QGuiApplication::screens()) {
                if (screen->name() == output)
                    return std::make_unique<PetWindow>(screen, &engine, petId);
            }
            return nullptr;
        });

    controller.setCrashLedger(&crashLedger);

#ifdef MURASAKI_HAVE_X11
    // On X11 a stage is a screen-sized translucent window, which without a compositing manager is an opaque rectangle over the desktop: it is drawn only while one runs.
    // The owner of the selection _NET_WM_CM_S<screen> is looked at now and every two seconds (a compositor can be started, stopped or suspended at any time).
    QTimer x11Compositing;
    if (x11Session) {
        const auto look = [&controller] { controller.setStageCompositing(murasaki::x11::compositingManagerRunning()); };
        look();
        x11Compositing.setInterval(2000);
        QObject::connect(&x11Compositing, &QTimer::timeout, &controller, look);
        x11Compositing.start();
    }
#endif

    // The sounds of the pets: the player process is started on the first sound and leaves when idle (docs/pets/audio.md). Silent unless a pet asks.
    ProcessPetAudioLink audioLink(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/murasaki/pets/audio"_s);
    PetAudioController petAudio(&audioLink);
    controller.setAudio(&petAudio);
    // The policy server says when a fullscreen window, a game or a listed application should silence them.
    SessionBusBackend surfaceBus(kSurfaceDBusService, kSurfaceDBusPath, kSurfaceDBusInterface, {u"PetAudioMutedChanged"_s, u"SessionFactsChanged"_s});
    SurfaceMuteWatcher surfaceMute(&surfaceBus, &petAudio);
    QObject::connect(&surfaceMute, &SurfaceMuteWatcher::fullscreenChanged, &controller, &OverlayController::setOutputFullscreen);
    QObject::connect(&surfaceMute, &SurfaceMuteWatcher::gameScreensChanged, &controller, &OverlayController::setGameScreens);
    surfaceMute.start();
    controller.setFrameCacheBudget(frameCacheBudgetBytes());

    // The hit masks of pets that play through the video path come from a background pass over their file, cached on disk.
    controller.setMaskLoader(&maskLoader);
    controller.setMaskBudget(maskBudgetBytes());

    // The interaction with the wallpaper: catcher windows above the desktop while the daemon's WallpaperInteract is on (docs/dbus-api.md).
    SessionBusBackend surface(kSurfaceDBusService, kSurfaceDBusPath, kSurfaceDBusInterface, {u"OutputsChanged"_s});
    InteractController interact(&daemon, &surface, [x11Session](const QString &output) -> std::unique_ptr<InteractSurface> {
        // Wayland with layer-shell only: an X11 window cannot take the keyboard (no Esc), and the Plasma host this serves is a Wayland feature.
        if (x11Session || useShellWindows()) {
            qWarning("murasaki-overlay: interaction with the wallpaper needs a Wayland session with layer-shell; not available in this session");
            return nullptr;
        }
        for (QScreen *screen : QGuiApplication::screens()) {
            if (screen->name() == output)
                return std::make_unique<InteractWindow>(screen);
        }
        return nullptr;
    });

    // Where the global cursor comes from, if the compositor gives it to anyone (docs/pets/behaviours.md, "Pointer sources").
    {
        const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const bool kwinOnBus = bus.interface() && bus.interface()->isServiceRegistered(u"org.kde.KWin"_s);
        const bool gnomeOnBus = bus.interface() && bus.interface()->isServiceRegistered(u"org.gnome.Shell"_s);
        const CompositorProfile profile = selectCompositorProfile(environment, kwinOnBus, {}, gnomeOnBus, x11Session);
        PointerEnvironment pointerEnvironment;
        pointerEnvironment.kwinOnBus = kwinOnBus;
        pointerEnvironment.x11Session = x11Session;
        pointerEnvironment.hyprlandSocketDir = HyprlandPointerSource::socketDir(environment);
        pointerEnvironment.override = environment.value(u"MURASAKI_POINTER"_s);
        const PointerCapability capability = choosePointerBackend(profile, pointerEnvironment);
        switch (capability.backend) {
        // (made on the first pet that wants the cursor: until then nothing is exported on the bus, nothing is asked of the compositor)
        case PointerBackend::KWinScript:
            pointerSource = std::make_unique<LazyPointerSource>(PointerSource::Level::Global, [] { return std::make_unique<KWinPointerSource>(); });
            break;
        case PointerBackend::HyprlandIpc:
            pointerSource = std::make_unique<LazyPointerSource>(PointerSource::Level::Global, [dir = pointerEnvironment.hyprlandSocketDir] {
                return std::make_unique<HyprlandPointerSource>(dir);
            });
            break;
        case PointerBackend::X11Poll:
            pointerSource = std::make_unique<LazyPointerSource>(PointerSource::Level::Global, [] {
                return std::make_unique<PollingPointerSource>([] { return std::optional<QPointF>(QCursor::pos()); });
            });
            break;
        case PointerBackend::Local:
            pointerSource = std::make_unique<LocalPointerSource>();
            break;
        }
        qInfo("murasaki-overlay: pointer: %s (%s)", capability.level == PointerSource::Level::Global ? "global" : "local", qPrintable(capability.reason));
        controller.setPointerSource(pointerSource.get());
    }

    // Battery and the power-saver profile hold the pets to a low rate (the daemon's setting petSaveEnergy turns that
    // off). Both services are optional: without them the machine counts as plugged in.
    PowerWatcher power(QDBusConnection::systemBus());
    PowerProfileWatcher powerProfile(QDBusConnection::systemBus());
    const auto pushPower = [&] { controller.setPowerState(power.onBattery(), powerProfile.powerSaver()); };
    QObject::connect(&power, &PowerWatcher::changed, &controller, pushPower);
    QObject::connect(&powerProfile, &PowerProfileWatcher::changed, &controller, pushPower);
    power.start();
    powerProfile.start();

    SessionWatch session;
    QObject::connect(&session, &SessionWatch::pausedChanged, &controller, &OverlayController::setSessionPaused);
    QObject::connect(&session, &SessionWatch::pausedChanged, &interact, &InteractController::setSessionPaused);
    session.watchScreenSaver(bus);
    controller.setSessionPaused(session.paused());
    interact.setSessionPaused(session.paused());

    const auto updateOutputs = [&controller, &session] {
        QStringList names;
        QHash<QString, double> scales;
        QHash<QString, QRect> geometry;
        for (const QScreen *screen : QGuiApplication::screens()) {
            names << screen->name();
            scales.insert(screen->name(), screen->devicePixelRatio());
            geometry.insert(screen->name(), screen->geometry());
        }
        const QScreen *primary = QGuiApplication::primaryScreen();
        controller.setOutputs(names, primary ? primary->name() : QString(), scales, geometry);
        session.setScreenOff(names.isEmpty());
    };
    // Qt has no signal for a changed device pixel ratio; a new scale shows up as a new logical geometry or DPI.
    const auto watchScreen = [&controller, updateOutputs](const QScreen *screen) {
        QObject::connect(screen, &QScreen::geometryChanged, &controller, updateOutputs);
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, &controller, updateOutputs);
    };
    QObject::connect(&app, &QGuiApplication::screenAdded, &controller, [watchScreen, updateOutputs](QScreen *screen) {
        watchScreen(screen);
        updateOutputs();
    });
    QObject::connect(&app, &QGuiApplication::screenRemoved, &controller, updateOutputs);
    QObject::connect(&app, &QGuiApplication::primaryScreenChanged, &controller, updateOutputs);
    for (const QScreen *screen : QGuiApplication::screens())
        watchScreen(screen);
    updateOutputs();

    // The global shortcut is optional: the GUI, `murasakictl pets arrange` and ToggleArrange() always work.
    std::unique_ptr<GlobalShortcut> shortcut;
    QString shortcutState = u"off"_s;
    std::function<void()> shortcutsChanged = [] {}; // set once the D-Bus service exists
    if (qEnvironmentVariableIntValue("MURASAKI_NO_SHORTCUT") == 0) {
        shortcutState = u"starting"_s;
        shortcut = makeShortcutChain(bus, u"io.github.uriel2121ger_art.Murasaki.Overlay"_s);
        QObject::connect(shortcut.get(), &GlobalShortcut::activated, &controller, &OverlayController::onShortcut);
        QObject::connect(shortcut.get(), &GlobalShortcut::activated, &interact, &InteractController::onShortcut);
        QObject::connect(shortcut.get(), &GlobalShortcut::registered, &app, [&shortcut, &shortcutState, &shortcutsChanged] {
            shortcutState = u"bound"_s;
            shortcutsChanged();
            // One line per action; an action whose key was taken (or cleared by the user) is said to be unbound, with the way to give it one.
            for (const QString &id : shortcut->actionIds()) {
                const QString trigger = shortcut->trigger(id);
                if (trigger.isEmpty()) {
                    QString usual;
                    for (const ShortcutAction &action : defaultShortcutActions()) {
                        if (action.id == id)
                            usual = action.text;
                    }
                    qWarning("murasaki-overlay: no key: %s is taken (by another application, or cleared) so %s has none; set one in %s, "
                             "or use murasakictl pets ...",
                             qPrintable(usual), qPrintable(id),
                             shortcut->backend() == u"portal"_s ? "the desktop's keyboard shortcuts settings" : "System Settings, Shortcuts, Murasaki");
                }
                else
                    qInfo("murasaki-overlay: global shortcut %s bound through %s as %s", qPrintable(id), qPrintable(shortcut->backend()),
                          qPrintable(trigger));
            }
        });
        QObject::connect(shortcut.get(), &GlobalShortcut::failed, &app, [&shortcutState, &shortcutsChanged](const QString &reason) {
            shortcutState = u"failed"_s;
            shortcutsChanged();
            qInfo("murasaki-overlay: no global shortcut (%s); use the GUI or murasakictl to arrange, hide and rescue the pets",
                  qPrintable(reason));
        });
    }

    OverlayService service(&controller);
    service.shortcutBackendProvider = [&shortcut] { return shortcut ? shortcut->backend() : QString(); };
    service.shortcutStateProvider = [&shortcutState] { return shortcutState; };
    shortcutsChanged = [&service] { emit service.ShortcutsChanged(); };
    // The shortcuts of the triggers: registered when the daemon lists enabled ones, and only then.
    TriggerShortcuts triggerShortcuts(&daemon, [&bus](const QList<ShortcutAction> &actions) {
        return qEnvironmentVariableIntValue("MURASAKI_NO_SHORTCUT") == 0 ? makeTriggerShortcutChain(bus, u"io.github.uriel2121ger_art.Murasaki.Overlay"_s, actions)
                                                                         : std::unique_ptr<GlobalShortcut>();
    }, nullptr, [&bus](const QStringList &gone) { forgetKGlobalAccelActions(bus, gone); });
    QObject::connect(&triggerShortcuts, &TriggerShortcuts::changed, &app, [&shortcutsChanged] { shortcutsChanged(); });
    service.shortcutTriggersProvider = [&shortcut, &triggerShortcuts] {
        QJsonObject triggers;
        if (shortcut) {
            for (const QString &id : shortcut->actionIds())
                triggers.insert(id, shortcut->trigger(id));
        }
        const QJsonObject own = triggerShortcuts.keys();
        for (auto it = own.begin(); it != own.end(); ++it)
            triggers.insert(TriggerShortcuts::actionId(it.key()), it.value());
        return QString::fromUtf8(QJsonDocument(triggers).toJson(QJsonDocument::Compact));
    };
    service.quitHandler = [] { QCoreApplication::quit(); };
    if (!bus.registerObject(QString(kOverlayDBusPath), &service,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals
                                | QDBusConnection::ExportScriptableProperties)
        || !bus.registerService(QString(kOverlayDBusService))) {
        qCritical("murasaki-overlay: %s is already taken; is another murasaki-overlay running?",
                  qPrintable(QString(kOverlayDBusService)));
        return 1;
    }

    controller.start();
    surface.start();
    if (shortcut)
        shortcut->start();
    const int code = app.exec();
    bus.unregisterService(QString(kOverlayDBusService));
    bus.unregisterObject(QString(kOverlayDBusPath));
    return code;
}
