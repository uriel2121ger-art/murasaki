#include "assignment.h"
#include "automationsources.h"
#include "automationstore.h"
#include "buildinfo.h"
#include "constants.h"
#include "screenrefstore.h"
#include "filelog.h"
#include "itempermissions.h"
#include "library.h"
#include "localpets.h"
#include "localpuppets.h"
#include "murasakiservice.h"
#include "petaudiomanager.h"
#include "overlayunit.h"
#include "petstore.h"
#include "playlist.h"
#include "playlistmanager.h"
#include "puppetchecker.h"
#include "sessionwatch.h"
#include "settings.h"
#include "sceneenginelookup.h"
#include "petimportmanager.h"
#include "steamcmdmanager.h"
#include "steamlocator.h"
#include "transcodemanager.h"
#include "workshopmetadataclient.h"
#include "workshopwatcher.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QStandardPaths>
#include <QTimer>
#include <QTextStream>

using namespace Qt::StringLiterals;
using namespace murasaki;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"murasakid"_s);
    QCoreApplication::setApplicationVersion(versionString());

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Murasaki wallpaper daemon"_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption listOption(u"list"_s,
                                        u"Print the library as tab-separated id, kind, status, title and exit."_s);
    parser.addOption(listOption);
    parser.process(app);
    installFileLogging(u"murasakid"_s);

    const SteamLocator steam(SteamLocator::defaultRootCandidates());
    const QString weInstallDir = steam.appInstallDir(kWallpaperEngineAppId);
    // What SteamCMD downloads for Murasaki is one more Workshop root, read like Steam's (and like it, never written).
    const QString steamCmdDir = SteamLocator::steamCmdInstallDir();
    const QStringList workshopDirs = steam.workshopDirCandidates(kWallpaperEngineAppId)
        + QStringList{SteamLocator::steamCmdContentDir(steamCmdDir, kWallpaperEngineAppId)};

    Library library;
    library.setWorkshopDirs(workshopDirs);
    const QStringList animaDirs = steam.workshopDirCandidates(kAnimaEngineAppId)
        + QStringList{SteamLocator::steamCmdContentDir(steamCmdDir, kAnimaEngineAppId)};
    library.setAnimaDirs(animaDirs);
    LocalPetStore localPets(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                            + u"/murasaki/local-pets.json"_s);
    localPets.load();
    library.setLocalPetFiles(localPets.files());
    // The puppet folders (revision 20) and the checker that reads them behind the sandbox; the library asks it for each folder's answer.
    LocalPetStore localPuppets(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/murasaki/local-puppets.json"_s);
    localPuppets.setItemIdFunction(&LocalPuppetImporter::itemIdFor);
    localPuppets.load();
    library.setLocalPuppetFolders(localPuppets.files());
    PuppetChecker puppetChecker{QString(), QString()};
    library.setPuppetCheckProvider([&puppetChecker](const QString &folder) { return puppetChecker.result(folder); });

    // Workshop metadata (title, tags, preview) of the Anima Engine items, cached under $XDG_CACHE_HOME/murasaki.
    // --list only reads the cache; the network is used by the running daemon.
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/murasaki"_s;
    WorkshopMetadataClient metadata(cacheDir);
    library.setPetDecorator([&metadata](WallpaperItem &item) {
        if (item.workshopId.isEmpty())
            return;
        if (const auto meta = metadata.metadata(item.workshopId)) {
            if (!meta->title.isEmpty())
                item.title = meta->title;
            if (!meta->tags.isEmpty())
                item.tags = meta->tags;
            if (!meta->previewPath.isEmpty())
                item.previewPath = meta->previewPath;
        }
    });
    library.rescan();
    TranscodeManager transcodes(cacheDir + u"/pets"_s);

    if (parser.isSet(listOption)) {
        QTextStream(stderr) << "status=" << MurasakiService::statusFor(steam) << '\n';
        QTextStream out(stdout);
        for (const WallpaperItem &item : library.items()) {
            out << item.id << '\t' << itemKindToString(item.kind) << '\t' << itemStatusToString(item.status)
                << '\t' << item.title << '\n';
        }
        return 0;
    }

    WorkshopWatcher watcher;
    watcher.setPaths(workshopDirs + steam.workshopManifestCandidates(kWallpaperEngineAppId) + animaDirs
                     + steam.workshopManifestCandidates(kAnimaEngineAppId)
                     + QStringList{SteamLocator::steamCmdWorkshopRoot(steamCmdDir)});
    QObject::connect(&watcher, &WorkshopWatcher::changed, &library, &Library::rescan);

    const auto refreshMetadata = [&library, &metadata](const QStringList &itemIds) {
        QStringList workshopIds;
        for (const QString &id : itemIds) {
            if (const auto item = library.item(id); item && item->kind == ItemKind::Pet && !item->workshopId.isEmpty())
                workshopIds.append(item->workshopId);
        }
        metadata.refresh(workshopIds);
    };
    const auto refreshAllMetadata = [&library, refreshMetadata] {
        QStringList ids;
        for (const WallpaperItem &item : library.items()) {
            if (item.kind == ItemKind::Pet)
                ids.append(item.id);
        }
        refreshMetadata(ids);
    };
    // Cache upkeep: previews of items that are gone, and conversions that no pet variant can use any more.
    MurasakiService *petService = nullptr; // set below, before the event loop runs the prune
    const auto pruneCaches = [&library, &metadata, &transcodes, &petService] {
        QStringList workshopIds;
        QStringList petItems;
        for (const WallpaperItem &item : library.items()) {
            if (item.kind != ItemKind::Pet)
                continue;
            if (!item.workshopId.isEmpty())
                workshopIds.append(item.workshopId);
            petItems.append(item.id);
        }
        metadata.prune(workshopIds);
        if (petService)
            transcodes.pruneCache(petService->expectedTranscodes(), petItems);
    };
    QObject::connect(&metadata, &WorkshopMetadataClient::metadataChanged, &library, &Library::rescan);
    QObject::connect(&library, &Library::itemsChanged, &library,
                     [refreshMetadata](const QStringList &added, const QStringList &) { refreshMetadata(added); });
    QTimer metadataTimer; // new versions of items appear while the daemon runs
    metadataTimer.setInterval(6 * 60 * 60 * 1000);
    QObject::connect(&metadataTimer, &QTimer::timeout, &library, refreshAllMetadata);
    metadataTimer.start();
    QTimer::singleShot(0, &library, refreshAllMetadata);
    // Only items that vanish: an item that is merely changed keeps its cache.
    QObject::connect(&library, &Library::itemsChanged, &library,
                     [pruneCaches](const QStringList &, const QStringList &, const QStringList &removed) {
                         if (!removed.isEmpty())
                             pruneCaches();
                     });
    QTimer::singleShot(0, &library, pruneCaches);

    AssignmentStore assignments(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                + u"/murasaki/assignments.json"_s);
    assignments.load();

    SettingsStore settings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + u"/murasaki/settings.json"_s);
    // The checker follows the import rule: confined whenever the sandbox helper is there, required when the user asked for it (importRequireSandbox).
    puppetChecker.setSandboxMode([&settings] { return settings.importSettings().requireSandbox ? PuppetChecker::Sandbox::Require : PuppetChecker::Sandbox::Prefer; });
    settings.load();

    ItemPermissionStore permissions(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                    + u"/murasaki/permissions.json"_s);
    permissions.load();

    PetStore pets(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/murasaki/pets.json"_s);
    pets.load();

    PlaylistStore playlists(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                            + u"/murasaki/playlists.json"_s);
    playlists.load();
    PlaylistManager playlistManager(&playlists, &assignments, &library,
                                    QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
                                        + u"/murasaki/playlist-state.json"_s);
    playlistManager.load();

    MurasakiService service(&library, &assignments, &settings, MurasakiService::statusFor(steam),
                            weInstallDir.isEmpty() ? QString() : weInstallDir + u"/assets"_s);
    service.attachPermissions(&permissions);
    SteamCmdManager steamcmd(steamCmdDir, QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                              + u"/murasaki/steamcmd.json"_s);
    service.attachSteamCmd(&steamcmd);
    service.attachPlaylists(&playlists, &playlistManager);
    service.attachPets(&pets, &transcodes);
    petService = &service;
    service.attachLocalPets(&localPets);
    service.attachLocalPuppets(&localPuppets, &puppetChecker);
    // Profiles, triggers and timelines (revision 18). Nothing exists until the user makes it: no store file, no timer, no subscription on a fresh install.
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/murasaki"_s;
    ProfileStore profileStore(configDir + u"/profiles.json"_s);
    profileStore.load();
    TriggerStore triggerStore(configDir + u"/triggers.json"_s);
    triggerStore.load();
    TimelineStore timelineStore(configDir + u"/timelines.json"_s);
    timelineStore.load();
    service.attachAutomation(&profileStore, &triggerStore, &timelineStore,
                             QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + u"/murasaki/automation-state.json"_s);
    // Screen triggers (revision 21): the reference pictures, the helper and the files of the watch. Nothing is opened until the user starts watching.
    ScreenRefStore screenRefs;
    service.attachScreen(&screenRefs, findScreenMatchHelper(), findSandboxHelper(), settings.importSettings().requireSandbox,
                         QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + u"/murasaki/screenmatch"_s,
                         QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + u"/murasaki/screen-state.json"_s);
    // Imports (revision 14): converted pets under <data>/murasaki/pets/imported, made by murasaki-petimport behind murasaki-sandbox.
    PetImportManager imports(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/murasaki/pets/imported"_s);
    const QString importHelper = findPetImportHelper(), importSandbox = findSandboxHelper();
    // The sandbox requirement is read again at every settings change, so turning it off or on takes effect for the next job without a restart.
    const auto applyImportSettings = [&] {
        imports.setHelper(importHelper, importSandbox, settings.importSettings().requireSandbox);
        imports.setMaxOutputBytes(qint64(settings.importSettings().maxOutputMiB) << 20);
    };
    applyImportSettings();
    QObject::connect(&settings, &SettingsStore::changed, &imports, applyImportSettings);
    imports.cleanStale();
    service.attachImports(&imports);
    // The sounds of the pets: assets in the data folder, imported by murasaki-petaudio behind the sandbox the settings ask for.
    PetAudioManager petAudio(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/murasaki/pets/audio"_s, QString(), QString());
    // The same rules as the pet imports: confined whenever the sandbox helper is there, required when the user asked for it.
    petAudio.setSandboxMode([&settings] {
        return settings.importSettings().requireSandbox ? PetAudioManager::Sandbox::Require : PetAudioManager::Sandbox::Prefer;
    });
    service.attachPetAudio(&petAudio);
    {
        // What no pet uses and was imported more than a day ago goes (a fresh import has time to be given to a pet).
        QStringList used;
        for (const PetPlacement &p : pets.pets())
            if (!p.audio.asset.isEmpty())
                used << p.audio.asset;
        petAudio.collect(used, 24 * 3600);
    }
    // murasaki-overlay runs only while some pet exists. MURASAKI_OVERLAY_UNIT=0 leaves it to the user (tests).
    SystemdOverlayUnit overlayUnit;
    if (qgetenv("MURASAKI_OVERLAY_UNIT") != "0")
        service.setOverlayUnit(&overlayUnit);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("murasakid: cannot connect to the D-Bus session bus");
        return 1;
    }
    if (!service.registerObject(bus) || !bus.registerService(QString(kDBusService))) {
        qCritical("murasakid: %s is already taken; is another murasakid running?",
                  qPrintable(QString(kDBusService)));
        return 1;
    }
    // Playlists do not rotate behind the lock screen. Only the lock matters here (screen off keeps rotating).
    SessionWatch session;
    QObject::connect(&session, &SessionWatch::pausedChanged, &playlistManager, &PlaylistManager::setSessionLocked);
    session.watchScreenSaver(bus);
    // What the triggers listen to, subscribed only while an enabled trigger needs it.
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    AutomationSources automationSources(service.automation(), bus, &session, runtimeDir.isEmpty() ? QString() : runtimeDir + u"/murasaki/triggers"_s);
    return QCoreApplication::exec();
}
