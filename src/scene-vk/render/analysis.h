#pragma once

// Everything that decides whether the renderer can draw a scene, on the CPU only (no GPU): every shader of the base
// passes and of all effect passes (visible or not: a property can switch them on) is translated and compiled, every
// texture is looked up, and the limits of the pass machinery are checked (vertex attributes a quad does not have,
// texture slots, uniform block size, targets that do not exist). `--check` calls it so an item is accepted only when
// SceneRenderer will be able to start.

#include "assets/vfs.h"
#include "scene/scene.h"

#include <string>
#include <vector>

namespace murasaki::scene {

// Reasons the scene cannot be rendered, in the same style as Scene::unsupported ("shader compile: effects/foo: ...").
// Empty when it can. `degraded`, when given, receives what the renderer will draw the scene without (the analysis
// counterpart of Scene::degraded).
std::vector<std::string> renderabilityReasons(const Scene &scene, const Vfs &vfs, std::vector<std::string> *degraded = nullptr);

} // namespace murasaki::scene
