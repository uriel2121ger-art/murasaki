#pragma once

// Wallpaper Engine shader dialect -> GLSL 450 for Vulkan -> SPIR-V, with the reflection the renderer needs to feed it.
//
// What the translator does to a vertex/fragment pair (docs/scene-vk/shaders.md has the rules in prose):
//   * defines every combo (declared default, texture-implied, then the overrides) plus `GLSL`, then our prelude of
//     helpers (`mul`, `texSample2D`, `saturate`, `frac`, `lerp`, `CAST3`, ...);
//   * runs glslang's preprocessor, so `#if` over combos leaves exactly the declarations that are live;
//   * turns loose `uniform`s into one std140 uniform block per stage (set 0: vertex binding 0, fragment binding 1),
//     `g_TextureN` samplers into `set = 1, binding = N`, `attribute`s into inputs with fixed locations by name and
//     `varying`s into outputs/inputs matched by name between the stages;
//   * `gl_FragColor` becomes the fragment output at location 0 and the vertex `main` is wrapped: clip y is negated
//     and the depth maps from OpenGL's [-1, 1] to Vulkan's [0, 1]. The shaders compute clip positions y-up while
//     their texture coordinates start at the top left, so with the flip every render target holds its image top row
//     first, like the textures read from files, and nothing else has to flip;
//   * compiles both stages and records where each uniform lives in its block.

#include "shaders/source.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace murasaki::scene {

// Bumped whenever the translation changes what it produces: part of every cache key.
inline constexpr int kTranslatorVersion = 45;

// The largest uniform block of a stage (what the pass machinery's descriptor window covers).
inline constexpr uint32_t kMaxUniformBlock = 16384;

// Uniforms the engine fills itself, recognised by name when a shader is translated so that filling a block is a switch
// and not a string comparison per uniform per draw.
enum class EngineInput : uint8_t {
    None,
    Time,
    Frametime,
    Daytime,
    PointerPosition,
    PointerPositionLast,
    PointerState,
    ParallaxPosition,
    Screen,
    EyePosition,
    ModelViewProjection,
    ModelViewProjectionInverse,
    ModelMatrix,
    ViewProjection,
    EffectTextureProjection,
    EffectTextureProjectionInverse,
    Texture0Rotation,
    Texture0Translation,
    TextureResolution, // `aux` is the slot N of g_Texture<N>Resolution
    ModelMatrixInverse,
    TexelSize,         // 1 / the size of the scene buffer, and half of it
    TexelSizeHalf,
    OrientationUp,     // particle shaders: the axes sprites are oriented along (the screen's)
    OrientationRight,
    OrientationForward,
    ViewUp,
    ViewRight,
    RenderVar0,        // particle shaders: trail lengths
    RenderVar1,        // particle shaders: sprite sheet size, frame count and the texture's aspect ratio
    AudioSpectrum,
    EffectModelViewProjection, // effects: the layer's quad (clip square) onto the screen
    TextureReductionScale,     // 1: textures are not reduced for speed
    LightAmbientColor,         // the scene's `ambientcolor` and `skylightcolor` (lit materials)
    LightSkylightColor,
    ViewForward,               // the camera's direction (g_ViewForward); ViewUp and ViewRight are its other axes
    AltViewProjection,         // g_AltViewProjectionMatrix: the camera's view-projection, as g_ViewProjectionMatrix
    NormalModelMatrix,         // 3D meshes: the model matrix's inverse transpose (mat3), for normals
    FogDistanceColor,          // the scene's fog (FOG_DIST / FOG_HEIGHT materials): colour and (start, end - start, start density,
    FogDistanceParams,         // end density - start density) of each band
    FogHeightColor,
    FogHeightParams,
    // the scene's lights (render/lights.h): WE's arrays per light type, and the older four-light arrays
    LPointColor,
    LPointOrigin,
    LSpotColor,
    LSpotOrigin,
    LSpotDirection,
    LSpotExponent,
    LTubeColor,
    LTubeOriginA,
    LTubeOriginB,
    LDirectionalColor,
    LDirectionalDirection,
    LightsPosition,
    LightsColorRadius,
    LightsColorPremultiplied,
    // shadows (render/shadows.h): what the lighting function reads to find a point in the shadow atlas
    ShadowPointProjection,
    ShadowPointProjectionTransform,
    ShadowProjection,
    ShadowProjectionTransform,
    ShadowCascadeProjection,
    ShadowCascadeTransform,
    ShadowCascadeTexel,
    TextureTexel,      // `aux` is the slot N of g_Texture<N>Texel: (1 / width, 1 / height, width, height) of what is bound there
    ViewportViewProjections, // shadow casters: g_ViewportViewProjectionMatrices[6], one matrix per picture the draw goes to
    Bones,             // puppet shaders: g_Bones[], one mat4x3 skinning matrix a bone
    TextureMipMapInfo, // `aux` is the slot N of g_Texture<N>MipMapInfo: the highest mip level of what is bound there (0 without a chain)
    // Append only: the shader cache stores these numbers (shaders/cache.cpp validates what it reads back against `Last`).
    Last = TextureMipMapInfo,
};

EngineInput classifyUniform(const std::string &name, int *aux);

struct UniformInfo {
    std::string name;         // "g_Brightness"
    std::string type;         // GLSL type name: "float", "vec3", "mat4", "mat4x3", "uint", ...
    uint32_t arrayCount = 0;  // 0: not an array
    uint32_t offset = 0;      // std140 offset in the stage's block
    uint32_t size = 0;        // bytes the member occupies (an array: stride * count)
    std::string materialKey;  // annotation "material": how materials and effect instances name it (may be empty)
    std::string defaultText;  // annotation "default" as text ("1", "0.5 0.5", ...); empty when absent
    bool isColor = false;     // annotation "type": "color": a user property for it may arrive as 0..255
    bool used = true;         // the stage's code names it (an effect vertex shader may declare the matrix and write clip positions itself)
    EngineInput input = EngineInput::None;
    int aux = 0;
    std::string lookupKey;    // `materialKey` lower-cased: how a material's constants are found
};

struct StageProgram {
    std::vector<uint32_t> spirv;
    std::vector<UniformInfo> uniforms;
    uint32_t blockSize = 0; // bytes of the std140 block (0: the stage has no uniforms)
};

struct SamplerInfo {
    std::string name;       // "g_Texture1"
    uint32_t binding = 0;   // N of g_TextureN
    std::string type;       // "sampler2D", "sampler3D"
    std::string comboName;  // annotation "combo": turned on when a texture is bound
    std::string defaultText; // annotation "default": a named render target such as "_rt_MipMappedFrameBuffer"
    bool hidden = false;
};

struct AttributeInfo {
    std::string name; // "a_Position"
    std::string type;
    uint32_t location = 0;
};

struct TranslatedShader {
    StageProgram vertex;
    StageProgram fragment;
    std::vector<SamplerInfo> samplers;     // both stages, by binding
    std::vector<AttributeInfo> attributes; // the vertex inputs that are live
    std::map<std::string, int> combos;     // what was defined, after defaults, textures and overrides
};

struct TranslateOptions {
    // Combo values from the material and the scene (names in any case; they are matched upper-cased). The scene's fog combos
    // (FOG_DIST, FOG_HEIGHT, FOG_COMPUTED) only apply to a shader whose own FOG combo is on: resolveCombos drops them otherwise.
    std::map<std::string, int> combos;
    // Texture slots (g_Texture<N>) a texture is bound to: a sampler whose annotation names a combo turns that combo on.
    std::vector<uint32_t> boundTextures;
    // The compatibility shim for Workshop shaders written for a lenient HLSL compiler (translateShader sets it for a second
    // attempt after the strict one fails to compile): global initializers that read uniforms move into main(), a float
    // uniform annotated `"int": true` reads as an int, and texture coordinates wider than two components are cut to two.
    bool lenient = false;
};

// The combos of a pair as they will be defined (declared defaults of both stages, sampler combos, overrides).
std::map<std::string, int> resolveCombos(const ShaderSource &vertex, const ShaderSource &fragment, const TranslateOptions &options);

// Translates and compiles. Failure returns nullopt with the reason (compiler messages included) in `error`.
// A hit in the shader cache (shaders/cache.h) answers without starting glslang.
std::optional<TranslatedShader> translateShader(const ShaderSource &vertex, const ShaderSource &fragment,
                                                const TranslateOptions &options, std::string *error = nullptr);

// The translation without compiling: the GLSL 450 text of each stage (tests and diagnostics).
struct TranslatedText {
    std::string vertex;
    std::string fragment;
    TranslatedShader layout; // everything but the SPIR-V
};
std::optional<TranslatedText> translateToGlsl(const ShaderSource &vertex, const ShaderSource &fragment, const TranslateOptions &options,
                                              std::string *error = nullptr);

// std140 size and alignment of a GLSL type name in a uniform block ({0, 0} for an unknown type).
struct Std140 {
    uint32_t size = 0;
    uint32_t alignment = 0;
};
Std140 std140Of(const std::string &glslType);

} // namespace murasaki::scene
