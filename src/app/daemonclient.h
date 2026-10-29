#pragma once

#include "constants.h"
#include "dbusbackend.h"

#include <QJsonObject>
#include <QObject>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace murasaki {

// Asynchronous client of the murasakid D-Bus API for the GUI. Nothing here blocks: every call goes through a
// DBusBackend and answers through signals, so QML can use it directly.
//
// When the daemon appears (or is already there) the client reads the properties, and the caches below are
// refreshed: `assignments`, `settings`, and the item list (itemsListed). `available` is true only for a daemon
// that speaks API revision >= 3; `apiVersion` is still reported for an older one (0 if it cannot be read), so
// the UI can ask the user to update or restart it.
//
// Pets (API revision >= 5): `petsAvailable` says whether the daemon has them. With an older daemon (revision 4) the
// library and wallpapers work as before, the pet cache stays empty and the pet calls report an error. `pets` caches
// ListPets (refreshed on PetsChanged) and `petsArrange` the PetsArrange property.
//
// Playlists (API revision >= 7): `playlistsAvailable` says whether the daemon has them; `playlists` caches
// ListPlaylists (refreshed on PlaylistsChanged). With an older daemon the cache stays empty and the playlist calls
// report an error.
//
// Every service appearance or disappearance (including a new process replacing the old one) starts a new
// generation: answers to requests of an earlier generation are dropped.
class DaemonClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int apiVersion READ apiVersion NOTIFY apiVersionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString assetsDir READ assetsDir NOTIFY assetsDirChanged)
    Q_PROPERTY(QVariantList assignments READ assignments NOTIFY assignmentsChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(bool petsAvailable READ petsAvailable NOTIFY petsAvailableChanged)
    Q_PROPERTY(QVariantList pets READ pets NOTIFY petsChanged)
    Q_PROPERTY(bool petsArrange READ petsArrange NOTIFY petsArrangeChanged)
    // The pets are hidden by the user (the daemon's PetsHidden, revision 12). petBehavioursAvailable says the daemon has the behaviour fields,
    // RescuePets and PetsHidden: the controls that need them are disabled otherwise.
    Q_PROPERTY(bool petsHidden READ petsHidden NOTIFY petsHiddenChanged)
    Q_PROPERTY(bool petBehavioursAvailable READ petBehavioursAvailable NOTIFY petBehavioursAvailableChanged)
    // SteamCMD downloads (API revision >= 9): `steamCmdSupported` says whether the daemon has them, `steamCmd` is the
    // daemon's SteamCmdStatus ({available, program, state, user, prompt, message, jobs}), refreshed on SteamCmdChanged.
    Q_PROPERTY(bool steamCmdSupported READ steamCmdSupported NOTIFY steamCmdSupportedChanged)
    // Pet imports (API revision >= 14): `importsSupported` says whether the daemon has them, `imports` is its ImportState().
    Q_PROPERTY(bool importsSupported READ importsSupported NOTIFY importsSupportedChanged)
    Q_PROPERTY(QVariantList imports READ imports NOTIFY importsChanged)
    // The sounds of the pets (API revision >= 15): `petAudioSupported` says whether the daemon has them, `petAudioAssets` is its ListPetAudioAssets() ([{asset, name,
    // durationMs, sourceFormat, bytes, refs}]), refreshed on PetAudioChanged and PetsChanged.
    // The fullscreen mode of a pet, "stage" (revision >= 17).
    Q_PROPERTY(bool petStageSupported READ petStageSupported NOTIFY petStageSupportedChanged)
    Q_PROPERTY(bool petStatesSupported READ petStatesSupported NOTIFY petStatesSupportedChanged)
    // Puppet pets (revision >= 20, docs/pets/puppets.md): a folder with a model becomes a pet item; the library lists it with format "puppet".
    Q_PROPERTY(bool puppetsSupported READ puppetsSupported NOTIFY puppetsSupportedChanged)
    Q_PROPERTY(bool petAudioSupported READ petAudioSupported NOTIFY petAudioSupportedChanged)
    Q_PROPERTY(QVariantList petAudioAssets READ petAudioAssets NOTIFY petAudioAssetsChanged)
    // Profiles, triggers and timelines (API revision >= 18, docs/pets/automation.md): `automationSupported` says whether the daemon has them; `profiles`, `triggers` and
    // `timelines` cache the lists; `activeProfile` is the id of the profile applied last and `profileModified` whether the pets differ from it; `triggerStates` is
    // {states, log} (TriggerState()); `runningTimelines` the ids that run; `lastSwitchReport` what the last switch skipped.
    Q_PROPERTY(bool automationSupported READ automationSupported NOTIFY automationSupportedChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY profilesChanged)
    Q_PROPERTY(bool profileModified READ profileModified NOTIFY profilesChanged)
    Q_PROPERTY(QVariantMap lastSwitchReport READ lastSwitchReport NOTIFY lastSwitchReportChanged)
    Q_PROPERTY(QVariantList triggers READ triggers NOTIFY triggersChanged)
    Q_PROPERTY(QVariantMap triggerStates READ triggerStates NOTIFY triggerStatesChanged)
    Q_PROPERTY(QVariantList timelines READ timelines NOTIFY timelinesChanged)
    Q_PROPERTY(QStringList runningTimelines READ runningTimelines NOTIFY timelinesChanged)
    // The demonstrations ("Try it", API revision >= 22): `hasDemos` says whether any profile, trigger or timeline carries the demo marker (origin "demo").
    // The screen triggers (API revision >= 21, docs/pets/automation.md "Screen triggers"): `screenSupported` says whether the daemon has them; `screenWatch` is the daemon's
    // ScreenWatchState ({state: off|asking|watching|stopped|refused|failed, reason, regions, ticksPerSecond, since, paused, notes}). The reference pictures of a capture arrive as
    // screenCaptured(request, result) and are kept only when commitScreenReference(request) says so.
    Q_PROPERTY(bool screenSupported READ screenSupported NOTIFY apiVersionChanged)
    Q_PROPERTY(QVariantMap screenWatch READ screenWatch NOTIFY screenWatchChanged)
    Q_PROPERTY(bool demosSupported READ demosSupported NOTIFY apiVersionChanged)
    // revision 23: the idle trigger's "state" (back: when I come back). An older daemon ignores the key and would keep an "away" trigger.
    Q_PROPERTY(bool idleBackSupported READ idleBackSupported NOTIFY apiVersionChanged)
    // The setting petsHideOnGame (API revision >= 24).
    Q_PROPERTY(bool petsGameSupported READ petsGameSupported NOTIFY apiVersionChanged)
    Q_PROPERTY(bool hasDemos READ hasDemos NOTIFY demosChanged)
    Q_PROPERTY(QVariantMap steamCmd READ steamCmd NOTIFY steamCmdChanged)
    Q_PROPERTY(bool playlistsAvailable READ playlistsAvailable NOTIFY playlistsAvailableChanged)
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    // Pets with an edit that is queued, on its way or not yet reflected in `pets`. Controls keep what the user set
    // while their pet is listed, then take the daemon's value (also when the daemon refused the edit).
    Q_PROPERTY(QStringList pendingPetIds READ pendingPetIds NOTIFY pendingPetUpdatesChanged)

public:
    // Takes ownership of the backend.
    explicit DaemonClient(DBusBackend *backend, QObject *parent = nullptr);
    // Talks to the real daemon on the session bus.
    explicit DaemonClient(QObject *parent = nullptr);

    bool available() const { return m_available; }
    int apiVersion() const { return m_apiVersion; }
    QString status() const { return m_status; }
    QString assetsDir() const { return m_assetsDir; }
    QVariantList assignments() const { return m_assignments; }
    QVariantMap settings() const { return m_settings; }
    bool petsAvailable() const { return m_available && m_apiVersion >= kPetsDBusApiVersion; }
    QVariantList pets() const { return m_pets; }
    bool petsArrange() const { return m_petsArrange; }
    bool petsHidden() const { return m_petsHidden; }
    bool petBehavioursAvailable() const { return m_available && m_apiVersion >= int(kPetBehavioursDBusApiVersion); }
    QStringList pendingPetIds() const;
    bool playlistsAvailable() const { return m_available && m_apiVersion >= int(kPlaylistsDBusApiVersion); }
    QVariantList playlists() const { return m_playlists; }
    bool steamCmdSupported() const { return m_available && m_apiVersion >= int(kSteamCmdDBusApiVersion); }
    bool importsSupported() const { return m_available && m_apiVersion >= int(kImportDBusApiVersion); }
    QVariantList imports() const { return m_imports; }
    QVariantMap steamCmd() const { return m_steamCmd; }
    bool petStageSupported() const { return m_available && m_apiVersion >= int(kPetStageDBusApiVersion); }
    bool petStatesSupported() const { return m_available && m_apiVersion >= int(kPetStatesDBusApiVersion); }
    bool puppetsSupported() const { return m_available && m_apiVersion >= int(kPuppetsDBusApiVersion); }
    bool petAudioSupported() const { return m_available && m_apiVersion >= int(kPetAudioDBusApiVersion); }
    QVariantList petAudioAssets() const { return m_petAudioAssets; }
    bool automationSupported() const { return m_available && m_apiVersion >= int(kAutomationDBusApiVersion); }
    bool demosSupported() const { return m_available && m_apiVersion >= int(kDemosDBusApiVersion); }
    bool idleBackSupported() const { return m_available && m_apiVersion >= int(kIdleBackDBusApiVersion); }
    bool petsGameSupported() const { return m_available && m_apiVersion >= int(kPetsGameDBusApiVersion); }
    bool screenSupported() const { return m_available && m_apiVersion >= int(kScreenDBusApiVersion); }
    QVariantMap screenWatch() const { return m_screenWatch; }
    bool hasDemos() const;
    QVariantList profiles() const { return m_profiles; }
    QString activeProfile() const { return m_activeProfile; }
    bool profileModified() const { return m_profileModified; }
    QVariantMap lastSwitchReport() const { return m_lastSwitchReport; }
    QVariantList triggers() const { return m_triggers; }
    QVariantMap triggerStates() const { return m_triggerStates; }
    QVariantList timelines() const { return m_timelines; }
    QStringList runningTimelines() const { return m_runningTimelines; }
    bool permissionsAvailable() const { return m_available && m_apiVersion >= int(kPermissionsDBusApiVersion); }

    // Pet edits queued by queuePetUpdate() are sent this long after the last one.
    static constexpr int PetUpdateDelayMs = 250;

    // Starts the connection; the constructors do not, so that callers can connect to the signals first.
    void start();

    Q_INVOKABLE void refreshItems();
    Q_INVOKABLE void getItem(const QString &id);
    Q_INVOKABLE void setAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                   const QVariantMap &options = {});
    Q_INVOKABLE void clearAssignment(const QString &output, const QString &desktop);
    Q_INVOKABLE void refreshAssignments();
    // The assignments that keep their own wallpaper inside the target (output, desktop; "*" for all) and so win over
    // one set on the target itself: those narrower than it, not the target's own key. An assignment for a desktop
    // that is not in `knownDesktops` never applies and is left out (an empty list means the desktops are not known:
    // every one exists).
    Q_INVOKABLE QVariantList overridingAssignments(const QString &output, const QString &desktop,
                                                   const QStringList &knownDesktops = {}) const;
    // The assignments of vanished desktops that setting an "all desktops" target would leave behind (those of the
    // target's outputs); replacing clears them with the overriding ones. Empty for a single-desktop target.
    Q_INVOKABLE QVariantList orphanAssignments(const QString &output, const QString &desktop,
                                               const QStringList &knownDesktops = {}) const;
    Q_INVOKABLE void resolveAssignment(const QString &output, const QString &desktop);
    Q_INVOKABLE void refreshSettings();
    Q_INVOKABLE void setSettings(const QVariantMap &settings);
    // Allows or denies a permission ("network") for one item (API revision >= 8; reports an error otherwise).
    Q_INVOKABLE void setItemPermission(const QString &itemId, const QString &permission, bool allowed);

    // Pets. The daemon announces its own changes (PetsChanged, TranscodeChanged, PetsArrangeChanged); the cache
    // follows without a local refresh.
    Q_INVOKABLE void refreshPets();
    // Answers with petAdded(itemId, petId). `variant` may be empty (the first), `output` is a connector or "*".
    Q_INVOKABLE void addPet(const QString &itemId, const QString &variant, const QString &output,
                            const QVariantMap &options = {});
    Q_INVOKABLE void updatePet(const QString &petId, const QVariantMap &changes);
    // Like updatePet, for sliders and spin boxes: edits to the same pet are merged and sent once the edits have
    // paused for PetUpdateDelayMs. flushPetUpdates() sends them now (a page that is closing).
    Q_INVOKABLE void queuePetUpdate(const QString &petId, const QVariantMap &changes);
    Q_INVOKABLE void flushPetUpdates();
    Q_INVOKABLE bool hasPendingPetUpdate(const QString &petId) const { return pendingPetIds().contains(petId); }
    Q_INVOKABLE void removePet(const QString &petId);
    // Answers with transcodeStateLoaded(itemId, variants).
    Q_INVOKABLE void transcodeState(const QString &itemId);
    // An empty variant means every variant of the item.
    Q_INVOKABLE void requestTranscode(const QString &itemId, const QString &variant);
    Q_INVOKABLE void setPetsArrange(bool arrange);
    // Hides or shows every pet (SetPetsHidden), and the rescue (RescuePets): click-through off, keep above on, the hover and fade flags, the dodge and
    // the lock off, opacity 1 when below 0.5. Both need revision 12.
    Q_INVOKABLE void setPetsHidden(bool hidden);
    Q_INVOKABLE void rescuePets();
    // Makes a pet item of an animation file; answers with localPetAdded(path, itemId).
    Q_INVOKABLE void addLocalPetItem(const QString &path);
    Q_INVOKABLE void removeLocalPetItem(const QString &itemId);
    // Makes a pet item of a puppet folder (one Inochi2D .inp, or a Cubism model3.json and its files); answers with localPuppetAdded(folder, itemId). Needs revision 20.
    Q_INVOKABLE void addLocalPuppetItem(const QString &folder);
    Q_INVOKABLE void removeLocalPuppetItem(const QString &itemId);
    // Pet imports (docs/pets/import.md). `spec` is the job description (kind, sources, name, options). importPet answers with importStarted(jobId); the
    // job then shows in `imports` until it is replaced by newer ones, and a done job carries the item id of the new pet. previewImport answers with
    // importPreviewed(preview) where preview is {ok, preview:{frames, width, height, totalDelayMs, estimatedBytes, degraded}} or {ok:false, error}.
    Q_INVOKABLE void importPet(const QVariantMap &spec);
    Q_INVOKABLE void previewImport(const QVariantMap &spec);
    Q_INVOKABLE void refreshImports();
    Q_INVOKABLE void cancelImport(const QString &jobId);
    // The sound of a pet (docs/pets/audio.md). importPetAudio converts a sound file (or the sound of a video) into a sound the pets can use; importItemAudio does it
    // for the kept sound of a pet imported from a video. Both answer with petAudioImportStarted(jobId), and later petAudioImportFinished(jobId, ok, asset, error) when
    // the job ends (error is a name of docs/pets/audio.md). A job is watched only from the call that started it.
    // Both return the ticket of the request (0 when it is not made), which petAudioImportStarted and petAudioImportRefused repeat, so that a card that asked can
    // tell its own answer from another's.
    Q_INVOKABLE int importPetAudio(const QString &path);
    Q_INVOKABLE int importItemAudio(const QString &itemId);
    Q_INVOKABLE void cancelPetAudioImport(const QString &jobId);
    Q_INVOKABLE void removePetAudioAsset(const QString &asset);
    Q_INVOKABLE void refreshPetAudio();
    // Removes a local pet; with deleteFiles also the converted files of an imported one.
    Q_INVOKABLE void removeImportedPet(const QString &itemId, bool deleteFiles);
    Q_INVOKABLE void startScene(const QString &petId, int index);

    // Playlists. The daemon announces its own changes (PlaylistsChanged); the cache follows. A playlist is played by
    // assigning "playlist:<id>" with setAssignment().
    Q_INVOKABLE void refreshSteamCmd();
    Q_INVOKABLE void steamCmdLogin(const QString &user);
    // kind is "password" or "guard". The secret goes to the daemon once; nothing here keeps it.
    Q_INVOKABLE void steamCmdSubmit(const QString &kind, const QString &secret);
    Q_INVOKABLE void steamCmdCancelLogin();
    Q_INVOKABLE void steamCmdLogout();
    Q_INVOKABLE void steamCmdDownload(const QString &appId, const QString &itemId);
    Q_INVOKABLE void steamCmdCancel(const QString &itemId);
    // Profiles, triggers and timelines (docs/pets/automation.md). The daemon announces its own changes; the caches follow. saveProfile answers with profileSaved(name, id),
    // switchProfile with profileSwitched(report) (report: {profile, kept, updated, added, removed, skipped: [{slot, itemId, reason}]}), setTrigger and setTimeline
    // with triggerSaved(id) / timelineSaved(id) (an empty id creates), timelineFromScene and importPackAutomation with packAutomationMade(result). Names, texts and
    // origins in these lists come from files: show them as plain text.
    Q_INVOKABLE void refreshAutomation();
    Q_INVOKABLE void saveProfile(const QString &name);
    Q_INVOKABLE void switchProfile(const QString &idOrName);
    Q_INVOKABLE void renameProfile(const QString &idOrName, const QString &newName);
    Q_INVOKABLE void deleteProfile(const QString &idOrName);
    Q_INVOKABLE void setTrigger(const QString &triggerId, const QVariantMap &changes);
    Q_INVOKABLE void deleteTrigger(const QString &triggerId);
    Q_INVOKABLE void setTriggerEnabled(const QString &triggerId, bool enabled);
    Q_INVOKABLE void fireTrigger(const QString &name);
    Q_INVOKABLE void testTrigger(const QString &triggerId);
    // Screen triggers (revision 21). start asks the desktop (its own dialog) to share the screens; stop always works. capture asks for the picture of a region of a screen trigger
    // (`steps` of them, one after another): screenCaptured({request, trigger, region, steps: [{step, width, height, png}]} or {request, error}) brings them, and only for the request
    // this client asked for last (another one's, or a late one, is dropped); the pictures are checked here (PNG header, at most 360 pixels, 4 MiB each), and nothing is kept until
    // commitScreenReference(request). refreshTriggerStates re-reads TriggerState() (the live scores) without waiting for a signal.
    Q_INVOKABLE void startScreenWatch();
    // The guided walk's start: the daemon ends a watch started this way when this program leaves the bus (a dead GUI must not leave the screen being read); a daemon without the
    // call (StartScreenWatchLeased, no revision of its own) gets the plain StartScreenWatch.
    Q_INVOKABLE void startScreenWatchLeased();
    Q_INVOKABLE void stopScreenWatch();
    Q_INVOKABLE void captureScreenReference(const QString &triggerId, int region, int steps);
    Q_INVOKABLE void commitScreenReference(const QString &request);
    // The page gave up on the capture it asked for (a timeout, a closed dialog): a result that comes later is dropped. The daemon drops what it holds after a minute.
    Q_INVOKABLE void cancelScreenCapture();
    // Tells the daemon to drop a capture it holds (the preview was closed or refused); a no-op on its side when it already forgot it.
    Q_INVOKABLE void discardScreenReference(const QString &request);
    // The guided walk of "Try a screen trigger" keeps a small journal of what it changed in the daemon (the example trigger, whether it turned the setting on, whether it
    // started the watch) in this app's own config, so that a GUI that dies in the middle does not leave any of it on: noteScreenDemo({trigger, settingTurnedOn, watchStarted})
    // writes it BEFORE the change it names; undoScreenDemo puts back exactly that (stops the watch it started, deletes the trigger only while it still carries the demo marker,
    // turns the setting off only when no other screen trigger is on) and clears the journal once the daemon answered; endScreenDemo only clears it (the walk was kept). When the
    // GUI connects and finds a journal no walk of this process owns, it does undoScreenDemo by itself. Without the daemon the journal stays for the next connection.
    Q_INVOKABLE void noteScreenDemo(const QVariantMap &journal);
    Q_INVOKABLE void endScreenDemo();
    Q_INVOKABLE void undoScreenDemo();
    // Tests: where the journal is written (default: screen-demo.ini in the app's config directory).
    void setScreenDemoJournalPath(const QString &path);
    Q_INVOKABLE void refreshScreenWatch();
    Q_INVOKABLE void refreshTriggerStates();
    Q_INVOKABLE void setTimeline(const QString &timelineId, const QVariantMap &changes);
    Q_INVOKABLE void deleteTimeline(const QString &idOrName);
    Q_INVOKABLE void runTimeline(const QString &idOrName);
    Q_INVOKABLE void stopTimeline(const QString &idOrName);
    Q_INVOKABLE void timelineFromScene(const QString &itemId, int index);
    Q_INVOKABLE void importPackAutomation(const QString &itemId);
    Q_INVOKABLE void clearPetRuntime(const QString &petId);
    // The rescue pauses the triggers for a minute (triggerStates.suspendedMs says how long is left); this ends the pause now.
    Q_INVOKABLE void resumeAutomation();
    // The demonstrations (src/core/demos.h). `kind` is timelines, triggers, profiles or tracking; `options` has `names` ({timeline, trigger, profile, tracking}: the names,
    // translated by the caller), `screenWidth` and `screenHeight` (the screen the example is laid out in) and, for tracking, `petId` (else the first pet that can walk).
    // The answer is demoDone(kind, result) (timelines: {timeline: the timeline's JSON with its id}; triggers: {trigger, timeline, shortcutId}; profiles: {profile, name}, saved without switching;
    // tracking: {timeline, petId, seconds, mode: follow or look}), or demoNeedsPet(kind) when there is no pet to make it from. They write the existing stores (marked origin "demo") and
    // never pets.json. removeDemos deletes exactly the marked triggers, timelines and profiles and answers demosRemoved({triggers, timelines, profiles}).
    Q_INVOKABLE void runDemo(const QString &kind, const QVariantMap &options);
    Q_INVOKABLE void removeDemos();
    Q_INVOKABLE void dismissDemoCard(const QString &kind);
    Q_INVOKABLE void refreshPlaylists();
    // Answers with playlistCreated(name, id).
    Q_INVOKABLE void createPlaylist(const QString &name);
    Q_INVOKABLE void updatePlaylist(const QString &playlistId, const QVariantMap &changes);
    Q_INVOKABLE void addToPlaylist(const QString &playlistId, const QStringList &itemIds);
    Q_INVOKABLE void removeFromPlaylist(const QString &playlistId, const QStringList &itemIds);
    Q_INVOKABLE void deletePlaylist(const QString &playlistId);
    Q_INVOKABLE void nextInPlaylist(const QString &output, const QString &desktop);
    Q_INVOKABLE void previousInPlaylist(const QString &output, const QString &desktop);

signals:
    void availableChanged();
    void automationSupportedChanged();
    void profilesChanged();
    void lastSwitchReportChanged();
    void triggersChanged();
    void triggerStatesChanged();
    void timelinesChanged();
    void profileSaved(const QString &name, const QString &id);
    void profileSwitched(const QVariantMap &report);
    void triggerSaved(const QString &id);
    void timelineSaved(const QString &id);
    void packAutomationMade(const QVariantMap &result);
    void triggerTested(const QString &triggerId, bool ran);
    void screenWatchChanged();
    void screenCaptured(const QString &request, const QVariantMap &result);
    void screenReferenceCommitted(const QString &request);
    void demosChanged();
    void demoDone(const QString &kind, const QVariantMap &result);
    void demoNeedsPet(const QString &kind);
    void demosRemoved(const QVariantMap &counts);
    void playlistsAvailableChanged();
    void steamCmdSupportedChanged();
    void steamCmdChanged();
    void importsSupportedChanged();
    void importsChanged();
    void petAudioSupportedChanged();
    void petStageSupportedChanged();
    void petStatesSupportedChanged();
    void puppetsSupportedChanged();
    void petAudioAssetsChanged();
    void petAudioImportStarted(const QString &jobId, int ticket);
    void petAudioImportRefused(int ticket, const QString &message);
    void petAudioImportFinished(const QString &jobId, bool ok, const QString &asset, const QString &error);
    void importStarted(const QString &jobId);
    void importPreviewed(const QVariantMap &preview);
    void playlistsChanged();
    void playlistCreated(const QString &name, const QString &id);
    void petsAvailableChanged();
    void petsChanged();
    void pendingPetUpdatesChanged();
    void petsArrangeChanged();
    void petsHiddenChanged();
    void petBehavioursAvailableChanged();
    void petAdded(const QString &itemId, const QString &petId);
    void transcodeStateLoaded(const QString &itemId, const QVariantList &variants);
    // Some variant of the pet item changed its transcode state or progress; ask transcodeState() for details.
    void transcodeChanged(const QString &itemId);
    void localPetAdded(const QString &path, const QString &itemId);
    void localPuppetAdded(const QString &folder, const QString &itemId);
    void apiVersionChanged();
    void statusChanged();
    void assetsDirChanged();
    // The ListItems answer: a list of item summary maps, in daemon order (sorted by title).
    void itemsListed(const QVariantList &items);
    void itemsChanged(const QStringList &added, const QStringList &changed, const QStringList &removed);
    void itemLoaded(const QString &id, const QVariantMap &item);
    void itemLoadFailed(const QString &id, const QString &message);
    // `assignment` is empty when nothing matches; `item` is empty when the item left the library. For a playlist
    // assignment, `assignment.itemId` is the item that is showing and `playlist` is {id, index, nextAt} (empty for
    // anything else, and from a daemon older than revision 7).
    void assignmentResolved(const QString &output, const QString &desktop, const QVariantMap &assignment,
                            const QVariantMap &item, const QVariantMap &playlist);
    void assignmentsChanged();
    void settingsChanged();
    // InvalidArgs and Failed answers of the daemon, with its message.
    void error(const QString &message);

private:
    void onAvailability(bool up);
    void onSignal(const QString &name, const QVariantList &args);
    void loadProperties();
    void setAvailable(bool available);
    void setPets(const QVariantList &pets);
    void setPetsArrangeValue(bool arrange);
    void setPetsHiddenValue(bool hidden);
    void clearPets();
    void notifyPendingPets();
    void releasePetUpdate(const QString &petId);
    void loadPetsArrange();
    void loadPetsHidden();
    bool requirePets();
    bool requirePlaylists();
    bool requireSteamCmd();
    bool requireImports();
    bool requirePetAudio();
    void watchPetAudioJob(const QString &jobId, int ticket);
    int requestPetAudioImport(const QString &method, const QString &argument);
    void checkPetAudioJob(const QString &jobId);
    void clearSteamCmd();
    void setPlaylists(const QVariantList &playlists);
    void clearPlaylists();
    void clearAutomation();
    void clearScreen();
    bool requireDemos();
    void saveDemoTimeline(const QJsonObject &spec, const std::function<void(const QString &id, const QJsonObject &timeline)> &done);
    void refreshProfiles();
    void refreshTriggers();
    void refreshTimelines();
    bool requireAutomation();
    void setInfo(int apiVersion, const QString &status, const QString &assetsDir);
    enum CallFlag {
        Silent = 1, // do not report InvalidArgs/Failed through error(); the `failed` handler deals with it
        Ungated = 2, // deliver the answer even if the daemon restarted meanwhile (writes)
    };
    // Runs `done` for a successful answer; reports InvalidArgs/Failed through error(). Answers that arrive after
    // the daemon went away or was replaced are dropped unless Ungated.
    void call(const QString &method, const QVariantList &args, std::function<void(const QVariant &)> done,
              std::function<void(const DBusBackend::Reply &)> failed = {}, int flags = 0);

    DBusBackend *m_backend;
    bool m_serviceUp = false;
    quint64 m_generation = 0;
    bool m_available = false;
    int m_apiVersion = 0;
    QString m_status;
    QString m_assetsDir;
    QVariantList m_assignments;
    QVariantMap m_settings;
    bool m_assignmentsLoaded = false;
    bool m_settingsLoaded = false;
    QVariantList m_imports;
    QVariantList m_petAudioAssets;
    QStringList m_watchedAudioJobs;
    int m_audioTicket = 0;
    QVariantMap m_steamCmd;
    bool m_steamCmdLoaded = false;
    QVariantList m_playlists;
    bool m_playlistsLoaded = false;
    QVariantList m_profiles;
    QString m_activeProfile;
    bool m_profileModified = false;
    QVariantMap m_lastSwitchReport;
    QVariantList m_triggers;
    QVariantMap m_triggerStates;
    QVariantMap m_screenWatch;
    // The capture the page asked for: its request id once the daemon answered, and a result that came before that answer (the signal can beat the reply).
    bool m_captureInFlight = false;
    QString m_captureRequest;
    quint64 m_captureSeq = 0;
    QVariantMap m_earlyCapture;
    void screenCaptureArrived(const QString &request, const QJsonObject &result);
    QVariantMap readScreenDemoJournal() const;
    void writeScreenDemoJournal(const QVariantMap &journal) const;
    QString m_screenDemoJournalPath;
    bool m_screenDemoLive = false; // a walk of this process owns the journal
    QVariantList m_timelines;
    QStringList m_runningTimelines;
    QVariantList m_pets;
    bool m_petsLoaded = false;
    bool m_petsArrange = false;
    bool m_petsHidden = false;
    QHash<QString, QVariantMap> m_pendingPetUpdates;
    QHash<QString, int> m_inflightPetUpdates; // sent, answer and the refreshed list not in yet
    QStringList m_lastPendingPetIds;
    QTimer m_petUpdateTimer;
};

} // namespace murasaki
