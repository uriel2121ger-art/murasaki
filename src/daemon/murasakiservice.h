#pragma once

#include "assignment.h"
#include "itempermissions.h"
#include "library.h"
#include "localpets.h"
#include "overlayunit.h"
#include "petimportmanager.h"
#include "petstore.h"
#include "playlist.h"
#include "settings.h"
#include "steamcmdmanager.h"
#include "steamlocator.h"
#include "stagegeometry.h"
#include "transcodemanager.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QTimer>

#include "shortcut/shortcutpolicy.h"

namespace murasaki {

class AutomationManager;
class PetAudioManager;
class PlaylistManager;
class PuppetChecker;
class ScreenRefStore;
class ScreenWatch;
class ProfileStore;
class TimelineStore;
class TriggerStore;

// D-Bus facade of murasakid (API documented in docs/dbus-api.md).
// Items travel as JSON strings so the schema can grow without changing D-Bus signatures.
class MurasakiService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki1")
    Q_PROPERTY(quint32 ApiVersion READ apiVersion CONSTANT)
    Q_PROPERTY(QString Status READ status CONSTANT)
    Q_PROPERTY(QString WallpaperEngineAssetsDir READ wallpaperEngineAssetsDir CONSTANT)
    // Arrange mode of the desktop pets (not persisted); changes are announced by PetsArrangeChanged.
    Q_PROPERTY(bool PetsArrange READ petsArrange)
    // The desktop pets are hidden by the user (not persisted, like the arrange mode: false after a daemon restart); announced by PetsHiddenChanged.
    Q_PROPERTY(bool PetsHidden READ petsHidden)
    // The user is clicking on the wallpaper (revision 13): the overlay catches the pointer above the desktop and passes it on to interactive wallpapers.
    // Not persisted (false after a restart); announced by WallpaperInteractChanged. Never on together with PetsArrange.
    Q_PROPERTY(bool WallpaperInteract READ wallpaperInteract)
    // Revision 18 (docs/pets/automation.md). The profile that was applied or saved last (its id; empty for none) and whether the pets on the desktop differ from it.
    // Revision 18. The pets' sounds are muted for this session (by a trigger's `mute` action or `SetPetsMuted`); never stored: false after a restart. On top of the setting petsMuted.
    Q_PROPERTY(bool PetsMuted READ petsMuted)
    Q_PROPERTY(QString ActiveProfile READ activeProfile)
    Q_PROPERTY(bool ProfileModified READ profileModified)
    // JSON {petId: {hidden?, x?, y?, output?, motion?, opacity?, variant?}}: what timelines and triggers changed about the pets without touching their stored placement.
    // Never stored (empty after a restart); announced by PetRuntimeChanged.
    // Revision 21: JSON {state: off|asking|watching|stopped|refused|failed, reason, regions, ticksPerSecond, since, paused, notes}.
    Q_PROPERTY(QString ScreenWatchState READ screenWatchState)
    Q_PROPERTY(QString PetRuntime READ petRuntime)
    // JSON [{id, description}]: the shortcut triggers that are enabled (the overlay registers them with the desktop); announced by TriggersChanged.
    Q_PROPERTY(QString ShortcutTriggerIds READ shortcutTriggerIds)

public:
    MurasakiService(Library *library, AssignmentStore *assignments, SettingsStore *settings, QString status,
                    QString assetsDir, QObject *parent = nullptr);

    // "ok" | "steam-not-found" | "wallpaper-engine-not-installed"
    static QString statusFor(const SteamLocator &steam);

    // Enables the pet API (revision 5): placements and the transcode cache. Without it the pet calls fail. Call
    // once, right after construction; both objects must outlive the service.
    void attachPets(PetStore *pets, TranscodeManager *transcodes);

    // Enables AddLocalPetItem and RemoveLocalPetItem; the store must outlive the service. The library must already
    // know the store's files (Library::setLocalPetFiles).
    void attachLocalPets(LocalPetStore *localPets);
    // Enables AddLocalPuppetItem and RemoveLocalPuppetItem (revision 20): the store of the puppet folders (a LocalPetStore on local-puppets.json with LocalPuppetImporter::itemIdFor) and the checker that runs
    // `murasaki-puppet --check` behind the sandbox. Both must outlive the service; the library must already be given the checker's answers (Library::setPuppetCheckProvider).
    void attachLocalPuppets(LocalPetStore *localPuppets, PuppetChecker *checker);

    // Enables the pets' sound calls (revision 15): importing a sound file and the asset folder. The manager and the pet store must outlive the service.
    void attachPetAudio(PetAudioManager *audio);

    // Enables the playlist API (revision 7): without it the playlist calls fail and "playlist:<id>" can not be
    // assigned. Call once, right after construction; both objects must outlive the service.
    void attachPlaylists(PlaylistStore *playlists, PlaylistManager *manager);

    // Enables SetItemPermission (revision 8); without it every item reports "no permissions" and the call fails.
    // The store must outlive the service. Call once, right after construction.
    void attachPermissions(ItemPermissionStore *permissions);

    // Enables the SteamCMD calls (revision 9); without it they fail. The manager must outlive the service.
    void attachSteamCmd(SteamCmdManager *steamcmd);

    // Enables the import calls (revision 14); without it they fail. The manager must outlive the service, and so must the local pet store (a finished
    // import is added as a local pet).
    void attachImports(PetImportManager *imports);

    // Enables the automation (revision 18): profiles, triggers, timelines and the runtime layer of the pets. The stores must outlive the service; statePath keeps the
    // label of the active profile. Call once, after attachPets().
    void attachAutomation(ProfileStore *profiles, TriggerStore *triggers, TimelineStore *timelines, const QString &statePath);
    AutomationManager *automation() const { return m_automation; }

    // Enables the screen triggers (revision 21): the reference store, the helper `murasaki-screenmatch` (empty: none found) with the sandbox that confines it, the crash ledger's
    // folder and the file that keeps the portal's restore token. Nothing is opened until the user calls StartScreenWatch. Call after attachAutomation(). `bus` is the bus the portal is
    // on (the session bus; a private one in tests).
    void attachScreen(ScreenRefStore *refs, const QString &helper, const QString &sandbox, bool requireSandbox, const QString &crashDir, const QString &statePath,
                      const QDBusConnection &bus = QDBusConnection::sessionBus());
    ScreenWatch *screenWatch() const { return m_screenWatch; }
    QString screenWatchState() const;

    // How long after a start request another one may be sent while pets exist (a start that failed is retried).
    void setOverlayRetryMs(qint64 ms) { m_overlayRetryMs = ms; }
    // How long the interaction with the wallpaper may last whatever happens (default 10 minutes).
    void setInteractCapMs(int ms) { m_interactCapMs = ms; }

    // The overlay process is started when the first pet exists (right away if there already is one) and stopped
    // when the last one goes. The unit must outlive the service. Call after attachPets().
    void setOverlayUnit(OverlayUnit *unit);

    // How long a resize or a change of the output scales waits for further ones before the transcodes are brought
    // up to date (and the copies nobody needs are removed).
    void setTranscodeDebounceMs(int ms) { m_syncTimer.setInterval(ms); }

    // How long after attachPets() the pets wait for the overlay's first SetOutputScales before their transcodes are
    // queued with the default scale (a HiDPI screen would otherwise get a blurry bucket first). Call before it.
    void setScalesGraceMs(int ms) { m_graceTimer.setInterval(ms); }

    // Every conversion worth keeping for the library's pet variants (see bucketsToKeep), for the startup prune.
    QList<TranscodeSource> expectedTranscodes() const;

    bool registerObject(QDBusConnection &bus);

    quint32 apiVersion() const;
    QString status() const { return m_status; }
    QString wallpaperEngineAssetsDir() const { return m_assetsDir; }
    bool petsArrange() const { return m_petsArrange; }
    bool wallpaperInteract() const { return m_wallpaperInteract; }
    bool petsHidden() const { return m_petsHidden; }
    bool petsMuted() const { return m_petsMuted; }
    QString activeProfile() const;
    bool profileModified() const;
    QString petRuntime() const;
    QString shortcutTriggerIds() const;
    // Starts scene `index` of the package on the pet (what StartScene does). A scene that hides the pet hides it in the runtime layer. False with *problem set.
    bool startScene(const QString &petId, int index, QString *problem);

public slots:
    Q_SCRIPTABLE QString ListItems() const;
    Q_SCRIPTABLE QString GetItem(const QString &id) const;
    Q_SCRIPTABLE void Rescan();
    Q_SCRIPTABLE void SetAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                    const QString &optionsJson);
    Q_SCRIPTABLE void ClearAssignment(const QString &output, const QString &desktop);
    Q_SCRIPTABLE QString Assignments() const;
    Q_SCRIPTABLE QString ResolveAssignment(const QString &output, const QString &desktop) const;
    Q_SCRIPTABLE QString Settings() const;
    // A wallpaper's click asked to start the application the user chose for the usershortcut property `key` of the item in `itemDir`, shown on
    // `output`. The surface already saw a real click; the daemon checks the rest from its own data (docs/design/specs/2026-10-21-user-shortcuts-design.md).
    // True when an application was started.
    Q_SCRIPTABLE bool LaunchShortcut(const QString &output, const QString &desktop, const QString &itemDir, const QString &key);
    // The desktop id the user stored for the usershortcut property `key` in the one assignment that shows the item in `itemDir` on (`output`,
    // `desktop`) (what ResolveAssignment says, a playlist's current item included), or an empty string. Nothing else is looked at: a wildcard
    // assignment that is not the one shown, or another desktop's, never counts.
    QString assignedShortcut(const QString &output, const QString &desktop, const QString &itemDir, const QString &key) const;
    Q_SCRIPTABLE void SetSettings(const QString &settingsJson);

    // Pets (API revision 5). Placements are JSON objects, see docs/dbus-api.md.
    Q_SCRIPTABLE QString ListPets() const;
    Q_SCRIPTABLE QString AddPet(const QString &itemId, const QString &variant, const QString &output,
                                const QString &optionsJson);
    Q_SCRIPTABLE void UpdatePet(const QString &petId, const QString &changesJson);
    Q_SCRIPTABLE void RemovePet(const QString &petId);
    Q_SCRIPTABLE QString TranscodeState(const QString &itemId) const;
    Q_SCRIPTABLE void RequestTranscode(const QString &itemId, const QString &variant);
    // Turning the arrange mode on turns the wallpaper interaction off (WallpaperInteractChanged follows).
    Q_SCRIPTABLE void SetPetsArrange(bool arrange);
    // Turns the interaction with the wallpaper on or off. Signals only when the value changes; never stored. InvalidArgs while the pets are being arranged.
    Q_SCRIPTABLE void SetWallpaperInteract(bool interact);
    // Hides or shows every desktop pet. Signals only when the value changes; never stored.
    Q_SCRIPTABLE void SetPetsHidden(bool hidden);
    // Revision 18: mutes or unmutes the pets' sounds for the session (not stored; the setting petsMuted is separate). Signals only when the value changes.
    Q_SCRIPTABLE void SetPetsMuted(bool muted);
    // The way out of a forgotten flag, in ONE write of the store: every pet gets clickThrough off, keepAbove on, hoverHide, focusFade, smartDodge and locked off,
    // and opacity 1 if it was below 0.5; rotation, flips, positions, sizes and items are untouched. The pets are shown again. Arrange mode is not turned on.
    Q_SCRIPTABLE void RescuePets();
    Q_SCRIPTABLE QString AddLocalPetItem(const QString &path);
    Q_SCRIPTABLE void RemoveLocalPetItem(const QString &itemId);
    // Revision 20: a puppet folder as a pet item (id "local:puppet:<sha1 of the canonical path>"), and forgetting it.
    Q_SCRIPTABLE QString AddLocalPuppetItem(const QString &path);
    Q_SCRIPTABLE void RemoveLocalPuppetItem(const QString &itemId);

    // Revision 15: the sound of a pet. ImportPetAudio queues the conversion of a sound file into an asset (the id of the job comes back; read it with
    // PetAudioImportState, which is JSON, and watch PetAudioChanged); ListPetAudioAssets is JSON; an asset is used by a pet through the placement key "audio".
    Q_SCRIPTABLE QString ImportPetAudio(const QString &path);
    // The sound of an imported pet whose video was imported with the audio kept (`keepAudio`): queues its conversion like ImportPetAudio and returns the job id.
    // `InvalidArgs` for an item that is not an import of this daemon or has no kept audio. The pets that use the item are not touched (their mode stays `none`).
    Q_SCRIPTABLE QString ImportItemAudio(const QString &itemId);
    Q_SCRIPTABLE QString PetAudioImportState(const QString &jobId) const;
    Q_SCRIPTABLE void CancelPetAudioImport(const QString &jobId);
    Q_SCRIPTABLE QString ListPetAudioAssets() const;
    Q_SCRIPTABLE void RemovePetAudioAsset(const QString &asset);
    Q_SCRIPTABLE int CollectPetAudio();

    // Revision 6: the overlay's output scales, {"DP-1": 1.0, ...}; decides how big the transcodes must be.
    Q_SCRIPTABLE void SetOutputScales(const QString &scalesJson);

    // Revision 7: playlists. Playlists are JSON objects, see docs/dbus-api.md.
    Q_SCRIPTABLE QString ListPlaylists() const;
    Q_SCRIPTABLE QString CreatePlaylist(const QString &name);
    Q_SCRIPTABLE void UpdatePlaylist(const QString &playlistId, const QString &changesJson);
    Q_SCRIPTABLE void AddToPlaylist(const QString &playlistId, const QStringList &itemIds);
    Q_SCRIPTABLE void RemoveFromPlaylist(const QString &playlistId, const QStringList &itemIds);
    Q_SCRIPTABLE void DeletePlaylist(const QString &playlistId);
    Q_SCRIPTABLE void NextInPlaylist(const QString &output, const QString &desktop);
    Q_SCRIPTABLE void PreviousInPlaylist(const QString &output, const QString &desktop);

    // Revision 8: what the user lets an item do. permission is "network" (web wallpapers); denied by default.
    Q_SCRIPTABLE void SetItemPermission(const QString &itemId, const QString &permission, bool allowed);

    // Revision 9: optional Workshop downloads through SteamCMD, with the user's own Steam account. Never stores the
    // password: SteamCmdSubmit hands it to SteamCMD's terminal once. See docs/dbus-api.md.
    Q_SCRIPTABLE QString SteamCmdStatus() const;
    Q_SCRIPTABLE void SteamCmdLogin(const QString &user);
    Q_SCRIPTABLE void SteamCmdSubmit(const QString &kind, const QString &secret);
    Q_SCRIPTABLE void SteamCmdCancelLogin();
    Q_SCRIPTABLE void SteamCmdLogout();
    Q_SCRIPTABLE void SteamCmdDownload(const QString &appId, const QString &itemId);
    Q_SCRIPTABLE void SteamCmdCancel(const QString &itemId);

    // Revision 14: pet imports (docs/pets/import.md). `specJson` is the job description of importspec.h. ImportPet queues a job and returns its id;
    // PreviewImport looks at the headers only and returns {"ok":true,"preview":{...}} or {"ok":false,"error":"<name>"}; ImportState lists the jobs;
    // CancelImport stops one. A finished job is added as a local pet by the daemon itself (the job's "itemId"). RemoveImportedPet removes a local pet
    // and, with deleteFiles, the converted files of an imported one (nothing else is ever deleted).
    Q_SCRIPTABLE QString ImportPet(const QString &specJson);
    Q_SCRIPTABLE QString PreviewImport(const QString &specJson);
    Q_SCRIPTABLE QString ImportState() const;
    Q_SCRIPTABLE void CancelImport(const QString &jobId);
    Q_SCRIPTABLE void RemoveImportedPet(const QString &itemId, bool deleteFiles);
    // The scenes of an imported .mdaa package (the item JSON lists them as "scenes", in actions.mddo order): starts scene `index` on a pet of that item by
    // setting its variant and, when the scene moves it, its position. A scene that hides the pet needs triggers a later phase brings: InvalidArgs.
    Q_SCRIPTABLE void StartScene(const QString &petId, int index);

    // Revision 18: profiles (a named set of the pets on the desktop). ListProfiles is JSON. SaveProfile saves the pets as the profile of that name (replacing one of the
    // same name) and returns its id; SwitchProfile takes an id or a name, makes the pets the profile's in ONE write and returns a JSON report {profile, kept, updated,
    // added, removed, skipped: [{slot, itemId, reason}]}.
    Q_SCRIPTABLE QString ListProfiles() const;
    Q_SCRIPTABLE QString SaveProfile(const QString &name);
    Q_SCRIPTABLE QString SaveExampleProfile(const QString &name);
    Q_SCRIPTABLE QString SwitchProfile(const QString &idOrName);
    Q_SCRIPTABLE void RenameProfile(const QString &idOrName, const QString &newName);
    Q_SCRIPTABLE void SetProfileOrigin(const QString &idOrName, const QString &origin);
    Q_SCRIPTABLE void DeleteProfile(const QString &idOrName);
    // Revision 18: triggers. SetTrigger with an empty id creates one (the id comes back), else changes it; `changesJson` is the trigger's JSON (docs/pets/automation.md).
    Q_SCRIPTABLE QString ListTriggers() const;
    Q_SCRIPTABLE QString SetTrigger(const QString &triggerId, const QString &changesJson);
    Q_SCRIPTABLE void DeleteTrigger(const QString &triggerId);
    Q_SCRIPTABLE void SetTriggerEnabled(const QString &triggerId, bool enabled);
    // The triggers named `name` fire (and, with the setting fireByTimelineName and none of that name, the timeline). True when something matched.
    Q_SCRIPTABLE bool FireTrigger(const QString &name);
    // Runs the trigger now as if its source had happened. True when its action ran.
    Q_SCRIPTABLE bool TestTrigger(const QString &triggerId);
    // JSON {states: [{id, enabled, available, outcome, agoMs?, rateLimited, note?}], log: [{id, agoMs, outcome}]}, the last 50 fires; never written to disk.
    Q_SCRIPTABLE QString TriggerState() const;
    // Revision 18: the rescue pauses the triggers for a minute (TriggerState says how long is left); this ends the pause now.
    Q_SCRIPTABLE void ResumeAutomation();
    // The overlay reports a key of a shortcut trigger (pressed or released).
    Q_SCRIPTABLE void TriggerShortcut(const QString &shortcutId, bool pressed);
    // Revision 18: timelines. Like SetTrigger. RunTimeline and StopTimeline take an id or a name.
    Q_SCRIPTABLE QString ListTimelines() const;
    Q_SCRIPTABLE QString SetTimeline(const QString &timelineId, const QString &changesJson);
    Q_SCRIPTABLE void DeleteTimeline(const QString &timelineId);
    Q_SCRIPTABLE void RunTimeline(const QString &idOrName);
    Q_SCRIPTABLE void StopTimeline(const QString &idOrName);
    Q_SCRIPTABLE QStringList RunningTimelines() const;
    // Revision 18: the automation of an imported package. TimelineFromScene makes scene `index` (counted from 0) a one-step timeline (it does not restore) and returns its
    // id; ImportPackAutomation makes the timeline of a Triggers pack and its two triggers (a `fire` one and the inert `screen` one), both DISABLED, and returns JSON
    // {timeline, triggers: [ids], notes}. Made again they replace what the same package made before. `InvalidArgs` for an item that is not an imported package.
    Q_SCRIPTABLE QString TimelineFromScene(const QString &itemId, int index);
    Q_SCRIPTABLE QString ImportPackAutomation(const QString &itemId);
    // Revision 21: screen triggers. StartScreenWatch asks the desktop (its own dialog) to share the screens and starts the sandboxed helper; it needs the setting screenTriggersEnabled,
    // an enabled screen trigger and an unlocked session (`InvalidArgs` otherwise). StopScreenWatch always works. CaptureScreenReference takes the picture of a region of a screen trigger
    // (`steps` of them, one after another) and answers with a request id; ScreenCaptured(request, json) brings it back and nothing is kept until CommitScreenReference(request).
    Q_SCRIPTABLE void StartScreenWatch();
    Q_SCRIPTABLE void StopScreenWatch();
    // StartScreenWatch for a caller that must not leave the screen being read behind: if this call starts the watch (it was not running), it is leased to the caller's unique bus name and
    // stops when that name leaves the bus (a GUI that died); a watch that was already running is the user's and is not leased. StopScreenWatch from anyone ends the lease. Not a new revision:
    // a client that gets UnknownMethod calls StartScreenWatch.
    Q_SCRIPTABLE void StartScreenWatchLeased();
    // (the answer, ScreenCaptured(s request, s json), is sent to the caller alone as a unicast signal, never broadcast: it holds pixels of the screen)
    Q_SCRIPTABLE QString CaptureScreenReference(const QString &triggerId, int region, int steps);
    Q_SCRIPTABLE void CommitScreenReference(const QString &request);
    // Drops a capture that is held (or running) without keeping it; only the name that asked may. `InvalidArgs` when there is no such request.
    Q_SCRIPTABLE void DiscardScreenReference(const QString &request);
    // The runtime layer: all of it, or one pet's, goes back to the stored state.
    Q_SCRIPTABLE void ClearPetRuntime(const QString &petId);

signals:
    Q_SCRIPTABLE void ProfilesChanged();
    Q_SCRIPTABLE void TriggersChanged();
    Q_SCRIPTABLE void TriggerFired(const QString &triggerId, const QString &outcome);
    Q_SCRIPTABLE void TimelinesChanged();
    Q_SCRIPTABLE void ScreenWatchChanged();
    Q_SCRIPTABLE void PetRuntimeChanged(const QString &petId);
    Q_SCRIPTABLE void ImportChanged(const QString &jobId);
    Q_SCRIPTABLE void ItemsChanged(const QStringList &added, const QStringList &changed, const QStringList &removed);
    Q_SCRIPTABLE void AssignmentsChanged();
    Q_SCRIPTABLE void SettingsChanged();
    Q_SCRIPTABLE void PetsChanged();
    Q_SCRIPTABLE void TranscodeChanged(const QString &itemId);
    Q_SCRIPTABLE void PetsArrangeChanged(bool arrange);
    Q_SCRIPTABLE void PetsHiddenChanged(bool hidden);
    Q_SCRIPTABLE void PetsMutedChanged(bool muted);
    Q_SCRIPTABLE void PetAudioChanged(const QString &jobId);
    Q_SCRIPTABLE void WallpaperInteractChanged(bool interact);
    Q_SCRIPTABLE void PlaylistsChanged();
    Q_SCRIPTABLE void SteamCmdChanged();

private:
    // InvalidArgs for D-Bus callers, a warning for direct (in-process) callers.
    void rejectCall(const QString &message) const;

    // Item JSON with the transcode state of every variant added for pets.
    QJsonObject itemJson(const WallpaperItem &item) const;
    // The item's summary or full JSON plus its "permissions" object.
    QJsonObject withPermissions(QJsonObject json, const QString &itemId) const;
    QJsonArray variantStates(const WallpaperItem &item) const;
    bool petsAvailable() const;
    // Whether the item can be put in a playlist; rejects the call with the reason and returns false otherwise.
    bool checkPlaylistItem(const QString &itemId) const;
    bool playlistsAvailable() const;
    // Shared by AddToPlaylist and RemoveFromPlaylist.
    void changePlaylistItems(const QString &playlistId, const QStringList &itemIds, bool add);
    void movePlaylist(const QString &output, const QString &desktop, int direction);
    // Queues the transcode of a heavy variant at the size its placements need, unless the pet plays the original.
    void autoQueue(const WallpaperItem &item, const QString &variant);
    void autoQueueAll();
    // The output scale that applies to a placement's output; "*" (the primary output, unknown here) takes the
    // largest known scale so that no screen gets a blurry pet.
    double scaleFor(const QString &output) const;
    // Pixels the placement needs of the variant's longest side.
    int needFor(const PetPlacement &placement, const PetVariant &variant) const;
    QList<StageOutput> stageOutputList() const;
    static StageRequest stageRequestOf(const PetPlacement &placement, QSize native);
    double stageFactor() const;
    // The needs of every placement of the variant that plays a transcode. The copies do not depend on the playback rate (it is applied
    // when the pet plays), so all placements share them.
    QList<int> needsFor(const QString &itemId, const PetVariant &variant) const;
    // Whether any placement shows the variant through a transcode.
    bool hasPlacements(const QString &itemId, const PetVariant &variant) const;
    // The finished copies a placement of this rate can play, newest first: those at the file's own timing and, for a pet at a fixed rate, the
    // ones retimed to a uniform rate by earlier versions (mpv's speed is the ratio of the rates).
    QList<int> doneUsableBy(const WallpaperItem &item, const PetVariant &variant, int fps) const;
    // The bucket to convert when some placement has no finished copy that is at least as big as it needs (the smallest bucket that
    // covers the biggest of those needs); nothing when every placement is served, or none shows the variant.
    std::optional<int> unservedBucket(const WallpaperItem &item, const PetVariant &variant) const;
    // The rate the variant is reported at (item JSON, TranscodeState, RequestTranscode): the first placement's, or
    // the default of the item when nothing shows it.
    int reportedFps(const WallpaperItem &item, const PetVariant &variant) const;
    // The state shown as the variant's "transcode": the finished copy in use, else the conversion that would serve it.
    TranscodeStatus reportedTranscode(const WallpaperItem &item, const PetVariant &variant) const;
    // The bucket to convert the variant to: the biggest need of its placements, or the cap when none shows it.
    int wantedBucket(const QString &itemId, const PetVariant &variant) const;
    TranscodeSource sourceFor(const WallpaperItem &item, const PetVariant &variant, int bucket, int playFps) const;
    // The conversions of the variant worth keeping (see bucketsToKeep): the copies at the file's timing, and the retimed ones that fully serve a placement of exactly their rate.
    QList<TranscodeManager::DoneCopy> keptCopies(const WallpaperItem &item, const PetVariant &variant) const;
    // Deletes the converted copies of the item's variants that no placement needs (at most two stay).
    void pruneBuckets(const QString &itemId);
    // Debounced: queues what the placements now need and prunes.
    void scheduleSync();
    void syncTranscodes();
    void cancelUnneededQueue();
    void syncOverlayUnit();

    Library *m_library;
    QStringList referencedPetAudio() const;
    PetStore *m_pets = nullptr;
    PetAudioManager *m_petAudio = nullptr;
    LocalPetStore *m_localPets = nullptr;
    LocalPetStore *m_localPuppets = nullptr;
    PuppetChecker *m_puppetChecker = nullptr;
    TranscodeManager *m_transcodes = nullptr;
    bool m_petsArrange = false;
    bool m_wallpaperInteract = false;
    QTimer m_interactCap; // the mode ends by itself: a state nobody can turn off (the overlay died) does not last
    bool m_petsHidden = false;
    bool m_petsMuted = false;
    bool m_quiet = false; // an automation action runs inside an incoming call: its failures are warnings, not errors of that call
    QHash<QString, double> m_outputScales;
    QHash<QString, OutputInfo> m_outputInfo; // the same with the places of the outputs (revision 17), for the size of a stage's copy
    QTimer m_syncTimer;
    QTimer m_graceTimer;
    bool m_awaitingScales = false; // the startup auto-queue waits for SetOutputScales or the grace period
    void releaseStartupHold();
    bool m_pruning = false;
    OverlayUnit *m_overlayUnit = nullptr;
    bool m_overlayWanted = false; // what the unit was last asked for
    QElapsedTimer m_overlayAsked; // when it was last asked to start
    qint64 m_overlayRetryMs = 30000;
    int m_interactCapMs = 10 * 60'000;
    ItemPermissionStore *m_permissions = nullptr;
    SteamCmdManager *m_steamcmd = nullptr;
    PetImportManager *m_imports = nullptr;
    // Adds the file as a local pet (the body of AddLocalPetItem): its item id, or an empty string with *problem set.
    QString addLocalPet(const QString &path, QString *problem);
    // True when the item lives in the import folder (a converted file the daemon made).
    bool isGenerated(const WallpaperItem &item) const;
    // The importer's share of the Workshop: with importWorkshopFiles / importWorkshopMdaa at "auto", imports the ZIPs and packages of pet items that were not
    // imported yet (remembered by path, size and time in auto-imports.json next to the imports). With "ask" nothing is touched.
    void autoImportWorkshopFiles();
    QSet<QString> m_autoImported;
    QString m_autoImportedFile;
    QString m_lastAutoSettings;
    // The "mdaa" object of the record of an imported package, or an empty object.
    QJsonObject packageRecord(const WallpaperItem &item) const;
    PlaylistStore *m_playlistStore = nullptr;
    AutomationManager *m_automation = nullptr;
    ScreenRefStore *m_screenRefs = nullptr;
    ScreenWatch *m_screenWatch = nullptr;
    QDBusConnection m_screenBus = QDBusConnection::sessionBus();
    QString m_captureRequest;   // the capture waiting to be kept, and the bus name that asked for it: the picture goes to that name alone and only it may keep it
    QString m_captureOwner;
    ProfileStore *m_profileStore = nullptr;
    TriggerStore *m_triggerStore = nullptr;
    TimelineStore *m_timelineStore = nullptr;
    PlaylistManager *m_playlistManager = nullptr;
    AssignmentStore *m_assignments;
    SettingsStore *m_settings;
    QString m_status;
    QString m_assetsDir;
    shortcut::GlobalLaunchPolicy m_shortcutPolicy;
};

} // namespace murasaki
