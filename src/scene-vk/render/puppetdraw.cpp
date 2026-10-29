#include "render/puppetdraw.h"

#include <algorithm>
#include <iterator>

namespace murasaki::scene {

namespace {

const GeometryAttribute kAttributes[] = {
    {"a_Position", VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"a_Normal", VK_FORMAT_R32G32B32_SFLOAT, 12},
    {"a_Tangent4", VK_FORMAT_R32G32B32A32_SFLOAT, 24},
    {"a_BlendIndices", VK_FORMAT_R32G32B32A32_UINT, 40},
    {"a_BlendWeights", VK_FORMAT_R32G32B32A32_SFLOAT, 56},
    {"a_TexCoord", VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(kPuppetTexCoordOffset)},
};

// a 3D mesh object's vertex (puppet/mdl.h, MeshPart)
const GeometryAttribute kMeshAttributes[] = {
    {"a_Position", VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"a_Normal", VK_FORMAT_R32G32B32_SFLOAT, 12},
    {"a_Tangent4", VK_FORMAT_R32G32B32A32_SFLOAT, 24},
    {"a_TexCoord", VK_FORMAT_R32G32_SFLOAT, 40},
};

} // namespace

const GeometryAttribute *meshAttributes(uint32_t &count)
{
    count = static_cast<uint32_t>(std::size(kMeshAttributes));
    return kMeshAttributes;
}

bool meshAttributeKnown(const std::string &name)
{
    return std::any_of(std::begin(kMeshAttributes), std::end(kMeshAttributes), [&](const GeometryAttribute &a) { return name == a.name; });
}

bool puppetSkinned(const ImageLayer &layer)
{
    // a puppet with spring bones is skinned even without animation layers: the bones sway about the bind pose
    return layer.puppet && !layer.puppet->bones.empty() && layer.puppet->usedBones > 0
        && ((!layer.animationLayers.empty() && !layer.puppet->animations.empty()) || (layer.puppet->simulatedBones > 0 && layer.bonePhysics));
}

MaterialPass puppetPass(const ImageLayer &layer)
{
    MaterialPass pass = layer.base;
    if (puppetSkinned(layer)) {
        pass.combos["SKINNING"] = 1;
        pass.combos["BONECOUNT"] = static_cast<int>(layer.puppet->boneSlots); // the bones the vertices name: g_Bones holds those
    }
    return pass;
}

const GeometryAttribute *puppetAttributes(uint32_t &count)
{
    count = static_cast<uint32_t>(std::size(kAttributes));
    return kAttributes;
}

bool puppetAttributeKnown(const std::string &name)
{
    return std::any_of(std::begin(kAttributes), std::end(kAttributes), [&](const GeometryAttribute &a) { return name == a.name; });
}

std::vector<PuppetLayerState> puppetLayerStates(const ImageLayer &layer, const PropertyStore &props)
{
    return puppetLayerStates(layer.animationLayers, props);
}

std::vector<PuppetLayerState> puppetLayerStates(const std::vector<AnimationLayer> &layers, const PropertyStore &props)
{
    std::vector<PuppetLayerState> out;
    puppetLayerStates(layers, props, out);
    return out;
}

void puppetLayerStates(const std::vector<AnimationLayer> &layers, const PropertyStore &props, std::vector<PuppetLayerState> &out)
{
    out.clear();
    for (const AnimationLayer &a : layers) {
        if (!resolve(a.visible, props)) {
            continue;
        }
        out.push_back({a.animation, resolve(a.blend, props), resolve(a.rate, props), a.additive, a.frameOffset});
    }
}

} // namespace murasaki::scene
