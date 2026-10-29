#include "bridgenode.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QtQuick/qsgtexture_platform.h>

namespace murasaki {

using namespace bridge;

namespace {

// True when the thread's current OpenGL context is the one the scene graph renders with. The render thread has
// it current while it syncs and deletes nodes; if it is not, importing would bind textures into a foreign context.
bool renderContextCurrent(QQuickWindow *window)
{
    QOpenGLContext *current = QOpenGLContext::currentContext();
    if (!current)
        return false;
    QSGRendererInterface *ri = window ? window->rendererInterface() : nullptr;
    auto *expected = ri ? static_cast<QOpenGLContext *>(ri->getResource(window, QSGRendererInterface::OpenGLContextResource))
                        : nullptr;
    return !expected || expected == current;
}

constexpr int kMaxBadFrames = 30; // consecutive frames without the right context before the item gives up

void destroyCache(const DmabufImporter &importer, BridgeCache &cache)
{
    if (!cache.buffers)
        return;
    for (auto &slot : cache.entries) {
        delete slot.texture;
        importer.destroy(slot.imported);
        slot = {};
    }
    delete cache.shmWrapper;
    if (cache.shmTexture) {
        const GLuint texture = cache.shmTexture;
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(1, &texture);
    }
    cache = BridgeCache();
}

// Frees GL resources of a destroyed node on the render thread, in the window's own context.
class CleanupJob : public QRunnable
{
public:
    CleanupJob(QQuickWindow *window, DmabufImporter importer, BridgeCache a, BridgeCache b)
        : m_window(window), m_importer(importer), m_a(std::move(a)), m_b(std::move(b))
    {
    }
    void run() override
    {
        if (!renderContextCurrent(m_window.data()))
            return; // nothing sensible left to do; the context is gone and takes its objects with it
        destroyCache(m_importer, m_a);
        destroyCache(m_importer, m_b);
    }

private:
    QPointer<QQuickWindow> m_window;
    DmabufImporter m_importer;
    BridgeCache m_a;
    BridgeCache m_b;
};

} // namespace

BridgeNode::BridgeNode(QQuickWindow *window, std::shared_ptr<BridgeReleaseState> releases, std::function<void()> contextLost)
    : m_window(window)
    , m_releases(std::move(releases))
    , m_contextLost(std::move(contextLost))
{
    setOwnsTexture(false);
    setFiltering(QSGTexture::Linear);
}

BridgeNode::~BridgeNode()
{
    if (renderContextCurrent(m_window.data())) {
        destroyCache(m_importer, m_current);
        destroyCache(m_importer, m_previous);
        return;
    }
    // Not the scene graph's context: deleting GL names here would hit the wrong context. Hand the resources to
    // a render job, which the window runs with its own context current.
    if (!m_current.buffers && !m_previous.buffers)
        return; // nothing was ever imported (a texture-less node, or a backend that is not OpenGL)
    qWarning("BridgeItem: render context not current while destroying the scene node; deferring GL cleanup");
    if (QQuickWindow *window = m_window.data()) {
        window->scheduleRenderJob(new CleanupJob(window, m_importer, std::move(m_current), std::move(m_previous)),
                                    QQuickWindow::NoStage);
    }
}

bool BridgeNode::update(const std::shared_ptr<BridgeFrameQueue> &queue, const QRectF &rect)
{
    if (queue != m_queue) {
        // A restarted host gets a new queue: a context problem that persists must be reported again.
        m_badFrames = 0;
        m_contextLostReported = false;
    }
    m_queue = queue;
    setRect(rect);
    if (!m_queue)
        return false;
    if (auto frame = m_queue->takeFrame())
        return attach(*frame);
    return false;
}

bool BridgeNode::attach(const BridgeFrame &frame)
{
    if (m_queue->buffers() != frame.buffers)
        return false; // superseded while it travelled; the producer ignores the stale release anyway
    if (!renderContextCurrent(m_window.data())) {
        m_queue->release(frame);
        if (++m_badFrames >= kMaxBadFrames && !m_contextLostReported) {
            m_contextLostReported = true;
            if (m_contextLost)
                m_contextLost();
        }
        return false;
    }
    m_badFrames = 0;
    if (frame.buffers != m_current.buffers) {
        // Keep the cache that backs the texture on screen until the new set shows its first frame.
        if (m_current.attached) {
            destroyCache(m_importer, m_previous);
            m_previous = std::move(m_current);
        } else {
            destroyCache(m_importer, m_current);
        }
        m_current = BridgeCache();
        m_current.buffers = frame.buffers;
    }
    const bool shm = frame.buffers->kind == BridgeBuffers::Kind::Shm;
    QSGTexture *texture = shm ? uploadShm(frame) : importDmabuf(frame);
    QQuickOpenGLUtils::resetOpenGLState();
    if (!texture) {
        if (!shm)
            m_queue->reportImportFailure(frame.generation());
        m_queue->release(frame); // it will never be shown
        return false;
    }
    setTexture(texture);
    setTextureCoordinatesTransform(frame.flipY ? QSGSimpleTextureNode::MirrorVertically
                                               : QSGSimpleTextureNode::NoTransform);
    m_current.attached = true;
    destroyCache(m_importer, m_previous);
    if (m_shownQueue)
        m_releases->push(std::move(m_shownQueue), std::move(m_shownFrame));
    m_shownQueue.reset();
    m_shownFrame = {};
    if (shm) {
        m_queue->release(frame); // the pixels were copied into the texture
    } else {
        m_shownQueue = m_queue;
        m_shownFrame = frame;
    }
    return true;
}

QSGTexture *BridgeNode::importDmabuf(const BridgeFrame &frame)
{
    const BridgeBuffers &set = *frame.buffers;
    BridgeCache::Slot &slot = m_current.entries[frame.buffer];
    if (slot.texture)
        return slot.texture;
    if (slot.failed)
        return nullptr;
    if (!m_importerTried) {
        m_importerTried = true;
        std::string error;
        if (!m_importer.init(&error))
            qWarning("BridgeItem: dma-buf import unavailable: %s", error.c_str());
    }
    if (!m_importer.ready()) {
        slot.failed = true;
        return nullptr;
    }
    DmabufDescription d;
    d.width = set.width;
    d.height = set.height;
    d.fourcc = set.fourcc;
    d.modifier = set.modifier;
    d.planeCount = set.planes;
    for (uint32_t p = 0; p < set.planes; ++p)
        d.planes[p] = {set.dmabufFd(frame.buffer, p), set.offset[frame.buffer][p], set.stride[frame.buffer][p]};
    std::string error;
    auto imported = m_importer.import(d, &error);
    if (!imported) {
        qWarning("BridgeItem: dma-buf import failed: %s", error.c_str());
        slot.failed = true;
        return nullptr;
    }
    slot.imported = *imported;
    m_queue->reportImportSuccess(set.generation);
    slot.texture = QNativeInterface::QSGOpenGLTexture::fromNative(
        slot.imported.texture, m_window.data(), QSize(static_cast<int>(set.width), static_cast<int>(set.height)));
    return slot.texture;
}

QSGTexture *BridgeNode::uploadShm(const BridgeFrame &frame)
{
    const BridgeBuffers &set = *frame.buffers;
    const auto pixels = set.ring->frame(frame.buffer);
    if (pixels.empty())
        return nullptr;
    QOpenGLContext *context = QOpenGLContext::currentContext();
    QOpenGLFunctions *gl = context->functions();
    const int width = static_cast<int>(set.width);
    const int height = static_cast<int>(set.height);
    const uint32_t stride = set.ring->geometry().stride;
    BridgeCache &cache = m_current;
    if (!cache.shmTexture) {
        GLuint texture = 0;
        gl->glGenTextures(1, &texture);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        cache.shmTexture = texture;
        cache.shmSize = QSize(width, height);
        cache.shmWrapper = QNativeInterface::QSGOpenGLTexture::fromNative(texture, m_window.data(), cache.shmSize);
    }
    gl->glBindTexture(GL_TEXTURE_2D, cache.shmTexture);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool tight = stride == set.width * 4;
#ifdef GL_UNPACK_ROW_LENGTH
    const bool rowLength = !context->isOpenGLES() || context->format().majorVersion() >= 3;
#else
    const bool rowLength = false;
#endif
    if (tight) {
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
#ifdef GL_UNPACK_ROW_LENGTH
    else if (rowLength) {
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
#endif
    else {
        for (int y = 0; y < height; ++y)
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                pixels.data() + static_cast<size_t>(y) * stride);
    }
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    return cache.shmWrapper;
}

} // namespace murasaki
