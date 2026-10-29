#pragma once

#include <QElapsedTimer>
#include <QQuickFramebufferObject>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

struct mpv_handle;

namespace murasaki {

class MpvNotifier;

// Plays one video file through libmpv's render API into a Qt Quick FBO. Needs the OpenGL scene graph backend
// (murasaki-surface forces it). Loops forever; `failed` reports files mpv cannot open or decode.
class VideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool audible READ audible WRITE setAudible NOTIFY audibleChanged)
    Q_PROPERTY(QString fill READ fill WRITE setFill NOTIFY fillChanged)
    // Spanning: this output shows a rectangle of a canvas that spreads over several outputs. {canvasW, canvasH, x, y, w, h}
    // in logical pixels (see canvas.h), or empty for the whole picture in this item. The fill mode then applies to the
    // canvas, and the item pans and zooms its player to show its part (mpv's video-aspect-override, -zoom and -pan).
    Q_PROPERTY(QVariantMap span READ span WRITE setSpan NOTIFY spanChanged)
    // Power saving: no more than this many frames a second are decoded and drawn (mpv's `fps` filter); 0 for the file's
    // own rate.
    Q_PROPERTY(int frameRateLimit READ frameRateLimit WRITE setFrameRateLimit NOTIFY frameRateLimitChanged)
    // Playback speed factor (mpv's `speed`): 1 plays the file with its own timing.
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    // Pet mode: keeps the alpha channel of the file (APNG, GIF, WebP, VP9 with alpha) instead of blending it over
    // black, so the item composites over whatever is behind its window. Software decoding, because hardware
    // decoders drop the alpha. Set it before `source`.
    Q_PROPERTY(bool transparent READ transparent WRITE setTransparent NOTIFY transparentChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);
    ~VideoItem() override;

    Renderer *createRenderer() const override;

    QString source() const { return m_source; }
    void setSource(const QString &source);
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    bool audible() const { return m_audible; }
    void setAudible(bool audible);
    QString fill() const { return m_fill; }
    void setFill(const QString &fill);
    int frameRateLimit() const { return m_frameRateLimit; }
    void setFrameRateLimit(int fps);
    QVariantMap span() const { return m_span; }
    void setSpan(const QVariantMap &span);
    // The playback position in seconds, or -1 when nothing plays; outputs of one spanned video are kept in step
    // with these two.
    Q_INVOKABLE double position() const;
    Q_INVOKABLE void seekTo(double seconds);
    // The length of the file in seconds, or -1 when it is not known (yet).
    Q_INVOKABLE double duration() const;
    // How long the last seek asked of seekTo() took to land (mpv's playback-restart), in seconds; -1 before the first one. An exact seek decodes from the last keyframe, so
    // on a 4K copy it takes a tenth of a second or more, in which the file went on playing: whoever aims a seek at a moving target has to allow for it.
    Q_INVOKABLE double seekLatency() const { return m_seekLatency; }
    double speed() const { return m_speed; }
    void setSpeed(double speed);
    bool transparent() const { return m_transparent; }
    void setTransparent(bool transparent);

signals:
    void sourceChanged();
    void pausedChanged();
    void audibleChanged();
    void fillChanged();
    void spanChanged();
    void frameRateLimitChanged();
    void speedChanged();
    void transparentChanged();
    void failed(const QString &reason);
    // The first frame of a loaded file is out (once per load, not per loop).
    void started();
    // The loop wraps at `monotonicNs` (CLOCK_MONOTONIC, as a double: nanoseconds stay exact to well under a microsecond for a year of uptime), `loop` counting
    // from 0 at the first showing. Announced when the previous wrap is seen, a whole loop ahead, from mpv's own position (`time-pos`, `duration`, `speed`): good to
    // a frame. The sound of a pet on this path starts on it (docs/pets/audio.md).
    void wrapped(double monotonicNs, double loop);
    // The announced wrap will not be at that time (pause, other speed, other file); a new announcement follows when playback goes on.
    void wrapCancelled();

private:
    void handleEvents();
    // Tells where the loop ends from mpv's position; `first` also announces the loop that has just begun.
    // `keepPending`: the wrap announced before is still wanted (the file just wrapped as announced), so it is not withdrawn first.
    void announceWrap(bool first, bool keepPending = false);
    void applyFill();
    void updateVideoSize();
    void logPlayback();
    QByteArray m_aspectReset; // the value that removes video-aspect-override on this mpv ("no" or "-1")
    void applySpeed();
    void applyTransparent();
    void setMpvProperty(const char *name, const char *value);
    void loadSource();
    void onRenderContextReady();

    // Shared with the renderer, which lives on the render thread and may outlive this item: the render context
    // must be freed before mpv_terminate_destroy, so whoever drops the last reference destroys the handle.
    std::shared_ptr<mpv_handle> m_mpv;
    std::shared_ptr<MpvNotifier> m_notifier;
    QString m_source;
    bool m_renderContextReady = false; // vo=libmpv fails a file loaded before the render context exists
    bool m_paused = false;
    bool m_audible = false; // muted until the playback policy says otherwise
    QString m_fill = QStringLiteral("fill");
    QVariantMap m_span;
    int m_frameRateLimit = 0;
    QSizeF m_spanCanvas;
    QRectF m_spanRect;
    QSizeF m_videoSize; // display size of the current file, once mpv has configured the video output
    double m_speed = 1;
    bool m_transparent = false;
    bool m_started = false; // started() was emitted for the current file
    double m_loop = 0;      // the loop that is playing (the wrap announced last is m_loop + 1)
    double m_announcedWrapNs = 0; // CLOCK_MONOTONIC of the wrap announced last, 0 when none stands
    QElapsedTimer m_seekClock;
    bool m_seekPending = false;
    double m_seekLatency = -1;
    QTimer m_reannounce;    // a loop long enough announces its wrap again just before it, from a fresh position
};

} // namespace murasaki
