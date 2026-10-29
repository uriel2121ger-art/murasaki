#include "script/scripthost.h"

#include "script/prelude.h"
#include "util/log.h"

extern "C" {
#include <quickjs.h>
}

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

#include <unistd.h>

namespace murasaki::scene {

ScriptValue ScriptValue::ofBool(bool b)
{
    ScriptValue v;
    v.type = Type::Bool;
    v.boolean = b;
    return v;
}
ScriptValue ScriptValue::ofNumber(double n)
{
    ScriptValue v;
    v.type = Type::Number;
    v.number = n;
    return v;
}
ScriptValue ScriptValue::ofText(std::string s)
{
    ScriptValue v;
    v.type = Type::String;
    v.text = std::move(s);
    return v;
}
ScriptValue ScriptValue::ofVector(std::initializer_list<double> list)
{
    ScriptValue v;
    v.type = Type::Vector;
    for (double d : list) {
        if (v.count < 4) {
            v.vec[static_cast<size_t>(v.count++)] = d;
        }
    }
    return v;
}

namespace {

using Clock = std::chrono::steady_clock;

// A realm: a context with the prelude evaluated, and the prelude's entry points for the host.
struct Realm {
    JSContext *ctx = nullptr;
    JSValue makeScope = JS_UNDEFINED, cursorEvent = JS_UNDEFINED, eventArg = JS_UNDEFINED, announceMedia = JS_UNDEFINED;
    // Vec2, Vec3 and Vec4 as the prelude made them (a script that replaces the global does not change what the host hands out); undefined
    // after one failed to construct, and values are then plain arrays
    std::array<JSValue, 3> vec{JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};

    void free()
    {
        if (ctx) {
            for (JSValue *v : {&makeScope, &cursorEvent, &eventArg, &announceMedia, &vec[0], &vec[1], &vec[2]}) {
                JS_FreeValue(ctx, *v);
                *v = JS_UNDEFINED;
            }
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
    }
};

struct Script {
    ScriptTarget target;
    size_t id = 0;
    JSContext *ctx = nullptr;     // its realm's: the shared one, or `own`'s
    Realm *realm = nullptr;
    std::unique_ptr<Realm> own;   // a script that changes what other scripts would see runs in a realm of its own
    size_t fixedBytes = 0;        // what its realm or its scope costs, over the memory the scripts may use
    JSValue update = JS_UNDEFINED;
    JSValue scope = JS_UNDEFINED; // the prelude's makeScope result: its names and the hooks below
    JSValue names = JS_UNDEFINED; // thisLayer, thisObject, engine, ...: globals of its own realm, or imports of '__scope/<id>'
    JSValue frame = JS_UNDEFINED; // the scope's hooks
    JSValue usesAudio = JS_UNDEFINED;
    JSValue ns = JS_UNDEFINED; // the module's namespace: the event functions it exports
    JSValue hasTimers = JS_UNDEFINED;
    bool needsTick = false;    // has audio buffers or timers: only then is its scope's `frame` called every frame
    bool hasResize = false;    // exports resizeScreen
    bool hasMedia = false;     // exports one of the media hooks
    size_t mediaPos = 0;       // the next entry of the media log this script has not heard
    bool hasCursor = false;    // exports one of the cursor events
    bool cursorInside = false, cursorPressed = false, cursorPressedInside = false;
    double screenW = -1.0, screenH = -1.0; // the output size this script was last told (resizeScreen)
    // events that did not fit in the frame ceiling of the call that had them: delivered at the next update, first
    std::map<std::string, ScriptValue> pendingUser, pendingScriptProperties, pendingSettings;
    bool userPending = false;
    double lastCursorX = -1e30, lastCursorY = -1e30;
    bool disabled = false;
    bool audio = false;
    bool initPending = false; // added while inits are deferred: initAll() runs its init with `initial`
    ScriptValue initial;
    std::string workshopId;  // from `__workshopId` in the source, empty when the script has none
    int layersCreated = 0;   // by this script, that are alive
    bool layerLimitSaid = false; // the limit was reported (once)
    int overruns = 0;        // consecutive frames that ran out of budget
    size_t logBytes = 0;     // what the script has written to the log
    bool logFull = false;
};

constexpr size_t kMaxLogMessage = 4096;      // bytes kept of one message
constexpr size_t kMaxLogPerScript = 65536;   // bytes of log one script may write in all
constexpr int kMaxLayersPerScript = 256;     // layers one script may create (a grid maker builds a line per column and row: 3802432712 needs about 80)
constexpr int kMaxOverruns = 3;              // consecutive over-budget frames before a script is stopped
constexpr size_t kMaxSourceBytes = 1u << 20; // a script's source (the library's biggest is 237 KB)

// The names a script has of its own (see makeScope in the prelude).
constexpr const char *kScopeNames[] = {"thisLayer", "thisObject", "thisScene", "engine", "setTimeout", "setInterval", "clearTimeout", "clearInterval",
                                       "createScriptProperties"};

// Text of an exception; takes ownership of it.
std::string describe(JSContext *ctx, JSValue exception)
{
    std::string out;
    const char *text = JS_ToCString(ctx, exception);
    out = text ? text : "an exception";
    JS_FreeCString(ctx, text);
    // only an object has a stack: asking null or a number for one raises a TypeError of its own that nobody would collect
    JSValue stack = JS_IsObject(exception) ? JS_GetPropertyStr(ctx, exception, "stack") : JS_UNDEFINED;
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
        if (const char *s = JS_ToCString(ctx, stack)) {
            if (*s) {
                out += "\n";
                out += s;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
    return out;
}

} // namespace

namespace {

bool identStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool identChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// The objects every script of the shared realm reaches by name: the standard library's and the prelude's.
bool isSharedName(std::string_view w)
{
    static constexpr std::string_view kNames[] = {
        "Math", "JSON", "Object", "Array", "String", "Number", "Boolean", "Date", "Promise", "Map", "Set", "WeakMap", "WeakSet", "RegExp", "Symbol",
        "Error", "TypeError", "RangeError", "Float32Array", "Float64Array", "Int32Array", "Uint8Array", "console", "input", "localStorage", "Vec2",
        "Vec3", "Vec4", "Mat3", "Mat4", "WEMath", "WEColor", "WEVector", "CameraTransforms", "CursorEvent", "AnimationEvent", "MediaPlaybackEvent",
        "IModelData"};
    return std::find(std::begin(kNames), std::end(kNames), w) != std::end(kNames);
}
bool isScopeName(std::string_view w)
{
    return std::find(std::begin(kScopeNames), std::end(kScopeNames), w) != std::end(kScopeNames);
}

} // namespace

std::string scriptNeedsOwnRealm(const std::string &src)
{
    const size_t n = src.size();
    const auto skipSpace = [&](size_t k) {
        while (k < n && std::isspace(static_cast<unsigned char>(src[k]))) {
            ++k;
        }
        return k;
    };
    const auto identAt = [&](size_t k) {
        size_t e = k;
        if (e < n && identStart(src[e])) {
            while (e < n && identChar(src[e])) {
                ++e;
            }
        }
        return std::string_view(src).substr(k, e - k);
    };
    // an assignment operator (not a comparison or an arrow) at k
    const auto assigns = [&](size_t k) {
        if (k >= n) {
            return false;
        }
        if (src[k] == '=') {
            return k + 1 >= n || (src[k + 1] != '=' && src[k + 1] != '>');
        }
        static constexpr std::string_view kOps[] = {"+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?=", "++", "--"};
        const std::string_view rest = std::string_view(src).substr(k, 4);
        return std::any_of(std::begin(kOps), std::end(kOps), [&](std::string_view op) { return rest.substr(0, op.size()) == op; });
    };
    // past a chain of `.name` and `[...]` from k. `computed` tells whether it has a `[...]`, `prototype` whether it names a prototype; a
    // bracket that does not close within 256 characters ends the scan there (the chain counts as members)
    const auto chain = [&](size_t k, bool *members, bool *prototype, bool *computed) {
        for (;;) {
            k = skipSpace(k);
            if (k < n && src[k] == '.' && src.compare(k, 3, "...") != 0) {
                const std::string_view name = identAt(skipSpace(k + 1));
                if (name.empty()) {
                    return k;
                }
                *members = true;
                *prototype = *prototype || name == "prototype" || name == "__proto__";
                k = skipSpace(k + 1) + name.size();
            } else if (k < n && src[k] == '[') {
                *members = true;
                *computed = true;
                int depth = 0;
                const size_t limit = std::min(n, k + 256);
                for (; k < limit; ++k) {
                    depth += src[k] == '[' ? 1 : src[k] == ']' ? -1 : 0;
                    if (depth == 0) {
                        ++k;
                        break;
                    }
                }
                if (depth != 0) {
                    return k;
                }
            } else {
                return k;
            }
        }
    };
    for (size_t i = 0; i < n;) {
        if (!identStart(src[i]) || (i > 0 && identChar(src[i - 1]))) {
            ++i;
            continue;
        }
        const std::string_view w = identAt(i);
        const size_t end = i + w.size();
        size_t before = i;
        while (before > 0 && std::isspace(static_cast<unsigned char>(src[before - 1]))) {
            --before;
        }
        const bool member = before > 0 && src[before - 1] == '.';
        const size_t next = skipSpace(end);
        if (w == "globalThis" || w == "Reflect" || w == "setPrototypeOf" || w == "getPrototypeOf" || w == "__defineGetter__" || w == "__defineSetter__") {
            return "uses " + std::string(w);
        }
        if ((w == "eval" || w == "Function") && !member && next < n && src[next] == '(') {
            return "calls " + std::string(w);
        }
        // x.constructor(...) reaches Function from any function
        if (w == "constructor" && member && next < n && src[next] == '(') {
            return "calls a constructor";
        }
        // a prototype is only read in place (`Object.prototype.hasOwnProperty.call(...)`); any other use may keep or change it
        if (w == "prototype" || w == "__proto__") {
            size_t k = next;
            std::string_view method;
            if (k < n && src[k] == '.') {
                method = identAt(skipSpace(k + 1));
                k = skipSpace(skipSpace(k + 1) + method.size());
            }
            std::string_view borrow;
            if (!method.empty() && k < n && src[k] == '.') {
                borrow = identAt(skipSpace(k + 1));
                k = skipSpace(skipSpace(k + 1) + borrow.size());
            }
            if (!(member && (borrow == "call" || borrow == "apply" || borrow == "bind") && k < n && src[k] == '(')) {
                return "uses " + std::string(w);
            }
        }
        // Object.defineProperty(Math, ...), Object.assign(Vec3, ...): the first argument is shared
        if (member && (w == "defineProperty" || w == "defineProperties" || w == "assign" || w == "freeze" || w == "seal" || w == "preventExtensions")) {
            if (next < n && src[next] == '(') {
                const size_t a = skipSpace(next + 1);
                const std::string_view first = identAt(a);
                if (!first.empty() && (isSharedName(first) || first == "shared" || first == "globalThis")) {
                    return "changes " + std::string(first) + " with " + std::string(w);
                }
            }
        }
        const bool sharedName = isSharedName(w);
        if (!member && (sharedName || isScopeName(w) || w == "shared")) {
            bool members = false, prototype = false, computed = false;
            const size_t k = skipSpace(sharedName ? chain(end, &members, &prototype, &computed) : end);
            if (sharedName && computed) {
                return "reaches into " + std::string(w) + " by a computed name";
            }
            // the name itself (a global of the realm, or one of the script's own: an import there, read only), or a member of a shared object
            if (assigns(k)) {
                return "assigns to " + std::string(w);
            }
            if (members && src.compare(before >= 6 ? before - 6 : 0, 6, "delete") == 0) {
                return "deletes from " + std::string(w);
            }
        }
        i = end;
    }
    return {};
}

struct ScriptHost::Impl {
    ScriptSceneBridge &bridge;
    Limits limits;
    JSRuntime *rt = nullptr;
    std::vector<std::unique_ptr<Script>> scripts;
    std::vector<std::string> messages;
    std::vector<uint8_t> preludeBytecode; // the prelude, compiled by the first context
    double curFrame[10] = {1.0 / 30.0, 0.0, 0.0, 1920.0, 1080.0, 0.0, 0.0, 0.0, 1920.0, 1080.0}; // frametime, time, time of day, canvas, cursor, down, screen
    Clock::time_point deadline = Clock::time_point::max();
    Clock::time_point frameDeadline = Clock::time_point::max(); // the whole frame's ceiling over every script
    bool interrupted = false;      // the interrupt handler stopped the running script
    bool overBudget = false;       // ... because its own deadline passed (not the frame's ceiling): only that counts against the script
    size_t nextStart = 0;          // the script a frame starts with: after a frame the ceiling cut, the first that did not run
    std::map<std::string, ScriptValue> lastAllUser; // every user property, for deferred applyUserProperties
    bool deferInits = false; // add() evaluates the module only; initAll() runs the inits
    // The media the host follows, and the events scripts have to hear about it: a log, each script knows how far it has heard.
    struct MediaRecord {
        const char *hook;
        std::string json;
    };
    MediaState media;
    double mediaAt = 0.0;       // the frame clock when `media` was reported
    double lastTimeline = -1e9; // ... and when the last timeline event was logged
    double lastClock = 0.0;     // the frame clock of the last update
    bool mediaThumbnail = false;
    MediaColors mediaThumbnailColors;
    std::deque<MediaRecord> mediaLog;
    size_t mediaBase = 0;       // the absolute index of mediaLog.front()
    static constexpr size_t kMaxMediaLog = 256;
    void logMedia(const char *hook, std::string json);
    std::string mediaSnapshotJson() const;
    void tickMedia(double now);
    bool deliverMedia(Script &s);
    JSValue shared = JS_UNDEFINED;
    JSValue storage = JS_UNDEFINED; // localStorage's data: {global: {}, screen: {}} of JSON texts, shared like `shared`
    // The realm the scripts share (each a module with names of its own, see makeScope); it owns `shared` and the storage and lives as
    // long as the runtime, so they never outlive their realm. A script that would change what the others see gets a realm of its own.
    Realm sharedRealm;
    Script *current = nullptr;   // the script the host is running: the natives act for it
    std::map<int, Script *> layerOwner; // which script made a layer (createLayer): its count goes down when the layer is destroyed
    Script *compiling = nullptr; // the script whose module is being loaded: '__scope/<id>' is its names
    size_t nextId = 0;
    size_t fixedBytes = 0;       // the realms' and scopes' fixed cost, over limits.memoryBytes
    std::string storageFile;     // where localStorage is kept between runs ("" for the run only)
    bool storageDirty = false;
    Clock::time_point storageChanged{};
    size_t ownRealms = 0;        // scripts with a realm of their own

    // A call's argument as one object per realm: the host's map is copied into each realm once, not once a script. Only for arguments the
    // scope converts before a script sees them (a script never holds this object).
    struct RealmObjects {
        const std::map<std::string, ScriptValue> &values;
        std::vector<std::pair<JSContext *, JSValue>> made;
        explicit RealmObjects(const std::map<std::string, ScriptValue> &v) : values(v) {}
        RealmObjects(const RealmObjects &) = delete;
        RealmObjects &operator=(const RealmObjects &) = delete;
        ~RealmObjects()
        {
            for (auto &[ctx, v] : made) {
                JS_FreeValue(ctx, v);
            }
        }
        JSValue in(JSContext *ctx)
        {
            for (auto &[c, v] : made) {
                if (c == ctx) {
                    return JS_DupValue(ctx, v);
                }
            }
            JSValue o = objectOf(ctx, values);
            made.emplace_back(ctx, o);
            return JS_DupValue(ctx, o);
        }
    };

    static void merge(std::map<std::string, ScriptValue> &into, const std::map<std::string, ScriptValue> &from)
    {
        for (const auto &[k, v] : from) {
            into[k] = v;
        }
    }

    void deliverUser(Script &s, JSValue changed, JSValue all, Clock::time_point ceiling); // takes the two values
    void deliverScriptProperties(Script &s, const std::map<std::string, ScriptValue> &own, Clock::time_point ceiling);
    void deliverSettings(Script &s, const std::map<std::string, ScriptValue> &changed, Clock::time_point ceiling);
    bool deliverPending(Script &s, Clock::time_point ceiling);
    Clock::time_point ceilingFromNow() const;
    void endCall(); // the deadlines off; the storage written when it is due

    // The script the natives act for while the host runs one.
    struct Running {
        Impl &h;
        Script *previous;
        Running(Impl &host, Script *s) : h(host), previous(host.current) { h.current = s; }
        ~Running() { h.current = previous; }
        Running(const Running &) = delete;
        Running &operator=(const Running &) = delete;
    };

    Impl(ScriptSceneBridge &b, Limits l) : bridge(b), limits(l) {}

    void note(const std::string &m)
    {
        messages.push_back(m);
        if (messages.size() > 200) {
            messages.erase(messages.begin());
        }
    }

    // ---- value conversion ----
    static ScriptValue fromJs(JSContext *ctx, JSValueConst v)
    {
        ScriptValue out;
        if (JS_IsBool(v)) {
            return ScriptValue::ofBool(JS_ToBool(ctx, v) != 0);
        }
        if (JS_IsNumber(v)) {
            double d = 0;
            JS_ToFloat64(ctx, &d, v);
            return ScriptValue::ofNumber(d);
        }
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            out = ScriptValue::ofText(s ? s : "");
            JS_FreeCString(ctx, s);
            return out;
        }
        if (JS_IsObject(v)) {
            out.type = ScriptValue::Type::Vector;
            if (JS_IsArray(v)) {
                JSValue len = JS_GetPropertyStr(ctx, v, "length");
                int32_t n = 0;
                JS_ToInt32(ctx, &n, len);
                JS_FreeValue(ctx, len);
                for (int32_t i = 0; i < n && i < 4; ++i) {
                    JSValue e = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
                    double d = 0;
                    JS_ToFloat64(ctx, &d, e);
                    JS_FreeValue(ctx, e);
                    out.vec[static_cast<size_t>(i)] = d;
                    out.count = i + 1;
                }
            } else {
                static const char *const keys[4] = {"x", "y", "z", "w"};
                for (int i = 0; i < 4; ++i) {
                    JSValue e = JS_GetPropertyStr(ctx, v, keys[i]);
                    if (JS_IsNumber(e)) {
                        double d = 0;
                        JS_ToFloat64(ctx, &d, e);
                        out.vec[static_cast<size_t>(i)] = d;
                        out.count = i + 1;
                    }
                    JS_FreeValue(ctx, e);
                }
            }
            if (out.count < 2) {
                return ScriptValue{};
            }
            return out;
        }
        return out;
    }

    static JSValue toJs(JSContext *ctx, const ScriptValue &v)
    {
        switch (v.type) {
        case ScriptValue::Type::Bool:
            return JS_NewBool(ctx, v.boolean);
        case ScriptValue::Type::Number:
            return JS_NewFloat64(ctx, v.number);
        case ScriptValue::Type::String:
            return JS_NewStringLen(ctx, v.text.data(), v.text.size());
        case ScriptValue::Type::Vector: {
            JSValue a = JS_NewArray(ctx);
            for (int i = 0; i < v.count; ++i) {
                JS_SetPropertyUint32(ctx, a, static_cast<uint32_t>(i), JS_NewFloat64(ctx, v.vec[static_cast<size_t>(i)]));
            }
            return a;
        }
        default:
            return JS_UNDEFINED;
        }
    }

    static JSValue objectOf(JSContext *ctx, const std::map<std::string, ScriptValue> &values)
    {
        JSValue o = JS_NewObject(ctx);
        for (const auto &[name, value] : values) {
            JS_SetPropertyStr(ctx, o, name.c_str(), toJs(ctx, value));
        }
        return o;
    }

    // ---- natives ----
    static Impl &host(JSContext *ctx) { return *static_cast<Impl *>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx))); }

    static std::string str(JSContext *ctx, JSValueConst v)
    {
        const char *s = JS_ToCString(ctx, v);
        std::string out = s ? s : "";
        JS_FreeCString(ctx, s);
        return out;
    }

    // the clock, canvas and cursor of the frame being run, read by the getters of `engine` and `input` (a script that does not read
    // them costs nothing per frame)
    static JSValue nativeFrame(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t i = 0;
        if (argc > 0) {
            JS_ToInt32(ctx, &i, argv[0]);
        }
        return JS_NewFloat64(ctx, i >= 0 && i < 10 ? host(ctx).curFrame[i] : 0.0);
    }

    // the UTF-8 length of a string (localStorage's quota)
    static JSValue nativeUtf8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t length = 0;
        if (argc > 0) {
            if (const char *text = JS_ToCStringLen(ctx, &length, argv[0])) {
                JS_FreeCString(ctx, text);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
        }
        return JS_NewFloat64(ctx, static_cast<double>(length));
    }

    // localStorage changed: written to its file a second later (one write for a burst of changes)
    static JSValue nativeStored(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        Impl &h = host(ctx);
        if (!h.storageDirty) {
            h.storageDirty = true;
            h.storageChanged = Clock::now();
        }
        return JS_UNDEFINED;
    }

    // The stored localStorage of a previous run: {"global": {key: json text}, "screen": {...}}, at most 1 MiB. A file that is not that
    // is kept beside as <file>.bad (the next write would lose it) and the storage starts empty.
    void loadStorage()
    {
        JSContext *ctx = sharedRealm.ctx;
        std::ifstream in(storageFile, std::ios::binary);
        if (!ctx || !in) {
            return;
        }
        std::string text((1u << 20) + 1, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(in.gcount()));
        in.close();
        const auto setAside = [&](const std::string &why) {
            std::error_code ec;
            std::filesystem::rename(storageFile, storageFile + ".bad", ec);
            note("localStorage: " + storageFile + " " + why + "; kept as .bad, starting empty");
            logError("localStorage: {} {}; kept as .bad, starting empty", storageFile, why);
        };
        if (text.size() > (1u << 20)) {
            setAside("is over 1 MiB");
            return;
        }
        JSValue data = JS_ParseJSON(ctx, text.c_str(), text.size(), "localStorage");
        if (JS_IsException(data) || !JS_IsObject(data)) {
            setAside("is not readable: " + (JS_IsException(data) ? describe(ctx, JS_GetException(ctx)) : std::string("not an object")));
            JS_FreeValue(ctx, data);
            return;
        }
        for (const char *location : {"global", "screen"}) {
            JSValue from = JS_GetPropertyStr(ctx, data, location);
            JSValue to = JS_GetPropertyStr(ctx, storage, location);
            JSPropertyEnum *keys = nullptr;
            uint32_t count = 0;
            if (JS_IsObject(from) && JS_GetOwnPropertyNames(ctx, &keys, &count, from, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < count; ++i) {
                    JSValue v = JS_GetProperty(ctx, from, keys[i].atom);
                    if (JS_IsString(v)) {
                        JS_SetProperty(ctx, to, keys[i].atom, v);
                    } else {
                        JS_FreeValue(ctx, v);
                    }
                }
                JS_FreePropertyEnum(ctx, keys, count);
            }
            JS_FreeValue(ctx, from);
            JS_FreeValue(ctx, to);
        }
        JS_FreeValue(ctx, data);
    }

    void writeStorage()
    {
        JSContext *ctx = sharedRealm.ctx;
        if (storageFile.empty() || !ctx) {
            storageDirty = false;
            return;
        }
        JSValue json = JS_JSONStringify(ctx, storage, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsString(json)) {
            // out of memory: nothing is written, and it is tried again a second later
            if (JS_IsException(json)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, json);
            storageChanged = Clock::now();
            return;
        }
        storageDirty = false;
        const std::string text = str(ctx, json);
        JS_FreeValue(ctx, json);
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(storageFile).parent_path(), ec);
        const std::string temp = storageFile + ".tmp." + std::to_string(::getpid());
        bool ok = false;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            ok = static_cast<bool>(out);
        }
        if (ok) {
            fs::rename(temp, storageFile, ec);
            ok = !ec;
        }
        if (!ok) {
            fs::remove(temp, ec);
            note("localStorage: cannot write " + storageFile + ": kept for this run only");
            logError("localStorage: cannot write {}: kept for this run only", storageFile);
            storageFile.clear();
        }
    }

    // a script that registers audio buffers or sets a timer gets its `__frame` called each frame from then on
    static JSValue nativeWake(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (Script *script = host(ctx).current) {
            script->needsTick = true;
            script->audio = script->audio || (argc > 0 && JS_ToBool(ctx, argv[0]) != 0);
        }
        return JS_UNDEFINED;
    }

    static JSValue nativeLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Script *script = host(ctx).current;
        if (argc < 2 || !script || script->logFull) {
            return JS_UNDEFINED;
        }
        std::string text = str(ctx, argv[1]);
        if (text.size() > kMaxLogMessage) {
            text.resize(kMaxLogMessage);
            text += "...";
        }
        script->logBytes += text.size() + 1;
        if (script->logBytes > kMaxLogPerScript) {
            script->logFull = true;
            text = "log limit reached, the rest is dropped";
        }
        host(ctx).note("[" + script->target.name + " " + str(ctx, argv[0]) + "] " + text);
        return JS_UNDEFINED;
    }
    static JSValue nativeGet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 2) {
            return JS_UNDEFINED;
        }
        int32_t layer = -1;
        JS_ToInt32(ctx, &layer, argv[0]);
        return toJs(ctx, host(ctx).bridge.get(layer, str(ctx, argv[1])));
    }
    static JSValue nativeSet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 3) {
            return JS_UNDEFINED;
        }
        int32_t layer = -1;
        JS_ToInt32(ctx, &layer, argv[0]);
        host(ctx).bridge.set(layer, str(ctx, argv[1]), fromJs(ctx, argv[2]));
        return JS_UNDEFINED;
    }
    static JSValue nativeCreateLayer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 2) {
            return JS_NewInt32(ctx, -1);
        }
        const bool json = JS_ToBool(ctx, argv[1]) != 0;
        Script *self = host(ctx).current;
        if (self && self->layersCreated >= kMaxLayersPerScript) {
            // (the counter stays at the limit: a layer destroyed later makes room again)
            if (!self->layerLimitSaid) {
                self->layerLimitSaid = true;
                host(ctx).note("[" + self->target.name + "] createLayer: a script may have at most " + std::to_string(kMaxLayersPerScript) + " layers it made alive");
            }
            return JS_NewInt32(ctx, -1);
        }
        std::string spec = str(ctx, argv[0]);
        if (!json) {
            if (self && !self->workshopId.empty()) {
                spec = host(ctx).bridge.resolvePath(spec, self->workshopId);
            }
        }
        const int made = host(ctx).bridge.createLayer(spec, json);
        if (made >= 0 && self) {
            // only a layer that exists takes from the allowance (a path that does not resolve costs nothing)
            ++self->layersCreated;
            host(ctx).layerOwner[made] = self;
        }
        return JS_NewInt32(ctx, made);
    }
    static JSValue nativeIndex(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t layer = -1;
        if (argc > 0) {
            JS_ToInt32(ctx, &layer, argv[0]);
        }
        return JS_NewInt32(ctx, host(ctx).bridge.indexOf(layer));
    }
    static JSValue nativeSort(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t layer = -1, index = 0;
        if (argc > 1) {
            JS_ToInt32(ctx, &layer, argv[0]);
            JS_ToInt32(ctx, &index, argv[1]);
            host(ctx).bridge.sort(layer, index);
        }
        return JS_UNDEFINED;
    }
    static JSValue nativeFind(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        return JS_NewInt32(ctx, argc > 0 ? host(ctx).bridge.find(str(ctx, argv[0])) : -1);
    }

    static JSValue nativeFindById(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        return JS_NewInt32(ctx, argc > 0 ? host(ctx).bridge.findById(intArg(ctx, argc, argv, 0)) : -1);
    }

    static JSValue ints(JSContext *ctx, const std::vector<int> &list)
    {
        JSValue a = JS_NewArray(ctx);
        for (size_t i = 0; i < list.size(); ++i) {
            JS_SetPropertyUint32(ctx, a, static_cast<uint32_t>(i), JS_NewInt32(ctx, list[i]));
        }
        return a;
    }
    static int32_t intArg(JSContext *ctx, int argc, JSValueConst *argv, int i, int32_t fallback = -1)
    {
        int32_t v = fallback;
        if (i < argc) {
            JS_ToInt32(ctx, &v, argv[i]);
        }
        return v;
    }
    static JSValue nativeMatrix(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        const std::array<double, 16> m = host(ctx).bridge.worldMatrix(intArg(ctx, argc, argv, 0));
        JSValue out = JS_NewArray(ctx);
        for (uint32_t i = 0; i < 16; ++i) {
            JS_SetPropertyUint32(ctx, out, i, JS_NewFloat64(ctx, std::isfinite(m[i]) ? m[i] : 0.0));
        }
        return out;
    }
    static JSValue nativeParent(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        return JS_NewInt32(ctx, host(ctx).bridge.parentOf(intArg(ctx, argc, argv, 0)));
    }
    static JSValue nativeSetParent(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        host(ctx).bridge.setParent(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1), argc > 2 && JS_ToBool(ctx, argv[2]) != 0);
        return JS_UNDEFINED;
    }
    static JSValue nativeChildren(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        return ints(ctx, host(ctx).bridge.childrenOf(intArg(ctx, argc, argv, 0)));
    }
    static JSValue nativeEnumerate(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        return ints(ctx, host(ctx).bridge.enumerate());
    }
    static JSValue nativeDestroy(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        const int layer = intArg(ctx, argc, argv, 0);
        const bool destroyed = host(ctx).bridge.destroy(layer);
        if (const auto owner = host(ctx).layerOwner.find(layer); destroyed && owner != host(ctx).layerOwner.end()) {
            // the limit is on the layers that are alive: a script that rebuilds what it made destroys it first
            owner->second->layersCreated = std::max(0, owner->second->layersCreated - 1);
            host(ctx).layerOwner.erase(owner);
        }
        return JS_NewBool(ctx, destroyed);
    }
    static JSValue nativeEffectCount(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        return JS_NewInt32(ctx, host(ctx).bridge.effectCount(intArg(ctx, argc, argv, 0)));
    }
    static JSValue nativeEffectGet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 3) {
            return JS_UNDEFINED;
        }
        return toJs(ctx, host(ctx).bridge.effectGet(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1), str(ctx, argv[2])));
    }
    static JSValue nativeEffectSet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 4) {
            return JS_UNDEFINED;
        }
        host(ctx).bridge.effectSet(intArg(ctx, argc, argv, 0), intArg(ctx, argc, argv, 1), str(ctx, argv[2]), fromJs(ctx, argv[3]));
        return JS_UNDEFINED;
    }

    const AudioSpectrum *audio = nullptr; // valid during update()
    static JSValue nativeAudio(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t bands = 16, channel = 0;
        if (argc > 1) {
            JS_ToInt32(ctx, &bands, argv[0]);
            JS_ToInt32(ctx, &channel, argv[1]);
        }
        JSValue out = JS_NewArray(ctx);
        if (bands != 16 && bands != 32 && bands != 64) {
            return out; // only the three resolutions the analyzer has
        }
        channel = channel != 0 ? 1 : 0;
        const AudioSpectrum *a = host(ctx).audio;
        const float *values = nullptr;
        if (a) {
            values = bands == 16 ? (channel ? a->right16.data() : a->left16.data())
                : bands == 32    ? (channel ? a->right32.data() : a->left32.data())
                                 : (channel ? a->right64.data() : a->left64.data());
        }
        for (int32_t i = 0; i < bands; ++i) {
            JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), JS_NewFloat64(ctx, values ? values[i] : 0.0));
        }
        return out;
    }

    // ---- modules ----
    static char *normalize(JSContext *ctx, const char *, const char *name, void *)
    {
        // a script reaches its own names only, and no other script's module (a module is looked up by this name before the loader is
        // asked: the scripts' own modules are loaded as "script:<name>")
        if (!std::strncmp(name, "script:", 7)) {
            JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
            return nullptr;
        }
        if (!std::strncmp(name, "__scope/", 8)) {
            const Script *s = host(ctx).compiling;
            if (!s || std::to_string(s->id) != name + 8) {
                JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
                return nullptr;
            }
        }
        return js_strdup(ctx, name);
    }
    // '__scope/<id>': the names of the script being loaded into the shared realm (its first line imports them)
    static int initScope(JSContext *ctx, JSModuleDef *m)
    {
        Script *s = host(ctx).compiling;
        for (const char *name : kScopeNames) {
            if (JS_SetModuleExport(ctx, m, name, s ? JS_GetPropertyStr(ctx, s->names, name) : JS_UNDEFINED) < 0) {
                return -1;
            }
        }
        return 0;
    }
    static JSModuleDef *loadModule(JSContext *ctx, const char *name, void *)
    {
        if (!std::strncmp(name, "__scope/", 8)) {
            const Script *s = host(ctx).compiling;
            // only the script being loaded reaches its own names
            if (!s || std::to_string(s->id) != name + 8) {
                JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
                return nullptr;
            }
            JSModuleDef *m = JS_NewCModule(ctx, name, &Impl::initScope);
            for (const char *export_ : kScopeNames) {
                if (!m || JS_AddModuleExport(ctx, m, export_) < 0) {
                    return nullptr;
                }
            }
            return m;
        }
        const char *source = nullptr;
        if (!std::strcmp(name, "WEMath")) {
            source = kModuleWEMath;
        } else if (!std::strcmp(name, "WEVector")) {
            source = kModuleWEVector;
        } else if (!std::strcmp(name, "WEColor")) {
            source = kModuleWEColor;
        }
        if (!source) {
            JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
            return nullptr;
        }
        JSValue v = JS_Eval(ctx, source, std::strlen(source), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(v)) {
            return nullptr;
        }
        auto *m = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(v));
        JS_FreeValue(ctx, v);
        return m;
    }

    static int interrupt(JSRuntime *, void *opaque)
    {
        auto *self = static_cast<Impl *>(opaque);
        const auto now = Clock::now();
        if (now > self->deadline || now > self->frameDeadline) {
            self->interrupted = true;
            self->overBudget = self->deadline <= self->frameDeadline;
            return 1;
        }
        return 0;
    }

    void start()
    {
        rt = JS_NewRuntime();
        JS_SetRuntimeOpaque(rt, this);
        JS_SetMemoryLimit(rt, limits.memoryBytes);
        JS_SetMaxStackSize(rt, 1u << 20);
        JS_SetInterruptHandler(rt, &Impl::interrupt, this);
        JS_SetModuleLoaderFunc(rt, &Impl::normalize, &Impl::loadModule, nullptr);
        // `shared` belongs to the shared realm, which outlives every script's own (a script that fails to load must not take it
        // down), without a prototype so there is nothing on it to tamper with
        if (JSContext *ctx = JS_NewContext(rt)) {
            shared = JS_NewObject(ctx);
            JS_SetPrototype(ctx, shared, JS_NULL);
            storage = JS_NewObject(ctx);
            for (const char *location : {"global", "screen"}) {
                JSValue keys = JS_NewObject(ctx);
                JS_SetPrototype(ctx, keys, JS_NULL); // a key is any text ("__proto__" too)
                JS_SetPropertyStr(ctx, storage, location, keys);
            }
            std::string error;
            if (!makeRealm(sharedRealm, ctx, &error)) {
                note("the shared realm: " + error); // every script then gets a realm of its own
            }
            fixedBytes += limits.contextBytes;
            JS_SetMemoryLimit(rt, limits.memoryBytes + fixedBytes);
        }
    }

    void setBudget(double seconds)
    {
        deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        frameDeadline = Clock::time_point::max();
        interrupted = false;
        overBudget = false;
    }

    // Promise jobs the running script queued, within its budget. A job that fails is logged (its exception would otherwise be left
    // pending in its realm); one stopped by a deadline ends the run.
    void runJobs()
    {
        for (;;) {
            JSContext *jobCtx = nullptr;
            const int r = JS_ExecutePendingJob(rt, &jobCtx);
            if (r == 0) {
                return;
            }
            if (r < 0) {
                const std::string why = jobCtx ? describe(jobCtx, JS_GetException(jobCtx)) : std::string("an exception");
                if (interrupted) {
                    return;
                }
                if (current && !current->logFull) {
                    note("[" + current->target.name + " error] promise job: " + why);
                }
            }
        }
    }

    // A value in the form scripts expect: vectors as Vec2/3/4 objects of the realm.
    JSValue toScriptObject(Realm &realm, const ScriptValue &v)
    {
        JSContext *ctx = realm.ctx;
        if (v.type != ScriptValue::Type::Vector || v.count < 2 || v.count > 4) {
            return toJs(ctx, v);
        }
        JSValue &ctor = realm.vec[static_cast<size_t>(v.count - 2)];
        if (!JS_IsFunction(ctx, ctor)) {
            return toJs(ctx, v);
        }
        JSValue args[4];
        for (int i = 0; i < v.count; ++i) {
            args[i] = JS_NewFloat64(ctx, v.vec[static_cast<size_t>(i)]);
        }
        JSValue obj = JS_CallConstructor(ctx, ctor, v.count, args);
        for (int i = 0; i < v.count; ++i) {
            JS_FreeValue(ctx, args[i]);
        }
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, ctor);
            ctor = JS_UNDEFINED;
            return toJs(ctx, v);
        }
        return obj;
    }

    // A script's `init` with the property's value, then the media hooks; what it does each frame is known after it. Empty, or why it
    // failed.
    std::string init(Script &script, const ScriptValue &initial)
    {
        JSContext *ctx = script.ctx;
        setBudget(limits.initSeconds);
        JSValue init = JS_GetPropertyStr(ctx, script.ns, "init");
        if (JS_IsFunction(ctx, init)) {
            JSValue arg = toScriptObject(*script.realm, initial);
            JSValue r = JS_Call(ctx, init, JS_UNDEFINED, initial.type == ScriptValue::Type::Undefined ? 0 : 1, &arg);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, init);
            if (JS_IsException(r)) {
                deadline = Clock::time_point::max();
                return "init: " + describe(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, r);
        } else {
            JS_FreeValue(ctx, init);
        }
        {
            // the media hooks: what plays now, or that nothing does (see announceMedia)
            script.mediaPos = mediaBase + mediaLog.size();
            const std::string snapshot = mediaSnapshotJson();
            JSValue args[2] = {script.ns, JS_NewStringLen(ctx, snapshot.data(), snapshot.size())};
            JSValue r = JS_Call(ctx, script.realm->announceMedia, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, args[1]);
            if (JS_IsException(r)) {
                deadline = Clock::time_point::max();
                return "media hook: " + describe(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, r);
        }
        runJobs(); // what the init queued runs for this script, within its budget
        deadline = Clock::time_point::max();
        if (!JS_IsFunction(ctx, script.update)) {
            JS_FreeValue(ctx, script.update);
            script.update = JS_UNDEFINED;
        }
        return {};
    }

    void disable(Script &s, const std::string &why)
    {
        if (!s.disabled) {
            s.disabled = true;
            note("script '" + s.target.name + "' stopped: " + why);
            logError("script '{}' stopped: {}", s.target.name, why);
            bridge.scriptStopped(s.target.layer, s.target.property);
        }
    }

    // Evaluates the prelude in `ctx` (a new context, which the realm takes) with the natives, `shared` and the storage. False with the
    // reason when the prelude fails: the realm then has no makeScope.
    bool makeRealm(Realm &realm, JSContext *ctx, std::string *error)
    {
        realm.ctx = ctx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue h = JS_NewObject(ctx);
        const struct {
            const char *name;
            JSCFunction *fn;
            int args;
        } natives[] = {{"log", &Impl::nativeLog, 2},         {"get", &Impl::nativeGet, 2},    {"set", &Impl::nativeSet, 3},
                       {"createLayer", &Impl::nativeCreateLayer, 2}, {"layerIndex", &Impl::nativeIndex, 1}, {"sortLayer", &Impl::nativeSort, 2},
                       {"findLayer", &Impl::nativeFind, 1}, {"findLayerById", &Impl::nativeFindById, 1},  {"audio", &Impl::nativeAudio, 2},
                       {"parent", &Impl::nativeParent, 1},   {"matrix", &Impl::nativeMatrix, 1},   {"setParent", &Impl::nativeSetParent, 3}, {"children", &Impl::nativeChildren, 1},
                       {"enumerate", &Impl::nativeEnumerate, 0}, {"destroy", &Impl::nativeDestroy, 1}, {"effectCount", &Impl::nativeEffectCount, 1},
                       {"effectGet", &Impl::nativeEffectGet, 3}, {"effectSet", &Impl::nativeEffectSet, 4},
                       {"frame", &Impl::nativeFrame, 1}, {"wake", &Impl::nativeWake, 1}, {"stored", &Impl::nativeStored, 0}, {"utf8", &Impl::nativeUtf8, 1}};
        for (const auto &n : natives) {
            JS_SetPropertyStr(ctx, h, n.name, JS_NewCFunction(ctx, n.fn, n.name, n.args));
        }
        JS_SetPropertyStr(ctx, global, "__host", h);
        JS_SetPropertyStr(ctx, global, "shared", JS_DupValue(ctx, shared));
        JS_SetPropertyStr(ctx, global, "__storage", JS_DupValue(ctx, storage));
        JS_FreeValue(ctx, global);
        // the prelude is compiled once, in the first realm, and every other one reads that bytecode (a scene with a thousand scripts can
        // have hundreds of realms, and parsing the same source in each took most of the load time)
        JSValue r = JS_UNDEFINED;
        if (preludeBytecode.empty()) {
            JSValue compiled = JS_Eval(ctx, kScriptPrelude, std::strlen(kScriptPrelude), "prelude", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
            if (!JS_IsException(compiled)) {
                size_t length = 0;
                if (uint8_t *bytes = JS_WriteObject(ctx, &length, compiled, JS_WRITE_OBJ_BYTECODE)) {
                    preludeBytecode.assign(bytes, bytes + length);
                    js_free(ctx, bytes);
                }
                r = JS_EvalFunction(ctx, compiled);
            } else {
                r = compiled;
            }
        } else {
            JSValue loaded = JS_ReadObject(ctx, preludeBytecode.data(), preludeBytecode.size(), JS_READ_OBJ_BYTECODE);
            r = JS_IsException(loaded) ? loaded : JS_EvalFunction(ctx, loaded);
        }
        if (JS_IsException(r)) {
            if (error) {
                *error = "prelude: " + describe(ctx, JS_GetException(ctx));
            }
            return false;
        }
        // the prelude's value: the host's entry points, which no script reaches
        realm.makeScope = JS_GetPropertyStr(ctx, r, "makeScope");
        realm.cursorEvent = JS_GetPropertyStr(ctx, r, "cursorEvent");
        realm.eventArg = JS_GetPropertyStr(ctx, r, "eventArg");
        realm.announceMedia = JS_GetPropertyStr(ctx, r, "announceMedia");
        JS_FreeValue(ctx, r);
        JSValue globals = JS_GetGlobalObject(ctx);
        for (size_t i = 0; i < realm.vec.size(); ++i) {
            realm.vec[i] = JS_GetPropertyStr(ctx, globals, i == 0 ? "Vec2" : i == 1 ? "Vec3" : "Vec4");
        }
        JS_FreeValue(ctx, globals);
        if (!JS_IsFunction(ctx, realm.makeScope)) {
            if (error) {
                *error = "prelude: no makeScope";
            }
            return false;
        }
        return true;
    }

    // The script's names and hooks in its realm (the prelude's makeScope). False when that throws (out of memory).
    bool makeScope(Script &script, std::string *error)
    {
        JSContext *ctx = script.ctx;
        const ScriptTarget &target = script.target;
        // a script on a puppet animation layer's visibility ("alayer.<n>.visible") has the animation layer as its object
        const int animationLayer = target.property.rfind("alayer.", 0) == 0 ? std::atoi(target.property.c_str() + 7) : -1;
        JSValue args[4] = {JS_NewInt32(ctx, target.layer), JS_NewInt32(ctx, animationLayer), objectOf(ctx, target.properties),
                           JS_NewStringLen(ctx, target.property.data(), target.property.size())};
        JSValue scope = JS_Call(ctx, script.realm->makeScope, JS_UNDEFINED, 4, args);
        JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, args[3]);
        if (JS_IsException(scope)) {
            if (error) {
                *error = describe(ctx, JS_GetException(ctx));
            }
            return false;
        }
        script.scope = scope;
        script.names = JS_GetPropertyStr(ctx, scope, "names");
        script.frame = JS_GetPropertyStr(ctx, scope, "frame");
        script.usesAudio = JS_GetPropertyStr(ctx, scope, "usesAudio");
        script.hasTimers = JS_GetPropertyStr(ctx, scope, "hasTimers");
        return true;
    }

    // Calls the scope's hook `name` with the arguments; takes them.
    JSValue callScope(Script &s, const char *name, int argc, JSValue *argv)
    {
        JSValue f = JS_GetPropertyStr(s.ctx, s.scope, name);
        JSValue r = JS_Call(s.ctx, f, JS_UNDEFINED, argc, argv);
        JS_FreeValue(s.ctx, f);
        for (int i = 0; i < argc; ++i) {
            JS_FreeValue(s.ctx, argv[i]);
        }
        return r;
    }

    void freeScript(Script &s)
    {
        if (s.ctx) {
            for (JSValue *v : {&s.update, &s.scope, &s.names, &s.frame, &s.usesAudio, &s.hasTimers, &s.ns}) {
                JS_FreeValue(s.ctx, *v);
                *v = JS_UNDEFINED;
            }
        }
        if (s.own) {
            s.own->free();
            s.own.reset();
        }
        s.ctx = nullptr;
    }

    // Whether the script has a timer waiting (asks its scope).
    bool timersPending(Script &s)
    {
        if (JS_IsUndefined(s.hasTimers)) {
            return false;
        }
        JSValue r = JS_Call(s.ctx, s.hasTimers, JS_UNDEFINED, 0, nullptr);
        const bool pending = JS_ToBool(s.ctx, r) != 0;
        JS_FreeValue(s.ctx, r);
        return pending;
    }

    // Calls `name` of the script's module with an argument made by `makeArg` (nothing when it returns undefined) if the module
    // exports it, then the promise jobs it queued. A throw disables the script; running out of budget does not (the caller judges
    // that). False when the script stopped or ran out of budget.
    template <class MakeArg>
    bool callEvent(Script &s, const char *name, MakeArg makeArg)
    {
        if (s.disabled) {
            return false;
        }
        JSContext *ctx = s.ctx;
        JSValue f = JS_GetPropertyStr(ctx, s.ns, name);
        if (!JS_IsFunction(ctx, f)) {
            JS_FreeValue(ctx, f);
            return true;
        }
        JSValue arg = makeArg(ctx);
        JSValue r = JS_Call(ctx, f, JS_UNDEFINED, JS_IsUndefined(arg) ? 0 : 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, f);
        if (JS_IsException(r)) {
            const std::string why = describe(ctx, JS_GetException(ctx));
            if (!interrupted) {
                disable(s, std::string(name) + ": " + why);
            }
            return false;
        }
        JS_FreeValue(ctx, r);
        runJobs();
        return true;
    }

    // The cursor and screen events of a frame for one script. False when the script ran out of budget or stopped.
    bool callEvents(Script &s, const ScriptFrame &frame, double screenW, double screenH)
    {
        if (s.hasMedia && !deliverMedia(s)) {
            return false;
        }
        if (screenW != s.screenW || screenH != s.screenH) {
            const bool first = s.screenW < 0;
            s.screenW = screenW;
            s.screenH = screenH;
            if (!first && !callEvent(s, "resizeScreen", [&](JSContext *ctx) {
                    JSValue a[3] = {JS_NewString(ctx, "resize"), JS_NewFloat64(ctx, screenW), JS_NewFloat64(ctx, screenH)};
                    JSValue v = JS_Call(ctx, s.realm->eventArg, JS_UNDEFINED, 3, a);
                    for (JSValue &x : a) {
                        JS_FreeValue(ctx, x);
                    }
                    return v;
                })) {
                return false;
            }
        }
        if (!s.hasCursor) {
            return true;
        }
        double lx = 0, ly = 0;
        const bool hit = bridge.hitTest(s.target.layer, frame.cursorX, frame.cursorY, &lx, &ly);
        const bool moved = frame.cursorX != s.lastCursorX || frame.cursorY != s.lastCursorY;
        s.lastCursorX = frame.cursorX;
        s.lastCursorY = frame.cursorY;
        const auto fire = [&](const char *name) { return fireCursor(s, name, frame, lx, ly); };
        if (hit && !s.cursorInside && !fire("cursorEnter")) {
            return false;
        }
        if (!hit && s.cursorInside && !fire("cursorLeave")) {
            return false;
        }
        s.cursorInside = hit;
        if (hit && moved && !fire("cursorMove")) {
            return false;
        }
        if (frame.cursorDown && !s.cursorPressed) {
            s.cursorPressed = true;
            s.cursorPressedInside = hit;
            if (hit && !fire("cursorDown")) {
                return false;
            }
        } else if (!frame.cursorDown && s.cursorPressed) {
            s.cursorPressed = false;
            if (hit && !fire("cursorUp")) {
                return false;
            }
            if (hit && s.cursorPressedInside && !fire("cursorClick")) {
                return false;
            }
        }
        return true;
    }

    // Makes the CursorEvent (the prelude's class) and calls the script's function with it.
    bool fireCursor(Script &s, const char *name, const ScriptFrame &frame, double lx, double ly)
    {
        JSContext *ctx = s.ctx;
        JSValue a[6] = {JS_NewString(ctx, name), JS_DupValue(ctx, s.ns), JS_NewFloat64(ctx, frame.cursorX), JS_NewFloat64(ctx, frame.cursorY),
                        JS_NewFloat64(ctx, lx), JS_NewFloat64(ctx, ly)};
        JSValue r = JS_Call(ctx, s.realm->cursorEvent, JS_UNDEFINED, 6, a);
        for (JSValue &x : a) {
            JS_FreeValue(ctx, x);
        }
        if (JS_IsException(r)) {
            const std::string why = describe(ctx, JS_GetException(ctx));
            if (!interrupted) {
                disable(s, std::string(name) + ": " + why);
            }
            return false;
        }
        JS_FreeValue(ctx, r);
        return true;
    }

    void finish()
    {
        for (auto &s : scripts) {
            freeScript(*s);
        }
        layerOwner.clear();
        scripts.clear();
        if (sharedRealm.ctx) {
            JS_FreeValue(sharedRealm.ctx, shared);
            JS_FreeValue(sharedRealm.ctx, storage);
            sharedRealm.free();
        }
        if (rt) {
            JS_FreeRuntime(rt);
        }
    }
};

ScriptHost::ScriptHost(ScriptSceneBridge &bridge, Limits limits) : m_impl(std::make_unique<Impl>(bridge, limits))
{
    m_impl->start();
}

ScriptHost::~ScriptHost()
{
    // `destroy` just before the scripts go; the host may be torn down in a hurry, so each gets a short budget of its own
    Impl &h = *m_impl;
    for (auto &sp : h.scripts) {
        if (!sp->disabled && !JS_IsUndefined(sp->ns)) {
            h.setBudget(0.02);
            const Impl::Running running(h, sp.get());
            h.callEvent(*sp, "destroy", [](JSContext *) { return JS_UNDEFINED; });
        }
    }
    if (h.storageDirty) {
        h.writeStorage();
    }
    h.finish();
}

void ScriptHost::Impl::deliverUser(Script &s, JSValue changed, JSValue all, Clock::time_point ceiling)
{
    setBudget(limits.frameSeconds);
    frameDeadline = ceiling;
    const Running running(*this, &s);
    JSContext *ctx = s.ctx;
    JSValue args[2] = {changed, all};
    JSValue converted = callScope(s, "setUserProperties", 2, args);
    if (JS_IsException(converted)) {
        const std::string why = describe(ctx, JS_GetException(ctx));
        if (!interrupted) {
            disable(s, "user properties: " + why);
        }
        return;
    }
    callEvent(s, "applyUserProperties", [&](JSContext *) { return JS_DupValue(ctx, converted); });
    JS_FreeValue(ctx, converted);
}

void ScriptHost::Impl::deliverScriptProperties(Script &s, const std::map<std::string, ScriptValue> &own, Clock::time_point ceiling)
{
    setBudget(limits.frameSeconds);
    frameDeadline = ceiling;
    const Running running(*this, &s);
    JSContext *ctx = s.ctx;
    JSValue arg = objectOf(ctx, own);
    JSValue result = callScope(s, "setScriptProperties", 1, &arg);
    if (JS_IsException(result)) {
        const std::string why = describe(ctx, JS_GetException(ctx));
        if (!interrupted) {
            disable(s, "script properties: " + why);
        }
        return;
    }
    JS_FreeValue(ctx, result);
}

void ScriptHost::Impl::deliverSettings(Script &s, const std::map<std::string, ScriptValue> &changed, Clock::time_point ceiling)
{
    setBudget(limits.frameSeconds);
    frameDeadline = ceiling;
    const Running running(*this, &s);
    callEvent(s, "applyGeneralSettings", [&](JSContext *ctx) { return objectOf(ctx, changed); });
}

// Events a call could not deliver within its ceiling; the next update delivers them first. True when the script has had them all.
bool ScriptHost::Impl::deliverPending(Script &s, Clock::time_point ceiling)
{
    if (s.userPending && !s.disabled) {
        if (Clock::now() >= ceiling) {
            return false;
        }
        s.userPending = false;
        const std::map<std::string, ScriptValue> changed = std::move(s.pendingUser);
        s.pendingUser.clear();
        deliverUser(s, objectOf(s.ctx, changed), objectOf(s.ctx, lastAllUser), ceiling);
    }
    if (!s.pendingScriptProperties.empty() && !s.disabled) {
        if (Clock::now() >= ceiling) {
            return false;
        }
        const std::map<std::string, ScriptValue> own = std::move(s.pendingScriptProperties);
        s.pendingScriptProperties.clear();
        deliverScriptProperties(s, own, ceiling);
    }
    if (!s.pendingSettings.empty() && !s.disabled) {
        if (Clock::now() >= ceiling) {
            return false;
        }
        const std::map<std::string, ScriptValue> settings = std::move(s.pendingSettings);
        s.pendingSettings.clear();
        deliverSettings(s, settings, ceiling);
    }
    return true;
}

Clock::time_point ScriptHost::Impl::ceilingFromNow() const
{
    return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(limits.frameTotalSeconds));
}

void ScriptHost::Impl::endCall()
{
    deadline = Clock::time_point::max();
    frameDeadline = Clock::time_point::max();
    if (storageDirty && Clock::now() - storageChanged > std::chrono::seconds(1)) {
        writeStorage();
    }
}

// Every call that runs scripts has the frame's ceiling over all of them: a script it does not reach gets the event at the next update.
void ScriptHost::applyUserProperties(const std::map<std::string, ScriptValue> &changed, const std::map<std::string, ScriptValue> &all)
{
    Impl &h = *m_impl;
    h.lastAllUser = all;
    const auto ceiling = h.ceilingFromNow();
    Impl::RealmObjects changedObjects(changed), allObjects(all);
    for (auto &sp : h.scripts) {
        Script &s = *sp;
        if (s.disabled) {
            continue;
        }
        if (s.userPending || Clock::now() >= ceiling) {
            Impl::merge(s.pendingUser, changed);
            s.userPending = true;
            continue;
        }
        h.deliverUser(s, changedObjects.in(s.ctx), allObjects.in(s.ctx), ceiling);
    }
    h.endCall();
}

void ScriptHost::applyScriptPropertyChanges(const std::map<std::string, ScriptValue> &changed)
{
    Impl &h = *m_impl;
    const auto ceiling = h.ceilingFromNow();
    for (auto &sp : h.scripts) {
        Script &s = *sp;
        if (s.disabled || s.target.propertyUsers.empty()) {
            continue;
        }
        std::map<std::string, ScriptValue> own;
        for (const auto &[key, user] : s.target.propertyUsers) {
            if (const auto it = changed.find(user); it != changed.end()) {
                own[key] = it->second;
            }
        }
        if (own.empty()) {
            continue;
        }
        if (!s.pendingScriptProperties.empty() || Clock::now() >= ceiling) {
            Impl::merge(s.pendingScriptProperties, own);
            continue;
        }
        h.deliverScriptProperties(s, own, ceiling);
    }
    h.endCall();
}

void ScriptHost::applyGeneralSettings(const std::map<std::string, ScriptValue> &changed)
{
    Impl &h = *m_impl;
    const auto ceiling = h.ceilingFromNow();
    for (auto &sp : h.scripts) {
        Script &s = *sp;
        if (s.disabled) {
            continue;
        }
        if (!s.pendingSettings.empty() || Clock::now() >= ceiling) {
            Impl::merge(s.pendingSettings, changed);
            continue;
        }
        h.deliverSettings(s, changed, ceiling); // its own object: the hook gets it as it is
    }
    h.endCall();
}

bool ScriptHost::add(const ScriptTarget &target, const ScriptValue &initial, std::string *error)
{
    Impl &h = *m_impl;
    if (h.scripts.size() >= h.limits.maxScripts) {
        if (error) {
            *error = "too many scripts";
        }
        h.bridge.scriptStopped(target.layer, target.property);
        return false;
    }
    if (target.source.size() > kMaxSourceBytes) {
        if (error) {
            *error = "the script has " + std::to_string(target.source.size() >> 10) + " KiB of source, over the 1 MiB a script may have";
        }
        h.bridge.scriptStopped(target.layer, target.property);
        return false;
    }
    auto script = std::make_unique<Script>();
    script->target = target;
    script->id = h.nextId++;
    {
        // a Workshop script's id: digits between quotes after `__workshopId =`
        const size_t at = target.source.find("__workshopId");
        if (at != std::string::npos) {
            const size_t quote = target.source.find_first_of("'\"", at);
            const size_t end = quote == std::string::npos ? quote : target.source.find_first_of("'\"", quote + 1);
            if (end != std::string::npos && end - quote - 1 <= 20) {
                const std::string id = target.source.substr(quote + 1, end - quote - 1);
                if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
                    script->workshopId = id;
                }
            }
        }
    }
    const Impl::Running running(h, script.get());
    struct Compiling {
        Impl &h;
        ~Compiling() { h.compiling = nullptr; }
    } compiling{h};
    h.compiling = script.get();
    h.setBudget(h.limits.initSeconds);
    // Running into the memory limit is not survivable (QuickJS is left half built), so the limit follows what the realms and scopes
    // cost: their fixed cost is not taken out of what the scripts may use
    const auto reserve = [&](size_t bytes) {
        script->fixedBytes = bytes;
        JS_SetMemoryLimit(h.rt, h.limits.memoryBytes + h.fixedBytes + bytes);
    };
    const auto fail = [&](const std::string &why) {
        if (error) {
            *error = why;
        }
        h.freeScript(*script);
        JS_SetMemoryLimit(h.rt, h.limits.memoryBytes + h.fixedBytes);
        h.bridge.scriptStopped(target.layer, target.property);
        return false;
    };
    const std::string moduleName = "script:" + target.name;
    JSValue compiled = JS_UNDEFINED;
    // The shared realm: the script is a module whose first line imports its own names (thisLayer, engine, ...), so its line numbers
    // stay the file's. A script that would change what the others see there (the global object, a built-in, a prototype), or that
    // declares one of those names itself, gets a realm of its own instead.
    if (h.sharedRealm.ctx && JS_IsFunction(h.sharedRealm.ctx, h.sharedRealm.makeScope) && scriptNeedsOwnRealm(target.source).empty()) {
        JSContext *ctx = h.sharedRealm.ctx;
        std::string source = "import {";
        for (const char *name : kScopeNames) {
            source += std::string(" ") + name + ",";
        }
        source += " } from '__scope/" + std::to_string(script->id) + "'; " + target.source;
        reserve(h.limits.scopeBytes);
        compiled = JS_Eval(ctx, source.c_str(), source.size(), moduleName.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            JS_FreeValue(ctx, JS_GetException(ctx)); // a realm of its own tells the script's own error, if it has one
            compiled = JS_UNDEFINED;
        } else {
            script->realm = &h.sharedRealm;
            script->ctx = ctx;
        }
    }
    if (!script->realm) {
        reserve(h.limits.contextBytes);
        JSContext *ctx = JS_NewContext(h.rt);
        if (!ctx) {
            return fail("out of memory");
        }
        script->own = std::make_unique<Realm>();
        script->realm = script->own.get();
        script->ctx = ctx;
        std::string why;
        if (!h.makeRealm(*script->own, ctx, &why)) {
            return fail(why);
        }
    }
    JSContext *ctx = script->ctx;
    if (!h.makeScope(*script, error)) {
        if (!JS_IsUndefined(compiled)) {
            JS_FreeValue(ctx, compiled);
        }
        return fail(error ? *error : std::string("out of memory"));
    }
    if (script->own) {
        // its names are the globals of its realm
        JSValue global = JS_GetGlobalObject(ctx);
        for (const char *name : kScopeNames) {
            JS_SetPropertyStr(ctx, global, name, JS_GetPropertyStr(ctx, script->names, name));
        }
        JS_FreeValue(ctx, global);
        compiled = JS_Eval(ctx, target.source.c_str(), target.source.size(), moduleName.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            return fail(describe(ctx, JS_GetException(ctx)));
        }
    }
    auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    JSValue evaluated = JS_EvalFunction(ctx, compiled);
    if (JS_IsException(evaluated)) {
        return fail(describe(ctx, JS_GetException(ctx)));
    }
    // a module evaluates to a promise; its failure is the promise's rejection
    if (JS_PromiseState(ctx, evaluated) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, evaluated);
        const std::string why = describe(ctx, reason);
        JS_FreeValue(ctx, evaluated);
        return fail(why);
    }
    JS_FreeValue(ctx, evaluated);
    JSValue ns = JS_GetModuleNamespace(ctx, module);
    script->ns = JS_DupValue(ctx, ns);
    for (const char *name : {"cursorEnter", "cursorLeave", "cursorMove", "cursorDown", "cursorUp", "cursorClick"}) {
        JSValue f = JS_GetPropertyStr(ctx, ns, name);
        script->hasCursor = script->hasCursor || JS_IsFunction(ctx, f);
        JS_FreeValue(ctx, f);
    }
    for (const char *name : {"mediaStatusChanged", "mediaPlaybackChanged", "mediaPropertiesChanged", "mediaThumbnailChanged", "mediaTimelineChanged"}) {
        JSValue f = JS_GetPropertyStr(ctx, ns, name);
        script->hasMedia = script->hasMedia || JS_IsFunction(ctx, f);
        JS_FreeValue(ctx, f);
    }
    {
        JSValue f = JS_GetPropertyStr(ctx, ns, "resizeScreen");
        script->hasResize = JS_IsFunction(ctx, f);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, ns);
    script->update = JS_GetPropertyStr(ctx, script->ns, "update");
    if (h.deferInits) {
        script->initPending = true;
        script->initial = initial;
    } else if (std::string why = h.init(*script, initial); !why.empty()) {
        return fail(why);
    }
    h.fixedBytes += script->fixedBytes;
    h.ownRealms += script->own ? 1 : 0;
    h.scripts.push_back(std::move(script));
    return true;
}

void ScriptHost::setDeferredInits(bool deferred)
{
    m_impl->deferInits = deferred;
}

std::vector<std::pair<std::string, std::string>> ScriptHost::initAll()
{
    Impl &h = *m_impl;
    std::vector<std::pair<std::string, std::string>> failures;
    // (an init may add scripts: by index, and only those that were waiting)
    for (size_t i = 0; i < h.scripts.size(); ++i) {
        Script &s = *h.scripts[i];
        if (!s.initPending) {
            continue;
        }
        s.initPending = false;
        const Impl::Running running(h, &s);
        if (std::string why = h.init(s, s.initial); !why.empty()) {
            // like a script whose init fails while it is added: it does not run
            s.disabled = true;
            h.bridge.scriptStopped(s.target.layer, s.target.property);
            failures.emplace_back(s.target.name, why);
        }
    }
    return failures;
}

namespace {

std::string jsonQuote(const std::string &text)
{
    std::string out = "\"";
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            out += ' ';
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string jsonNumber(double v)
{
    char buffer[40];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), std::isfinite(v) ? v : 0.0);
    return std::string(buffer, r.ptr);
}

std::string jsonColor(const std::array<float, 3> &c)
{
    return "[" + jsonNumber(c[0]) + "," + jsonNumber(c[1]) + "," + jsonNumber(c[2]) + "]";
}

std::string statusJson(const MediaState &m)
{
    return std::string("{\"enabled\":") + (m.enabled ? "true" : "false") + "}";
}

std::string playbackJson(const MediaState &m)
{
    return "{\"state\":" + std::to_string(static_cast<int>(m.playback)) + "}";
}

std::string propertiesJson(const MediaState &m)
{
    std::string genres = "[";
    for (size_t i = 0; i < m.genres.size(); ++i) {
        genres += (i ? "," : "") + jsonQuote(m.genres[i]);
    }
    genres += "]";
    // (no subtitle or content type is known from MPRIS: empty)
    return "{\"title\":" + jsonQuote(m.title) + ",\"artist\":" + jsonQuote(m.artist) + ",\"subTitle\":\"\",\"albumTitle\":" + jsonQuote(m.albumTitle)
        + ",\"albumArtist\":" + jsonQuote(m.albumArtist) + ",\"genres\":" + genres + ",\"contentType\":\"\"}";
}

std::string thumbnailJson(bool has, const MediaColors &c)
{
    return std::string("{\"hasThumbnail\":") + (has ? "true" : "false") + ",\"primaryColor\":" + jsonColor(c.primary)
        + ",\"secondaryColor\":" + jsonColor(c.secondary) + ",\"tertiaryColor\":" + jsonColor(c.tertiary) + ",\"textColor\":" + jsonColor(c.text)
        + ",\"highContrastColor\":" + jsonColor(c.highContrast) + "}";
}

std::string timelineJson(double position, double duration)
{
    return "{\"position\":" + jsonNumber(position) + ",\"duration\":" + jsonNumber(duration) + "}";
}

} // namespace

void ScriptHost::Impl::logMedia(const char *hook, std::string json)
{
    mediaLog.push_back({hook, std::move(json)});
    while (mediaLog.size() > kMaxMediaLog) { // a script that never listens does not hold the log back
        mediaLog.pop_front();
        ++mediaBase;
    }
}

std::string ScriptHost::Impl::mediaSnapshotJson() const
{
    const MediaState &m = media;
    std::string out = "{\"status\":" + statusJson(m) + ",\"playback\":" + playbackJson(m);
    out += ",\"properties\":" + (m.enabled ? propertiesJson(m) : std::string("null"));
    out += ",\"thumbnail\":" + (m.enabled ? thumbnailJson(mediaThumbnail, mediaThumbnailColors) : std::string("null"));
    out += ",\"timeline\":" + (m.enabled ? timelineJson(extrapolatedPosition(m, std::max(0.0, lastClock - mediaAt)), m.duration) : std::string("null"));
    return out + "}";
}

void ScriptHost::Impl::tickMedia(double now)
{
    lastClock = now;
    if (media.enabled && media.playback == MediaPlayback::Playing && now - lastTimeline >= 1.0) {
        lastTimeline = now;
        logMedia("mediaTimelineChanged", timelineJson(extrapolatedPosition(media, std::max(0.0, now - mediaAt)), media.duration));
    }
}

bool ScriptHost::Impl::deliverMedia(Script &s)
{
    if (s.mediaPos < mediaBase) {
        s.mediaPos = mediaBase;
    }
    while (s.mediaPos < mediaBase + mediaLog.size()) {
        const MediaRecord &record = mediaLog[s.mediaPos - mediaBase];
        ++s.mediaPos;
        if (!callEvent(s, record.hook, [&](JSContext *ctx) {
                JSValue a[3] = {JS_NewString(ctx, "media"), JS_NewStringLen(ctx, record.json.data(), record.json.size()), JS_UNDEFINED};
                JSValue v = JS_Call(ctx, s.realm->eventArg, JS_UNDEFINED, 3, a);
                JS_FreeValue(ctx, a[0]);
                JS_FreeValue(ctx, a[1]);
                return v;
            })) {
            return false;
        }
    }
    return true;
}

bool ScriptHost::wantsMedia() const
{
    return std::any_of(m_impl->scripts.begin(), m_impl->scripts.end(), [](const auto &s) { return !s->disabled && s->hasMedia; });
}

void ScriptHost::setMedia(const MediaState &state, double now)
{
    Impl &h = *m_impl;
    const MediaState old = h.media;
    h.media = state;
    h.mediaAt = now;
    h.lastClock = std::max(h.lastClock, now);
    if (state.enabled != old.enabled) {
        h.logMedia("mediaStatusChanged", statusJson(state));
    }
    if (state.playback != old.playback || state.enabled != old.enabled) {
        h.logMedia("mediaPlaybackChanged", playbackJson(state));
    }
    if (state.enabled && (!old.enabled || !state.sameProperties(old))) {
        h.logMedia("mediaPropertiesChanged", propertiesJson(state));
    }
    if (state.enabled && (!old.enabled || state.position != old.position || state.duration != old.duration || state.playback != old.playback)) {
        h.lastTimeline = now;
        h.logMedia("mediaTimelineChanged", timelineJson(state.position, state.duration));
    }
}

void ScriptHost::setMediaThumbnail(bool has, const MediaColors &colors)
{
    Impl &h = *m_impl;
    h.mediaThumbnail = has;
    h.mediaThumbnailColors = has ? colors : MediaColors{};
    if (h.media.enabled || has) {
        h.logMedia("mediaThumbnailChanged", thumbnailJson(has, h.mediaThumbnailColors));
    }
}

void ScriptHost::update(const ScriptFrame &frame)
{
    Impl &h = *m_impl;
    h.audio = frame.audio;
    const auto ceiling = h.ceilingFromNow();
    const double screenW = frame.screenWidth > 0 ? frame.screenWidth : frame.canvasWidth;
    const double screenH = frame.screenHeight > 0 ? frame.screenHeight : frame.canvasHeight;
    const double values[10] = {frame.frametime, frame.time, frame.timeOfDay, frame.canvasWidth, frame.canvasHeight, frame.cursorX, frame.cursorY,
                               frame.cursorDown ? 1.0 : 0.0, screenW, screenH};
    std::copy(std::begin(values), std::end(values), h.curFrame);
    h.tickMedia(frame.time);
    const size_t count = h.scripts.size();
    const size_t start = h.nextStart < count ? h.nextStart : 0;
    h.nextStart = 0;
    for (size_t n = 0; n < count; ++n) {
        const size_t index = (start + n) % count;
        Script &s = *h.scripts[index];
        if (s.disabled) {
            continue;
        }
        // the frame's ceiling over every script: the rest wait for the next frame, which starts with them (none is starved by the ones
        // before it in the file)
        if (Clock::now() >= ceiling || !h.deliverPending(s, ceiling)) {
            h.nextStart = index;
            break;
        }
        // a script has something to do when it updates, has a cursor or resize hook, or has a timer pending (a script of timers and
        // hooks alone has no `update`)
        const bool timers = s.needsTick && h.timersPending(s);
        if (s.disabled || !(!JS_IsUndefined(s.update) || s.hasCursor || s.hasResize || s.hasMedia || timers || s.audio)) {
            continue;
        }
        // every script has a budget of its own; the ceiling bounds all of them together
        h.setBudget(h.limits.frameSeconds);
        h.frameDeadline = ceiling;
        const Impl::Running running(h, &s);
        JSContext *ctx = s.ctx;
        // A script that ran out of its own budget is stopped only when it keeps doing so: one slow frame (a busy machine) is forgiven. One
        // stopped by the frame's ceiling is not charged: the frame ends and it runs first in the next.
        bool cut = false;
        const auto interrupted = [&] {
            if (!h.overBudget) {
                cut = true;
                return;
            }
            if (++s.overruns >= kMaxOverruns) {
                h.disable(s, "over its time budget in " + std::to_string(kMaxOverruns) + " frames running");
            }
        };
        const auto failed = [&](JSValue exception) {
            const std::string why = describe(ctx, exception);
            h.interrupted ? interrupted() : h.disable(s, why);
        };
        if (s.audio || timers) {
            JSValue r = JS_Call(ctx, s.frame, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(r)) {
                failed(JS_GetException(ctx));
                if (cut) {
                    h.nextStart = index;
                    break;
                }
                continue;
            }
            JS_FreeValue(ctx, r);
        }
        if (!h.callEvents(s, frame, screenW, screenH)) {
            if (h.interrupted) {
                interrupted();
            }
            if (cut) {
                h.nextStart = index;
                break;
            }
            continue;
        }
        if (!JS_IsUndefined(s.update)) {
            // property scripts get the current value
            JSValue arg = JS_UNDEFINED;
            int argc = 0;
            if (!s.target.property.empty()) {
                const ScriptValue current = h.bridge.get(s.target.layer, s.target.property);
                if (current.type != ScriptValue::Type::Undefined) {
                    arg = h.toScriptObject(*s.realm, current);
                    argc = 1;
                }
            }
            JSValue result = JS_Call(ctx, s.update, JS_UNDEFINED, argc, &arg);
            JS_FreeValue(ctx, arg);
            if (JS_IsException(result)) {
                failed(JS_GetException(ctx));
                if (cut) {
                    h.nextStart = index;
                    break;
                }
                continue;
            }
            if (!s.target.property.empty() && !JS_IsUndefined(result)) {
                const ScriptValue out = Impl::fromJs(ctx, result);
                if (out.type != ScriptValue::Type::Undefined) {
                    h.bridge.set(s.target.layer, s.target.property, out);
                }
            }
            JS_FreeValue(ctx, result);
        }
        // promise jobs a script queued run here, within the same budget
        h.runJobs();
        if (h.interrupted) {
            interrupted();
            if (cut) {
                h.nextStart = index;
                break;
            }
        } else {
            s.overruns = 0;
        }
    }
    h.audio = nullptr;
    h.endCall();
}

void ScriptHost::setStorageFile(const std::string &path)
{
    m_impl->storageFile = path;
    if (!path.empty()) {
        m_impl->loadStorage();
    }
}

size_t ScriptHost::scriptCount() const
{
    return m_impl->scripts.size();
}

size_t ScriptHost::ownRealmCount() const
{
    return m_impl->ownRealms;
}

size_t ScriptHost::memoryUsed() const
{
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(m_impl->rt, &usage);
    return static_cast<size_t>(usage.malloc_size);
}

size_t ScriptHost::disabledCount() const
{
    size_t n = 0;
    for (const auto &s : m_impl->scripts) {
        n += s->disabled ? 1 : 0;
    }
    return n;
}

bool ScriptHost::hasUpdates() const
{
    for (const auto &s : m_impl->scripts) {
        if (!s->disabled && (!JS_IsUndefined(s->update) || m_impl->timersPending(*s))) {
            return true;
        }
    }
    return false;
}

bool ScriptHost::active() const
{
    for (const auto &s : m_impl->scripts) {
        if (!s->disabled && (!JS_IsUndefined(s->update) || s->hasCursor || s->hasResize || s->needsTick || (s->hasMedia && m_impl->media.playback == MediaPlayback::Playing))) {
            return true;
        }
    }
    return false;
}

bool ScriptHost::usesAudio() const
{
    for (const auto &s : m_impl->scripts) {
        if (s->audio) {
            return true;
        }
    }
    return false;
}

const std::vector<std::string> &ScriptHost::messages() const
{
    return m_impl->messages;
}

} // namespace murasaki::scene
