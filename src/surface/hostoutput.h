#pragma once

#include "bridge/channel.h"
#include "bridge/producer.h"
#include "outputsink.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QObject>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QTimer>

#include <memory>
#include <optional>

class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class QSocketNotifier;

namespace murasaki {

// The output sink of the Plasma host mode (murasaki-surface --host): Wallpaper.qml rendered offscreen by a
// QQuickRenderControl into the textures of a bridge Producer, whose frames the Plasma plugin's BridgeItem shows
// inside plasmashell. There is no window on screen. The process still runs under a compositor, because Qt
// WebEngine needs a real platform plugin and the offscreen/minimal ones cannot host it.
//
// Threading: everything lives on the GUI thread. QQuickRenderControl renders synchronously there (no render
// thread), so Qt's scene graph, libmpv, Qt WebEngine's compositor texture and the bridge consumer of a nested
// SceneItem all share the one OpenGL context created here (it shares with Qt's global share context). The
// producer needs that context current for service(), resize(), beginFrame() and endFrame(); every entry point
// below makes it current and calls QQuickOpenGLUtils::resetOpenGLState() before returning to Qt Quick.
//
// Pause: a paused output renders nothing, whatever still asks for frames (a scene engine that keeps drawing
// while paused would otherwise keep the consumer busy). Content, size and playback changes still get a short
// grace period to paint (a new video's first frame, the frame after a resize), then the last frame stays. A web
// page that loads while the output is paused may therefore show its first paint only after the unpause (its
// requestAnimationFrame callbacks are held back while paused).
//
// Frame loop: the scene asks for a frame (renderRequested/sceneChanged), the request is paced by the frame-rate
// limit and then rendered: producer.beginFrame -> the texture becomes the window's render target ->
// polish/sync/render -> producer.endFrame(flipY = true, Qt's origin is bottom-left). When the consumer holds
// every buffer the frame is skipped and retried when its Release arrives.
//
// Control messages from the consumer:
//   Resize   - the window and render size change (a new buffer set follows).
//   Pointer  - synthesised mouse move/press/release and hover events at those pixel coordinates; ignored while
//              the content is not interactive (a video lets the pointer through, like the layer-shell window).
//   Pause    - pauseRequested(): the controller treats it as one more pause input.
//   Quit     - quitRequested().
//   Volume, Scaling, Fps, Properties - deliberately ignored: what plays, how it is scaled, how fast it is drawn
//              and which property values it has come from the daemon (assignment, settings), and audibility
//              comes from the playback policy. A consumer default (BridgeItem starts muted at 30 fps) must not
//              override them.
// A closed socket also means quit.
class HostOutput : public QObject, public OutputSink
{
    Q_OBJECT

public:
    // Null (after a warning) when the OpenGL context, the producer or Wallpaper.qml cannot be set up.
    static std::unique_ptr<HostOutput> create(const SinkRequest &request, bridge::UniqueFd bridgeFd, QSize size);
    ~HostOutput() override;

    QString outputName() const override { return m_name; }
    void setContent(const QVariantMap &content) override;
    void setPlayback(bool paused, bool audible) override;
    void setFrameRateLimit(int fps) override;
    void setInteractive(bool interactive) override;
    bool wantsAudio() const override;
    void setPlaybackCap(int fps) override;
    void setAudioSpectrum(const QList<float> &spectrum) override;
    std::optional<double> videoPosition() const override;
    void seekVideo(double seconds) override;
    // Hover from the compositor's cursor position, scaled to the render size (so a scaled or resized output
    // needs no further conversion). Keeps the buttons the bridge Pointer messages reported.
    void setCursorFraction(double x, double y) override;
    // Position, buttons and wheel of the overlay's interaction mode, as the Pointer message of the bridge would bring them.
    void injectPointerFraction(double x, double y, unsigned buttons, int wheelX, int wheelY) override;

    QSize size() const { return m_size; }
    bool pointerInteractive() const { return m_interactive; }
    // Pacing: the shortest time between two rendered frames for the content on screen.
    qint64 frameIntervalMs() const;

    // Entry point of the Pointer message (public for tests).
    void injectPointer(int x, int y, quint32 buttons);
    // A wheel turn at a pixel of the render size (ignored while the content is not interactive).
    void injectWheel(int x, int y, int deltaX, int deltaY);

signals:
    void pauseRequested(bool paused);
    void quitRequested();

private slots:
    void onWantsAudio() { notifyWantsAudioChanged(); }

private:
    HostOutput(QString name, QSize size);
    bool initialize(const SinkRequest &request, bridge::UniqueFd bridgeFd);
    bool makeCurrent();
    void onControl(const bridge::ControlMessage &message);
    void onSocketReadable();
    void onResize(QSize size);
    void requestRender();
    void scheduleRender();
    void renderFrame();
    void resetGlState();
    void stayActive(int ms);

    QString m_name;
    QSize m_size;
    QString m_kind = QStringLiteral("none"); // WallpaperContent::kind on screen
    int m_frameRateLimit = 30;
    qreal m_refreshRate = 60;
    bool m_interactive = false;
    bool m_paused = false;
    QDeadlineTimer m_activeUntil{QDeadlineTimer::Forever}; // while paused: frames are rendered until then
    bool m_dirty = false;
    bool m_quitting = false;
    quint32 m_buttons = 0;
    QPoint m_pointer{-1, -1};

    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
    bridge::Channel m_channel;
    std::unique_ptr<bridge::Producer> m_producer;
    std::optional<uint32_t> m_viewport;
    QQuickRenderControl *m_control = nullptr;
    QQuickWindow *m_window = nullptr;
    QPointer<QQuickItem> m_root;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QTimer m_renderTimer;
    QElapsedTimer m_sinceRender;
    QElapsedTimer m_statClock;
    int m_statRendered = 0;
    int m_statNoBuffer = 0;
    qint64 m_statCostMs = 0;
};

} // namespace murasaki
