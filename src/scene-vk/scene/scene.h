#pragma once

// The typed scene: what scene.json (plus the models and materials it points to) says, reduced to what the renderer
// draws. Milestone 1 understands plain image layers. Everything else the file can contain is recorded in
// `unsupported` with a short reason, which is what `--check` reports and what Murasaki's engine choice acts on.
// Reasons end with the milestone that will lift them ("effects: M2").
//
// Coordinates follow the scene: x to the right, y up, origin at the lower left of the orthogonal projection
// (`general.orthogonalprojection`, a width and a height in scene units). Draw order is file order.

#include "assets/vfs.h"
#include "particles/definition.h"
#include "puppet/bonephysics.h"
#include "puppet/mdl.h"
#include "scene/effect.h"
#include "scene/material.h"
#include "scene/properties.h"
#include "script/scripthost.h"
#include "util/json.h"
#include "util/math.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace murasaki::scene {

// Position of every object, including groups (an object with no drawable of its own that others hang on).
// The attachment points of a puppet layer in the puppet's mesh space, as they stand this frame (the renderer refreshes them from the
// animated and simulated bones); an object whose `attachment` names one of them is placed on it (SceneNode::attachPoses).
struct AttachmentPoses {
    std::vector<Mat4> matrices;
    size_t users = 0; // the objects standing on a point (the renderer refreshes the points only when someone does)
};

struct SceneNode {
    int id = 0;
    int parentId = -1; // -1: none
    std::string name;
    Vec3 origin;
    Vec3 scale{1, 1, 1};
    Vec3 angles; // radians
    // user properties the transform is bound to ("" when literal); see applyUserVectors
    std::string originUser, scaleUser, anglesUser;
    Bound<bool> visible{true, {}};
    // `attachment`: the object stands on a named point of its parent puppet's rig. The point's matrix (rotation included) is
    // applied between the object's own transform and its parent's (docs/scene-vk/puppets.md, "Attachments"). Null: a plain child.
    std::shared_ptr<const AttachmentPoses> attachPoses;
    size_t attachIndex = 0;
};

// A keyframed property (`"alpha": {"animation": {"c0": [{"frame": 0, "value": 1}, ...], "options": {"fps": 30, "length": 60,
// "mode": "single"}}}`): the object's origin, scale or angles (components c0..c2), or an image layer's alpha (c0). Sampled with
// the scene clock; between keyframes the curve is the bezier the keys' handles describe (see AnimationKey; inferred from the files,
// not checked against WE). A property that also has a script keeps its keyframes: the script runs after them each frame (what its
// update returns wins) and may play, pause and stop them (`thisObject.getAnimation()`, docs/scene-vk/scripts.md).
constexpr size_t kMaxPropertyAnimations = 4096;
constexpr size_t kMaxAnimationKeyframes = 4096; // per component
// A keyframe with the editor's bezier handles. A handle is (x, y) in units of the segment it belongs to: x as a share of the frames
// to the neighbouring key, y as a share of the value difference. The default pair, front (1, 0) and back (-1, 0), is an ease in and
// out; a handle that is not `enabled` is the straight line's third.
struct AnimationKey {
    float frame = 0.0f;
    float value = 0.0f;
    bool backOn = false, frontOn = false;
    float backX = -1.0f, backY = 0.0f, frontX = 1.0f, frontY = 0.0f;
};
struct PropertyAnimation {
    enum class Target { Origin, Scale, Angles, Alpha, EffectConstant, CameraZoom, LightIntensity, LightColor, LightRadius, LightExponent, LightInnerCone, LightOuterCone, LightDensity, LightVolumetricsExponent };
    enum class Mode { Loop, Single, Mirror };
    Target target = Target::Alpha;
    size_t index = 0; // a node for the transforms, an image layer for the alpha and the effect constants, a camera object for its zoom,
                      // a light for the values of its own (intensity, colour, ...)
    size_t effect = 0, pass = 0; // EffectConstant: which effect of the layer, which pass of it
    std::string key;             // EffectConstant: the constant, lower-cased
    float fps = 30.0f;
    float length = 1.0f; // frames
    Mode mode = Mode::Single;
    int components = 1;
    std::array<std::vector<AnimationKey>, 3> curves; // sorted by frame
    // `"relative": true`: the keyframe values are offsets added to the property's own value (`base`, the file's `value`)
    bool relative = false;
    std::array<float, 3> base{0.0f, 0.0f, 0.0f};
    std::string name;      // `options.name`: what a script's getAnimation(name) finds
    std::string parentKey; // `options.parent.key`: a linked track plays with its parent, the object's animation of that key
    size_t node = 0;       // the scene node of the object it belongs to
    // Its clock, which scripts steer: the animation's time is `anchor + (scene time - from) * rate` while it plays and `anchor` while
    // it is paused. By default it plays on the scene clock; `options.startpaused` holds it at 0 until a script plays it.
    struct Playback {
        bool playing = true;
        double from = 0.0, anchor = 0.0, rate = 1.0;
    } playback;
    bool released = false; // started paused, but its script does not run: it plays on the scene clock (SceneScripts::scriptStopped)
    double clock(double sceneSeconds) const { return playback.playing ? playback.anchor + (sceneSeconds - playback.from) * playback.rate : playback.anchor; }
    // The animation's time as a frame of its own (wrapped by its mode), and whether a Single one has reached its end: its last key (or
    // `length`), from where its value no longer changes (endSeconds).
    double frameAt(double sceneSeconds) const;
    bool finishedAt(double sceneSeconds) const;
    double endFrame() const;
    // The scene time from which the value no longer changes: a Single animation holds its last key (or the value at `length`), a Loop
    // or Mirror one never stops, a paused one has not changed since it was paused.
    double endSeconds() const;
};
// The value of one component at scene time `timeSeconds` (through the animation's clock); `fallback` when the component has no keyframes
// (a relative animation is then its base).
float sampleAnimation(const PropertyAnimation &animation, int component, double timeSeconds, float fallback);

struct ImageMaterial {
    std::string shader;       // "genericimage2"
    Blending blending = Blending::Translucent;
    std::string texturePath;  // "materials/<name>.tex"
    Bound<float> alpha{1.0f, {}};
    Bound<float> brightness{1.0f, {}};
    Bound<Vec3> color{{1, 1, 1}, {}};
};

// A text object: rendered into a texture by the engine (text/), shown as an image layer.
struct TextSpec {
    std::string value;        // the string shown (a script or a bound property changes it)
    std::string user;         // property the string is bound to
    std::string font;         // "fonts/x.ttf" or "systemfont_name"
    float pointSize = 32.0f;
    std::string horizontal = "center"; // where the text box hangs on the origin: left, center, right
    std::string vertical = "center";   // top, center, bottom
    float padding = 0.0f;
    float maxWidth = 0.0f;    // scene units; 0: no wrapping
};

// One entry of an object's `animationlayers`: an animation of the puppet that plays (looping as the animation says) at
// `rate` times its own speed, weighted by `blend`. Layers add up in proportion to their blends (docs/scene-vk/puppets.md).
constexpr size_t kMaxAnimationLayers = 16; // per object
constexpr float kShapeSize = 1024.0f;       // side of a shape layer at scale 1 when the object has no `size` (an assumption)

struct AnimationLayer {
    uint32_t animation = 0; // the id the model file gives it
    Bound<float> blend{1.0f, {}};
    Bound<float> rate{1.0f, {}};
    Bound<bool> visible{true, {}};
    bool additive = false;
    float frameOffset = 0.0f; // frames ahead of the clock: a script's setFrame
};

struct ImageLayer {
    size_t node = 0;         // index into Scene::nodes
    int colorBlendMode = 0;  // WE's blend mode number (1..32) applied against the scene underneath; 0 is the plain blending
    Vec2 size;               // scene units
    std::string alignment = "center";
    std::string model;       // the model file the object names ("models/x.json"): what a script reads as `image` of its initial config
    ImageMaterial material;
    Bound<float> alpha{1.0f, {}}; // object level, multiplies the material's
    Bound<float> brightness{1.0f, {}};
    Bound<Vec3> color{{1, 1, 1}, {}};
    std::vector<EffectInstance> effects; // applied in order after the base pass (loaded, not yet drawn: see M2 task 5)
    MaterialPass base;                   // the model's material pass in full (shader, combos, constants, textures)
    Vec2 parallaxDepth{1.0f, 1.0f};      // how much camera parallax moves this layer (0: not at all)
    std::string sizeUser, parallaxDepthUser; // user properties the size and the parallax depth are bound to ("" when literal)
    std::optional<TextSpec> text;        // a text object: the texture comes from the string, not from a file
    std::shared_ptr<const Puppet> puppet; // a `puppet` model: the layer is this mesh, not a quad
    std::vector<AnimationLayer> animationLayers;
    std::shared_ptr<AttachmentPoses> attachmentPoses; // the puppet's attachment points, when its model has any
    std::shared_ptr<BonePhysicsState> bonePhysics; // the simulation of a puppet with spring bones (null without any); scripts push it
    int textureFrame = -1;               // a script chose this frame of the sprite sheet (-1: the sheet plays on the clock)
    bool flatColor = false;              // `solidlayer`: a flat colour (the `flat` shader), no texture at all
    bool solid = false;                  // the object takes cursor events (scripts' cursorEnter .. cursorClick)
    bool perspective = false;            // the layer's perspective mode (scripts read and set it; the 2D draw ignores it)
    bool projectLayer = false;           // `projectlayer` model without a size: takes the scene's size once that is known
    bool fullscreen = false;             // `fullscreen` model: covers the whole scene and starts from what is drawn so far
    bool passthrough = false;            // `passthrough` model (compose and project layers): the shader places its own quad and finds
                                         // the picture from the layer's screen position, so it needs the layer's real matrix
};

// A particle system object: the definition (with its children) and what the object overrides.
struct ParticleLayer {
    size_t node = 0;
    std::shared_ptr<const ParticleDefinition> definition;
    Bound<float> alpha{1.0f, {}};
    Bound<float> size{1.0f, {}};
    Bound<float> lifetime{1.0f, {}};
    Bound<float> rate{1.0f, {}};
    Bound<float> speed{1.0f, {}};
    Bound<float> count{1.0f, {}};
    Bound<Vec3> color{{1, 1, 1}, {}}; // `colorn` (0..1), or `color` (0..255) when that is all there is
    Vec2 parallaxDepth{0.0f, 0.0f};
    std::array<std::optional<Vec3>, 8> controlPoints{}; // `controlpointN` of the object's instance override
    // what scripts do to the playback (IParticleSystem): paused freezes the simulation, stopped ends the emission, a burst
    // asks for that many more particles at once (a counter: the renderer spawns what it has not spawned yet)
    bool paused = false;
    bool stopped = false;
    uint32_t burst = 0;
};

// A 3D mesh object (`"model": "models/x/x.mdl"`): a static mesh of one or more parts, each drawn with the material the model
// names for it (WE's `generic4` in almost every library item), with depth. Placed by its node like any other object.
struct MeshLayer {
    size_t node = 0;
    std::string path;                          // the .mdl
    std::shared_ptr<const MeshModel> model;
    std::vector<MaterialPass> materials;       // one per part, in the model's order
    bool solid = false;                        // `solid`: the object takes cursor events (read; no script reaches meshes yet)
    bool castShadow = true;                    // `castshadow` (default on: formats.md, "Shadows")
    std::vector<AnimationLayer> animationLayers; // `animationlayers` of a skinned mesh (docs/scene-vk/puppets.md)
    bool skinning = false;                     // drawn skinned: the model has bones the vertices use, animations and layers to play them
};

// A light object (`"light": "lpoint" | "lspot" | "ltube" | "ldirectional"`, or the older `"point"`): placed by its node like any
// other object (origin, angles, scale, parent, visibility, keyframes, scripts on the transform). What it gives the lit materials is
// packed per frame by render/lights.h; docs/scene-vk/formats.md, "Lights", has the keys and the readings that are assumptions.
// Per type at most kMaxLightsPerType reach the shaders (the rest are named "too many lights").
constexpr size_t kMaxLightsPerType = 32;
constexpr size_t kMaxLegacyLights = 4; // the older `point` lights: g_LightsPosition[4] of genericimage2 and generic2
// Shadow-casting lights a scene can have per type (more are drawn unshadowed and named "too many shadowed lights"): what the shadow
// atlas has room for (render/shadows.h).
constexpr int kMaxShadowPointLights = 4;
constexpr int kMaxShadowSpotLights = 4;
constexpr int kMaxShadowDirectionalLights = 2;
enum class LightType { Point, Spot, Tube, Directional, LegacyPoint };
struct SceneLight {
    size_t node = 0;
    LightType type = LightType::Point;
    Bound<Vec3> color{{1, 1, 1}, {}};
    Bound<float> intensity{1.0f, {}};
    Bound<float> radius{1000.0f, {}};   // scene units: the light reaches no further (not for directional lights; the node's scale is not
                                         // applied to it: a scaled light is named "light scale")
    Bound<float> exponent{2.0f, {}};     // the falloff's exponent: (1 - distance / radius) ^ exponent
    Bound<float> innerCone{20.0f, {}};   // spot: half-angles in degrees, full light inside the inner, none outside the outer
    Bound<float> outerCone{30.0f, {}};
    std::optional<Vec3> controlPoint;    // tube: the other end of the segment, in the light's own space (origin to control point)
    // read and kept, drawn by later pieces (shadows, volumetrics, cookies)
    Bound<float> density{2.0f, {}};
    Bound<float> volumetricsExponent{1.0f, {}};
    Bound<float> lightSourceSize{0.0f, {}};
    std::array<Bound<float>, 3> cascadeDistances{};
    bool castShadow = false;
    bool castVolumetrics = false;
    bool useCookie = false;
    std::string cookie;
};

// A sound object: music or an effect the scene plays while it runs.
constexpr size_t kMaxSoundFiles = 512; // per sound object (the files are played one at a time: only the playing one is in memory)

struct SoundObject {
    size_t node = 0;
    std::string name;
    std::vector<std::string> files; // "sounds/x.mp3": the ones that exist, in the file's order (at most kMaxSoundFiles)
    Bound<float> volume{1.0f, {}};
    // `playbackmode`: "loop" plays the list in order, over and over (one file loops without a gap); "random" plays a random
    // file of the list, rests a random time between `mintime` and `maxtime` seconds, and again. See docs/scene-vk/formats.md.
    bool loop = true;
    bool random = false;
    float minTime = 0.0f;
    float maxTime = 0.0f;
    bool startSilent = false;
    bool playing = true; // a script's play and stop: a sound that is not playing is silent
};

// A script attached to a property of an object (`{"script": ..., "scriptproperties": {...}, "value": ...}`).
struct ScriptBinding {
    bool scene = false;   // a property of the scene itself ("scene.bloomstrength"), not of an object
    size_t node = 0;
    std::string property; // "origin", "visible", "text", "rate", ...
    std::string source;
    std::map<std::string, ScriptValue> properties;
    std::map<std::string, std::string> propertyUsers; // script property -> the user property it is bound to (`{"user": ..., "value": ...}`); `properties` holds the file's value
    std::string name;     // object name and property, for messages
};

// What is drawn, in file order: an image layer, a particle system or a 3D mesh. Meshes test and write the scene's depth buffer
// (cleared every frame) among themselves; 2D layers and particles are drawn over what is there without depth, as before.
struct DrawEntry {
    enum class Kind { Image, Particle, Mesh };
    Kind kind = Kind::Image;
    size_t index = 0; // into Scene::layers, Scene::particles or Scene::meshes
};

// The scene's camera parallax: layers move against the pointer in proportion to their own depth. Properties of the
// scene (`cameraparallax*`), each possibly bound to a user property.
struct CameraParallax {
    Bound<bool> enabled{false, {}};
    Bound<float> amount{1.0f, {}};
    Bound<float> delay{0.0f, {}};          // smoothing: 0 follows the pointer at once
    Bound<float> mouseInfluence{1.0f, {}};
};

// The scene's bloom (`general.bloom*`): bright parts glow. Each value may be bound to a user property.
struct SceneBloom {
    Bound<bool> enabled{false, {}};
    Bound<float> strength{2.0f, {}};
    Bound<float> threshold{0.65f, {}};
    Vec3 tint{1, 1, 1};
    // An HDR scene's bloom (`bloomhdr*`; strength and threshold above are then `bloomhdrstrength` and `bloomhdrthreshold`): how
    // many times the picture is halved, how much each smaller level adds back on the way up, and the soft knee under the threshold
    // (docs/scene-vk/formats.md, "HDR scenes").
    Bound<float> iterations{8.0f, {}};
    Bound<float> scatter{1.619f, {}};
    Bound<float> feather{0.1f, {}};
};

// The scene's fog (`general.fogdistance*` and `general.fogheight*`): a material whose FOG combo is on mixes its colour towards
// the fog's by the distance from the eye (distance fog) or by its world height (height fog). Every value may be bound to a user
// property. See docs/scene-vk/formats.md, "Fog".
struct FogBand {
    Bound<bool> enabled{false, {}};
    Bound<Vec3> color{{0, 0, 0}, {}};
    Bound<float> start{0.0f, {}};
    Bound<float> end{100.0f, {}};
    Bound<float> startDensity{0.0f, {}};
    Bound<float> endDensity{1.0f, {}};
};
struct SceneFog {
    FogBand distance;
    FogBand height;
};

// One path of a camera object (the `path` file, `{"paths": [...]}`): keyframed curves for the camera's `eye`, `center` and `up`
// (components c0..c2, in world units) and its `fov` and `zoom` (a plain list of keys), played for `options.length` frames at
// `options.fps`. A curve the file leaves null is not driven. The keys are the property animations' (sampleAnimation).
constexpr size_t kMaxCameraPaths = 256; // per camera object
struct CameraPath {
    PropertyAnimation eye, center, up, fov, zoom; // Mode::Single, the path's fps and length
    bool visible = true;
    double seconds() const; // how long it plays: length / fps
    bool drivesCamera() const; // it has both an eye and a centre curve (a path without them is skipped)
};
// The camera a path puts at `seconds` from its start; what the path does not drive is left empty.
struct CameraPathPose {
    std::optional<Vec3> eye, center, up;
    std::optional<float> fov, zoom;
};
CameraPathPose sampleCameraPath(const CameraPath &path, double seconds);

// A camera object (`"camera": "default"` on an object): the first visible one sets the view.
// Orthographic scenes (assumptions read from the library: 3276911872, 3326873240, 3605722997): its world origin x and y are an
// offset of the view's centre from the scene's centre (the scripted ones multiply a -1..1 slider by the canvas size, so the
// default 0 keeps the view centred), and `zoom` 1 is the scene's projection, 2 shows half the width and height; `angles`, `fov`
// and the origin's z are not used.
// Perspective scenes: the object is the camera. It sits at its world origin and looks down its own -z (angles 0: the world's
// -z) with its own +y up, under its own `fov`; `zoom` narrows the view as in an orthographic scene; while it has paths they move
// it (docs/scene-vk/formats.md, "Camera").
struct SceneCameraObject {
    size_t node = 0;           // index into Scene::nodes: the origin, the visibility and the parents
    Bound<float> zoom{1.0f, {}};
    Bound<float> fov{50.0f, {}};
    std::string path;          // `path`: a json file of camera paths ({"paths": []} in every orthographic scene of the library)
    std::string queueMode;     // `queuemode`: "random", or in order
    size_t pathCount = 0;      // paths in that file
    std::vector<CameraPath> paths; // the ones read (a perspective scene plays them)
};

struct CameraShake {
    Bound<bool> enabled{false, {}};
    Bound<float> amplitude{0.5f, {}};
    Bound<float> roughness{1.0f, {}};
    Bound<float> speed{3.0f, {}};
};

// The scene's own camera: the `camera` block (eye, center, up: any orientation) with `general`'s fov, nearz and farz. A perspective
// scene draws through it when no camera object is visible; an orthographic one only takes its orientation (formats.md, "Camera").
struct SceneCamera {
    Vec3 eye;
    Vec3 center{0, 0, -1};
    Vec3 up{0, 1, 0};
    float fov = 50.0f;     // degrees, vertical
    std::string fovUser;   // user property the fov is bound to ("" when literal)
    float nearZ = 0.01f;
    float farZ = 10000.0f;
    Bound<bool> fade{false, {}}; // general.camerafade (may be bound to a user property): the picture fades out and in as a camera path ends and the next starts
    Bound<float> zoom{1.0f, {}}; // general.zoom (the editor's camera zoom): multiplies a camera object's own zoom, formats.md "Camera"
};

// A perspective scene has no size of its own: its picture is a canvas of this size (16:9, the aspect the library's perspective
// scenes are made for) that the scaling modes place like an orthographic scene's (formats.md, "Camera").
constexpr Vec2 kPerspectiveCanvas{3840.0f, 2160.0f};

// What scripts steer in the player of a video texture (SceneScript's IVideoTexture) and what the renderer tells back. Kept by the
// texture's name: the layers that show one texture share its decoder, so they share this.
struct VideoControl {
    bool playing = true;
    float rate = 1.0f;          // times the video's own speed; a rate that is not above 0 holds the picture
    bool loop = true;
    std::optional<double> seek; // seconds: setCurrentTime and stop; the renderer takes it
    double position = 0.0;      // reported: where in the video the picture on show is
    double duration = 0.0;      // reported: 0 until the renderer has opened the video
    uint32_t ended = 0;         // reported: how many times the video has reached its end
};

struct Scene {
    Vec2 orthoSize{1920, 1080};           // the orthogonal projection's size; kPerspectiveCanvas in a perspective scene
    bool perspective = false;             // no `orthogonalprojection`: drawn through a perspective camera
    Vec3 clearColor{0, 0, 0};
    // general.transparentsorting (ASSUMED reading, formats.md "Draw order"): opaque meshes first in file order, then everything
    // else back to front by the view depth of its origin
    bool transparentSorting = false;
    Bound<bool> clearEnabled{true, {}};   // general.clearenabled: off, the scene target keeps the last frame under the new one
    Vec3 ambientColor{0.2f, 0.2f, 0.2f};  // general.ambientcolor and skylightcolor: what lit materials read without lights
    Vec3 skylightColor{0.3f, 0.3f, 0.3f};
    SceneCamera camera;
    CameraParallax parallax;
    SceneBloom bloom;
    // `general.hdr`: the scene's buffers hold floating point colour (values above 1 survive until the final combine) and the
    // bloom is the HDR chain. Read once: the buffers' format is chosen when the renderer is built.
    bool hdr = false;
    SceneFog fog;
    CameraShake shake;
    BoneWorld boneWorld; // general.gravitydirection, gravitystrength, windenabled, windstrength, winddirection: what bone physics reads
    std::vector<SceneNode> nodes;
    std::unordered_map<int, size_t> nodeIndexById; // object id -> index in `nodes`, for parent lookups
    std::vector<ImageLayer> layers;
    std::map<std::string, VideoControl> videos; // by the name of the video texture
    std::vector<ParticleLayer> particles;
    std::vector<MeshLayer> meshes;
    std::vector<SceneLight> lights;      // in file order
    std::vector<SceneCameraObject> cameraObjects;
    std::vector<SoundObject> sounds;
    std::vector<ScriptBinding> scripts;
    std::string scriptStorageFile; // where the scripts' localStorage is kept between runs (the engine sets it; empty: for the run only)
    std::vector<PropertyAnimation> animations;
    // The file's value of every vector bound to a user property (origin, scale, angles, size, parallax depth), which
    // applyUserVectors falls back to when the property is missing or unreadable.
    struct UserVector {
        enum class Kind { Origin, Scale, Angles, Size, ParallaxDepth };
        Kind kind = Kind::Origin;
        size_t index = 0; // a node, or an image layer for Size and ParallaxDepth
        Vec3 file;
    };
    std::vector<UserVector> userVectors;
    std::vector<DrawEntry> order; // layers, particles and meshes, in file order
    std::vector<std::string> unsupported; // reasons, in discovery order, no duplicates
    std::vector<std::string> userTextureProperties; // user properties that name a texture slot (default picture drawn; the renderer says when one is set)
    std::vector<std::string> degraded; // drawn without these features, in discovery order, no duplicates

    bool supported() const { return unsupported.empty(); }
};

// Reads `sceneFile` ("scene.json") from the Vfs. Fails only when the file is missing or is not a scene; anything the
// engine cannot draw is a reason in `unsupported`, not a failure; what it draws while leaving a feature out is in
// `degraded`. Texture headers are inspected (video, animation, format) so the verdict is complete without loading
// pixels.
std::optional<Scene> loadScene(const Vfs &vfs, const std::string &sceneFile, std::string *error = nullptr);

// Adds a layer to a scene that is already loaded (what `thisScene.createLayer` does): an image layer from a model file,
// or a text layer from a JSON description ({"text": "...", "pointsize": 32, "font": ..., "color": "r g b", "alpha": 1}).
// The new object is at the origin, visible, last in the draw order. Returns the layer's index in `scene.layers`, or -1
// with the reason in `error`.
int addImageLayerFromModel(Scene &scene, const Vfs &vfs, const std::string &modelPath, std::string *error = nullptr);
int addTextLayer(Scene &scene, const Vfs &vfs, const std::string &json, std::string *error = nullptr);
// An image layer from a script's description (`thisScene.createLayer({image, origin, size, ...})`): see scene.cpp for the keys.
int addDescribedImageLayer(Scene &scene, const Vfs &vfs, const Json &description, std::string *error = nullptr);

// The few project.json fields the engine looks at.
struct ProjectInfo {
    std::string type;       // "scene", "video", "web"; any case
    std::string file;       // the entry file
    std::string dependency; // a preset names the item it customizes
};
// Reads and parses project.json once; nullopt when it is missing or not a JSON object.
std::optional<ProjectInfo> readProject(const Vfs &vfs);

// Gives `props` the defaults of project.json's general.properties for the keys it does not hold yet (what Wallpaper Engine
// reads for a property nobody has set; the GUI's values and `--set-property` are put in afterwards and win).
void applyProjectDefaults(const Vfs &vfs, PropertyStore &props);

// Name of the scene file in an item: project.json's "file", else "scene.json".
std::string sceneFileName(const Vfs &vfs);

// World matrix of a node (parents applied) and whether it is visible (all its ancestors too). Cycles are cut.
Mat4 nodeWorldMatrix(const Scene &scene, size_t node);

// Writes the user properties' values into the bound transforms, sizes and parallax depths: a string "x y z" ("x y" for a
// size or a depth) or an array sets those components, one number sets them all; a property that is missing or does not read as
// numbers leaves the file's value. Angles are taken as the file gives them. Returns whether anything changed.
bool applyUserVectors(Scene &scene, const PropertyStore &props);
bool nodeVisible(const Scene &scene, size_t node, const PropertyStore &props);

// "_rt_imageLayerComposite_<id>_a" (or _b), the composite of another object's image layer as a material names it: the object id.
std::optional<int> layerCompositeId(const std::string &name);

// The index in scene.layers of the image layer of the object with id `id`, if there is one.
std::optional<size_t> imageLayerOfObject(const Scene &scene, int id);

} // namespace murasaki::scene
