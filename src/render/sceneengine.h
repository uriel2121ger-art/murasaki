#pragma once

#include "bridgesupport.h"      // RestartBackoff, bridgeScalingMode(), bridgePropertiesJson()
#include "sceneenginelookup.h" // findVulkanSceneEngine(), for the users of this header

#include <QHash>
#include <QList>
#include <QObject>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <chrono>
#include <deque>
#include <optional>

namespace murasaki {

// Everything the scene engine is launched with.
struct SceneLaunch {
    int bridgeFd = -1;         // engine end of the socketpair, already inherited by the child
    QSize size;                // viewport in device pixels
    QString outputName;        // --screen-root name; "murasaki" when empty
    QString itemDir;
    QString assetsDir;         // Wallpaper Engine "assets" directory; omitted when empty
    QString fill = QStringLiteral("fill"); // fill | fit | stretch
    int fps = 30;
    int volume = 0;            // bridge scale 0..128; 0 while muted (the engine is never started with --silent)
    double renderScale = 0.0;  // Vulkan engine only: target pixels per output pixel; 0: automatic (nothing is passed)
    QVariantMap properties;    // initial user properties, see scenePropertyValue()
};

// `pixels` scaled by `scale` (clamped to 0.25..1), each side even and never below 64 (or the output's own side when smaller).
QSize scaledBufferSize(const QSize &pixels, double scale);

// Command line for the engine (without the program name). Global options come first, then the per-screen group
// `--screen-root <name> --bg <dir> --scaling <mode>`, then the properties.
QStringList sceneEngineArguments(const SceneLaunch &launch);

// A program and its arguments.
struct SandboxedCommand {
    QString program;
    QStringList arguments;
};

// The command that runs `program arguments` behind murasaki-sandbox with the scene profile for this item (see
// src/sandbox/profile.h for what it allows). The helper execs the engine, so the engine is still the process the
// caller started, with the descriptors it inherited.
// With `require` the helper refuses to run the engine unless Landlock applies and TCP is refused.
SandboxedCommand sandboxSceneEngine(const QString &helper, const QString &itemDir, const QString &assetsDir,
                                    const QString &program, const QStringList &arguments, bool require = false);

// An output name is passed as `--screen-root <name>`: empty means the default, otherwise it must not start with
// '-' (it would read as an option) nor contain control characters.
bool sceneOutputNameValid(const QString &name);

// Property value as the engine's --set-property wants it: booleans as true/false, numbers without exponent,
// strings as they are, colors and vectors (lists of numbers) as space separated components. Floating
// point components keep a '.', which makes the engine read a color as 0..1 floats. Null when the type has no
// representation.
std::optional<QString> scenePropertyValue(const QVariant &value);

// ---- choosing the engine -------------------------------------------------------------------------------------
//
// Scenes are drawn by murasaki-scene-vk (see docs/scene-vk/README.md). The daemon setting `sceneEngine` is `auto` or
// `vulkan`; either way the Vulkan engine draws the item, and a scene that `murasaki-scene-vk --check` does not accept
// in full is labelled incomplete.
// The verdict of --check is cached per item and engine in $XDG_CACHE_HOME/murasaki/scene-check.json.

// The first enumerator is what a SceneEngineRequest starts with.
enum class SceneEngineMode {
    Auto,
    Vulkan
};

// An unknown string reads as the default (kDefaultSceneEngine).
SceneEngineMode sceneEngineModeFromString(const QString &text);

// What `murasaki-scene-vk --check` said about an item.
struct SceneCheckVerdict {
    bool ok = false;
    QStringList reasons; // why not, each ending in the milestone that lifts it
};

// Reads the verdict line of `<engine> --check` (the last non-empty line of its output). Null when there is none
// (`*error` says why).
std::optional<SceneCheckVerdict> parseSceneCheckOutput(const QByteArray &output, QString *error = nullptr);

// A key that changes when the item (the newest modification time of its folder tree, bounded to 2000 entries), the
// Wallpaper Engine assets directory or the engine binary changes, so a cached verdict never outlives any of them.
QString sceneCheckKey(const QString &itemDir, const QString &enginePath, const QString &assetsDir = {});

// $XDG_CACHE_HOME/murasaki/scene-check.json. Verdicts and the crash counts of the Vulkan engine per key.
class SceneCheckCache
{
public:
    // `maxEntries` bounds the file; the default is plenty for a library and the cap only guards against growth.
    explicit SceneCheckCache(QString path = defaultPath(), int maxEntries = 2000);
    static QString defaultPath();
    // One cache for the whole process: every output's SceneItem reads and writes the same file, and each would drop
    // the others' entries if it kept its own copy.
    static SceneCheckCache &shared();

    std::optional<SceneCheckVerdict> verdict(const QString &key) const;
    void storeVerdict(const QString &itemDir, const QString &key, const SceneCheckVerdict &verdict);
    // A check that could not give a verdict (crash, timeout, garbage) is remembered too, for kCheckFailureTtl, so a
    // broken engine is not started again for every launch. `atMs` is for tests.
    void storeFailure(const QString &itemDir, const QString &key, const QString &reason, qint64 atMs = -1);
    std::optional<QString> failure(const QString &key, qint64 nowMs = -1) const; // the reason, while it is fresh
    int crashes(const QString &key) const;
    // Returns the new count. A key that has no entry yet (the verdict is not cached) is created.
    int recordCrash(const QString &itemDir, const QString &key);

private:
    struct Entry {
        QString dir;
        bool hasVerdict = false;
        SceneCheckVerdict verdict;
        QString failure;       // set: the last check failed with this
        qint64 failedAtMs = 0;
        int crashes = 0;
    };
    void load() const;
    void save() const;
    void forgetOtherKeysOf(const QString &dir, const QString &key) const;

    QString m_path;
    int m_maxEntries;
    mutable bool m_loaded = false;
    mutable QHash<QString, Entry> m_entries;
};

// How long a failed check (not a "no", a failure to answer: it timed out, was killed, or could not start) is believed.
// Such a failure says nothing about the item, only about the moment (a loaded machine, a restart), so it is retried soon;
// only a real verdict ({"ok": false, reasons}) stays in the cache.
inline constexpr qint64 kCheckFailureTtlMs = 2ll * 60 * 1000;

// Runs `--check` processes without ever blocking the caller: one per key at a time, whatever the number of askers,
// and at most kMaxParallelChecks at once. Each check starts a GPU context, so a burst of switches (a playlist, a
// script, a user clicking through the library) used to start dozens of them at once, starve each other and time out.
// The newest request is served first, and the oldest wait is dropped past kMaxQueuedChecks (nothing is cached for
// it: it is asked again if it is ever needed). The outcome (a verdict or a failure) goes into the cache and
// `verdictReady(itemDir)` is emitted from the event loop, so a label can refresh. Needs a running event loop.
inline constexpr int kMaxParallelChecks = 2;
inline constexpr int kMaxQueuedChecks = 4;

class SceneCheckRunner : public QObject
{
    Q_OBJECT
public:
    explicit SceneCheckRunner(SceneCheckCache *cache = nullptr, int timeoutMs = 30000, QObject *parent = nullptr);
    ~SceneCheckRunner() override;
    // The process-wide runner, working on SceneCheckCache::shared().
    static SceneCheckRunner &shared();

    // Starts `<engine> --check <itemDir> [--assets-dir <dir>]` unless one is already running for `key`.
    void request(const QString &itemDir, const QString &assetsDir, const QString &engine, const QString &key);
    bool running(const QString &key) const;
    int runningCount() const { return int(m_jobs.size()); }
    int queuedCount() const { return int(m_queue.size()); }
    // `--check` parses the item, so it runs behind murasaki-sandbox like the engine itself (the default). With `require`
    // a check that could not be sandboxed is not run at all (it fails with that reason).
    void setSandbox(bool sandbox) { m_sandbox = sandbox; }
    void setRequireSandbox(bool require) { m_require = require; }

signals:
    void verdictReady(const QString &itemDir);

private:
    struct Job;
    struct Queued {
        QString itemDir, assetsDir, engine, key;
    };
    void start(const Queued &what);
    void startNext();
    void finish(const QString &key, const std::optional<SceneCheckVerdict> &verdict, const QString &failure);

    SceneCheckCache *m_cache;
    int m_timeoutMs;
    bool m_sandbox = true;
    bool m_require = false;
    QHash<QString, Job *> m_jobs;
    QList<Queued> m_queue; // newest first
};

// The Vulkan engine is given up for an item after this many crashes of it (the first one gets a restart).
inline constexpr int kVulkanCrashLimit = 2;

struct SceneEngineRequest {
    SceneEngineMode mode{};
    QString itemDir;
    QString assetsDir;
    QString vulkanProgram; // empty when not installed
    SceneCheckCache *cache = nullptr;   // null: verdicts are not cached (and `auto` cannot learn anything)
    SceneCheckRunner *runner = nullptr; // null: no checks are started; `auto` then only uses cached verdicts
    // The scene is one picture across several outputs. Only the Vulkan engine can draw a part of a canvas, so the
    // mode is overridden: see SceneEngineChoice::refusal.
    bool span = false;
};

struct SceneEngineChoice {
    QString program;  // empty: no engine installed
    bool vulkan = false;
    QString label;    // "Vulkan" or "Vulkan (incomplete: reason)": what the GUI shows after "Engine:"
    bool pending = false; // `auto` is waiting for a check (never a blocking wait)
    QString refusal;  // set (and program empty): a spanned scene that no installed engine can draw, and why
    QString key;      // sceneCheckKey() of the Vulkan engine for this item, empty without one (used by the crash ledger)
};

// Never blocks: a verdict that is not cached yet is requested from the runner and the label follows once it is back.
SceneEngineChoice chooseSceneEngine(const SceneEngineRequest &request);


} // namespace murasaki
