#pragma once

// Draws the scene's particle systems. Each system of a particle object (the object's own and its static children)
// owns a ParticleSystem (particles/system.h) and a host-visible vertex buffer; every frame the live particles become
// four vertices each in WE's particle layout (a_Position, a_TexCoordVec4 = uv + rotation z + size, a_Color,
// a_TexCoordVec4C1 = velocity + lifetime or sheet frame, a_TexCoordC2 = rotation x, y) and one indexed draw through the
// PassRunner draws them with the material's translated shader (`genericparticle`), additive, translucent or normal.
// Per-vertex data, not instancing: the shaders take their attributes per vertex and are used unchanged.

#include "core/context.h"
#include "particles/system.h"
#include "render/passes.h"
#include "render/shaderlib.h"
#include "render/targets.h"
#include "render/texture.h"
#include "scene/scene.h"

#include <functional>
#include <memory>
#include <vector>

namespace murasaki::scene {

// The vertex layout shaders are matched against; also what analysis checks a particle shader's attributes with.
bool particleAttributeKnown(const std::string &name);

class ParticleRenderer
{
public:
    using TextureLookup = std::function<const GpuTexture *(const std::string &)>;

    ParticleRenderer(const VkContext &ctx, PassRunner &runner, ShaderLibrary &shaders, const Scene &scene, const PropertyStore &props,
                     TextureLookup textures);
    ~ParticleRenderer();

    bool empty() const { return m_layers.empty(); }
    // Whether any particle object is shown (and so the scene changes from frame to frame).
    bool anyVisible() const;

    // Runs the simulation of the visible systems by `dt` seconds. The pointer is top-left based, 0..1 over the scene.
    // `audio` (may be null: silence) is the spectrum emitters with an audio response follow; see usesAudio().
    void update(double dt, Vec2 pointer, bool pointerKnown, Vec2 parallaxShiftScale, const AudioSpectrum *audio = nullptr);
    // Whether any emitter reacts to the spectrum (the engine then has to capture audio).
    bool usesAudio() const { return m_usesAudio; }

    struct Frame {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        Mat4 viewProjection = Mat4::identity();
        Vec2 parallax{0.0f, 0.0f}; // the camera's smoothed displacement
        Vec2 orthoSize{1920, 1080};
        bool perspectiveScene = false; // the scene's camera is a perspective one: every system is seen through viewProjection
        float fov = 50.0f; // degrees, of the camera perspective particles use in an orthographic scene
        float nearZ = 0.01f;
        float farZ = 10000.0f;
        VkImageView target = VK_NULL_HANDLE;
        VkFormat targetFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkExtent2D extent{};
        FrameInputs inputs;
        std::function<RenderTarget &()> snapshot; // a copy of the scene so far, for REFRACT materials
        // A scratch target of the given size (null: systems are always drawn straight into the scene). Big soft systems are
        // drawn into it at half size and added to the scene (see draw()).
        std::function<RenderTarget &(VkExtent2D)> scratch;
    };
    // Draws the systems of particle object `layer` (index into Scene::particles).
    // Returns the number of quads drawn (for the profile).
    size_t draw(const Frame &frame, size_t layer);

    uint64_t liveParticles() const;

private:
    struct Runtime;
    struct Layer;
    void build(Layer &layer, const ParticleDefinition &def, const Mat4 &relative, size_t depth, uint32_t salt, const ParticleChild *follow = nullptr,
               size_t parent = 0);
    std::unique_ptr<ParticleSystem> makeSystem(const Layer &layer, const Runtime &r, const ParticleDefinition &def, uint32_t seed);
    void syncFollowers(Layer &layer, Runtime &follower, double dt);
    void syncEvents(Layer &layer, Runtime &child, double dt);
    void rebuild(Layer &layer, size_t index);
    ParticleOverrides overridesOf(const ParticleLayer &p) const;
    size_t ensureCapacity(Runtime &r, size_t quads);
    bool ensureIndices(size_t particles);

    const VkContext &m_ctx;
    PassRunner &m_runner;
    ShaderLibrary &m_shaders;
    const Scene &m_scene;
    const PropertyStore &m_props;
    TextureLookup m_textures;
    std::vector<std::unique_ptr<Layer>> m_layers; // by Scene::particles index
    GpuBuffer m_indices;
    size_t m_indexParticles = 0;
    std::vector<GpuBuffer> m_retiredIndices; // replaced while a frame was being recorded: freed when the next one starts
    bool m_indexWarned = false;
    size_t m_vertexBytes = 0;                // vertex memory of every system
    bool m_usesAudio = false;
    double m_clock = 0.0;                    // seconds simulated, for the rebuild limit
};

} // namespace murasaki::scene
