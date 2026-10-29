#pragma once

// SceneScript: the JavaScript a scene attaches to its objects' properties, run with QuickJS-ng.
//
// Every script is an ES module. The scripts of a scene share one runtime (one memory limit) and one realm: a context with the
// prelude (script/prelude.h) evaluated once, where each script is a module of its own that imports its own names: `thisLayer`
// (the object the script is attached to; one object per scene object, as every script of the realm sees it), `thisObject`,
// `thisScene`, `engine` (its timers, audio buffers and user properties), the timer functions and `createScriptProperties`.
// The realm's globals are `input`, `shared`, `localStorage`, `console`, the vector classes and the WE modules. A script that
// would change what the others see there (it names the global object, assigns to a built-in or a prototype, or declares one of
// its own names itself) gets a realm of its own, where those names are globals (scriptNeedsOwnRealm). A script exports
// `init(value)` (once) and `update(value)` (every frame); for a property script the value is the property's, and what `update`
// returns replaces it.
//
// Sandbox: no files, no network, no processes (QuickJS's libc is not linked, so there is nothing to call); a memory limit
// for the whole runtime; a wall clock budget for init and for each frame's updates, enforced by the interrupt handler.
// A script that throws or runs out of budget is logged once and disabled; the others carry on.
//
// The host side of every object is behind ScriptSceneBridge so the host can be tested with a fake scene.

#include "audio/spectrum.h"
#include "media/mediastate.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace murasaki::scene {

struct ScriptValue {
    enum class Type { Undefined, Bool, Number, String, Vector };
    Type type = Type::Undefined;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::array<double, 4> vec{};
    int count = 0; // components of a vector (2 to 4)

    static ScriptValue ofBool(bool b);
    static ScriptValue ofNumber(double n);
    static ScriptValue ofText(std::string s);
    static ScriptValue ofVector(std::initializer_list<double> v);
};

// What scripts see of the scene. Layer handles are the bridge's own numbers.
class ScriptSceneBridge
{
public:
    virtual ~ScriptSceneBridge() = default;
    virtual ScriptValue get(int layer, const std::string &property) = 0;
    virtual void set(int layer, const std::string &property, const ScriptValue &value) = 0;
    // `spec` is a model path ("models/bar.json") or, when `json` is true, an object description ({"text": ...}).
    // Returns the new layer's handle, or -1.
    virtual int createLayer(const std::string &spec, bool json) = 0;
    virtual int indexOf(int layer) = 0;
    virtual void sort(int layer, int index) = 0;
    virtual int find(const std::string &name) = 0;
    // The object with this id in the scene file (the "id" of its entry, not its draw index), or -1.
    virtual int findById(int /*id*/) { return -1; }
    // A Workshop script (it declares `export let __workshopId = '<id>'`) names files as if its package were the root:
    // `models/bar.json` is `models/workshop/<id>/bar.json` in the scene. The bridge maps the path (unchanged by default).
    virtual std::string resolvePath(const std::string &path, const std::string &/*workshopId*/) { return path; }
    // Whether the point (scene units, y up) is on the object, for the cursor events (only solid objects get them); `local`
    // receives the point in the object's own units from its centre. The default object is never hit.
    virtual bool hitTest(int /*layer*/, double /*x*/, double /*y*/, double * /*localX*/, double * /*localY*/) { return false; }

    // The scene graph: parents and children by handle, the layers in draw order (destroyed ones left out), destruction.
    virtual int parentOf(int /*layer*/) { return -1; }
    // An object's world matrix, column major with the position in 12..14.
    virtual std::array<double, 16> worldMatrix(int /*layer*/) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
    virtual void setParent(int /*layer*/, int /*parent*/, bool /*keepWorldTransform*/) {}
    virtual std::vector<int> childrenOf(int /*layer*/) { return {}; }
    virtual std::vector<int> enumerate() { return {}; }
    virtual bool destroy(int /*layer*/) { return false; }
    // The effects of an image layer: how many, and their properties ("name", "visible", "passes", "constant:<pass>:<key>").
    virtual int effectCount(int /*layer*/) { return 0; }
    virtual ScriptValue effectGet(int /*layer*/, int /*effect*/, const std::string & /*property*/) { return {}; }
    virtual void effectSet(int /*layer*/, int /*effect*/, const std::string & /*property*/, const ScriptValue & /*value*/) {}
    // A script will not run (it did not load, its init failed, or it was stopped): `layer` and `property` are its target's.
    virtual void scriptStopped(int /*layer*/, const std::string & /*property*/) {}
};

struct ScriptTarget {
    int layer = -1;                 // the object the script is attached to
    std::string property;           // the property it drives ("text", "visible", "rate", ...)
    std::string source;
    std::map<std::string, ScriptValue> properties; // `scriptproperties` of the object
    std::map<std::string, std::string> propertyUsers; // script property -> the user property it is bound to
    std::string name;               // for messages
};

// Per frame inputs of scripts.
struct ScriptFrame {
    double time = 0.0;        // seconds since the engine started
    double frametime = 1.0 / 30.0;
    double timeOfDay = 0.0;   // 0..1
    float canvasWidth = 1920.0f, canvasHeight = 1080.0f;
    float screenWidth = 0.0f, screenHeight = 0.0f; // output size in pixels; 0: the canvas
    float cursorX = 0.0f, cursorY = 0.0f; // scene units, y up
    bool cursorDown = false;
    const AudioSpectrum *audio = nullptr;
};

// Why a script cannot share the realm (it names the global object, assigns to a built-in, a prototype or one of its own names, ...);
// empty when it can. A guess from the source: a false alarm costs a realm, a miss lets the script change what the others see.
std::string scriptNeedsOwnRealm(const std::string &source);

class ScriptHost
{
public:
    struct Limits {
        size_t memoryBytes = 96u << 20;   // what the scripts may use between them, over the fixed cost of their realms and scopes
        size_t contextBytes = 384u << 10; // that fixed cost for a realm (a context with the prelude: about 210 KiB, 320 KiB resident)
        size_t scopeBytes = 16u << 10;    // and for a script in the shared realm (its names and module: about 7 KiB)
        size_t maxScripts = 2048;
        double initSeconds = 1.0;    // for one script's module evaluation and init
        double frameSeconds = 0.05;  // for one script's update in a frame
        double frameTotalSeconds = 0.25; // for all the updates of a frame together
    };

    explicit ScriptHost(ScriptSceneBridge &bridge, Limits limits);
    ScriptHost(ScriptSceneBridge &bridge) : ScriptHost(bridge, Limits{}) {}
    ~ScriptHost();
    ScriptHost(const ScriptHost &) = delete;
    ScriptHost &operator=(const ScriptHost &) = delete;

    // Compiles the script and runs its `init` with the property's current value. False (with the reason) when it does
    // not compile or init fails; the script is then not kept.
    bool add(const ScriptTarget &target, const ScriptValue &initial, std::string *error);
    // Loading a scene: while deferred, add() compiles and evaluates a script's module but leaves its `init` for initAll(), which runs
    // every waiting init in the order the scripts were added and returns the (script name, reason) of those that failed (they do not
    // run). Every module is evaluated before any init, as WE does: a library script further down the file defines through `shared`
    // what an earlier script's init calls.
    void setDeferredInits(bool deferred);
    std::vector<std::pair<std::string, std::string>> initAll();
    // Runs every script's `update`. A value a property script returns is written through the bridge.
    void update(const ScriptFrame &frame);

    // The user properties changed (`changed`: only those, `all`: every one, for engine.userProperties): each script that exports
    // applyUserProperties hears it. Call once after every script is added with all of them as changed.
    void applyUserProperties(const std::map<std::string, ScriptValue> &changed, const std::map<std::string, ScriptValue> &all);
    // User properties that changed (by name): every script that bound one of its script properties to it sees the new value in its
    // `scriptProperties` object, ahead of applyUserProperties.
    void applyScriptPropertyChanges(const std::map<std::string, ScriptValue> &changed);
    // Where localStorage is kept between runs (a JSON file of the item's own; its directory is made when first written). The host reads
    // it now and writes it back a second after a script changes the storage, and when it ends. Empty: kept for the run only.
    void setStorageFile(const std::string &path);
    // The app's general settings (the language key) for applyGeneralSettings.
    void applyGeneralSettings(const std::map<std::string, ScriptValue> &changed);

    // The media the host follows (media/mediastate.h), as it was reported at `now` (seconds on the frame clock). A script that exports
    // mediaStatusChanged, mediaPlaybackChanged, mediaPropertiesChanged, mediaThumbnailChanged or mediaTimelineChanged hears what
    // changed at its next update; the timeline once a second while the media plays. A script that starts later is told the
    // present state after its init.
    void setMedia(const MediaState &state, double now);
    // Some script that still runs exports one of the media hooks.
    bool wantsMedia() const;
    // The cover of the track: `has` false for none (the colours are then grey).
    void setMediaThumbnail(bool has, const MediaColors &colors);

    size_t scriptCount() const;
    size_t ownRealmCount() const; // scripts in a realm of their own
    size_t memoryUsed() const;    // what the runtime has allocated, realms included
    size_t disabledCount() const;
    bool hasUpdates() const;   // some script has an `update` (the scene changes without input)
    // Some script that still runs has something to do each frame: an `update`, a cursor or resize hook, a timer or audio buffers. A
    // scene whose scripts are done with their init, or all stopped, is as still as one without scripts.
    bool active() const;
    bool usesAudio() const;    // some script registered audio buffers
    const std::vector<std::string> &messages() const; // everything the scripts logged and every failure

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace murasaki::scene
