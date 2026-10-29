#pragma once

#include "puppet/model.h"
#include "puppet/view.h"

#include "core/context.h"

#include <memory>
#include <string>
#include <vector>

namespace murasaki::puppet {

// The Vulkan renderer of the puppet engine (docs/pets/puppets.md, section 19). It draws the draw list of a ModelBackend frame - meshes with a texture, a tint, a screen tint and an opacity, the blend modes of
// the Inochi2D reference, stencil masks (Mask and DodgeMask) and composite groups - into an RGBA8 image with **premultiplied alpha**, the contract of the puppet producer. Textures are expected premultiplied
// already (premultiplyInPlace). Nothing here talks to the bridge or reads a file.

// A decoded, premultiplied texture.
struct TextureImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // premultiplied, top row first
};

// Decodes a PNG or TGA (any of the formats the bounded decoder reads) within the limits of puppetlimits.h and premultiplies it. False (with `error`) when it is unreadable or over a limit.
bool decodeTexture(const uint8_t *bytes, size_t size, TextureImage &out, std::string &error);
void premultiplyInPlace(std::vector<uint8_t> &rgba);

// Why PuppetRenderer::create said no: the device lacks something (or Vulkan failed), or this model does not fit the device (a texture that does not fit its memory): the first is "no GPU", the second a
// refused model.
enum class CreateFailure { None, Device, Model };

class PuppetRenderer
{
public:
    // `model` gives the shapes (uvs and indices) and the number of drawables; `textures` are the model's, in order (a draw item names one by index). `pointSampling`: sample the textures with points,
    // not linearly (meta.preservePixels). Null with `error` when the device lacks what is needed (a depth-stencil format, for the masks).
    static std::unique_ptr<PuppetRenderer> create(scene::VkContext &ctx, const ModelBackend &model, const std::vector<TextureImage> &textures, bool pointSampling, std::string &error,
                                                  CreateFailure *failure = nullptr);
    ~PuppetRenderer();
    PuppetRenderer(const PuppetRenderer &) = delete;
    PuppetRenderer &operator=(const PuppetRenderer &) = delete;

    // Records the commands that draw `frame` into `target` (an RGBA8 view whose image is in COLOR_ATTACHMENT_OPTIMAL, `extent` big), clearing it to transparent first. The image is left in the same
    // layout. The caller waits for the commands before the next call (one frame in flight). Returns the number of meshes drawn. The buffer of vertex positions grows to what the frame needs (a mask source is
    // written again for each item that uses it), up to kMaxPositionVertices; a frame that needs more draws nothing (it only clears) and counts every item as skipped.
    uint32_t cmdDraw(VkCommandBuffer cb, VkImageView target, VkExtent2D extent, const Frame &frame, const ViewTransform &view);

    // Counters of the last cmdDraw, for tests and the engine's statistics.
    struct Stats {
        uint32_t meshes = 0;
        uint32_t groups = 0;
        uint32_t maskDraws = 0;
        uint32_t skipped = 0; // items that could not be drawn (unknown drawable, positions of the wrong size, out of room), and mask sources that could not be drawn for an item that uses them
    };
    const Stats &stats() const { return m_stats; }

private:
    struct Impl;
    explicit PuppetRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
    Stats m_stats;
};

} // namespace murasaki::puppet
