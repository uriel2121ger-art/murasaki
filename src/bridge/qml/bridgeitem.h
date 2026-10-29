#pragma once

#include "bridgesupport.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QQuickItem>
#include <QTimer>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class QProcess;

namespace murasaki {

class BridgeClient;
class BridgeFrameQueue;
struct BridgeReleaseState;

// How to start the host process: the program, its command line, and whether that command line already carries
// the initial fill, frame rate limit, volume and properties (so the first live sync sends only later changes).
// A non-empty `error` makes the item give up with that reason (`failed`).
struct BridgeLaunch {
    QString program;
    QStringList arguments;
    bool carriesState = false;
    QString error;
    QSize size;           // the buffer size the host was launched with, when it is not the viewport's (see bufferSize())
};

// Shows the frames of a host process that renders over the Murasaki bridge: it launches `program` with an
// inherited socket (and the viewport size, see buildLaunch()), imports the frames it announces as dma-bufs, or
// from shared memory when the driver cannot import them, and forwards the pointer. Used for scenes (SceneItem
// is a thin subclass) and, in the Plasma plugin, for `murasaki-surface --host`. Needs the OpenGL scene graph
// backend (murasaki-surface forces it); any other backend reports failed("unsupported graphics backend").
//
// Threads: the GUI thread owns the process, the socket and every property. The scene-graph render thread only
// imports and draws buffers, in updatePaintNode(), and talks to the GUI thread through a BridgeFrameQueue (one
// mutex-protected hand-over point).
//
// The host is restarted with a 1 s / 2 s / 4 s backoff when it dies; when it keeps crashing the item gives up
// and emits `failed`. `failed` is also emitted when the program cannot be launched.
class BridgeItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString program READ program WRITE setProgram NOTIFY programChanged)
    Q_PROPERTY(QStringList arguments READ arguments WRITE setArguments NOTIFY argumentsChanged)
    Q_PROPERTY(QVariantMap properties READ properties WRITE setProperties NOTIFY propertiesChanged)
    Q_PROPERTY(QString fill READ fill WRITE setFill NOTIFY fillChanged)
    // Spanning: this item shows a rectangle of a canvas that spreads over several outputs, {canvasW, canvasH, x, y, w,
    // h} in logical pixels, or empty for the whole picture. The fill mode then applies to the canvas. Sent to the host
    // as a SpanView message (live, and again after a restart); a host that predates it makes the item fail.
    Q_PROPERTY(QVariantMap span READ span WRITE setSpan NOTIFY spanChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool audible READ audible WRITE setAudible NOTIFY audibleChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)

public:
    explicit BridgeItem(QQuickItem *parent = nullptr);
    ~BridgeItem() override;

    // The host program and its fixed arguments. The default command line is `arguments` followed by
    // `--murasaki-bridge-fd <fd> --murasaki-size <W>x<H>` (device pixels). Changing either restarts the host.
    QString program() const { return m_program; }
    void setProgram(const QString &program);
    QStringList arguments() const { return m_arguments; }
    void setArguments(const QStringList &arguments);
    // User properties by key. A value is a bool, a number, a string, a QColor or a list of numbers (a Wallpaper
    // Engine color is "r g b" or [r, g, b]). Changing the map sends only the keys whose value changed or
    // is new; a key that disappears is not sent (Wallpaper Engine never removes a property, it only changes it).
    QVariantMap properties() const { return m_properties; }
    void setProperties(const QVariantMap &properties);
    QString fill() const { return m_fill; }
    void setFill(const QString &fill);
    QVariantMap span() const { return m_span; }
    void setSpan(const QVariantMap &span);
    bool spanEnabled() const { return !m_span.isEmpty(); }
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    // Audio is off until `audible` is true; both directions are live (a Volume message, volume 0 while muted).
    bool audible() const { return m_audible; }
    void setAudible(bool audible);
    // 0.0 .. 1.0, mapped to the bridge's 0..128 scale. Only audible while `audible` is true.
    qreal volume() const { return m_volume; }
    void setVolume(qreal volume);
    // True once a frame of the current configuration has been drawn; false again when a changed program, arguments or
    // subclass setting restarts the host. Lets the host keep a placeholder (the preview) until then.
    bool hasFrame() const { return m_hasFrame; }
    int fps() const { return m_fps; }
    void setFps(int fps);

signals:
    void programChanged();
    void argumentsChanged();
    void propertiesChanged();
    void fillChanged();
    void spanChanged();
    void pausedChanged();
    void audibleChanged();
    void volumeChanged();
    void fpsChanged();
    void hasFrameChanged();
    void buttonPressed(); // a pointer button went down over the item (a real event: the host saw it)
    void failed(const QString &reason);

protected:
    // What the subclass decides. canStart() gates the launch (default: a program is set); configurationError()
    // is checked once the graphics backend is known and, when not empty, makes the item give up with it;
    // buildLaunch() produces the command line for the given host end of the socket and viewport size (device
    // pixels) and is called again on every restart.
    virtual bool canStart() const;
    virtual QString configurationError() const { return {}; }
    virtual BridgeLaunch buildLaunch(int bridgeFd, const QSize &size) const;
    // The size of the buffers the host is asked to render, for a viewport of `pixels`: the viewport itself by default. A subclass
    // whose host cannot render below its output returns less; the item then draws the buffer scaled up.
    virtual QSize bufferSize(const QSize &pixels) const { return pixels; }
    // The host died or the bridge was lost. A subclass that can make the next launch different (for example fall back
    // to another program, see SceneItem) changes its state and returns true: the host is then restarted at once, with
    // a fresh crash budget. The default (false) leaves it to the backoff.
    virtual bool handleHostLost(const QString &reason);
    // The producer's Hello was accepted: `client` is ready to be sent to (a host with more to tell the producer overrides this).
    virtual void clientReady(BridgeClient *) {}
    // The user properties as the host process is given them, from `properties` (default: as they are). A subclass keeps a value from the
    // process it hosts (a SceneItem does not hand the engine the application a user chose for a shortcut, only its name).
    virtual QVariantMap propertiesForEngine(const QVariantMap &properties) const { return properties; }
    // The properties changed to `properties` (before they are sent).
    virtual void propertiesApplied(const QVariantMap &) {}
    // The name of the log file (without ".log") that receives what the host prints, under
    // ~/.local/state/murasaki/logs. Default: the program's name, plus the output after "--host" when there is one.
    virtual QString logName(const BridgeLaunch &launch) const;
    // The host process ended with this status (before the item decides what to do about it). A subclass that tells a refusal from a crash reads the exit code here; the default does nothing.
    virtual void hostExited(int code, QProcess::ExitStatus status) { Q_UNUSED(code); Q_UNUSED(status); }
    // A host that was stopped (retired) ended by itself with this status before the retirement killed it; only with an exit wait above 0. The default does nothing.
    virtual void retiredHostExited(int code, QProcess::ExitStatus status) { Q_UNUSED(code); Q_UNUSED(status); }
    // A host that must be seen to end, and of which only one may live (a puppet engine, whose crashes are counted). With a wait above 0 (set by the subclass; ms): (1) when the connection is lost while the process has
    // not been seen to end, the item does not decide yet: the `finished` handler comes back with the exit status (hostExited() sees it - a host that crashed has closed its socket before its parent hears of the exit), and a host
    // that closed its socket and lives on is killed after that long, which is a crash; (2) stopping the host (a restart, a new configuration, the item going away) retires the process (HostRetirement): it is
    // killed after that long if it is still there, and no new host of this kind is started until it is gone, so at most one is alive. Nothing here blocks. 0 (the default): neither.
    void setExitWaitMs(int ms) { m_exitWaitMs = ms; }
    int exitWaitMs() const { return m_exitWaitMs; }
    // A host that stays paused this long (ms) is stopped to give its memory back, and started again when the pause ends; its last frame
    // stays on screen meanwhile. 0 (the default): a paused host keeps running. Only for a host that can start over from nothing (a
    // scene): what it was in the middle of is lost.
    void setParkAfterMs(int ms);
    bool parked() const { return m_parked; }
    // Tears the host down (see below), and what happens when the connection to it is lost; here for the subclasses and the tests.
    void stopHost();
    void onHostLost(const QString &reason);
    // Takes a process as the running host, wired like the one startHost makes (no socket, no client): for tests.
    void adoptHost(QProcess *process);
    // The client of the running host, or null between hosts: a subclass sends its own messages through it.
    BridgeClient *hostClient() const { return m_client; }
    // Restarts the host with the current configuration (a subclass calls it when its own settings change).
    void restartHost();
    // The program of the host was replaced on disk (an install): the host is started again from the new file. Unlike restartHost() the last frame stays up, and it is not a crash.
    void reloadHost();
    bool isCompleted() const { return m_completed; }
    // How the host is called in failure reasons ("<name> crashed", "<name> exited with code 1").
    void setHostName(const QString &name) { m_hostName = name; }
    // What the item shows, as a prefix of the other failure reasons ("<noun> rendering context unavailable",
    // "cannot create the <noun> bridge socket"); empty for none.
    void setFailureNoun(const QString &noun) { m_noun = noun; }

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    struct Applied {
        QString fill;
        QVariantMap span;
        int fps = 0;
        uint32_t volume = 0; // effective 0..128, 0 while muted
        bool paused = false;
        QSize size;
        QVariantMap properties;
    };

    void scheduleStart();
    void startHost();
    void park();
    // Tears the host down. Process ownership passes to a reaper that waits for it to exit (and kills it
    // after 2 s), so the GUI thread never blocks.
    void setHasFrame(bool has);
    void onReady();
    void onRenderContextLost();
    void syncLive();
    void sendResize();
    void forwardPointer(const QPointF &position, Qt::MouseButtons buttons);
    void connectWindow(QQuickWindow *window);
    bool openGlBackend(bool *known) const;
    void wireProcess(QProcess *process, const QString &logName);
    QSize pixelSize() const;
    uint32_t effectiveVolume() const;

    QString m_program;
    QStringList m_arguments;
    QVariantMap m_properties;
    QString m_fill = QStringLiteral("fill");
    QVariantMap m_span;
    QVariantMap m_refusedSpan; // the span the host could not take, so that it is reported once
    bool m_paused = false;
    bool m_audible = false;
    qreal m_volume = 1.0;
    int m_fps = 30;
    QString m_hostName = QStringLiteral("bridge host");
    QString m_noun;

    bool m_completed = false;
    bool m_startQueued = false;
    bool m_gaveUp = false;
    bool m_hasFrame = false;
    std::atomic<bool> m_frameReported{false}; // render thread: the hasFrame hop is already queued
    Applied m_applied;
    uint32_t m_lastButtons = 0; // the buttons of the last pointer event forwarded
    RestartBackoff m_backoff;
    QElapsedTimer m_clock;
    QTimer m_restartTimer;
    QTimer m_exitTimer; // a host whose socket closed and that has not ended: killed when it fires
    QTimer m_resizeTimer;
    ProgramWatch m_programWatch;
    QProcess *m_process = nullptr;
    BridgeClient *m_client = nullptr;
    std::shared_ptr<BridgeFrameQueue> m_queue;
    std::shared_ptr<BridgeReleaseState> m_releases;
    QStringList m_log; // last host output lines, kept for failure reports
    QMetaObject::Connection m_swapConnection;
    int m_exitWaitMs = 0;
    int m_parkAfterMs = 0;
    bool m_parked = false; // stopped while paused; started again when the pause ends
    QTimer m_parkTimer;
};

} // namespace murasaki
