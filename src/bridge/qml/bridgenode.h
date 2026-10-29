#pragma once

#include "bridgeclient.h"
#include "dmabufimport.h"

#include <QPointer>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace murasaki {


// Releases that wait for the frame which replaced them to reach the screen. Pushed by the render thread while it
// attaches a frame, flushed from the window's frameSwapped signal (render thread or GUI thread depending on the
// scene graph loop), so it has its own mutex.
struct BridgeReleaseState {
    std::mutex mutex;
    std::vector<std::pair<std::shared_ptr<BridgeFrameQueue>, BridgeFrame>> awaiting;

    void push(std::shared_ptr<BridgeFrameQueue> queue, BridgeFrame frame)
    {
        std::lock_guard lock(mutex);
        awaiting.emplace_back(std::move(queue), std::move(frame));
    }
    void flush()
    {
        std::vector<std::pair<std::shared_ptr<BridgeFrameQueue>, BridgeFrame>> ready;
        {
            std::lock_guard lock(mutex);
            ready.swap(awaiting);
        }
        for (auto &[queue, frame] : ready)
            queue->release(frame);
    }
};

// Everything the render thread owns for one buffer set: imported dma-bufs or the upload texture.
struct BridgeCache {
    struct Slot {
        ImportedDmabuf imported;
        QSGTexture *texture = nullptr;
        bool failed = false;
    };

    std::shared_ptr<const BridgeBuffers> buffers;
    Slot entries[bridge::kMaxBuffers];
    uint shmTexture = 0;
    QSize shmSize;
    QSGTexture *shmWrapper = nullptr;
    bool attached = false; // the node has shown a texture of this cache
};

// The scene node of BridgeItem. Created, updated and destroyed on the render thread, with the OpenGL context
// current. It imports the frames of a BridgeFrameQueue (dma-buf or shared memory) and shows them as a texture.
class BridgeNode : public QSGSimpleTextureNode
{
public:
    BridgeNode(QQuickWindow *window, std::shared_ptr<BridgeReleaseState> releases, std::function<void()> contextLost);
    ~BridgeNode() override;

    bool hasTexture() const { return texture() != nullptr; }
    // True when this call put a new frame on the texture.
    bool update(const std::shared_ptr<BridgeFrameQueue> &queue, const QRectF &rect);

private:
    bool attach(const BridgeFrame &frame);
    QSGTexture *importDmabuf(const BridgeFrame &frame);
    QSGTexture *uploadShm(const BridgeFrame &frame);

    QPointer<QQuickWindow> m_window;
    std::shared_ptr<BridgeReleaseState> m_releases;
    std::function<void()> m_contextLost;
    int m_badFrames = 0;
    bool m_contextLostReported = false;
    std::shared_ptr<BridgeFrameQueue> m_queue;
    DmabufImporter m_importer;
    bool m_importerTried = false;
    BridgeCache m_current;
    BridgeCache m_previous;
    // The dma-buf frame on screen (null queue: none); released once its successor has been swapped in.
    std::shared_ptr<BridgeFrameQueue> m_shownQueue;
    BridgeFrame m_shownFrame;
};

} // namespace murasaki
