#include "spriteitem.h"

#include <time.h>

#include "engine/fpscontrol.h" // the scene engine's adaptive frame rate, shared with it (its source is built into this library)

#include <QQuickWindow>
#include <QScreen>
#include <QSGImageNode>
#include <QSGTexture>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kUploadsPerFrame = 6; // textures created per rendered frame, so that a big pet does not stall one
constexpr int kWorkerNice = 10;


// Owns the textures. Lives on the render thread. The leaf that draws exists only while there is a texture to draw.
class SpriteRoot : public QSGNode
{
public:
    // `rect` is where the whole frame (frameSize pixels) goes; a texture that holds only part of it goes to its part of `rect`.
    void show(QQuickWindow *window, int frame, const QRectF &rect, bool draw, const QSize &frameSize)
    {
        if (textures.empty() || !draw) {
            if (leaf) {
                removeChildNode(leaf);
                delete leaf;
                leaf = nullptr;
            }
            return;
        }
        QRectF part = rect;
        const QSGTexture *texture = textures[size_t(frame)].get();
        if (size_t(frame) < places.size() && frameSize.width() > 0 && frameSize.height() > 0) {
            const QPoint at = places[size_t(frame)];
            const double sx = rect.width() / frameSize.width(), sy = rect.height() / frameSize.height();
            const QSize own = texture->textureSize();
            part = QRectF(rect.x() + at.x() * sx, rect.y() + at.y() * sy, own.width() * sx, own.height() * sy);
        }
        if (!leaf) {
            // Made by the window, so that every scene graph backend (also the software one) draws it.
            leaf = window->createImageNode();
            leaf->setFiltering(QSGTexture::Linear);
            leaf->setOwnsTexture(false);
            leaf->setTexture(textures[size_t(frame)].get());
            leaf->setRect(part);
            appendChildNode(leaf); // (the software backend needs the texture before the node is in the tree)
        }
        leaf->setTexture(textures[size_t(frame)].get());
        leaf->setRect(part);
    }

    std::vector<std::unique_ptr<QSGTexture>> textures; // the frames on screen
    std::vector<std::unique_ptr<QSGTexture>> incoming; // being uploaded
    std::vector<QPoint> places;                        // where each texture of `textures` sits in the frame (cropped frames), parallel to it
    std::vector<QPoint> incomingPlaces;
    QSGImageNode *leaf = nullptr;
};

} // namespace

// One thread for all pets: they decode one after the other, at a low priority.
QThreadPool &SpriteItem::workerPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool;
        created->setMaxThreadCount(1);
        created->setThreadPriority(QThread::LowPriority);
        return created;
    }();
    return *pool;
}

struct SpriteItem::Link {
    std::mutex mutex;
    SpriteItem *item = nullptr; // null once the item is gone

    // Posts to the item's thread under the lock: the item can not be destroyed in between.
    template<typename F>
    void post(F &&function)
    {
        std::lock_guard lock(mutex);
        if (item)
            QMetaObject::invokeMethod(item, std::forward<F>(function), Qt::QueuedConnection);
    }
};

SpriteItem::SpriteItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_link(std::make_shared<Link>())
{
    m_link->item = this;
    setFlag(ItemHasContents, true);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SpriteItem::tick);
    m_wall.start();
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        m_wallStartNs = qint64(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }
}

SpriteItem::~SpriteItem()
{
    cancelDecoding();
    std::lock_guard lock(m_link->mutex);
    m_link->item = nullptr;
}

void SpriteItem::cancelDecoding()
{
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
}

// A wrap was announced for a time that no longer holds (the clock stopped, or its timing changed): say so, and let the next tick announce it again.
void SpriteItem::cancelWrapAnnouncement()
{
    const qint64 counted = m_frames.loops(nowMs());
    if (m_loopAnnounced > counted) {
        m_loopAnnounced = counted;
        emit wrapCancelled();
    }
}

void SpriteItem::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    if (m_paused) {
        m_frames.pause(nowMs());
        cancelWrapAnnouncement();
        m_timer.stop();
    } else {
        m_frames.resume(nowMs());
        scheduleNext();
    }
    emit pausedChanged();
}

void SpriteItem::setTimingFps(int fps)
{
    if (fps == m_frames.timing().fps)
        return;
    PetTiming timing = m_frames.timing();
    timing.fps = fps;
    if (fps > 0)
        timing.speed = 1;
    m_frames.setTiming(timing, nowMs());
    cancelWrapAnnouncement();
    emit timingChanged();
    scheduleNext();
}

void SpriteItem::setTimingSpeed(double speed)
{
    if (speed == m_frames.timing().speed)
        return;
    PetTiming timing = m_frames.timing();
    timing.speed = speed;
    if (speed != 1)
        timing.fps = 0;
    m_frames.setTiming(timing, nowMs());
    cancelWrapAnnouncement();
    emit timingChanged();
    scheduleNext();
}

void SpriteItem::setFpsCap(int fps)
{
    fps = std::max(0, fps);
    if (fps == m_fpsCap)
        return;
    m_fpsCap = fps;
    m_adaptive.reset(); // its ladder starts at the new cap
    applyEffectiveCap();
    emit fpsCapChanged();
    scheduleNext();
}

// The cap in force is the outside one, lowered to the adaptive controller's rate once that has stepped down.
void SpriteItem::applyEffectiveCap()
{
    int cap = m_fpsCap;
    if (m_adaptive && m_adaptive->current() < m_adaptive->requested())
        cap = cap > 0 ? std::min(cap, m_adaptive->current()) : m_adaptive->current();
    m_frames.setCap(cap);
}

// Shows the frame the clock says is due now and sets the timer for the next change. The pets' one source of timing: a
// timer that fires late (the machine is busy) only shows a later frame, it never stretches the animation.
void SpriteItem::scheduleNext()
{
    if (m_paused || m_durations.size() < 2) {
        m_timer.stop();
        return;
    }
    const double now = nowMs();
    const auto step = m_frames.update(now);
    if (step.frame != m_frame) {
        m_frame = step.frame;
        update(); // the only redraw an unchanging pet asks for
        emit frameChanged();
    }
    // The wraps, for the sound of the pet: the latest one we have not told about (the start, or one a stall made us miss), then the next one when it is
    // near or no tick will come before it.
    constexpr double kWrapLookaheadMs = 150;
    if (m_loopAnnounced < step.loop) {
        m_loopAnnounced = step.loop;
        emit wrapped(wrapNs(step.loop), step.loop);
    }
    if (m_loopAnnounced == step.loop) {
        const double next = m_frames.nextWrapMs(now);
        if (next >= 0 && (next - now <= kWrapLookaheadMs || step.dueMs < 0 || step.dueMs >= next - 1)) {
            m_loopAnnounced = step.loop + 1;
            emit wrapped(wrapNs(step.loop + 1), step.loop + 1);
        }
    }
    if (step.dueMs < 0) {
        m_timer.stop();
        return;
    }
    m_dueMs = step.dueMs;
    m_timer.start(int(std::max(1.0, std::ceil(step.dueMs - nowMs())))); // (not `now`: the signals above may have taken a while)
}

void SpriteItem::tick()
{
    if (m_durations.size() < 2)
        return;
    if (m_dueMs >= 0)
        noteLateness(std::max(0.0, nowMs() - m_dueMs));
    scheduleNext();
}

// How late the timer fired is the load the machine is under; the controller of the scene engine turns a load that
// stays high into a lower rate, and back up when it eases.
void SpriteItem::noteLateness(double lateMs)
{
    if (!m_adaptive) {
        const QScreen *screen = window() ? window()->screen() : nullptr;
        const int refresh = screen ? int(std::lround(screen->refreshRate())) : 60;
        // a timer's lateness is this process's own (no other GPU client's wait in it): the shorter window of the first controller
        scene::FpsController::Config config;
        config.downFrames = 24;
        m_adaptive = std::make_unique<scene::FpsController>(m_fpsCap > 0 ? m_fpsCap : std::clamp(refresh, 20, 240), config);
    }
    // One stall (the session slept, the process was stopped) must not count as a whole window of load: at most four intervals.
    lateMs = std::min(lateMs, 4000.0 / std::max(1, m_adaptive->current()));
    if (m_adaptive->update(lateMs))
        applyEffectiveCap();
}

void SpriteItem::request(const QString &file, int width, int height, double maxBytes, int fps, double maskBudget)
{
    cancelDecoding();
    const quint64 generation = ++m_generation;
    m_incomingMasks.reset();
    m_incoming = {};
    m_uploaded = 0;
    m_resetIncoming = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const QSize size(width, height);
    const qint64 limit = qint64(maxBytes);
    const qint64 maskBytes = qint64(maskBudget);
    const std::shared_ptr<Link> link = m_link;
    workerPool().start([=] {
        if (cancel->load())
            return; // cancelled while it waited its turn
        // A thread of its own (not the process): lowering its nice value is allowed to everyone.
        setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), kWorkerNice);
        const SpriteDecoder::MaskRequest maskRequest{std::size_t(std::max<qint64>(0, maskBytes)), true};
        // (the margins of a pet cost memory and nothing on screen: the frames are cut as they are decoded, and the budget counts what is kept)
        SpriteDecoder::Result result = SpriteDecoder::decode(file, size, limit, cancel.get(), fps, maskBytes > 0 ? &maskRequest : nullptr, true);
        if (cancel->load())
            return;
        link->post([link, generation, result = std::move(result)]() mutable {
            // Runs on the item's thread; the item is alive or this event was dropped with it.
            if (link->item)
                link->item->onDecoded(generation, std::move(result));
        });
    });
}

int SpriteItem::pendingCpuFrames() const
{
    int held = 0;
    for (const QImage &image : m_incoming.images)
        held += image.isNull() ? 0 : 1;
    return held;
}

void SpriteItem::restart()
{
    if (!m_hasFrames || m_durations.isEmpty())
        return;
    cancelWrapAnnouncement();
    m_frames.setFrames(m_durations, nowMs());
    m_frame = 0;
    scheduleNext();
    update();
    emit frameChanged();
}

void SpriteItem::setDrawn(bool drawn)
{
    if (drawn == m_drawn)
        return;
    m_drawn = drawn;
    update();
    emit drawnChanged();
}

void SpriteItem::clear()
{
    cancelDecoding();
    m_settledGeneration = ++m_generation;
    m_incoming = {};
    m_uploaded = 0;
    m_resetIncoming = true;
    m_drop = true;
    m_durations.clear();
    m_frames.setFrames({}, nowMs());
    cancelWrapAnnouncement();
    m_loopAnnounced = -1;
    m_frame = 0;
    m_cacheBytes = 0;
    m_incomingMasks.reset();
    m_masks.reset();
    m_timer.stop();
    update();
    m_hasFrames = false;
    if (m_hasFramesReported) {
        m_hasFramesReported = false;
        emit hasFramesChanged();
    }
}

void SpriteItem::onDecoded(quint64 generation, SpriteDecoder::Result result)
{
    if (generation != m_generation || result.cancelled)
        return;
    if (!result.ok) {
        const QString reason = result.error;
        clear();
        m_settledGeneration = generation;
        emit failed(reason);
        return;
    }
    m_incomingMasks = result.masks.isNull() ? nullptr : std::make_shared<const MaskSet>(std::move(result.masks));
    adopt(std::move(result.frames));
}

void SpriteItem::showFrames(SpriteFrames frames)
{
    cancelDecoding();
    ++m_generation;
    m_incomingMasks.reset();
    adopt(std::move(frames));
}

void SpriteItem::adopt(SpriteFrames frames)
{
    if (!frames.consistent()) { // the places of the images do not match the images: nothing is shown rather than a cut picture stretched over the frame
        qWarning("murasaki-overlay: frames with %lld places for %lld images are refused", qlonglong(frames.offsets.size()), qlonglong(frames.images.size()));
        return;
    }
    m_incoming = std::move(frames);
    m_uploaded = 0;
    m_resetIncoming = true;
    update();
}

void SpriteItem::afterUpload(quint64 generation)
{
    if (generation == m_generation) { // the frames of the last request are on screen: so are their masks, indexed the same way
        m_masks = std::move(m_incomingMasks);
        m_incomingMasks.reset();
    }
    scheduleNext();
    emit frameChanged();
    if (!m_hasFramesReported) {
        m_hasFramesReported = true;
        emit hasFramesChanged();
    }
    if (generation == m_generation) {
        m_settledGeneration = generation;
        emit loaded(double(m_cacheBytes));
    }
}

QSGNode *SpriteItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<SpriteRoot *>(old);
    if (!node)
        node = new SpriteRoot;
    if (m_drop) {
        node->textures.clear();
        node->places.clear();
        node->incoming.clear();
        node->incomingPlaces.clear();
        m_drop = false;
    }
    if (m_resetIncoming) {
        node->incoming.clear();
        node->incomingPlaces.clear();
        m_resetIncoming = false;
    }
    // Upload a few of the new frames. The old ones play until all are in.
    if (QQuickWindow *win = window(); win && m_uploaded < m_incoming.images.size()) {
        for (int n = 0; n < kUploadsPerFrame && m_uploaded < m_incoming.images.size(); ++n, ++m_uploaded) {
            node->incoming.emplace_back(
                win->createTextureFromImage(m_incoming.images.at(m_uploaded), QQuickWindow::TextureHasAlphaChannel));
            if (m_uploaded < m_incoming.offsets.size()) // (frames that were cut down; whole frames have no place to keep)
                node->incomingPlaces.push_back(m_incoming.offsets.at(m_uploaded));
            m_incoming.images[m_uploaded] = QImage(); // the CPU copy goes as soon as the GPU has it
        }
        if (m_uploaded == m_incoming.images.size()) {
            node->textures = std::move(node->incoming);
            node->places = std::move(node->incomingPlaces);
            node->incoming.clear();
            node->incomingPlaces.clear();
            m_durations = m_incoming.durationsMs;
            cancelWrapAnnouncement();
            m_frames.setFrames(m_durations, nowMs());
            m_loopAnnounced = m_skipFirstWrap ? 0 : -1; // (the loop 0 the video announced is not announced again)
            m_skipFirstWrap = false;
            m_cacheBytes = m_incoming.bytes;
            m_frameSize = m_incoming.size;
            m_frame = 0;
            m_hasFrames = true;
            m_incoming = {};
            m_uploaded = 0;
            const quint64 generation = m_generation;
            QMetaObject::invokeMethod(this, [this, generation] { afterUpload(generation); }, Qt::QueuedConnection);
        } else {
            update(); // the rest follows with the next frame
        }
    }
    node->show(window(), node->textures.empty() ? 0 : std::min<int>(m_frame, int(node->textures.size()) - 1), boundingRect(), m_drawn, m_frameSize);
    return node;
}

} // namespace murasaki
