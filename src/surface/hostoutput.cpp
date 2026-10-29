#include "hostoutput.h"

#include "wallpaperroot.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>

#include <QQuickOpenGLUtils>

#include <EGL/egl.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kContentGraceMs = 2000; // a paused output paints new content for this long
constexpr int kPauseGraceMs = 500;    // ... and the frames in flight when it is paused
constexpr int kResizeGraceMs = 500;

constexpr uint32_t kButtonLeft = 1u << 0;
constexpr uint32_t kButtonRight = 1u << 1;
constexpr uint32_t kButtonMiddle = 1u << 2;

QSize clampedSize(QSize size)
{
    return {std::clamp(size.width(), 1, int(bridge::kMaxDimension)), std::clamp(size.height(), 1, int(bridge::kMaxDimension))};
}

} // namespace

HostOutput::HostOutput(QString name, QSize size)
    : m_name(std::move(name))
    , m_size(clampedSize(size))
{
    m_statClock.start();
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_renderTimer, &QTimer::timeout, this, &HostOutput::renderFrame);
}

std::unique_ptr<HostOutput> HostOutput::create(const SinkRequest &request, bridge::UniqueFd bridgeFd, QSize size)
{
    std::unique_ptr<HostOutput> output(new HostOutput(request.name, size));
    if (!output->initialize(request, std::move(bridgeFd)))
        return nullptr;
    return output;
}

bool HostOutput::initialize(const SinkRequest &request, bridge::UniqueFd bridgeFd)
{
    if (!QOpenGLContext::globalShareContext()) {
        qWarning("murasaki-surface: host mode needs an OpenGL platform (Qt has no global share context)");
        return false;
    }
    if (const QScreen *screen = [&]() -> const QScreen * {
            for (const QScreen *candidate : QGuiApplication::screens()) {
                if (candidate->name() == m_name)
                    return candidate;
            }
            return nullptr;
        }())
        m_refreshRate = screen->refreshRate();
    if (m_refreshRate < 1)
        m_refreshRate = 60;

    // The recipe verified by the P1 probe (Rect, WebEngineView, VideoItem all render this way): a core context
    // that shares with Qt's global one (WebEngine's compositor and mpv live there), an offscreen surface, and a
    // render control whose window is never shown.
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(format);
    m_context->setShareContext(QOpenGLContext::globalShareContext());
    if (!m_context->create()) {
        qWarning("murasaki-surface: cannot create the OpenGL context");
        return false;
    }
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!makeCurrent()) {
        qWarning("murasaki-surface: cannot make the OpenGL context current");
        return false;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        qWarning("murasaki-surface: host mode needs an EGL OpenGL context (platform \"%s\")",
                 qPrintable(QGuiApplication::platformName()));
        return false;
    }

    m_channel = bridge::Channel(std::move(bridgeFd));
    m_producer = std::make_unique<bridge::Producer>(m_channel, display);
    if (!m_producer->valid()) {
        qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        return false;
    }
    m_viewport = m_producer->addViewport(m_name.toStdString(), uint32_t(m_size.width()), uint32_t(m_size.height()));
    if (!m_viewport) {
        qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        return false;
    }
    m_producer->setControlHandler([this](const bridge::ControlMessage &message) { onControl(message); });
    if (!m_producer->start()) {
        qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        return false;
    }

    m_control = new QQuickRenderControl(this);
    m_window = new QQuickWindow(m_control);
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_context.get()));
    if (!m_control->initialize()) {
        qWarning("murasaki-surface: cannot initialize the Qt Quick render control");
        return false;
    }
    m_window->setColor(Qt::black);
    m_window->resize(m_size);

    QQmlComponent component(request.engine, QUrl(u"qrc:/qt/qml/Murasaki/Render/Wallpaper.qml"_s));
    QObject *created = component.create();
    m_root = qobject_cast<QQuickItem *>(created);
    if (!m_root) {
        for (const QQmlError &error : component.errors())
            qWarning("murasaki-surface: %s", qPrintable(error.toString()));
        delete created;
        return false;
    }
    m_root->setParentItem(m_window->contentItem());
    m_root->setSize(m_size);
    m_root->setProperty("webProfile", QVariant::fromValue(request.webProfile));
    m_root->setProperty("refreshRate", m_refreshRate);
    m_root->setProperty("outputName", m_name);
    new wallpaperroot::HealthRelay(m_root, [this](const QString &key, const QString &event, const QString &reason, const QString &engine) {
        notifyHealth({key, event, reason, engine});
    });
    const QMetaObject *meta = m_root->metaObject();
    const QMetaProperty wants = meta->property(meta->indexOfProperty("wantsAudio"));
    if (wants.isValid() && wants.hasNotifySignal()) {
        QObject::connect(m_root, wants.notifySignal(), this,
                         metaObject()->method(metaObject()->indexOfSlot("onWantsAudio()")));
    }

    connect(m_control, &QQuickRenderControl::renderRequested, this, &HostOutput::requestRender);
    connect(m_control, &QQuickRenderControl::sceneChanged, this, &HostOutput::requestRender);

    m_notifier = std::make_unique<QSocketNotifier>(m_channel.fd(), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &HostOutput::onSocketReadable);

    resetGlState();
    requestRender();
    qInfo("murasaki-surface: host mode for %s, %dx%d, %s", qPrintable(m_name), m_size.width(), m_size.height(),
          m_producer->transport(*m_viewport) == bridge::Transport::Shm ? "shared memory" : "dma-buf");
    return true;
}

HostOutput::~HostOutput()
{
    m_notifier.reset();
    m_renderTimer.stop();
    if (m_context && makeCurrent()) {
        delete m_root.data(); // before the window: WebEngine and mpv free their GL objects here
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (m_control)
            m_control->invalidate();
        delete m_window;
        m_window = nullptr;
        delete m_control;
        m_control = nullptr;
        m_producer.reset(); // frees its textures: the context must be current
        m_context->doneCurrent();
    }
}

bool HostOutput::makeCurrent()
{
    return m_context && m_surface && m_context->makeCurrent(m_surface.get());
}

void HostOutput::resetGlState()
{
    // Whatever the producer or a foreign GL user (mpv, WebEngine) left bound is not what Qt Quick expects.
    QQuickOpenGLUtils::resetOpenGLState();
}

// ---- OutputSink ----------------------------------------------------------------------------------------

void HostOutput::setContent(const QVariantMap &content)
{
    m_kind = content.value(u"kind"_s).toString();
    wallpaperroot::setContent(m_root, content);
    stayActive(kContentGraceMs); // a paused output still shows what it is given
    requestRender();
}

void HostOutput::setPlayback(bool paused, bool audible)
{
    wallpaperroot::setPlayback(m_root, paused, audible);
    if (paused != m_paused) {
        m_paused = paused;
        stayActive(kPauseGraceMs); // frames already on their way (and the paused state itself) may land
    }
    requestRender();
}

void HostOutput::stayActive(int ms)
{
    if (!m_paused)
        return;
    const QDeadlineTimer deadline(ms);
    if (m_activeUntil.isForever() || deadline > m_activeUntil)
        m_activeUntil = deadline;
}

void HostOutput::setFrameRateLimit(int fps)
{
    m_frameRateLimit = std::max(1, fps);
    wallpaperroot::setFrameRateLimit(m_root, fps);
}

void HostOutput::setInteractive(bool interactive)
{
    m_interactive = interactive;
}

void HostOutput::setPlaybackCap(int fps)
{
    wallpaperroot::setPlaybackCap(m_root, fps);
}

bool HostOutput::wantsAudio() const
{
    return wallpaperroot::wantsAudio(m_root);
}

void HostOutput::setAudioSpectrum(const QList<float> &spectrum)
{
    wallpaperroot::setAudioSpectrum(m_root, spectrum);
}

std::optional<double> HostOutput::videoPosition() const
{
    const double position = wallpaperroot::videoPosition(m_root);
    return position >= 0 ? std::optional<double>(position) : std::nullopt;
}

void HostOutput::seekVideo(double seconds)
{
    wallpaperroot::seekVideo(m_root, seconds);
}

// ---- frame loop ----------------------------------------------------------------------------------------

qint64 HostOutput::frameIntervalMs() const
{
    // Videos run at their own rate up to the display's; web pages and scenes obey the daemon's limit.
    const bool limited = m_kind == u"web" || m_kind == u"scene";
    const double fps = limited ? std::min<double>(m_refreshRate, m_frameRateLimit) : m_refreshRate;
    return std::max<qint64>(1, std::llround(1000.0 / std::max(1.0, fps)));
}

void HostOutput::requestRender()
{
    m_dirty = true;
    scheduleRender();
}

void HostOutput::scheduleRender()
{
    if (!m_dirty || m_quitting || m_renderTimer.isActive())
        return;
    const qint64 interval = frameIntervalMs();
    const qint64 elapsed = m_sinceRender.isValid() ? m_sinceRender.elapsed() : interval;
    m_renderTimer.start(int(std::max<qint64>(0, interval - elapsed)));
}

void HostOutput::renderFrame()
{
    if (!m_dirty || m_quitting || !m_viewport)
        return;
    if (m_paused && m_activeUntil.hasExpired()) {
        m_dirty = false; // paused: nothing is drawn until the unpause (or new content) asks again
        return;
    }
    if (!makeCurrent())
        return;
    const std::optional<bridge::FrameTarget> target = m_producer->beginFrame(*m_viewport);
    if (!target) {
        ++m_statNoBuffer;
        // The consumer holds every buffer (or is gone). Stay dirty: the next Release schedules the frame.
        return;
    }
    m_dirty = false; // changes made while rendering ask for the next frame
    const QSize size(int(target->width), int(target->height));
    m_window->setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(target->texture, size));

    ++m_statRendered;
    QElapsedTimer cost;
    cost.start();
    if (m_statClock.elapsed() >= 2000) {
        if (qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG"))
            qInfo("murasaki.host: %d frames rendered (%lld ms each on average), %d retries without a free buffer in %lld ms",
                  m_statRendered, m_statRendered ? m_statCostMs / m_statRendered : 0, m_statNoBuffer,
                  m_statClock.elapsed());
        m_statRendered = m_statNoBuffer = 0;
        m_statCostMs = 0;
        m_statClock.restart();
    }
    m_control->polishItems();
    m_control->beginFrame();
    m_control->sync();
    m_control->render();
    m_control->endFrame();

    const bridge::FrameResult result = m_producer->endFrame(*m_viewport, true);
    resetGlState();
    m_statCostMs += cost.elapsed();
    // A QQuickRenderControl window never swaps, so Qt never emits frameSwapped for it. A nested BridgeItem (the
    // scene engine's frames) releases the dma-buf it showed from that signal; without it the engine runs out of
    // buffers after three frames and stalls. This is the host's "frame presented" moment.
    emit m_window->frameSwapped();
    m_sinceRender.restart();
    switch (result) {
    case bridge::FrameResult::Sent:
        break;
    case bridge::FrameResult::Skipped:
        m_dirty = true; // the socket was backed up; try again at the next slot
        break;
    case bridge::FrameResult::Closed:
        m_quitting = true;
        emit quitRequested();
        return;
    case bridge::FrameResult::Failed:
        qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        m_quitting = true;
        emit quitRequested();
        return;
    }
    scheduleRender();
}

// ---- control messages ----------------------------------------------------------------------------------

void HostOutput::onSocketReadable()
{
    if (m_quitting || !makeCurrent())
        return;
    const bridge::ServiceResult result = m_producer->service();
    resetGlState();
    if (result != bridge::ServiceResult::Ok) {
        if (result == bridge::ServiceResult::Failed)
            qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        m_quitting = true;
        m_renderTimer.stop();
        emit quitRequested();
        return;
    }
    scheduleRender(); // a Release may have freed the buffer the last frame was waiting for
}

void HostOutput::onControl(const bridge::ControlMessage &message)
{
    if (const auto *pause = std::get_if<bridge::msg::Pause>(&message)) {
        emit pauseRequested(pause->paused != 0);
    } else if (const auto *resize = std::get_if<bridge::msg::Resize>(&message)) {
        if (m_viewport && resize->viewport == *m_viewport)
            onResize(QSize(int(std::min(resize->width, bridge::kMaxDimension)),
                           int(std::min(resize->height, bridge::kMaxDimension))));
    } else if (const auto *pointer = std::get_if<bridge::msg::Pointer>(&message)) {
        if (m_viewport && pointer->viewport == *m_viewport)
            injectPointer(pointer->x, pointer->y, pointer->buttons);
    } else if (std::holds_alternative<bridge::msg::Quit>(message)) {
        m_quitting = true;
        m_renderTimer.stop();
        emit quitRequested();
    }
    // Volume, Scaling, Fps and Properties: ignored on purpose, see the class comment.
}

void HostOutput::onResize(QSize requested)
{
    const QSize size = clampedSize(requested);
    if (size == m_size || !m_viewport)
        return;
    if (!m_producer->resize(*m_viewport, uint32_t(size.width()), uint32_t(size.height()))) {
        qWarning("murasaki-surface: bridge producer: %s", m_producer->lastError().c_str());
        return;
    }
    m_size = size;
    m_window->resize(size);
    m_root->setSize(size);
    stayActive(kResizeGraceMs);
    requestRender();
}

void HostOutput::setCursorFraction(double x, double y)
{
    if (!m_window)
        return;
    const int px = std::clamp(int(std::lround(std::clamp(x, 0.0, 1.0) * m_size.width())), 0, m_size.width() - 1);
    const int py = std::clamp(int(std::lround(std::clamp(y, 0.0, 1.0) * m_size.height())), 0, m_size.height() - 1);
    if (qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG"))
        qInfo("murasaki.host: cursor %d,%d of %dx%d%s", px, py, m_size.width(), m_size.height(),
              m_interactive ? "" : " (content is not interactive)");
    injectPointer(px, py, m_buttons);
}

void HostOutput::injectPointerFraction(double x, double y, unsigned buttons, int wheelX, int wheelY)
{
    const std::optional<QPoint> pixel = pixelOfFraction(x, y, m_size);
    if (!m_window || !pixel)
        return;
    injectPointer(pixel->x(), pixel->y(), buttons & 7u);
    if (wheelX != 0 || wheelY != 0)
        injectWheel(pixel->x(), pixel->y(), wheelX, wheelY);
}

void HostOutput::injectWheel(int x, int y, int deltaX, int deltaY)
{
    if (!m_interactive || !m_window)
        return;
    const QPointF position(x, y);
    Qt::MouseButtons held;
    if (m_buttons & kButtonLeft)
        held |= Qt::LeftButton;
    if (m_buttons & kButtonRight)
        held |= Qt::RightButton;
    if (m_buttons & kButtonMiddle)
        held |= Qt::MiddleButton;
    QWheelEvent event(position, position, QPoint(), QPoint(deltaX, deltaY), held, Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventNotSynthesized, QPointingDevice::primaryPointingDevice());
    QCoreApplication::sendEvent(m_window, &event);
}

void HostOutput::injectPointer(int x, int y, quint32 buttons)
{
    if (!m_interactive || !m_window)
        return;
    const QPointF position(x, y);
    const QPointingDevice *device = QPointingDevice::primaryPointingDevice();
    const auto qtButtons = [](quint32 mask) {
        Qt::MouseButtons result;
        if (mask & kButtonLeft)
            result |= Qt::LeftButton;
        if (mask & kButtonRight)
            result |= Qt::RightButton;
        if (mask & kButtonMiddle)
            result |= Qt::MiddleButton;
        return result;
    };
    const Qt::MouseButtons before = qtButtons(m_buttons);
    const Qt::MouseButtons after = qtButtons(buttons);

    if (QPoint(x, y) != m_pointer) {
        const QPointF old = m_pointer.x() < 0 ? position : QPointF(m_pointer);
        QHoverEvent hover(QEvent::HoverMove, position, position, old, Qt::NoModifier, device);
        QCoreApplication::sendEvent(m_window, &hover);
        QMouseEvent move(QEvent::MouseMove, position, position, position, Qt::NoButton, before, Qt::NoModifier, device);
        QCoreApplication::sendEvent(m_window, &move);
        m_pointer = QPoint(x, y);
    }
    // One event per changed button, at the new position, in a fixed order (left, right, middle).
    Qt::MouseButtons state = before;
    for (const Qt::MouseButton button : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton}) {
        const bool was = before.testFlag(button);
        const bool now = after.testFlag(button);
        if (was == now)
            continue;
        state.setFlag(button, now);
        QMouseEvent event(now ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, position, position, position,
                          button, state, Qt::NoModifier, device);
        QCoreApplication::sendEvent(m_window, &event);
    }
    m_buttons = buttons;
}

} // namespace murasaki
