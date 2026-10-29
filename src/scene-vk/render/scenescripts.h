#pragma once

// The scene as scripts see it: handles for the objects of a Scene (image and text layers, particle systems, plain
// nodes), properties read and written through them, and layers created at run time. Implements ScriptSceneBridge on top
// of a Scene the renderer owns; every change marks the renderer's flags so the scene is drawn again.
//
// Angles cross the boundary in degrees (scripts) and radians (the file's and the renderer's), colours as 0..1 vectors.

#include "assets/vfs.h"
#include "scene/properties.h"
#include "scene/scene.h"
#include "script/scripthost.h"

#include <map>
#include <memory>
#include <functional>
#include <set>
#include <vector>

namespace murasaki::scene {

struct ScriptFlags {
    bool dirty = false;            // something visible changed: record the scene again
    bool particlesChanged = false; // a particle override changed
    bool textChanged = false;      // a text object's string, size or font changed
    bool animationsChanged = false; // a script steered a keyframe animation (see PropertyAnimation::Playback)
};

// The file an item's localStorage is kept in between runs: <shader cache>/script-storage/<item folder>.json (the engine's cache
// directory, which the sandbox lets it write). Empty when there is no cache directory.
std::string scriptStorageFile(const std::string &itemDir);

// The layers scripts made that are alive at once, all scripts together (a script's own allowance, 256 in script/scripthost.cpp, is smaller, so
// two scripts that each build a grid fit), and the layers they may create over the scene's lifetime: a destroyed layer is only hidden (its
// node, order entry and handle stay, as other things refer to layers by index), so this bounds what a script that creates and destroys
// every frame can make the scene hold.
constexpr size_t kMaxScriptLayers = 512;
constexpr size_t kMaxScriptLayerCreations = 4096;

class SceneScripts final : public ScriptSceneBridge
{
public:
    SceneScripts(Scene &scene, const Vfs &vfs, const PropertyStore &props, ScriptFlags &flags);
    ~SceneScripts() override;

    // Limits for the host (before start()); --check uses short ones.
    void setLimits(const ScriptHost::Limits &limits) { m_limits = limits; }
    // Compiles and initialises every script of the scene (init may create layers). Failures are logged, the scripts
    // concerned are left out, and the scene carries on.
    void start();
    void update(const ScriptFrame &frame);
    bool hasUpdates() const;
    bool active() const; // see ScriptHost::active
    bool usesAudio() const;
    bool wantsMedia() const { return m_host && m_host->wantsMedia(); }
    // engine.openUserShortcut(name): the handler sends the request to the host and says whether it did (see shortcutLaunch).
    void setShortcutLaunchHandler(std::function<bool(const std::string &)> handler) { m_shortcutLaunch = std::move(handler); }
    // The media the host follows, for the scripts' media hooks (see ScriptHost::setMedia).
    void setMedia(const MediaState &state, double now) { if (m_host) m_host->setMedia(state, now); }
    void setMediaThumbnail(bool has, const MediaColors &colors) { if (m_host) m_host->setMediaThumbnail(has, colors); }
    size_t failed() const { return m_failed; }
    size_t disabledCount() const { return m_host ? m_host->disabledCount() : 0; } // scripts stopped while running
    // What the scripts cost: the runtime's memory, the scripts in a realm of their own, and the time of the last frame's updates.
    size_t memoryUsed() const;
    size_t ownRealms() const;
    double lastUpdateSeconds() const { return m_lastUpdate; }
    double startSeconds() const { return m_startSeconds; } // what start() took: modules, inits and the first announcements
    // One line per script that did not load ("name: reason"), for --check.
    const std::vector<std::string> &failures() const { return m_failures; }
    const std::vector<std::string> &messages() const;

    // ScriptSceneBridge
    ScriptValue get(int layer, const std::string &property) override;
    void set(int layer, const std::string &property, const ScriptValue &value) override;
    int createLayer(const std::string &spec, bool json) override;
    int indexOf(int layer) override;
    void sort(int layer, int index) override;
    int find(const std::string &name) override;
    int findById(int id) override;
    std::string resolvePath(const std::string &path, const std::string &workshopId) override;
    bool hitTest(int layer, double x, double y, double *localX, double *localY) override;
    int parentOf(int layer) override;
    void setParent(int layer, int parent, bool keepWorldTransform) override;
    std::vector<int> childrenOf(int layer) override;
    std::vector<int> enumerate() override;
    bool destroy(int layer) override;
    std::array<double, 16> worldMatrix(int layer) override;
    int effectCount(int layer) override;
    ScriptValue effectGet(int layer, int effect, const std::string &property) override;
    void effectSet(int layer, int effect, const std::string &property, const ScriptValue &value) override;
    void scriptStopped(int layer, const std::string &property) override;

private:
    struct Handle {
        enum class Kind { Node, Image, Particle } kind = Kind::Node;
        size_t index = 0; // into nodes, layers or particles
    };
    int handleFor(Handle::Kind kind, size_t index);
    int handleForNode(size_t node);
    ScriptValue sceneGet(const std::string &property);
    bool sceneSet(const std::string &property, const ScriptValue &value);
    std::string initialConfig(const Handle &h) const;
    // the sound object of a node, or null
    SoundObject *soundOf(size_t node);
    bool setExtra(int layer, const Handle &h, const std::string &p, const ScriptValue &v);
    // The player of an image layer's video texture (IVideoTexture): the texture's name ("" when the layer shows no video), and the
    // "video.*" properties that read and steer it through Scene::videos.
    std::string videoNameOf(const ImageLayer &l);
    ScriptValue videoGet(const ImageLayer &l, const std::string &p);
    bool videoSet(const ImageLayer &l, const std::string &p, const ScriptValue &v);
    std::map<std::string, bool> m_videoTextures; // texture name -> whether its header says video
    // A script asks for the application of the property `name`: false when the name is not a usable key or the property holds nothing
    // (the user chose no application), else what the handler says. The scene picks a property, never an application.
    bool shortcutLaunch(const std::string &name);
    std::function<bool(const std::string &)> m_shortcutLaunch;
    struct Rig {
        std::vector<AnimationLayer> *layers = nullptr;
        const Puppet *puppet = nullptr; // its bones and animations (null for a mesh that is not skinned)
        BonePhysicsState *physics = nullptr; // the spring bones' simulation of a puppet layer (null without any)
    };
    Rig rigOf(const Handle &h);
    ScriptValue rigGet(const Rig &rig, const std::string &p) const;
    bool rigSet(const Rig &rig, const std::string &p, const ScriptValue &v);
    bool createAnimationLayer(const Rig &rig, const std::string &json);
    // The keyframe animations of an object (IAnimation): "animation:<name>" finds one by name (the first when the name is empty),
    // "animation@<property>" the one on that property, "animation.<i>.<field>" reads or steers animation i of the scene.
    ScriptValue animationGet(const Handle &h, const std::string &p) const;
    void animationSet(const Handle &h, const std::string &p, const ScriptValue &v);
    // The user properties that changed since the last call (all of them the first time) go to the scripts' applyUserProperties.
    void announceProperties();
    size_t nodeOf(const Handle &h) const;

    Scene &m_scene;
    const Vfs &m_vfs;
    const PropertyStore &m_props;
    ScriptFlags &m_flags;
    std::vector<Handle> m_handles;
    std::unique_ptr<ScriptHost> m_host;
    ScriptHost::Limits m_limits;
    std::vector<std::string> m_failures;
    size_t m_created = 0;                 // the layers scripts made that are alive (at most kMaxScriptLayers)
    size_t m_totalCreated = 0;            // ... and all they ever made (at most kMaxScriptLayerCreations)
    bool m_layerLimitSaid = false, m_creationLimitSaid = false; // each reported once
    std::set<int> m_madeByScripts;        // their handles
    size_t m_failed = 0;
    std::map<std::string, PropertyValue> m_announced; // the user properties scripts were last told
    uint64_t m_revision = ~0ull;
    double m_time = 0.0;                                 // the clock of the last update (puppet animation frames)
    double m_lastUpdate = 0.0, m_updateTotal = 0.0, m_updateMax = 0.0; // seconds the hosts' updates took
    double m_startSeconds = 0.0;
    uint64_t m_updates = 0;
    std::set<int> m_destroyed;                           // handles of layers a script destroyed
    std::map<std::string, ScriptValue> m_stored;         // properties that are only kept: the engine has nothing to apply them to
    std::vector<std::string> m_initial;                  // by handle: the object as it was in the file (getInitialLayerConfig)
};

} // namespace murasaki::scene
