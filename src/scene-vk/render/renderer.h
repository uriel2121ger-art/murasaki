#pragma once

// What the engine loop asks of a renderer once per frame. The loop owns the command buffer, the fence and the
// hand-over of the target to the consumer; the renderer only records the drawing.

#include "core/vk.h"

#include "bridge/protocol.h"
#include "bridge/mediaview.h"
#include "bridge/spanview.h"
#include "media/mediastate.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace murasaki::scene {

struct FrameParams {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;     // the export image, already in COLOR_ATTACHMENT_OPTIMAL
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    double timeSeconds = 0.0;           // since the engine started
    // Ends the command buffer being recorded, sends it to the GPU and returns the next one to go on with (ChunkedSubmit::split).
    // Empty: one command buffer for the frame. A renderer calls it between draws (never inside a render pass) so that the GPU's
    // scheduler can run other clients' work in between; params.cb is only the first.
    std::function<VkCommandBuffer(VkCommandBuffer)> split;
};

class Renderer
{
public:
    virtual ~Renderer() = default;
    // Records all drawing into params.image. It must cover every pixel (the previous contents are undefined).
    virtual void render(const FrameParams &params) = 0;
    virtual void setScaling(bridge::msg::ScalingMode) {}
    // Spanning: this output shows a rectangle of a larger canvas (see bridge::msg::SpanView); disabled draws the whole
    // picture in the output.
    virtual void setSpanView(const bridge::msg::SpanView &) {}
    // The media the host follows: the player's state, and the cover of the track (RGBA8, straight alpha; no pixels: no cover).
    // Whether the scene shows the media cover or listens to the media events: only then does the host tell it what plays.
    virtual bool wantsMedia() const { return false; }
    virtual void setMedia(const MediaState &) {}
    virtual void setMediaThumbnail(uint32_t /*width*/, uint32_t /*height*/, std::span<const uint8_t> /*rgba*/) {}
    // The icon of the application the user chose for the property `name` (none: 0 x 0). The launch handler is what a script's
    // engine.openUserShortcut(name) calls: it sends the request to the host and says whether it was sent.
    virtual void setShortcutIcon(const std::string & /*name*/, uint32_t /*width*/, uint32_t /*height*/, std::span<const uint8_t> /*rgba*/) {}
    virtual void setShortcutLaunchHandler(std::function<bool(const std::string &)> /*handler*/) {}
    virtual void setPointer(int /*x*/, int /*y*/, unsigned /*buttons*/, VkExtent2D /*viewport*/) {}
    virtual void setVolume(unsigned /*bridgeVolume 0..128*/) {}
    virtual void setPaused(bool /*paused*/) {}
    // The frame rate the loop draws at now: what plays on its own (video textures) decodes no more pictures than that.
    virtual void setFps(int /*fps*/) {}
    // Target pixels per output pixel (see SceneRenderer::setRenderScale); renderers that draw at one size ignore it.
    virtual void setRenderScale(float /*scale*/) {}
    // False when a render() now would draw exactly what the last one drew (nothing changed and nothing depends on time):
    // the loop then keeps the last frame on screen instead of drawing it again.
    virtual bool needsFrame() const { return true; }
    // Called once the GPU finished the frame that render() recorded.
    virtual void frameCompleted() {}
    // GPU milliseconds of the last completed frame; negative when unknown.
    virtual double gpuFrameMs() const { return -1.0; }
};

} // namespace murasaki::scene
