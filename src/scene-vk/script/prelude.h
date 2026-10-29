#pragma once

// The JavaScript every script context starts with: the vector classes, the maths and colour modules, `console`, `engine`,
// `input`, `shared`, `thisScene`, the layer proxies and `createScriptProperties`. Written for this engine from the
// public description of SceneScript's interface (the corpus of scripts in the library tells which parts are used).

namespace murasaki::scene {

// Evaluated as a classic script in every context. It expects the native object `__host` (see scripthost.cpp).
extern const char *const kScriptPrelude;

// Modules a script may import: 'WEMath', 'WEVector', 'WEColor'.
extern const char *const kModuleWEMath;
extern const char *const kModuleWEVector;
extern const char *const kModuleWEColor;

} // namespace murasaki::scene
