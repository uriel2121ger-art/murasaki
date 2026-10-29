#pragma once

// What drawing a puppet layer needs besides the model: the material pass with the skinning combos, the vertex layout the GPU
// reads from the model's own bytes, and the animation layers the object plays at a time.

#include "puppet/mdl.h"
#include "render/passes.h"
#include "scene/properties.h"
#include "scene/scene.h"

namespace murasaki::scene {

// Whether the layer plays animations: it names animation layers, and the model has a skeleton, vertices on its bones and
// animations. Without that the puppet is drawn in its bind pose, unskinned (the mesh as authored).
bool puppetSkinned(const ImageLayer &layer);

// The layer's base pass: the model's material, plus SKINNING and BONECOUNT when it is skinned.
MaterialPass puppetPass(const ImageLayer &layer);

// The vertex layout of a puppet vertex (80 bytes: position, normal, tangent, blend indices, blend weights, uv).
const GeometryAttribute *puppetAttributes(uint32_t &count);
bool puppetAttributeKnown(const std::string &name);

// The vertex layout of a 3D mesh object's vertex (48 bytes: position, normal, tangent, uv).
const GeometryAttribute *meshAttributes(uint32_t &count);
bool meshAttributeKnown(const std::string &name);

// The animation layers that play now (visible ones, with their blend and rate resolved against the properties).
std::vector<PuppetLayerState> puppetLayerStates(const ImageLayer &layer, const PropertyStore &props);
std::vector<PuppetLayerState> puppetLayerStates(const std::vector<AnimationLayer> &layers, const PropertyStore &props);
// The same into `out` (cleared first), keeping its storage from frame to frame.
void puppetLayerStates(const std::vector<AnimationLayer> &layers, const PropertyStore &props, std::vector<PuppetLayerState> &out);

} // namespace murasaki::scene
