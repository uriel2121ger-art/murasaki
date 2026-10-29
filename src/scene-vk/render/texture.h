#pragma once

// A Wallpaper Engine texture on the GPU: all the mips the file carries (or a generated chain when it carries one),
// with the sampler settings its flags ask for.

#include "assets/tex.h"
#include "core/context.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

class GpuTexture
{
public:
    // Uploads `tex` (which must have pixels). Fails (null + `error`) when the device cannot sample the format.
    static std::unique_ptr<GpuTexture> create(const VkContext &ctx, const Tex &tex, std::string &error);

    // A texture the renderer rewrites as a video plays: RGBA8, one mip, linear. `update` copies a width x height x 4 picture into it
    // (synchronously, with a command buffer of its own: call it before the frame's own is recorded).
    static std::unique_ptr<GpuTexture> createDynamic(const VkContext &ctx, uint32_t width, uint32_t height, bool nearest, bool clamp, std::string &error);
    void update(const VkContext &ctx, const uint8_t *rgba);

    // A dynamic texture drawn into by the GPU instead of written from the CPU (a video picture converted from the decoder's surface):
    // `before` (optional) is recorded first, outside the render pass (barriers of the images the draw reads); `draw` records the draws of one render pass into the texture (the image is a colour attachment inside it) and the texture is
    // sampled-ready afterwards. Synchronous, with a command buffer of its own like `update`.
    void drawInto(const VkContext &ctx, const std::function<void(VkCommandBuffer)> &draw,
                  const std::function<void(VkCommandBuffer)> &before = {});
    VkFormat format() const { return VK_FORMAT_R8G8B8A8_UNORM; }

    VkImageView view() const { return m_image.view(); }
    VkImage image() const { return m_image.handle(); }
    uint32_t width() const { return m_image.width(); }
    uint32_t height() const { return m_image.height(); }
    uint32_t mipLevels() const { return m_image.mipLevels(); }
    // The images of a sprite sheet: the first (view(), width(), height()) and the pages after it. A frame names the one it lies on
    // (TexFrame::index); a number past the last is the last. A plain picture has one.
    size_t pageCount() const { return 1 + m_pages.size(); }
    VkImageView pageView(size_t page) const { return page == 0 || m_pages.empty() ? m_image.view() : m_pages[std::min(page, m_pages.size()) - 1].view(); }
    uint32_t pageWidth(size_t page) const { return page == 0 || m_pages.empty() ? m_image.width() : m_pages[std::min(page, m_pages.size()) - 1].width(); }
    uint32_t pageHeight(size_t page) const { return page == 0 || m_pages.empty() ? m_image.height() : m_pages[std::min(page, m_pages.size()) - 1].height(); }
    // Images of the sheet the file has and this texture does not (they would have taken it over the size limit).
    uint32_t droppedPages() const { return m_droppedPages; }
    uint32_t imageWidth() const { return m_imageWidth; }   // the picture inside the storage
    uint32_t imageHeight() const { return m_imageHeight; }
    // The picture's share of the storage: shaders sample u in [0, uMax], v in [0, vMax].
    float uMax() const { return m_uMax; }
    float vMax() const { return m_vMax; }
    // Sprite-sheet frames (TEXS), empty for a plain picture, and the sheet's size in texels.
    const std::vector<TexFrame> &frames() const { return m_frames; }
    // The file's format code (WE's numbering: 0 RGBA8888 ... 9 R8 ... 12 BC7), which shaders ask for as TEX<n>FORMAT.
    int32_t rawFormat() const { return m_rawFormat; }
    bool nearest() const { return m_nearest; }
    bool clamp() const { return m_clamp; }

private:
    GpuImage m_image;
    std::vector<GpuImage> m_pages;  // sprite sheets: images 1 and on
    uint32_t m_droppedPages = 0;
    GpuBuffer m_staging;            // dynamic textures: the picture on its way in
    bool m_written = false;         // dynamic textures: the image has a layout to come from
    std::vector<TexFrame> m_frames;
    int32_t m_rawFormat = 0;
    uint32_t m_imageWidth = 0;
    uint32_t m_imageHeight = 0;
    float m_uMax = 1.0f;
    float m_vMax = 1.0f;
    bool m_nearest = false;
    bool m_clamp = false;
};

VkFormat vulkanFormat(PixelFormat format);

} // namespace murasaki::scene
