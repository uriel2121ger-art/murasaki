#pragma once

#include "puppet/model.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// The open back end: an Inochi2D puppet, INP version 1 (docs/pets/puppets.md, section 12), read and evaluated by our own code. Untrusted input: every count and size is bounded by
// puppetlimits.h before it is used, every index is checked against its range, and a puppet that breaks a rule is refused whole (not repaired); only what the reference itself skips (a node
// of an unknown type, a binding whose node is missing) is skipped, and named in `warnings`.
struct TextureBlob {
    int encoding = 0;        // 0 PNG, 1 TGA (2, BC7, is refused: no decoder is bundled)
    size_t offset = 0;       // where the encoded image lies in the file kept by the model (OpenModel::textureBytes); decoding is the renderer's job
    size_t size = 0;
};

struct OpenModelInfo {
    std::string name;
    std::string version;
    bool preservePixels = false;      // sample textures with points, not linearly
    std::vector<TextureBlob> textures;
    std::vector<std::string> warnings;
    int physicsNodes = 0;             // SimplePhysics drivers found (read, not stepped yet)
    int partCount = 0;                // Part nodes (what a draw list can hold)
};

class OpenModel;

struct OpenLoadResult {
    std::unique_ptr<OpenModel> model;
    std::string error;                // empty when model is set; no file content in it
};

class OpenModel final : public ModelBackend
{
public:
    ~OpenModel() override;
    // From the bytes of a `.inp`. `error` explains a refusal. The second form takes the bytes over (the textures stay in them, as ranges: nothing is copied); the first copies.
    static OpenLoadResult load(std::string_view inp);
    static OpenLoadResult load(std::string &&inp);
    // From a file of an item folder, opened with openInItem (no link on the way, the last component included) and read up to kMaxInpBytes (a longer file is refused).
    static OpenLoadResult loadFile(const std::string &folder, std::string_view relative);
    // The encoded bytes of texture `index` (empty if there is no such texture); valid as long as the model lives.
    std::string_view textureBytes(int index) const;
    // The size of texture `index` as its header states it (PNG: the IHDR chunk; TGA: the 18 byte header), without decoding anything. False when there is no such texture, the bytes are gone (releaseFile) or
    // the header is too short or is not of the encoding the file names. A hostile header can lie; the decoder is what decides, within its own limits.
    bool textureSize(int index, uint32_t &width, uint32_t &height) const;
    // Lets go of the file's bytes (up to kMaxInpBytes): the textures are ranges of them, so after the textures are decoded nothing else needs them. textureBytes is empty and textureSize false from then on.
    void releaseFile();
    bool fileKept() const;

    const OpenModelInfo &info() const;
    std::string name() const override;
    const ParameterSet &parameters() const override;
    const PartSet &parts() const override;
    size_t drawableCount() const override;
    std::string drawableId(int drawable) const override; // the node's name
    const Shape &shape(int drawable) const override;
    Bounds bounds() const override;
    bool evaluate(const ParameterSet &values, const PartSet &parts, Frame &out) override;

    struct Impl; // the model's data; not for callers
    explicit OpenModel(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

// Adds up the textures' pixels from their headers, before any of them is decoded: false (with `error`) when a header cannot be read, one texture is over kMaxTexturePixels or the total is over
// kMaxTotalTexturePixels. A model whose textures decode to gigabytes is refused here, not after the first of them has already taken its share of memory.
bool textureBudgetOk(const OpenModel &model, std::string &error);

} // namespace murasaki::puppet
