#include "videoitem.h"

#include "canvas.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QThreadPool>
#include <QOpenGLFunctions>

#if defined(QT_FEATURE_wayland) && QT_FEATURE_wayland == 1
#include <QtGui/qguiapplication_platform.h>
#define MURASAKI_MPV_WAYLAND 1
#endif

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <time.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace murasaki {

// Bridges mpv's callbacks, which run on mpv threads, to the GUI thread. It only posts queued signals, and it is
// deleted with deleteLater() by whichever owner (item or renderer) releases it last, after that owner has
// unregistered its callback.
class MpvNotifier : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    static void onWakeup(void *self)
    {
        QMetaObject::invokeMethod(static_cast<MpvNotifier *>(self), &MpvNotifier::eventsPending, Qt::QueuedConnection);
    }
    static void onFrame(void *self)
    {
        QMetaObject::invokeMethod(static_cast<MpvNotifier *>(self), &MpvNotifier::frameReady, Qt::QueuedConnection);
    }

signals:
    void eventsPending();
    void frameReady();
    void renderContextReady();
};

namespace {

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    return context ? reinterpret_cast<void *>(context->getProcAddress(QByteArray(name))) : nullptr;
}

class VideoRenderer : public QQuickFramebufferObject::Renderer
{
public:
    VideoRenderer(std::shared_ptr<mpv_handle> mpv, std::shared_ptr<MpvNotifier> notifier)
        : m_mpv(std::move(mpv))
        , m_notifier(std::move(notifier))
    {
    }

    ~VideoRenderer() override
    {
        if (m_context)
            mpv_render_context_free(m_context); // no more frame callbacks after this returns
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (!m_context && m_mpv)
            createContext(); // the render thread's GL context is current here
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        m_transparent = static_cast<VideoItem *>(item)->transparent();
    }

    void render() override
    {
        QOpenGLFramebufferObject *fbo = framebufferObject();
        if (!m_context) {
            QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
            fbo->bind();
            gl->glClearColor(0.f, 0.f, 0.f, m_transparent ? 0.f : 1.f); // a pet shows nothing before its first frame
            gl->glClear(GL_COLOR_BUFFER_BIT);
            QQuickOpenGLUtils::resetOpenGLState();
            return;
        }
        mpv_render_context_update(m_context);
        mpv_opengl_fbo target{static_cast<int>(fbo->handle()), fbo->width(), fbo->height(), 0};
        int flipY = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &target},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_context, params);
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    void createContext()
    {
        mpv_opengl_init_params gl{getProcAddress, nullptr};
        char apiType[] = MPV_RENDER_API_TYPE_OPENGL;
        std::vector<mpv_render_param> params{
            {MPV_RENDER_PARAM_API_TYPE, apiType},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        };
#ifdef MURASAKI_MPV_WAYLAND
        // Lets mpv share the compositor connection for VA-API hardware decoding interop.
        if (auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            params.push_back({MPV_RENDER_PARAM_WL_DISPLAY, wayland->display()});
#endif
        params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});
        // mpv treats a pending GL error as its own failure; whatever Qt, the bridge or WebEngine left is not its.
        if (QOpenGLContext *current = QOpenGLContext::currentContext()) {
            for (int i = 0; i < 16 && current->functions()->glGetError() != GL_NO_ERROR; ++i) {
            }
        }
        if (mpv_render_context_create(&m_context, m_mpv.get(), params.data()) < 0) {
            qWarning("VideoItem: cannot create the mpv render context");
            m_context = nullptr;
            return;
        }
        mpv_render_context_set_update_callback(m_context, &MpvNotifier::onFrame, m_notifier.get());
        QMetaObject::invokeMethod(m_notifier.get(), &MpvNotifier::renderContextReady, Qt::QueuedConnection);
    }

    std::shared_ptr<mpv_handle> m_mpv;
    std::shared_ptr<MpvNotifier> m_notifier;
    mpv_render_context *m_context = nullptr;
    bool m_transparent = false;
};

} // namespace

VideoItem::VideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_notifier(new MpvNotifier, [](MpvNotifier *notifier) { notifier->deleteLater(); })
{
    m_reannounce.setSingleShot(true);
    connect(&m_reannounce, &QTimer::timeout, this, [this] { announceWrap(false); });
    mpv_handle *handle = mpv_create();
    if (!handle) {
        qWarning("VideoItem: mpv_create failed (is LC_NUMERIC set to \"C\"?)");
        return;
    }
    // mpv_terminate_destroy can block while mpv shuts down its threads, so never run it on the GUI or render
    // thread. The renderer frees its render context before dropping its reference, so by the time the last
    // reference goes away (on either thread) the context is already gone.
    m_mpv.reset(handle, [](mpv_handle *h) { QThreadPool::globalInstance()->start([h] { mpv_terminate_destroy(h); }); });
    const std::pair<const char *, const char *> options[] = {
        {"vo", "libmpv"},
        {"hwdec", "auto-safe"},
        // Hardware decoding is the normal path. When it is not available (a size or codec the GPU cannot do, e.g. 7680x1440
        // H.264 on VAAPI) software decoding must not take every core of the machine for a wallpaper: mpv's default of one
        // frame thread per core made such a file use more than four cores.
        {"vd-lavc-threads", "4"},
        {"loop-file", "inf"},
        {"keep-open", "yes"},
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"osc", "no"},
        {"terminal", "no"},
        {"ytdl", "no"},
        {"audio-client-name", "murasaki"},
        {"mute", "yes"}, // nothing is audible before the policy says so
        // A wallpaper competes with the user's apps for the GPU; mpv's "fast" profile drops the expensive scalers
        // and the rgba16f intermediate, which otherwise saturate integrated GPUs on 4K sources.
        {"profile", "fast"},
        // Bilinear scaling of a 4K source down to the output looks the same on a wallpaper and roughly halves the
        // remaining render cost.
        {"scale", "bilinear"},
        {"dscale", "bilinear"},
        {"cscale", "bilinear"},
    };
    for (const auto &[name, value] : options)
        mpv_set_option_string(handle, name, value);
    if (mpv_initialize(handle) < 0) {
        qWarning("VideoItem: mpv_initialize failed");
        m_mpv.reset();
        return;
    }
    // mpv's own warnings explain most black screens (missing codec, GL interop failures); MURASAKI_MPV_LOG raises
    // the level (e.g. "v" or "debug") when diagnosing.
    const QByteArray logLevel = qEnvironmentVariableIsSet("MURASAKI_MPV_LOG") ? qgetenv("MURASAKI_MPV_LOG")
                                                                              : QByteArrayLiteral("warn");
    mpv_request_log_messages(handle, logLevel.constData());
    connect(m_notifier.get(), &MpvNotifier::frameReady, this, &QQuickItem::update);
    connect(m_notifier.get(), &MpvNotifier::eventsPending, this, &VideoItem::handleEvents);
    connect(m_notifier.get(), &MpvNotifier::renderContextReady, this, &VideoItem::onRenderContextReady);
    mpv_set_wakeup_callback(handle, &MpvNotifier::onWakeup, m_notifier.get());
    applyFill();
    applySpeed();
}

VideoItem::~VideoItem()
{
    if (m_mpv)
        mpv_set_wakeup_callback(m_mpv.get(), nullptr, nullptr);
}

QQuickFramebufferObject::Renderer *VideoItem::createRenderer() const
{
    return new VideoRenderer(m_mpv, m_notifier);
}

void VideoItem::setMpvProperty(const char *name, const char *value)
{
    // Async so the GUI thread never blocks on mpv; the value is copied, so pointing at a local is fine.
    if (m_mpv)
        mpv_set_property_async(m_mpv.get(), 0, name, MPV_FORMAT_STRING, &value);
}

void VideoItem::setSource(const QString &source)
{
    if (source == m_source)
        return;
    m_source = source;
    loadSource();
    emit sourceChanged();
}

void VideoItem::setTransparent(bool transparent)
{
    if (transparent == m_transparent)
        return;
    m_transparent = transparent;
    applyTransparent();
    loadSource(); // the decoder choice depends on it
    emit transparentChanged();
}

void VideoItem::applyTransparent()
{
    // "alpha=yes" makes mpv render the file's alpha into the target instead of blending it over the background;
    // "background=none" keeps the cleared areas transparent (the target is premultiplied, as Qt Quick expects).
    setMpvProperty("background", m_transparent ? "none" : "color");
    setMpvProperty("alpha", m_transparent ? "yes" : "blend");
    setMpvProperty("hwdec", m_transparent ? "no" : "auto-safe");
    // A pet is small and silent: no audio decoding at all, two decoder threads, and a small demuxer cache (the
    // defaults buffer 150 MiB of a looping file).
    setMpvProperty("aid", m_transparent ? "no" : "auto");
    setMpvProperty("vd-lavc-threads", m_transparent ? "2" : "4");
    setMpvProperty("demuxer-max-bytes", m_transparent ? "8MiB" : "150MiB");
    setMpvProperty("demuxer-max-back-bytes", m_transparent ? "1MiB" : "0");
    if (!m_transparent)
        setMpvProperty("vd", "");
}

void VideoItem::loadSource()
{
    // Until the render thread creates the render context, mpv has no video output and would play audio only;
    // onRenderContextReady() loads the source then.
    if (!m_mpv || !m_renderContextReady)
        return;
    const QByteArray path = m_source.toUtf8();
    // FFmpeg's native VP9 decoder drops the alpha plane of a WebM; libvpx keeps it.
    const bool webm = m_transparent && m_source.endsWith(u".webm"_s, Qt::CaseInsensitive);
    if (m_transparent)
        setMpvProperty("vd", webm ? "libvpx-vp9" : "");
    const char *command[] = {m_source.isEmpty() ? "stop" : "loadfile", m_source.isEmpty() ? nullptr : path.constData(),
                             nullptr};
    mpv_command_async(m_mpv.get(), 0, command);
}

void VideoItem::onRenderContextReady()
{
    m_renderContextReady = true;
    loadSource();
}

void VideoItem::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    setMpvProperty("pause", paused ? "yes" : "no");
    emit pausedChanged();
    if (paused) {
        m_reannounce.stop();
        m_announcedWrapNs = 0;
        emit wrapCancelled();
    } else if (m_started)
        announceWrap(false); // goes on from where it stopped: the end of this loop is a pause later than it was
}

void VideoItem::setAudible(bool audible)
{
    if (audible == m_audible)
        return;
    m_audible = audible;
    setMpvProperty("mute", audible ? "no" : "yes");
    emit audibleChanged();
}

void VideoItem::setFill(const QString &fill)
{
    if (fill == m_fill)
        return;
    m_fill = fill;
    applyFill();
    emit fillChanged();
}

void VideoItem::setSpeed(double speed)
{
    // mpv accepts 0.01 to 100; anything else (or not a number) is the file's own timing.
    if (!(speed >= 0.01 && speed <= 100))
        speed = 1;
    if (speed == m_speed)
        return;
    m_speed = speed;
    applySpeed();
    emit speedChanged();
    if (m_started)
        announceWrap(false); // (withdraws the old announcement itself)
    else
        emit wrapCancelled();
}

void VideoItem::applySpeed()
{
    setMpvProperty("speed", QByteArray::number(m_speed, 'g', 10).constData());
}

void VideoItem::applyFill()
{
    const bool spanning = !m_spanCanvas.isEmpty() && !m_spanRect.isEmpty() && !m_videoSize.isEmpty();
    if (spanning) {
        // The picture is placed on the whole canvas by the fill mode; this item shows its rectangle of it.
        const MpvViewport view = mpvViewport(m_spanCanvas, m_spanRect, m_videoSize, parseFillMode(m_fill).value_or(FillMode::Fill));
        setOpacity(view.visible ? 1.0 : 0.0); // a picture that stays out of this output (a Fit band) shows nothing
        const auto number = [](double value) { return QByteArray::number(value, 'g', 12); };
        setMpvProperty("keepaspect", "yes");
        setMpvProperty("panscan", "0.0");
        setMpvProperty("video-aspect-override", number(view.aspect).constData());
        setMpvProperty("video-zoom", number(view.zoom).constData());
        setMpvProperty("video-pan-x", number(view.panX).constData());
        setMpvProperty("video-pan-y", number(view.panY).constData());
        return;
    }
    setOpacity(1.0);
    // Newer mpv wants "no" to drop the override and warns about -1; older ones only know -1. Ask once, at the first use.
    if (m_aspectReset.isEmpty() && m_mpv)
        m_aspectReset = mpv_set_property_string(m_mpv.get(), "video-aspect-override", "no") >= 0 ? "no" : "-1";
    setMpvProperty("video-aspect-override", m_aspectReset.isEmpty() ? "-1" : m_aspectReset.constData());
    setMpvProperty("video-zoom", "0");
    setMpvProperty("video-pan-x", "0");
    setMpvProperty("video-pan-y", "0");
    // fill = cover and crop (Wallpaper Engine's behavior), fit = letterbox, stretch = ignore the aspect ratio.
    setMpvProperty("keepaspect", m_fill == u"stretch" ? "no" : "yes");
    setMpvProperty("panscan", m_fill == u"fill" ? "1.0" : "0.0");
}

void VideoItem::setFrameRateLimit(int fps)
{
    fps = std::clamp(fps, 0, 1000);
    if (fps == m_frameRateLimit)
        return;
    m_frameRateLimit = fps;
    // mpv's fps filter resamples to the cap (dropping frames above it); an empty chain lifts the cap.
    setMpvProperty("vf", fps > 0 ? QByteArray("fps=" + QByteArray::number(fps)).constData() : "");
    emit frameRateLimitChanged();
}

void VideoItem::setSpan(const QVariantMap &span)
{
    if (span == m_span)
        return;
    m_span = span;
    const auto number = [&](const char *key) { return span.value(QLatin1String(key)).toDouble(); };
    m_spanCanvas = span.isEmpty() ? QSizeF() : QSizeF(number("canvasW"), number("canvasH"));
    m_spanRect = span.isEmpty() ? QRectF() : QRectF(number("x"), number("y"), number("w"), number("h"));
    applyFill();
    emit spanChanged();
}

double VideoItem::position() const
{
    double seconds = 0;
    if (!m_mpv || mpv_get_property(m_mpv.get(), "time-pos", MPV_FORMAT_DOUBLE, &seconds) < 0)
        return -1;
    return seconds;
}

double VideoItem::duration() const
{
    double seconds = 0;
    if (!m_mpv || mpv_get_property(m_mpv.get(), "duration", MPV_FORMAT_DOUBLE, &seconds) < 0 || !(seconds > 0) || !std::isfinite(seconds))
        return -1;
    return seconds;
}

void VideoItem::seekTo(double seconds)
{
    if (!m_mpv || !(seconds >= 0))
        return;
    const QByteArray target = QByteArray::number(seconds, 'f', 3);
    const char *command[] = {"seek", target.constData(), "absolute+exact", nullptr};
    m_seekClock.start();
    m_seekPending = true;
    mpv_command_async(m_mpv.get(), 0, command);
}

// What mpv ended up using for this file (the media sweep reads it): the hardware decoder, the codec and the size.
void VideoItem::logPlayback()
{
    const auto property = [this](const char *name) {
        char *value = mpv_get_property_string(m_mpv.get(), name);
        const QString text = value ? QString::fromUtf8(value) : QStringLiteral("?");
        mpv_free(value);
        return text;
    };
    qInfo("murasaki.media: path vo=%s hwdec-interop=%s hw-pixelformat=%s pixelformat=%s", qPrintable(property("current-vo")),
          qPrintable(property("hwdec-interop")), qPrintable(property("video-params/hw-pixelformat")),
          qPrintable(property("video-params/pixelformat")));
    qInfo("murasaki.media: video hwdec=%s codec=%s size=%sx%s fps=%s audio=%s mute=%s", qPrintable(property("hwdec-current")),
          qPrintable(property("video-codec")), qPrintable(property("video-params/w")), qPrintable(property("video-params/h")),
          qPrintable(property("container-fps")), qPrintable(property("audio-codec-name")), qPrintable(property("mute")));
    qInfo("murasaki.media: audio ao=%s volume=%s", qPrintable(property("current-ao")), qPrintable(property("volume")));
}

void VideoItem::updateVideoSize()
{
    int64_t width = 0;
    int64_t height = 0;
    QSizeF size;
    if (m_mpv && mpv_get_property(m_mpv.get(), "video-params/dw", MPV_FORMAT_INT64, &width) >= 0
        && mpv_get_property(m_mpv.get(), "video-params/dh", MPV_FORMAT_INT64, &height) >= 0)
        size = QSizeF(double(width), double(height));
    if (size == m_videoSize)
        return;
    m_videoSize = size;
    if (!m_spanCanvas.isEmpty())
        applyFill();
}

void VideoItem::announceWrap(bool first, bool keepPending)
{
    m_reannounce.stop();
    if (!keepPending) {
        m_announcedWrapNs = 0;
        emit wrapCancelled(); // whatever stood is replaced: a restart that was no wrap (a seek, a new track) must not leave its start behind
    }
    if (!m_mpv || m_paused)
        return;
    double position = -1, duration = -1;
    if (mpv_get_property(m_mpv.get(), "time-pos", MPV_FORMAT_DOUBLE, &position) < 0 || mpv_get_property(m_mpv.get(), "duration", MPV_FORMAT_DOUBLE, &duration) < 0
        || !(position >= 0) || !(duration > 0) || !std::isfinite(duration) || !(m_speed > 0))
        return;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = double(ts.tv_sec) * 1e9 + double(ts.tv_nsec);
    position = std::min(position, duration);
    const double remainingNs = (duration - position) / m_speed * 1e9;
    if (first)
        emit wrapped(now - position / m_speed * 1e9, m_loop); // the loop that is playing began this long ago
    m_announcedWrapNs = now + remainingNs;
    emit wrapped(m_announcedWrapNs, m_loop + 1);
    // The estimate drifts over a long loop (the duration of some files is only what the container says, the clock of mpv is not ours): say it again 200 ms
    // before, from a fresh position. Short loops have no room for a second look.
    constexpr double kAgainBeforeNs = 200e6;
    if (remainingNs > 3 * kAgainBeforeNs)
        m_reannounce.start(int((remainingNs - kAgainBeforeNs) / 1e6));
}

void VideoItem::handleEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv.get(), 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto *log = static_cast<const mpv_event_log_message *>(event->data);
            qWarning("mpv[%s] %s: %s", log->level, log->prefix, QByteArray(log->text).trimmed().constData());
            continue;
        }
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && !m_started) {
            m_started = true;
            m_loop = 0;
            if (qEnvironmentVariableIsSet("MURASAKI_MEDIA_LOG"))
                logPlayback();
            emit started();
            announceWrap(true);
            continue;
        }
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && m_seekPending) { // the seek asked of seekTo() has landed
            m_seekPending = false;
            m_seekLatency = m_seekClock.nsecsElapsed() / 1e9;
        }
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) { // loop-file: the file went back to its start (or a seek: then the announcement is withdrawn)
            m_loop += 1;
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double now = double(ts.tv_sec) * 1e9 + double(ts.tv_nsec);
            constexpr double kAsAnnouncedNs = 150e6;
            announceWrap(false, m_announcedWrapNs > 0 && std::abs(now - m_announcedWrapNs) < kAsAnnouncedNs);
            continue;
        }
        if (event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            updateVideoSize();
            continue;
        }
        if (event->event_id == MPV_EVENT_START_FILE) {
            m_started = false;
            m_reannounce.stop();
            m_announcedWrapNs = 0;
            emit wrapCancelled();
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end && end->reason == MPV_END_FILE_REASON_ERROR)
                emit failed(QString::fromUtf8(mpv_error_string(end->error)));
        }
    }
}

} // namespace murasaki

#include "videoitem.moc"
