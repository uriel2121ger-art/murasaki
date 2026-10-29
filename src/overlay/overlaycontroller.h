#pragma once

#include "constants.h"
#include "alphamask.h"
#include "dbusbackend.h"
#include "maskloader.h"
#include "pointersource.h"
#include "petfps.h"
#include "petruntime.h"
#include "petstatemachine.h"
#include "pettracker.h"
#include "petsurface.h"
#include "stagegeometry.h"
#include "transcodebuckets.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>

namespace murasaki {

// Keeps one pet surface per placement of the daemon: lists the pets, follows PetsChanged, TranscodeChanged and
// PetsArrangeChanged, decides what each pet plays, and sends the user's edits back (UpdatePet, RemovePet). It
// knows nothing about windows; the factory makes them, which is also how the tests fake them.
class CrashLedger;
class PetAudioController;

class OverlayController : public QObject
{
    Q_OBJECT

public:
    // Creates the surface of one pet on the named output (never "*": the primary output is resolved first).
    using SurfaceFactory = std::function<std::unique_ptr<PetSurface>(const QString &output, const QString &petId)>;

    OverlayController(DBusBackend *daemon, SurfaceFactory factory, QObject *parent = nullptr);
    ~OverlayController() override;

    // Guards the files the windows open: a file that took the process down three times is not opened again (see
    // CrashLedger). Not owned; call before start().
    void setCrashLedger(CrashLedger *ledger) { m_ledger = ledger; }
    // Where the pets' sounds go (not owned; without one the pets are silent). Call before start().
    void setAudio(PetAudioController *audio) { m_audio = audio; }
    PetAudioController *audio() const { return m_audio; }

    // The pointer: where the global cursor comes from (not owned, nullptr: the local level only). The level the pets work at is the source's;
    // when a global source says it is unavailable the session falls back to the local level and every pet is told. The source is asked to
    // run (wanted) only while some pet needs the global cursor.
    void setPointerSource(PointerSource *source);
    PointerSource::Level pointerLevel() const { return m_pointerLevel; }
    // Where the hit masks of the pets that play through the video path come from (not owned; without one they take their whole rectangle),
    // and how many bytes the masks of all the pets may take together (kMaskBudgetBytes by default; each pet's share also holds its union).
    void setMaskLoader(MaskLoader *loader) { m_maskLoader = loader; }
    void setMaskBudget(qint64 bytes);
    qint64 maskBudgetFor() const;

    // The clock the states of the pets run on, in milliseconds (CLOCK_MONOTONIC by default); the tests drive it. Set it before the pets arrive.
    void setStateClock(std::function<qint64()> clock) { m_stateClock = std::move(clock); }
    // Moves the states of the pets on to the clock's now: a sequence that is due advances, a release that is over goes idle. The pets' timer calls it.
    void tickStates();
    // Shows a state of the pet for a few seconds (the GUI's try button). False when the pet is unknown, has no states, or is not shown.
    bool previewState(const QString &petId, const QString &state);
    // Whether the timer of the states is running: false while no pet has a change due (and always for pets without states).
    bool statesTimerActive() const { return m_stateTimer.isActive(); }

    // Connects to the daemon (the backend's start()).
    void start();

    // The outputs the compositor shows now and which one "*" means. A pet whose output is not listed has no
    // surface until it returns.
    // scales are the outputs' device pixel ratios (default 1): they decide how big a transcode a pet needs, and are
    // sent to the daemon (SetOutputScales) whenever they change.
    // geometry is each output's rectangle in the compositor's logical layout (the fullscreen mode, docs/pets/stage.md, needs it; without it a pet in that
    // mode is drawn as an ordinary pet and says "stage: not fullscreen on this session").
    void setOutputs(const QStringList &names, const QString &primary, const QHash<QString, double> &scales = {},
                    const QHash<QString, QRect> &geometry = {});
    // Which outputs have a fullscreen window now, from the surface's session facts (docs/pets/session-facts.md): output name to the application's class
    // ("" when not told; "*" is every output, for a game or a listed application). `known` is false when the session can not say.
    void setOutputFullscreen(const QHash<QString, QString> &apps, bool known);
    // The screens a game plays on ("*" is all of them; SurfaceMuteWatcher::gameScreensChanged): with the setting petsHideOnGame the pets there hide.
    void setGameScreens(const QSet<QString> &outputs);
    // The windows of a `desktop` stage are one picture on several players: those that drifted more than 50 ms from the first one's position are seeked to it. Called
    // by a timer once a second while such a stage exists; the tests call it.
    void syncStageVideo();
    // Whether a stage can be drawn at all on this session: on X11 only when a compositing manager runs (without one a screen-sized translucent window is an opaque
    // rectangle over the desktop). True by default (Wayland and GNOME have their compositor); the overlay tells it on X11 and when it changes.
    void setStageCompositing(bool available);
    // The playback policy: every pet pauses while the session is locked or the screen is off.
    void setSessionPaused(bool paused);
    // "Try again" for a pet the overlay gave up on (blocked after failures or crashes): forgets its failures and the
    // crash record of its file and opens it again. False when the pet is unknown.
    bool resetFailures(const QString &petId);
    // The memory pets may use for decoded frames (the GPU frame cache), 0 for none. Pets are admitted by how much
    // work the cache saves; the others keep playing through the video path.
    void setFrameCacheBudget(qint64 bytes);
    qint64 frameCacheBudget() const { return m_spriteBudget; }
    // What the machine says about power (UPower's OnBattery, power-profiles-daemon's power-saver): the pets are held
    // to a low rate then, unless the setting petSaveEnergy is off.
    void setPowerState(bool onBattery, bool powerSaver);
    // The most frames per second a pet may change at, from the daemon's settings and the power state; 0: no limit.
    int currentFpsCap() const;
    // Flips the arrange mode through the daemon (it announces the result).
    void toggleArrange();
    // Hides or shows every pet through the daemon (PetsHidden), and the rescue (RescuePets): the global shortcuts and the tests call these.
    void toggleHidden();
    void rescue();
    // A global shortcut was pressed: "toggle-arrange", "toggle-hidden" or "rescue"; anything else is ignored.
    void onShortcut(const QString &actionId);

    bool arrange() const { return m_arrange; }
    bool petsHidden() const { return m_petsHidden; }
    bool sessionPaused() const { return m_sessionPaused; }
    int surfaceCount() const;
    // Test hook (see PetSurface::simulateEdit); false when the pet has no surface or the kind is unknown.
    bool simulateEdit(const QString &petId, const QString &kind, double a, double b);
    // JSON array describing every placement: petId, itemId, output, shown, x, y, width, height, interactive,
    // paused, file, bucket (longest side of the transcoded copy in use, 0 for the original), fps (the placement's fixed rate, 0: none), speed (the mpv speed factor), timing ("file", "fixed" or "speed"), timingSpeed (the placement's speed multiplier), fpsCap (the most frames per second the pet changes at, 0: no limit), mode ("sprite" when the
    // frames are drawn from GPU textures, else "video"), cacheBytes (what those textures take), failures, blocked,
    // error (why a blocked pet was given up on), degraded (the names of what its window does less than asked, e.g. "keep above off: always below windows", and of what the pointer input does less: "pointer: local", "hit mask: coarse"...), and for a pet with a surface the pointer input: pointer ("global" or "local"), hitMask ("frame", "silhouette" or "none"), maskBytes, hidden (faded by hover or focus).
    QJsonArray describe() const;

signals:
    // Something visible to describe() changed.
    void stateChanged();
    // The daemon speaks an API older than 5; nothing is shown.
    void incompatibleDaemon(quint32 version);

private:
    struct Variant {
        QString name;
        QString file;
        QSize size;
        struct DoneFile {
            int bucket = 0;
            int fps = -1; // the playback rate it was made at; -1: a daemon that does not say (the file's own timing)
            QString file;
        };
        QList<DoneFile> doneFiles; // finished transcodes
        bool copyPending = false;  // a copy is queued or being made (the original plays meanwhile)
        double fps = 0;
        int frames = 0;
    };
    struct ItemInfo {
        bool known = false; // the daemon answered for this item
        QList<Variant> variants;
        // A puppet item (format "puppet", docs/pets/puppets.md): its folder, the model file in it (relative), and what it can do. The pet is drawn by an engine process, not played from a file.
        bool puppet = false;
        QString puppetDir;
        QString puppetFile;
        QStringList motionGroups;
        QStringList expressions;
    };
    struct Entry {
        PetPlacement stored;    // as the daemon stores it
        PetPlacement placement; // what is shown: the stored one with the runtime layer (variant, output, opacity, position) on top
        std::unique_ptr<PetSurface> surface;
        // The fullscreen mode: the windows of the outputs after the first (surface is the first's), and what is told about it in describe().
        struct StageWindow {
            QString output;
            std::unique_ptr<PetSurface> surface;
        };
        std::vector<StageWindow> stageWindows;
        QStringList stageOutputs;   // the outputs the stage covers now
        QSet<QString> stageHiddenOutputs; // the windows that were hidden by a fullscreen window at the last apply
        QSet<QString> syncPlaying;  // the windows that had a position at the last span sync (the reference is one of them)
        QStringList stageDegraded;  // names from stagedegrade
        double stageScale = 1;      // the fraction of full resolution it is drawn at
        QString stageHidden;        // why it is not drawn: "fullscreen", "session" or "hidden"; empty while it is
        QString output;      // the output the surface was made for
        QString file;        // what it plays now
        // A puppet pet: how its engine is doing ("running", "restarting", "refused", "nogpu" or "blocked"; empty before the first start), the names it degrades by, the motion group and expression last handed to the window,
        // the request counter that makes a window play again, and whether the gaze follows the pointer (Mouse Tracking `look`).
        QString puppetState;
        QStringList puppetDegraded;
        QString puppetStateGroup;
        QString puppetGroup;
        QString puppetExpression;
        int puppetSerial = 0;
        bool puppetLook = false;
        int bucket = 0;      // the bucket of that file (0: the original)
        int failures = 0;
        QString lastError;   // why the file could not be played (empty while it plays)
        bool blocked = false; // too many failures; waits for another file
        QString blockedFile;
        QString badTranscode; // a transcoded copy that failed to play: the original is used instead
        bool usesTranscode = false;
        PetView lastView;
        QString startedFile;  // the file that played (or whose frames showed) last
        // The GPU frame cache
        bool spriteAdmitted = false;
        QSize spriteSize;
        QString spritePlanKey;   // file and size the plan wants frames of
        qint64 spriteCost = 0;   // what it holds against the budget: the estimate, or the real size once the frames are in
        qint64 spriteOwn = 0;    // the frames this pet is waiting for (or has, when they are built)
        qint64 spriteAllowance = 0; // the most a decode for this pet may take
        bool spriteDecoding = false; // a key was handed over and its decode has not reported yet
        QString spriteKey;       // the last key handed to the window
        qint64 spriteBytes = 0;  // what the frames on screen take (0: none)
        QString spriteFailedKey; // a key that could not be decoded: not tried again
        // The preloaded picture (docs/pets/states.md): a second set of frames for the state likely to come next, within the same budget (only a pet whose states
        // differ in picture, on the frame cache, with room for it).
        bool preloadAdmitted = false;
        QString preloadFile;
        QSize preloadSize;
        QString preloadKey;          // what the plan wants prepared
        qint64 preloadEstimate = 0;
        qint64 preloadAllowance = 0;
        QString preloadHandedKey;    // what the window was told to prepare
        QString preloadReadyKey;     // what it says is ready
        qint64 preloadBytes = 0;     // what that set takes
        QString preloadFailedKey;    // not tried again
        // The hit masks of a pet on the video path (the mask pass of the file it plays); a pet on the frame cache brings its own.
        std::shared_ptr<const MaskSet> passMasks;
        QString passFile;        // the file passMasks (or the pass under way) is for
        bool passPending = false;
        bool masksWanted = false; // the frame decode makes masks (the pet has needed them since its frames were last asked for)
        // The states of the pet (docs/pets/states.md): only a pet whose placement has `states` has the machine, and so a timer, an event or a decode more.
        std::unique_ptr<PetStateMachine> states;
        unsigned stateGeneration = 0; // the machine's generation that placement.variant was taken from
        unsigned stateRestarts = 0;   // the machine's count of clicks that play the same picture again
        int restartToken = 0;         // what the window is told: another value, the animation starts from its first frame
        quint64 stateItemsRevision = quint64(-1); // of m_items, when the machine was last told which variants exist
        bool stateHolding = false;    // the window was told to hold the last frame
        PetPoint dragPos;             // where the pointer is, from where the drag began (the machine reads positions)
        qint64 switchStartedAt = -1;  // when a change of state handed the window another file; -1 once it plays
        qint64 switchMs = -1;         // how long the last such change took to show (-1: none yet)
        // Mouse Tracking (docs/pets/states.md): only a pet whose placement tracks, on a session with the global pointer, has the tracker.
        std::unique_ptr<PetTracker> tracker;
        std::optional<bool> trackMirror; // the mirror the window was last handed
        PetPoint trackOffset;            // and the displacement of a follower
        QPoint trackOrigin;              // the origin of the space the tracker works in (its output's, or that of the first slice of a stage)
        bool trackMoving = false;        // what Windows() last had to say about a follower: walking
        bool trackParked = false;
        bool trackForceHome = false;     // the follow came from the runtime layer (a timeline): it always glides home and never stores a place
        qint64 lastPreviewAt = -1000000; // when the GUI last asked to see a state
        std::optional<PetPoint> pendingHome; // where the pet was stored (a drop, or a follower that stopped where it was) until the daemon says so
    };

    void daemonCall(const QString &method, const QVariantList &args, DBusBackend::Callback callback);
    void daemonProperty(const QString &name, DBusBackend::Callback callback);
    void onAvailability(bool available);
    void onSignal(const QString &name, const QVariantList &args);
    void requestList();
    void loadSettings();
    void setPetLimits(int maxFps, bool saveEnergy);
    void sendOutputScales();
    double scaleOf(const PetPlacement &placement) const;
    void fetchItem(const QString &itemId);
    void reconcile();
    void applyEntry(Entry &entry);
    // A puppet pet's apply: no sprites, no transcode, no mask pass; the engine is the window's. The states and clicks become motion groups and expressions.
    void applyPuppetEntry(Entry &entry, PetView view);
    void onPuppetCrashed(const QString &petId, bool afterLongRun);
    void onPuppetRefused(const QString &petId, const QString &name);
    bool isPuppet(const QString &itemId) const;
    qint64 m_puppetMaxPixels = 4194304; // the setting puppetMaxPixels
    bool m_requirePuppetSandbox = true; // the setting importRequireSandbox: the engine runs only confined
    // placement = the stored one with the runtime layer, and the variant of the state the pet is in when it has states (the machine is created, fed and dropped here).
    void refreshPlacement(Entry &entry);
    void refreshStates(Entry &entry);
    void refreshTracking(Entry &entry);
    bool inactive(const Entry &entry) const;   // hidden, paused or arranged: the pet reacts to nothing
    bool trackerRuns(const Entry &entry) const;
    void trackerMoved(Entry &entry);
    void homeAnswered(const QString &petId, bool ok);
    void sendHome(const QString &petId, PetPoint place);
    PetPoint cursorInOutput(const Entry &entry, QPointF global) const;
    bool fullscreenOn(const Entry &entry) const; // a fullscreen window is on the pet's output (a follower parks)
    void onPointerEvent(const QString &petId, const PetPointerEvent &event);
    void finishSwitch(const QString &petId, Entry &entry);
    // The machine moved: when that changed what the pet shows, the window gets the new picture; and the timer is set for the next due change.
    void statesMoved(Entry &entry);
    void deferStatesMoved(const QString &petId);
    void scheduleStates();
    qint64 stateNow() const;
    bool itemUsed(const QString &itemId) const;
    bool resolveView(const PetPlacement &placement, const QString &badTranscode, PetView *view) const;
    void onStarted(const QString &petId);
    void onSpriteDone(const QString &petId, bool ok, qint64 bytes, const QString &reason);
    void onPreloadDone(const QString &petId, bool ok, qint64 bytes, const QString &key);
    void planSprites();
    const Variant *variantOf(const PetPlacement &placement) const;
    QString effectiveOutput(const PetPlacement &placement) const;
    void wire(Entry &entry);
    void onFailed(const QString &petId, const QString &reason);
    // Closes the pet's window and the "currently opening" marker that goes with it.
    void dropSurface(const QString &petId, Entry &entry);
    // The pet's file is no longer "opening" (it plays, failed, or the window is gone): the next pet may open.
    void finishOpening(const QString &petId);
    void requestMaskPass(Entry &entry, const PetView &view);
    void onMaskPass(const QString &petId, const QString &file, std::shared_ptr<const MaskSet> masks);
    void updatePointerWanted();
    void onPointerMoved(QPointF global);
    void onPointerUnavailable(const QString &reason);
    void reapplyAll();
    // The fullscreen mode (docs/pets/stage.md): the layout of a stage pet on the outputs there are, whether another stage has its outputs, and whether a
    // fullscreen window hides it on `output`.
    bool stageLayoutFor(Entry &entry, const PetView &view, StageLayout *layout);
    // The layout of a stage placement on the outputs there are, without touching an entry; false when the session did not say where they are.
    QList<StageOutput> stageOutputList() const;
    StageRequest stageRequestOf(const PetPlacement &placement, QSize native) const;
    bool buildStageLayout(const PetPlacement &placement, QSize native, StageLayout *layout) const;
    // The longest side, in pixels, the stage's animation is drawn at (the resolution cap included); 0 when it can not be told.
    int stageNeedFor(const PetPlacement &placement, QSize native, int sourceLongest) const;
    // The fraction of full resolution all the stages are drawn at so that together they stay within stageMaxPixels (they share the cap).
    double computeStageFactor() const;
    // The placement as it is drawn: a stage the session can not draw (no layout) is an ordinary pet, so it is without its stage for everything that decides
    // how a pet is played (masks, the copy, the pointer).
    PetPlacement effectivePlacement(const PetPlacement &placement) const;
    bool anyStage() const;
    bool stageTaken(const Entry &entry) const;
    QStringList stageOutputsOf(const Entry &entry) const;
    bool hiddenByFullscreen(const Entry &entry, const QString &output) const;
    void dropStageWindows(Entry &entry);
    // Tells the sound controller what the pet is now (shown, paused, click-through, its sound settings).
    void pushAudio(const Entry &entry, bool shown);
    // The runtime layer (docs/pets/automation.md): what timelines and triggers changed about the pets without touching their stored placement.
    void loadRuntime();
    void applyMute();
    PetPlacement withRuntime(const PetPlacement &stored) const;
    PetTracking trackingOf(const PetPlacement &stored) const;
    bool runtimeHidden(const QString &petId) const;
    bool gameHidden(const PetPlacement &placement) const; // on a game's screen while petsHideOnGame is on (a stage follows its own rule)
    void updateMotionTimer();
    void onMotionTick();

    DBusBackend *m_daemon;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // false once the controller is gone (see daemonCall)
    SurfaceFactory m_factory;
    std::map<QString, Entry> m_entries; // by petId
    QStringList m_order;             // petIds in the daemon's order
    QHash<QString, ItemInfo> m_items;
    QSet<QString> m_fetching;
    QSet<QString> m_refetch; // asked again while a fetch was in flight
    QStringList m_outputs;
    QHash<QString, QRect> m_geometry;
    QHash<QString, QString> m_fullscreen;
    long long m_stageMaxPixels = kDefaultStageMaxPixels;
    double m_stageFactor = 1.0;
    bool m_stageCompositing = true;
    bool m_fullscreenAnswered = false; // the surface said something (or is known to be absent): until then a stage that would hide for a fullscreen window waits
    QTimer m_stageSync;
    QElapsedTimer m_syncClock;
    QHash<QString, qint64> m_lastSeek; // petId/output -> when its window was last seeked (m_syncClock)
    bool m_fullscreenKnown = false;
    bool m_stageHideOverFullscreen = kDefaultStageHideOverFullscreen;
    bool m_petsHideOnGame = kDefaultPetsHideOnGame;
    QSet<QString> m_gameScreens;
    QString m_primary;
    QHash<QString, double> m_scales;
    quint32 m_apiVersion = 0;
    qint64 m_spriteBudget = 0;
    // What the frames of a (file, size) key really took once decoded (cut to their opaque rect): the admission uses it, instead of the whole frames, for the same key again (another pet that
    // plays it, or this one after its frames were let go). Forgotten when a decode of the key fails; never larger than the whole frames.
    QHash<QString, qint64> m_croppedBytes;
    bool m_arrange = false;
    bool m_settingsMuted = false;  // the setting petsMuted
    bool m_sessionMuted = false;   // the daemon's PetsMuted (revision 18): muted by a trigger, never stored
    bool m_petsHidden = false; // the daemon's PetsHidden: the windows are unmapped and the pets paused
    QHash<QString, PetRuntime> m_runtime; // the daemon's PetRuntime: hidden, moved or re-animated pets (never stored)
    quint64 m_runtimeSeq = 0;
    QTimer m_motionTimer;                 // runs only while some pet has a motion that has not ended
    bool m_sessionPaused = false;
    int m_petMaxFps = kDefaultPetMaxFps;
    bool m_petSaveEnergy = kDefaultPetSaveEnergy;
    bool m_onBattery = false;
    bool m_powerSaver = false;
    bool m_available = false;
    bool m_compatible = true;
    quint64 m_listSeq = 0;
    CrashLedger *m_ledger = nullptr;
    PetAudioController *m_audio = nullptr;
    PointerSource *m_pointer = nullptr;
    PointerSource::Level m_pointerLevel = PointerSource::Level::Local;
    MaskLoader *m_maskLoader = nullptr;
    qint64 m_maskBudget = qint64(kMaskBudgetBytes);
    // With a ledger, one window at a time opens its file, so that a crash is charged to the one file that was
    // being opened and not to every pet that happened to start in the same pass.
    QString m_openingPet;
    QTimer m_openTimer;
    std::function<qint64()> m_stateClock;
    quint64 m_itemsRevision = 0;           // grows whenever what is known of the items changes
    std::optional<qint64> m_stateWake;     // when the state timer was last set to fire
    QTimer m_stateTimer; // single shot, for the earliest change due among the pets that have states
};

} // namespace murasaki
