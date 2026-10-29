#pragma once

// Draws a Scene: for every visible image layer the base material pass, the layer's effect passes (render graph below)
// and the composite into the scene target; then one pass scales the scene into the export image with the bridge's
// fill, fit or stretch mode. Every buffer is RGBA8, except in an HDR scene (`general.hdr`): there the scene target and every
// buffer drawn on the way to it hold half floats, and the bloom (WE's HDR chain) or the scaling pass bring the picture back to
// 8 bits. 3D mesh objects are drawn in the same file order, straight into the scene target with a depth buffer of its size
// (cleared by the first mesh of a frame) that only meshes test and write.
//
// Render graph of one layer, in WE's own terms (docs/scene-vk/shaders.md, formats.md):
//   * no visible effect (and nothing else that needs a buffer): the base pass draws straight into the scene target
//     with the layer's transform and blending;
//   * otherwise the base pass writes the layer's buffer A (the picture, straight alpha, layer-sized), each effect
//     pass reads the layer's current buffer (`previous`) and named targets (effect `fbos`, `_rt_*`) and writes a
//     target or the other ping-pong buffer, and the final buffer is composited into the scene with the base
//     material's blending.
// Textures are bound per slot: the material's, an effect `bind` (`previous` or a target), a sampler's default render
// target, or white. Uniforms come from render/uniforms.h.
//
// The scene pass runs only when something it depends on changed (first frame, a user property, the scaling mode, the
// output size, the pointer when a shader reads it) or when a shader reads the clock or the audio spectrum; every other
// frame only runs the scaling pass, so a still wallpaper costs next to nothing.

#include <unordered_map>

#include "assets/videodecoder.h"
#include "audio/sceneaudio.h"
#include "core/context.h"
#include "render/camera.h"
#include "render/gpuprofiler.h"
#include "render/lights.h"
#include "render/particlerenderer.h"
#include "render/passes.h"
#include "render/passprobe.h"
#include "render/renderer.h"
#include "render/scenescripts.h"
#include "render/shadows.h"
#include "render/shaderlib.h"
#include "render/targets.h"
#include "render/texture.h"
#include "render/videoconvert.h"
#include "render/viewmapping.h"
#include "scene/properties.h"
#include "scene/scene.h"
#include "text/textraster.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace murasaki::scene {

class SceneRenderer final : public Renderer
{
public:
    // Loads every texture the scene names, translates every shader and builds the pass machinery. Throws VkException
    // or std::runtime_error with a readable reason when something cannot be done (a missing texture, a shader that
    // does not compile). `vfs` is copied: the renderer reads files only while it is built.
    SceneRenderer(VkContext &ctx, Scene scene, Vfs vfs, const PropertyStore &properties);
    ~SceneRenderer() override;

    // Where the scene's audio goes: the PipeWire backend unless replaced (the tests use a fake one).
    using AudioBackendFactory = std::function<std::unique_ptr<AudioBackend>()>;
    static void setAudioBackendFactory(AudioBackendFactory factory);

    void render(const FrameParams &params) override;
    void setScaling(bridge::msg::ScalingMode mode) override;
    void setSpanView(const bridge::msg::SpanView &view) override;
    bool wantsMedia() const override;
    void setMedia(const MediaState &state) override;
    void setMediaThumbnail(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) override;
    void setShortcutIcon(const std::string &name, uint32_t width, uint32_t height, std::span<const uint8_t> rgba) override;
    void setShortcutLaunchHandler(std::function<bool(const std::string &)> handler) override;
    void setPointer(int x, int y, unsigned buttons, VkExtent2D viewport) override;
    void setVolume(unsigned bridgeVolume) override;
    void setPaused(bool paused) override;
    void setFps(int fps) override;
    bool needsFrame() const override;
    void frameCompleted() override;
    double gpuFrameMs() const override;
    // Target pixels per output pixel of the scene target (1 = as many as the output shows, below 1 = coarser and cheaper).
    void setRenderScale(float scale) override;
    float renderScale() const { return m_renderScale; }
    const TargetPlan &targetPlan() const { return m_plan; }

    // The --disable-mouse / --disable-parallax options: the pointer stays where it is (the centre) / the camera never moves.
    void setPointerEnabled(bool enabled) { m_pointerEnabled = enabled; }
    void setParallaxEnabled(bool enabled) { m_parallaxEnabled = enabled; }

    // Counters for tests and logs.
    uint64_t scenePasses() const { return m_scenePasses; }
    // The user texture properties a property update set to a picture (they were reported once as "drawing without: user texture <name>").
    const std::set<std::string> &reportedUserTextures() const { return m_reportedUserTextures; }
    uint64_t draws() const { return m_runner->draws(); }
    // Layers whose buffers were reused instead of drawn again (render/scenerenderer.cpp, layer caching).
    uint64_t layerCacheHits() const { return m_layerCacheHits; }
    // Shadow passes drawn (the atlas is drawn again only when a shadowed light or a caster moved, render/shadows.h).
    uint64_t shadowPasses() const { return m_shadowPasses; }
    // Shadow pictures drawn (a point light's face, a spot, a cascade), over the run.
    uint64_t shadowPicturesDrawn() const { return m_shadowPicturesDrawn; }
    // Pass optimiser (render/passprobe.h): effect passes found to be the identity and not drawn, passes drawn at a fraction of
    // the layer and enlarged, and the probes that decided.
    struct PassOptStats {
        uint64_t skipped = 0;
        uint64_t reduced = 0;
        uint64_t partial = 0;  // drawn only over the rectangle they change
        uint64_t probes = 0;
    };
    PassOptStats passOptStats() const
    {
        PassOptStats s = m_passOpt;
        s.probes = m_probe ? m_probe->probes() : 0;
        return s;
    }
    // The scene pass runs every frame (a shader reads the clock or the audio) / when the pointer moves.
    bool animated() const { return m_animated || animationsRunning() || (m_particles && !m_particles->empty()); }
    bool usesPointer() const { return m_usesPointer; }
    const std::set<std::string> &unknownUniforms() const { return m_runner->unknownUniforms(); }

private:
    struct PassPlan;
    // What prepare() learns about a layer: whether its base pass and its whole chain are free of the clock, the pointer,
    // the audio and the scene drawn so far.
    struct LayerPlan {
        bool baseStatic = false;
        bool allStatic = false;
    };
    struct LayerCache {
        uint64_t baseEpoch = ~0ull;  // m_staticEpoch the kept base picture was drawn in
        uint64_t finalEpoch = ~0ull; // ... and the final buffer
        char finalKey = 'a';         // which ping-pong buffer holds it
    };
    const GpuTexture *textureNamed(const std::string &name) const;
    void loadTextures();
    // The picture a shader's sampler names as its `default` ("util/perlin_256") when the material binds nothing there.
    void loadDefaultTexture(const std::string &name);
    void prepare();
    void startAudio();
    void viewBox(Vec2 &low, Vec2 &size) const; // the part of the scene the view shows
    // The camera's view-projection (g_ViewProjectionMatrix): the orthogonal projection of the view box, or the perspective
    // camera over it; what layers, particles and meshes are drawn with.
    Mat4 sceneProjection() const;
    Mat4 screenProjection() const; // the view box as a flat projection: what is already in screen space (fullscreen layers)
    void updateView();             // the camera's view matrix, eye and axes, after the camera object, parallax and shake
    void startScripts();
    void reportUserTextures();
    void updateTexts();
    void updateAttachments(double time);
    std::vector<PuppetLayerState> m_attachmentStates; // updateAttachments' layer list, kept from frame to frame
    std::shared_ptr<TextRasterizer> fontFor(const std::string &font);
    void rasterizeText(size_t layer, const std::string &value, unsigned pixels, float averageScale);
    void updateParallax();
    void updateCameraObject(double time);
    void ensureTarget(VkExtent2D output);
    void recordScenePass(VkCommandBuffer &cb, const FrameParams &params);
    // Draws an image layer into the scene. With `bufferOnly` it renders the layer's composite (its picture after its effects, in
    // the layer's own buffers, whether the layer is visible or not) and returns that buffer instead: what a 3D mesh reads as
    // _rt_imageLayerComposite_<id>_a. nullptr when there is nothing to draw.
    RenderTarget *drawLayer(VkCommandBuffer &cb, const ImageLayer &layer, size_t index, bool bufferOnly = false);
    void drawParticles(VkCommandBuffer &cb, size_t index);
    // A 3D mesh object: its parts with their materials, depth-tested against the scene's depth buffer.
    void drawMesh(VkCommandBuffer &cb, size_t index);
    // The beams of the lights that cast volumetrics (formats.md, "Volumetric lights"), drawn over the finished scene with WE's own
    // volumetricsback / volumetricsfront shaders.
    RenderTarget *drawVolumetrics(VkCommandBuffer &cb);
    // A copy of the scene as drawn so far (what REFRACT materials and `_rt_FullFrameBuffer` read); the scene target
    // is back in COLOR_ATTACHMENT_OPTIMAL afterwards.
    // The scene drawn so far, copied. `mips` gives the copy a full mip chain (`_rt_MipMappedFrameBuffer`, which a rough REFLECTION
    // samples at a higher level): a box-filtered chain of blits, made when a shader reads g_Texture<N>MipMapInfo of the slot.
    RenderTarget &snapshotScene(VkCommandBuffer &cb, bool mips = false);
    void drawPresent(const FrameParams &params);
    // Bright pass, blur and combine over the finished scene; returns the target to show.
    RenderTarget *drawBloom(VkCommandBuffer &cb, RenderTarget &source);
    // An HDR scene's bloom (WE's HDR chain: halved `bloomhdriterations` times from a soft-thresholded first level, added back up
    // level by level with the scatter) and the final combine into an 8 bit target; returns the target to show.
    RenderTarget *drawHdrBloom(VkCommandBuffer &cb, RenderTarget &source);
    // The scene's fog bands as they are now (user properties may switch or move them): the shaders' FOG_* combos and the fog uniforms.
    void updateFog();
    RenderTarget &layerTarget(size_t layer, const std::string &key, VkExtent2D extent, VkFormat format);
    void clearTarget(VkCommandBuffer &cb, RenderTarget &target);
    void profileMark(VkCommandBuffer cb, const std::string &label);
    // Between draws: when what was recorded since the last piece reaches m_splitPixels, the frame's command buffer is sent and
    // recording goes on in a new one (FrameParams::split); `pixels` is about what the next draw writes.
    void yieldPoint(VkCommandBuffer &cb, uint64_t pixels);
    std::string layerLabel(const ImageLayer &layer, size_t index) const;

    VkContext &m_ctx;
    Scene m_scene;
    Vfs m_vfs;
    const PropertyStore &m_props;
    bridge::msg::ScalingMode m_scaling = bridge::msg::ScalingMode::Fill;
    bridge::msg::SpanView m_span{}; // enabled 0: the output shows the whole picture

    std::map<std::string, std::unique_ptr<GpuTexture>> m_textures; // by name as materials write it
    struct VideoTexture {
        std::string name; // in m_textures, which the decoder rewrites as the clock runs
        std::unique_ptr<VideoDecoder> decoder;
        bool zeroCopy = false; // the pictures arrive as the decoder's dma-bufs and are converted on the GPU
        double clock = 0.0;    // the video's own clock: runs with the scene's at the video's rate while it plays (a script steers it: VideoControl)
        bool loopApplied = true;
        bool spent = false;    // a video that does not loop has reached its end and said so
        double lastPosition = 0.0;
    };
    void stepVideo(VideoTexture &v);
    std::vector<VideoTexture> m_videos;
    // What the names of an effect's buffers stand for after its `swap` commands, by "layer:effect:name" (absent: the name itself).
    std::map<std::string, std::string> m_fboAlias;
    static std::string fboAliasKey(size_t layer, size_t effect, const std::string &name);
    const std::string &fboAlias(size_t layer, size_t effect, const std::string &name) const;
    // The cover of the track the host follows: m_textures holds it as "$mediaThumbnail" and the one before as
    // "$mediaPreviousThumbnail" (the names MediaBinding slots are rewritten to); a replaced texture waits a few frames, a recorded
    // command may still name it.
    void applyMediaSlots();
    std::vector<std::pair<uint64_t, std::unique_ptr<GpuTexture>>> m_retiredTextures;
    MediaState m_media;
    std::function<bool(const std::string &)> m_shortcutLaunchHandler; // a script's openUserShortcut(name), to the host
    std::unique_ptr<VideoConverter> m_videoConverter; // made with the first video; null when this device cannot import the surfaces
    bool m_videoConverterTried = false;
    int m_fps = 0; // the loop's frame rate, for the video decoders
    std::unique_ptr<ShaderLibrary> m_shaders;
    std::unique_ptr<PassRunner> m_runner;
    std::unique_ptr<TargetPool> m_targets;
    std::unique_ptr<GpuProfiler> m_profiler;
    struct TextState {
        std::shared_ptr<TextRasterizer> raster;
        std::string fontKey;
        std::string shown;
        unsigned pixels = 0;
        float maxWidth = 0.0f;
        bool failed = false;
    };
    std::map<std::string, std::shared_ptr<TextRasterizer>> m_fonts; // by font path or name: layers share one rasterizer
    // A puppet layer's mesh on the GPU (the model's own bytes), its material pass (with the skinning combos) and, while it plays
    // animations, the skinning matrices of this frame. By layer index.
    struct PuppetState {
        GpuBuffer vertices;
        GpuBuffer indices;
        uint32_t indexCount = 0;
        MaterialPass pass;
        bool skinned = false;
        std::vector<float> bones;
    };
    std::map<size_t, PuppetState> m_puppets;
    void preparePuppets();
    // A 3D model's parts on the GPU (device-local copies of the file's 48 byte vertices, or 80 byte skinned ones, and u16 or u32
    // indices), shared by every object that uses the model; m_meshes holds one per mesh index.
    struct MeshGpu {
        struct Part {
            GpuBuffer vertices;
            GpuBuffer indices;
            uint32_t indexCount = 0;
            VkIndexType indexType = VK_INDEX_TYPE_UINT16;
            GpuBuffer positions; // a static mesh that casts shadows: its positions alone (12 bytes a vertex), what the casters read
            // ... and, for a big part, its indices cut into runs with their bounds (own space), so a shadow picture draws only the runs
            // it can see
            struct Chunk {
                IndexRange range;
                Vec3 low, high;
            };
            std::vector<Chunk> chunks;
        };
        std::vector<Part> parts;
    };
    std::vector<std::shared_ptr<const MeshGpu>> m_meshes;
    std::vector<std::vector<float>> m_meshBones; // by mesh index: a skinned mesh's g_Bones of this frame
    // By layer index: the buffer a layer's chain ended in this frame (a layer drawn through its buffers, or for a mesh that samples
    // its composite). A layer is rendered once a frame however many meshes sample it, and its own draw in the scene reuses it.
    std::map<size_t, RenderTarget *> m_layerFinal;
    // The order the scene pass draws in: the file's, or with `transparentsorting` the opaque meshes first (file order) and the rest
    // back to front by the view depth of their origins (ties in file order). Rebuilt each frame (objects and camera move).
    const std::vector<DrawEntry> &frameOrder();
    std::vector<DrawEntry> m_sortedOrder;
    std::vector<std::pair<float, size_t>> m_sortKeys;
    void prepareMeshes();
    // Shadows (render/shadows.h, formats.md "Shadows"). prepareShadows builds each casting part's caster pass; drawShadows draws, at the
    // start of a scene pass, the pictures of the atlas whose casters moved (render/scenerenderer.cpp has the rules); bindShadowAtlas gives
    // a lit draw the atlas in the slot its shader names `_rt_shadowAtlas`.
    void prepareShadows();
    void drawShadows(VkCommandBuffer &cb);
    void bindShadowAtlas(DrawRequest &request) const;
    // A skinned mesh's bones of this frame, worked out once a frame (the shadow pass and the mesh's own draw both use them).
    void updateMeshBones(size_t mesh);
    bool m_shadows = false;                                 // the scene draws shadows (sceneHasShadows)
    struct CasterPass {
        MaterialPass pass;
        const TranslatedShader *shader = nullptr;
    };
    std::vector<std::vector<CasterPass>> m_casterPasses;    // by mesh index, one per part (no shader: the part casts nothing)
    // What reads the atlas (a draw whose shader samples `_rt_shadowAtlas`): a picture of a light no receiver can see is not drawn.
    std::vector<bool> m_meshReceives;                       // by mesh index
    std::vector<uint8_t> m_layerReceives;                   // by layer index: 0 no, 1 its quad, 2 anywhere (fullscreen, puppet)
    std::unique_ptr<RenderTarget> m_shadowAtlas;
    bool m_shadowLinear = true;                             // the atlas's format filters linearly: the comparisons are 2 x 2 filtered
    VkFormat m_shadowFormat = VK_FORMAT_UNDEFINED;
    ShadowSettings m_shadowSettings;                        // the cell and the filter quality (the render scale's ladder)
    ShadowUniforms m_shadowUniforms;                        // what the lit shaders read (m_inputs.shadows points here)
    uint64_t m_shadowSignature = 0;                         // what forces the whole atlas to be drawn again: casters, pictures, size
    struct ShadowCasterState {
        size_t mesh = 0;
        Mat4 model;
        uint64_t bones = 0; // a hash of a skinned caster's bones (0: not skinned)
    };
    // One picture of the atlas as it was last drawn: its matrix, the casters then, whether a receiver saw it, and when (scene seconds).
    struct ShadowPicture {
        Mat4 viewProjection;
        std::vector<ShadowCasterState> casters;
        bool seen = false;
        bool drawn = false;
        double drawnAt = 0.0;
    };
    std::vector<std::vector<ShadowPicture>> m_shadowPictures; // per light, per picture
    bool m_shadowDrawn = false;
    double m_shadowLastTime = -1.0;   // the scene time of the last shadow pass
    bool m_shadowPending = false;     // a picture whose casters moved waits for its turn (a scene pass is still needed)
    uint64_t m_shadowPasses = 0;      // shadow passes recorded (one that draws nothing is not counted)
    uint64_t m_shadowPicturesDrawn = 0;
    // The casters of this frame (visible meshes that cast): world matrix, bones and their parts' boxes (world and own space).
    struct ShadowCaster {
        size_t mesh = 0;
        Mat4 model;
        uint64_t bones = 0;
        std::vector<std::array<Vec3, 8>> parts;
        std::vector<std::array<Vec3, 8>> local;
    };
    // Per-frame working space of the shadow pass, kept between frames (its capacity is reused).
    std::vector<ShadowCaster> m_shadowCasters;
    std::vector<std::array<Vec3, 8>> m_shadowReceivers;
    std::vector<DrawRequest> m_shadowRequests;
    std::vector<std::vector<bool>> m_shadowDirty, m_shadowSeen; // per light, per picture: drawn this frame, seen by a receiver
    std::vector<std::pair<size_t, size_t>> m_shadowRuns;    // per request: its first run in m_shadowRanges and how many
    std::vector<IndexRange> m_shadowRanges;
    std::vector<uint8_t> m_shadowRangeMasks;
    std::vector<std::array<Mat4, 6>> m_shadowMatrices;      // per shadowed light: what its casters' draws read (kept while recorded)
    std::vector<std::vector<VkViewport>> m_shadowViewports;
    std::vector<Geometry> m_shadowGeometry;
    std::vector<uint64_t> m_meshBonesFrame;                 // by mesh index: the frame m_meshBones was worked out for
    uint64_t m_frameIndex = 0;
    // The unit sphere and cone the volumetric lights are drawn over (indexed triangles, positions only), made with the first beam.
    struct VolumeMesh {
        GpuBuffer buffer; // vertices, then u16 indices
        uint32_t indexCount = 0;
        VkDeviceSize indexOffset = 0;
    };
    VolumeMesh m_volumeSphere, m_volumeCone;
    // What drawVolumetrics keeps from frame to frame: the lights of this frame, the shaders of the beams and of the blur and combine, and
    // each light's requests (their uniform maps are filled in place).
    struct PostPass {
        MaterialPass pass;
        const TranslatedShader *shader = nullptr;
    };
    struct VolumeDraw {
        PostPass front;
        Geometry geometry;
        DrawRequest back, frontRequest;
        bool shadowed = false;
        bool point = false;
    };
    std::vector<VolumetricLight> m_volumetricLights;
    std::vector<VolumeDraw> m_volumeDraws;
    PostPass m_volumeBack, m_volumeBlurH, m_volumeBlurV, m_volumeCombine;
    uint64_t m_volumeShadersFor = 0;
    VkFormat m_volumeDepthFormat = VK_FORMAT_UNDEFINED;
    bool m_warnedVolumeDepth = false;
    bool m_volumetricsFailed = false; // a shader of the beams did not build: reported once, no beam is drawn
    bool m_depthCleared = false; // the scene pass being recorded has cleared the depth buffer (the first mesh does)
    std::map<size_t, TextState> m_texts;      // by layer index: the string each text layer's texture shows
    ScriptFlags m_scriptFlags;
    std::unique_ptr<SceneScripts> m_scripts;   // null when the scene has no script
    std::unique_ptr<SceneAudio> m_audio;           // null when the scene has no sound and nothing reads the spectrum
    std::unique_ptr<ParticleRenderer> m_particles; // null when the scene has no particle object
    // The format of the scene target and of every buffer layers, effects, particles and meshes draw into: 8 bit, or half floats in
    // an HDR scene (values above 1 survive until the final combine).
    VkFormat m_colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    RenderTarget *m_scene_target = nullptr;
    RenderTarget *m_present_source = nullptr; // what the last pass shows: the scene, or the scene with bloom
    VkExtent2D m_outputExtent{};
    TargetPlan m_plan;
    float m_renderScale = 1.0f;
    std::function<VkCommandBuffer(VkCommandBuffer)> m_split; // this frame's (FrameParams::split), empty outside render()
    uint64_t m_splitPixels = 4000000;   // MURASAKI_SCENE_VK_SPLIT_MPIX (0: one piece)
    uint64_t m_pixelsSinceSplit = 0;
    size_t m_pieces = 1;
    int m_noSplit = 0;                  // > 0 inside a draw that a split must not cut (a layer drawn for a mesh's material)
    VkCommandBuffer m_cb = VK_NULL_HANDLE;
    FrameInputs m_inputs;
    LightUniforms m_lights; // what the scene's lights give the lit shaders this frame (m_inputs.lights points here)
    bool m_lightsChanged = false; // the lights differ from the last frame's: the scene pass runs

    std::set<std::string> m_reportedUserTextures;
    bool m_warnedSlots = false;       // a material with more textures than slots was reported
    bool m_dirty = true;
    bool m_sceneCleared = false; // the scene target has been cleared once: with `clearenabled` off it is not cleared again
    bool m_animated = false;
    // The keyframed properties stop moving at some time (a Single animation holds its last key): the scene is still from then on, unless
    // something else moves it. `m_lastTime` is the frame before the one being drawn, which is the one that decides.
    double m_animationsEnd = 0.0;
    bool animationsRunning() const { return m_lastTime < m_animationsEnd; }
    bool m_usesPointer = false;
    bool m_usesAudio = false;         // a shader reads the spectrum
    bool m_audioPaused = false;
    unsigned m_volume = 0;
    bool m_pointerEnabled = true;
    bool m_parallaxEnabled = true;
    bool m_parallaxPossible = false;  // the scene enables camera parallax (perhaps through a property)
    bool m_parallaxSettling = false;  // the smoothed displacement has not reached the pointer yet
    Vec2 m_cameraOffset{0.0f, 0.0f};  // the active camera object's offset of the view's centre, in scene units
    float m_cameraZoom = 1.0f;        // and its zoom
    Vec2 m_shake{0.0f, 0.0f};         // the camera's displacement from the shake, in scene units
    CameraBasis m_cameraBasis;        // where the camera is and looks, before the parallax (updateCameraObject)
    float m_cameraFov = 50.0f;        // its vertical field of view (perspective)
    float m_cameraReach = 1.0f;       // its eye to centre distance: the eye's movement for a full parallax displacement
    Mat4 m_view = Mat4::identity();   // world to camera (perspective), or the orthogonal camera's turn about the scene's centre
    std::vector<CameraPathQueue> m_cameraPaths; // per camera object: which of its paths plays
    float m_cameraFadeLevel = 0.0f;   // 0..1: how far the picture has faded to black at the end and start of camera paths (camerafade)
    bool m_cameraPathPlaying = false; // the active camera object moves along a path: every frame is drawn
    bool m_shaking = false;           // the shake is on: every frame is drawn
    Vec2 m_parallax{0.0f, 0.0f};      // the smoothed displacement, in half-scenes
    uint64_t m_lastRevision = 0;
    uint64_t m_scenePasses = 0;
    double m_lastTime = 0.0;
    std::vector<LayerPlan> m_layerPlans;
    std::vector<LayerCache> m_layerCache;
    uint64_t m_staticEpoch = 0;
    uint64_t m_sceneWrites = 0;          // draws recorded onto the scene target this frame (a copy of it older than the last is stale)
    uint64_t m_mipCopyWrites = 0;        // m_sceneWrites when m_mipCopy was taken
    RenderTarget *m_mipCopy = nullptr;   // the scene copy with a mip chain, reused while the scene has not changed this frame
    uint64_t m_layerCacheHits = 0;
    bool m_layerCaching = true;
    bool m_halfSizeParticles = true; // big soft particle systems are drawn at half size
    // What the pass optimiser concluded about one effect pass (keyed by layer, effect and pass): valid while the pass's
    // signature (shader, combos, constants, textures, sizes, quality tier) stays the same.
    struct PassVerdict {
        uint64_t signature = 0;
        uint64_t shape = 0;
        bool known = false;
        bool giveUp = false;     // the signature never settles (an animated constant): stop measuring
        uint32_t changes = 0;
        ProbeOutcome outcome;
    };
    std::unordered_map<uint64_t, PassVerdict> m_passVerdicts;
    std::unique_ptr<PassProbe> m_probe;
    PassOptStats m_passOpt;
    uint32_t m_probesThisFrame = 0;
    // MURASAKI_SCENE_VK_PASS_OPT: 0 turns the optimiser off, `skip` (the default) keeps only the identity skip, `lossless`
    // that and the partial draws, anything else (`full`) adds the reduced passes.
    bool m_skipIdentity = true;
    bool m_partialDraw = true;
    bool m_probeOnly = false;
    bool m_reducePasses = true;
    bool m_reduceChains = true; // godrays and shine work at a quarter of the layer instead of a half
    float m_sceneToTarget = 1.0f; // scene units -> pixels of the scene target
};

} // namespace murasaki::scene
