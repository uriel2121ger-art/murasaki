#pragma once

// A video picture from the decoder's own GPU surface into the RGBA texture the scene samples, without a trip through the CPU.
//
// The decoder (assets/videoplugin.cpp) exports the surface it decoded into as a dma-buf (assets/videoapi.h). This imports it as a
// Vulkan image (VK_EXT_external_memory_dma_buf with the buffer's DRM format modifier), samples it through a sampler YCbCr conversion
// (the hardware does the colour matrix, the range and the chroma upsampling) and draws it into the video's dynamic texture with one
// full-screen triangle. The imports are kept per buffer, so a surface the decoder reuses costs nothing the second time.
// Nothing here can crash a scene: every step that can fail says why (once) and the caller goes on with the copy path.

#include "assets/videoapi.h"
#include "core/context.h"
#include "render/texture.h"

#include <memory>
#include <string>

namespace murasaki::scene {

class VideoConverter
{
public:
    // Null, with `why`, when this device or driver cannot do it (no dma-buf import, no sampler YCbCr conversion, no dynamic rendering).
    static std::unique_ptr<VideoConverter> create(const VkContext &ctx, std::string &why);
    ~VideoConverter();
    VideoConverter(const VideoConverter &) = delete;
    VideoConverter &operator=(const VideoConverter &) = delete;

    // Draws the picture into `target` (RGBA8). Takes the descriptor in `picture` (closes it, whatever happens). False, with `why`, when this
    // picture cannot be converted this way (the next picture may be; a format the driver refuses is refused every time).
    bool convert(const MurasakiVideoExport &picture, GpuTexture &target, std::string &why);

    // Number of imported buffers kept (the tests look at it).
    size_t importedCount() const;

private:
    struct Impl;
    explicit VideoConverter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace murasaki::scene
