#pragma once

// Camera shake (`general.camerashake*`). Wallpaper Engine documents no formula, so this is an assumption, chosen to be
// subtle and smooth: the camera moves by smooth noise, independently in x and y, of at most `amplitude` percent
// of the scene's size on each axis (amplitude 0.5 on a 1920 wide scene: 9.6 units either way). `speed` is how many noise
// lattice steps pass each second (3: a full wander every third of a second... a lattice step is 1/speed seconds) and
// `roughness` mixes in two finer octaves (0: a single smooth wave, 1 and above: visibly jittery).

#include "util/math.h"

namespace murasaki::scene {

// The camera's displacement in scene units at `time` seconds.
Vec2 cameraShake(double time, float amplitude, float roughness, float speed, Vec2 sceneSize);

} // namespace murasaki::scene
