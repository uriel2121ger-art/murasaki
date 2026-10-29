#pragma once

#include "audiocapture.h"
#include "buspropertywatcher.h"
#include "constants.h"
#include "idlesource.h"
#include "cursorrouter.h"
#include "compositorprofile.h"
#include "outputhealth.h"
#include "outputsink.h"
#include "pausepolicy.h"
#include "playbackpolicy.h"
#include "sessionmonitor.h"
#include "wallpapercontent.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QQmlEngine>
#include <QTimer>

#include <QPointer>

#include <map>
#include <memory>
#include <optional>

class QDBusServiceWatcher;
class QScreen;

namespace murasaki {

class WebProfileFactory;
class ItemSchemeHandler;
class SurfaceService;

// The shortest time between two cursor messages to the hosts (a little under 30 per second).
inline constexpr int kCursorMinIntervalMs = 33;
// How often the outputs of a spanned video are compared and brought back together.
inline constexpr int kSpanSyncIntervalMs = 4000;

struct SurfaceOptions {
    // How long a wallpaper may draw nothing before it is reported as failed (tests shorten it).
    int noFrameTimeoutMs = kEngineNoFrameTimeoutMs;
    bool windowed = false; // layer-shell mode only: normal windows instead of the desktop layer
    // Host mode (the Plasma presenter): manage exactly this output, whatever screens the compositor shows. It
    // does not own the Surface1 D-Bus name and follows no compositor monitor. It registers with the policy server
    // (the murasaki-surface that owns the name) and plays what that says: desktop, paused, audible, frame rate.
    // Without a policy server the session reads "*", not covered, and the output plays (unless setExternalPause()
    // says otherwise); the host registers again whenever a policy server appears.
    QString hostOutput;
    SinkFactory sinkFactory; // empty: layer-shell windows (OutputSurface)
    // Output name of a screen; empty: QScreen::name(). Tests use it on platforms whose screens have no name.
    std::function<QString(QScreen *)> screenName;
    // The bus UPower is on (the system bus when unset) and the compositor monitor (the one that fits the session when
    // unset). Tests give a private bus and a monitor they drive.
    std::optional<QDBusConnection> powerBus;
    std::function<std::unique_ptr<SessionMonitor>()> monitorFactory;
    // The source of the idle fact (ext-idle-notify when the compositor offers it); tests give one they drive, and an empty function in a test platform means none.
    std::function<std::unique_ptr<IdleSource>()> idleFactory;
};

// Owns one OutputSink per output (a screen, or the single host output), asks murasakid what each output shows on
// its current desktop, and applies PlaybackPolicy whenever assignments, desktops or occlusion change.
class SurfaceController : public QObject
{
    Q_OBJECT

public:
    explicit SurfaceController(SurfaceOptions options, QObject *parent = nullptr);
    ~SurfaceController() override;

    // False when D-Bus is unavailable, another murasaki-surface is running or the host sink cannot be created.
    bool start();

    // An extra pause input from outside the policy (the Plasma plugin hides its wallpaper): every output is
    // paused and silent while it is set. A host also tells the policy server (SetHostVisible).
    void setExternalPause(bool paused);
    bool externalPause() const { return m_externalPause; }
    bool isHost() const { return !m_hostOutput.isEmpty(); }

signals:
    // Host mode only: the daemon's ApiVersion is below kMinDBusApiVersion, so assignments and the host protocol would
    // be misread. The caller is expected to exit non-zero; the reason has been logged.
    void incompatibleDaemon(quint32 version);

private slots:
    void refreshAll();
    void loadSettings();
    void loadAssetsDir();
    void onDaemonRegistered();
    void checkDaemonVersion();
    void onHostsChanged(const QString &output);
    void onHostStateChanged(const QString &output);
    void onCursorReported(const QPointF &global);
    void onHostCursor(const QString &output, double x, double y, double width, double height);
    void onHostCursorLeft(const QString &output);
    void onHostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY);
    void onHostCommand(const QString &output, const QString &command);
    void registerWithPolicyServer();
    void onPolicyServerGone();
    void fallBackToPlaying();

private:
    // What the policy server said about this host's output.
    struct HostPolicy {
        QString desktop;
        bool paused = false;
        bool audible = true;
        int frameRateLimit = kDefaultWebFpsLimit;
        int fpsCap = 0;   // the policy's frame rate cap (battery), 0 for none
        QVariantMap span; // this output's rectangle of the spanned canvas, when its content is spanned
    };

    struct Output {
        std::unique_ptr<OutputSink> sink; // null while the output is hosted (or the presenter is plasma)
        QPointer<QScreen> screen;
        QString desktop;      // desktop of the latest request
        QString shownDesktop; // desktop of the content actually on screen
        WallpaperContent content;
        quint64 request = 0;
        bool paused = false;
        bool pending = false; // a ResolveAssignment call is in flight
        bool occluded = false; // as of the last applyPlayback()
        QString reason;       // the preview reason last logged
        int fpsCap = 0;       // the frame rate cap the sink was last given (battery), 0 for none
        QVariantMap lastSpan; // the viewport the sink was last given (spanned content only)
        PlaybackDecision decision;
        QElapsedTimer contentClock; // since the content on screen was set (the first-frame time of MURASAKI_MEDIA_LOG)
        OutputHealth health;    // why nothing is shown, if so (see outputhealth.h)
        QString engineOverride; // an engine setting value the user chose for `overrideItem` alone, until it changes
        QString overrideItem;
        int retries = 0;        // "retry" counts, part of the content key so that the delegate is made again
    };

    QString nameOf(QScreen *screen) const;
    bool addOutput(const QString &name, QScreen *screen);
    void addScreen(QScreen *screen);
    void removeScreen(QScreen *screen);
    void refresh(const QString &name, bool isRetry = false);
    void resolveOutputs();
    void onSessionChanged();
    void applyPlayback();
    bool pausedByRule(const Output &output) const; // the pause verdict holds for this output (a game: only where a window covers it)
    void updateAudioCapture();
    void pushSpectrum(const QList<float> &spectrum);
    bool wantsSpectrum(const Output &output) const;
    void updateReadableRoots();
    void finishAssetsRequest(quint64 request, const QString &dir, bool ok);
    // Whether the scene engines run in the sandbox; scenes on screen start again when it changes.
    void applySandboxSetting(bool sandbox, bool require);
    void applyRenderScale(double scale);
    void applySettings(int webFpsLimit, bool pauseWhenCovered, const QString &presenter, const QString &sceneEngine);
    // Creates or destroys the sink so that it exists exactly when this process should draw the output.
    void syncSink(const QString &name);
    bool createSink(const QString &name);
    bool wantsWindow(const QString &name) const;
    QString desktopFor(const QString &name) const;
    int frameRateLimit() const;
    void fetchHostState();
    void applyHostState(const QJsonObject &state);
    void sendHostVisible();
    void updateCursorReporting();
    void dispatchCursor(const QList<CursorMessage> &messages);
    void armCursorFlush();
    QList<CursorScreen> cursorScreens() const;
    QJsonObject hostStateJson(const QString &name) const;
    QVariantMap contentMap(const QString &name, const WallpaperContent &content, const QJsonObject &changedProperties);
    // Spanning: {canvasW, canvasH, x, y, w, h} of this output's rectangle of the canvas of all outputs (logical
    // pixels), or empty when its content is not spanned. A host takes it from the policy server's HostState.
    QVariantMap spanFor(const QString &name) const;
    QVariantMap spanViewport(const QString &name) const;
    // The screens moved, came or went: spanned outputs get their new rectangle (the content key does not change).
    void updateSpans();
    // Outputs that play one spanned video are kept in step (see spansync.h).
    void syncSpanVideos();
    // What the session says right now, for the pause rules.
    PauseFacts pauseFacts() const;
    QJsonArray outputsJson() const;
    // The sink of `name` reported something about the content it shows.
    void onHealthEvent(const QString &name, const SinkHealthEvent &event);
    // The failure state of `name` changed: publish it (Outputs(), or the policy server for a host).
    void publishHealth(const QString &name);
    QJsonObject failureJson(const QString &name, const Output &output) const;
    // The user's choices from the GUI. A hosted output's host does them (HostCommand).
    void retryOutput(const QString &name);
    QJsonArray desktopsJson() const;
    std::unique_ptr<SessionMonitor> createMonitor();
    void publishSessionFacts(const SessionState &state, bool quiet);
    std::unique_ptr<IdleSource> createIdleSource();

    bool m_windowed;
    QString m_hostOutput;
    SinkFactory m_sinkFactory;
    std::function<QString(QScreen *)> m_screenName;
    bool m_externalPause = false;
    QQmlEngine m_engine;
    WebProfileFactory *m_webProfiles;
    QTimer m_heapTrim; // what a switch frees goes back to the system a moment after the last one (no periodic wake-up)
    SurfaceService *m_service;
    std::unique_ptr<SessionMonitor> m_monitor;
    std::unique_ptr<IdleSource> m_idle; // the compositor's word on whether the user is away (none: unknown)
    bool m_factsQuiet = false;          // `quiet` of the last published facts, for a republication that only the idle changed
    std::function<std::unique_ptr<IdleSource>()> m_idleFactory;
    CompositorProfile m_compositor; // what createMonitor() found out about the compositor
    QDBusServiceWatcher *m_daemonWatcher = nullptr;
    std::map<QString, Output> m_outputs;
    AudioCapture m_audio;
    QTimer m_startupWait;
    QTimer m_spanSync;
    PauseRules m_pauseRules;     // the daemon's settings
    PauseVerdict m_pauseVerdict; // what they say about the facts as of the last applyPlayback()
    std::function<std::unique_ptr<SessionMonitor>()> m_monitorFactory;
    std::optional<QDBusConnection> m_powerBus;
    std::unique_ptr<GameModeWatcher> m_gameMode;
    std::unique_ptr<PowerWatcher> m_power; // keeps the players of a spanned video together
    quint64 m_nextRequest = 0;
    quint64 m_settingsRequest = 0;
    bool m_settingsRetried = false; // the one retry of a failed Settings() call
    quint64 m_assetsRequest = 0;
    QString m_assetsDir; // the daemon's WallpaperEngineAssetsDir; empty without Wallpaper Engine
    bool m_assetsKnown = false; // false while a WallpaperEngineAssetsDir request is in flight: nothing resolves yet
    bool m_refreshPending = false; // a refreshAll() was held back by that
    QTimer m_assetsWait; // bounds the wait for the daemon's answer
    int m_noFrameTimeoutMs = kEngineNoFrameTimeoutMs;
    int m_webFpsLimit = kDefaultWebFpsLimit; // the daemon's settings, once loaded
    bool m_pauseWhenCovered = kDefaultPauseWhenCovered;
    QString m_presenter = QString(kDefaultPresenter);
    QString m_sceneEngine = QString(kDefaultSceneEngine); // which engine draws scenes, see sceneengine.h
    bool m_sandboxEngines = kDefaultSandboxEngines;        // scene engines run behind murasaki-sandbox
    bool m_requireSandbox = kDefaultRequireSandbox;        // and are not started without it
    double m_sceneRenderScale = kDefaultSceneRenderScale;  // the Vulkan engine's resolution
    bool m_presenterKnown = false; // false until the daemon answered (or failed): no window before that
    QTimer m_presenterWait;
    // Policy server: the compositor's cursor on its way to the hosts (kwin-cursor.js, only while a host is visible).
    CursorRouter m_cursorRouter{kCursorMinIntervalMs};
    QElapsedTimer m_cursorClock;
    QTimer m_cursorFlush; // releases the position the router held back
    QPointF m_lastCursor;
    bool m_haveCursor = false;
    bool m_cursorReporting = false;
    // Host mode
    QDBusServiceWatcher *m_policyWatcher = nullptr;
    bool m_hostRegistered = false;
    bool m_loggedNoPolicy = false;
    QTimer m_registerRetry; // a refused RegisterHost is retried with backoff
    int m_registerBackoff = 1000; // kFirstRegisterBackoffMs
    quint64 m_hostRequest = 0;     // HostState fetches
    quint64 m_registerRequest = 0; // RegisterHost calls
    std::optional<HostPolicy> m_hostPolicy;
    bool m_resolving = false; // false until the first monitor state (or its timeout) arrived
};

} // namespace murasaki
