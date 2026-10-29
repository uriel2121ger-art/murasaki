#include "daemonclient.h"

#include "constants.h"

#include <QDBusArgument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QStringList toStringList(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return qdbus_cast<QStringList>(value);
    return value.toStringList();
}

QJsonDocument parse(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8());
}

QByteArray toJson(const QVariantMap &map)
{
    return QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact);
}

} // namespace

DaemonClient::DaemonClient(DBusBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_backend->setParent(this);
    connect(m_backend, &DBusBackend::availabilityChanged, this, &DaemonClient::onAvailability);
    connect(m_backend, &DBusBackend::signalReceived, this, &DaemonClient::onSignal);
    // (the demo marker is read from the three caches)
    for (const auto signal : {&DaemonClient::profilesChanged, &DaemonClient::triggersChanged, &DaemonClient::timelinesChanged})
        connect(this, signal, this, &DaemonClient::demosChanged);
    m_petUpdateTimer.setSingleShot(true);
    m_petUpdateTimer.setInterval(PetUpdateDelayMs);
    connect(&m_petUpdateTimer, &QTimer::timeout, this, &DaemonClient::flushPetUpdates);
}

DaemonClient::DaemonClient(QObject *parent)
    : DaemonClient(new SessionBusBackend(kDBusService, kDBusPath, kDBusInterface,
                                         {u"ItemsChanged"_s, u"AssignmentsChanged"_s, u"SettingsChanged"_s, u"PetsChanged"_s,
                                          u"TranscodeChanged"_s, u"PetsArrangeChanged"_s, u"PetsHiddenChanged"_s,
                                          u"PlaylistsChanged"_s, u"SteamCmdChanged"_s, u"ImportChanged"_s, u"PetAudioChanged"_s,
                                          u"ProfilesChanged"_s, u"TriggersChanged"_s, u"TimelinesChanged"_s, u"TriggerFired"_s, u"ScreenWatchChanged"_s, u"ScreenCaptured"_s}),
                   parent)
{
}

void DaemonClient::start()
{
    m_backend->start();
}

void DaemonClient::setAvailable(bool available)
{
    if (m_available == available)
        return;
    const bool pets = petsAvailable();
    const bool playlists = playlistsAvailable();
    const bool steamcmd = steamCmdSupported();
    const bool behaviours = petBehavioursAvailable();
    const bool imports = importsSupported();
    const bool audio = petAudioSupported();
    const bool automation = automationSupported();
    const bool stage = petStageSupported();
    const bool states = petStatesSupported();
    const bool puppets = puppetsSupported();
    const bool screen = screenSupported();
    const bool demos = demosSupported();
    m_available = available;
    emit availableChanged();
    // (screenSupported and demosSupported are notified by apiVersionChanged; a daemon that goes away and comes back with the same revision flips them here)
    if (screen != screenSupported() || demos != demosSupported())
        emit apiVersionChanged();
    if (puppets != puppetsSupported())
        emit puppetsSupportedChanged();
    if (states != petStatesSupported())
        emit petStatesSupportedChanged();
    if (stage != petStageSupported())
        emit petStageSupportedChanged();
    if (automation != automationSupported())
        emit automationSupportedChanged();
    if (audio != petAudioSupported())
        emit petAudioSupportedChanged();
    if (imports != importsSupported())
        emit importsSupportedChanged();
    if (behaviours != petBehavioursAvailable())
        emit petBehavioursAvailableChanged();
    if (steamcmd != steamCmdSupported())
        emit steamCmdSupportedChanged();
    if (pets != petsAvailable())
        emit petsAvailableChanged();
    if (playlists != playlistsAvailable())
        emit playlistsAvailableChanged();
}

void DaemonClient::setInfo(int apiVersion, const QString &status, const QString &assetsDir)
{
    if (m_apiVersion != apiVersion) {
        const bool pets = petsAvailable();
        const bool playlists = playlistsAvailable();
        const bool steamcmd = steamCmdSupported();
        const bool behaviours = petBehavioursAvailable();
        const bool imports = importsSupported();
        const bool audio = petAudioSupported();
        const bool automation = automationSupported();
        const bool stage = petStageSupported();
        const bool states = petStatesSupported();
        const bool puppets = puppetsSupported();
        m_apiVersion = apiVersion;
        emit apiVersionChanged();
        if (puppets != puppetsSupported())
            emit puppetsSupportedChanged();
        if (states != petStatesSupported())
            emit petStatesSupportedChanged();
        if (stage != petStageSupported())
            emit petStageSupportedChanged();
        if (automation != automationSupported())
            emit automationSupportedChanged();
        if (audio != petAudioSupported())
            emit petAudioSupportedChanged();
        if (imports != importsSupported())
            emit importsSupportedChanged();
        if (behaviours != petBehavioursAvailable())
            emit petBehavioursAvailableChanged();
        if (steamcmd != steamCmdSupported())
            emit steamCmdSupportedChanged();
        if (pets != petsAvailable())
            emit petsAvailableChanged();
        if (playlists != playlistsAvailable())
            emit playlistsAvailableChanged();
    }
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
    if (m_assetsDir != assetsDir) {
        m_assetsDir = assetsDir;
        emit assetsDirChanged();
    }
}

void DaemonClient::onAvailability(bool up)
{
    if (up == m_serviceUp)
        return;
    m_serviceUp = up;
    ++m_generation;
    if (up) {
        loadProperties();
    } else {
        setAvailable(false);
        setInfo(0, {}, {});
        m_petUpdateTimer.stop();
        m_pendingPetUpdates.clear();
        m_inflightPetUpdates.clear();
        notifyPendingPets();
        clearPets();
        clearPlaylists();
        clearAutomation();
        clearSteamCmd();
        if (!m_imports.isEmpty()) {
            m_imports.clear();
            emit importsChanged();
        }
        m_watchedAudioJobs.clear();
        if (!m_petAudioAssets.isEmpty()) {
            m_petAudioAssets.clear();
            emit petAudioAssetsChanged();
        }
    }
}

void DaemonClient::loadProperties()
{
    const quint64 generation = m_generation;
    m_backend->property(u"ApiVersion"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation != m_generation)
            return;
        if (!reply.ok) {
            setAvailable(false);
            setInfo(0, {}, {});
            emit error(tr("Cannot read the API version of murasakid: %1").arg(reply.message));
            return;
        }
        const int version = reply.value.toInt();
        if (quint32(version) < kMinDBusApiVersion) {
            setAvailable(false);
            setInfo(version, {}, {});
            emit error(tr("murasakid speaks API revision %1, but %2 is needed; update and restart it.")
                           .arg(version)
                           .arg(kMinDBusApiVersion));
            return;
        }
        m_backend->property(u"Status"_s, [this, generation, version](const DBusBackend::Reply &statusReply) {
            if (generation != m_generation)
                return;
            const QString status = statusReply.ok ? statusReply.value.toString() : QString();
            m_backend->property(u"WallpaperEngineAssetsDir"_s,
                                [this, generation, version, status](const DBusBackend::Reply &dirReply) {
                                    if (generation != m_generation)
                                        return;
                                    setInfo(version, status, dirReply.ok ? dirReply.value.toString() : QString());
                                    setAvailable(true);
                                    refreshItems();
                                    refreshAssignments();
                                    refreshSettings();
                                    if (petsAvailable()) {
                                        loadPetsArrange();
                                        if (petBehavioursAvailable())
                                            loadPetsHidden();
                                        refreshPets();
                                    }
                                    if (playlistsAvailable())
                                        refreshPlaylists();
                                    if (steamCmdSupported())
                                        refreshSteamCmd();
                                    if (importsSupported())
                                        refreshImports();
                                    if (petAudioSupported())
                                        refreshPetAudio();
                                    if (automationSupported())
                                        refreshAutomation();
                                    if (screenSupported()) {
                                        refreshScreenWatch();
                                        // (what a walk that died left on, see noteScreenDemo)
                                        if (!m_screenDemoLive)
                                            undoScreenDemo();
                                    }
                                });
        });
    });
}

void DaemonClient::call(const QString &method, const QVariantList &args, std::function<void(const QVariant &)> done,
                        std::function<void(const DBusBackend::Reply &)> failed, int flags)
{
    const quint64 generation = m_generation;
    m_backend->call(method, args,
                    [this, generation, flags, done = std::move(done), failed = std::move(failed)](const DBusBackend::Reply &reply) {
                        if (!(flags & Ungated) && generation != m_generation)
                            return;
                        if (reply.ok) {
                            if (done)
                                done(reply.value);
                            return;
                        }
                        // Anything else (service vanished, timeout) is reflected by `available`, not worth a message.
                        if (!(flags & Silent) && (reply.isInvalidArgs() || reply.isFailed()))
                            emit error(reply.message);
                        if (failed)
                            failed(reply);
                    });
}

void DaemonClient::onSignal(const QString &name, const QVariantList &args)
{
    if (name == u"ItemsChanged" && args.size() >= 3) {
        emit itemsChanged(toStringList(args[0]), toStringList(args[1]), toStringList(args[2]));
    } else if (name == u"AssignmentsChanged") {
        refreshAssignments();
    } else if (name == u"SettingsChanged") {
        refreshSettings();
    } else if (name == u"PetsChanged") {
        refreshPets();
        refreshPetAudio(); // (the uses of each sound may have changed)
    } else if (name == u"PlaylistsChanged") {
        refreshPlaylists();
    } else if (name == u"ProfilesChanged") {
        refreshProfiles();
    } else if (name == u"TriggersChanged") {
        refreshTriggers();
    } else if (name == u"TimelinesChanged") {
        refreshTimelines();
    } else if (name == u"TriggerFired") {
        if (automationSupported())
            call(u"TriggerState"_s, {}, [this](const QVariant &value) {
                const QVariantMap states = parse(value).object().toVariantMap();
                if (states != m_triggerStates) {
                    m_triggerStates = states;
                    emit triggerStatesChanged();
                }
            });
    } else if (name == u"ScreenWatchChanged") {
        refreshScreenWatch();
    } else if (name == u"ScreenCaptured" && args.size() >= 2) {
        screenCaptureArrived(args[0].toString().left(64), parse(args[1]).object());
    } else if (name == u"SteamCmdChanged") {
        refreshSteamCmd();
    } else if (name == u"ImportChanged") {
        refreshImports();
    } else if (name == u"PetAudioChanged") {
        refreshPetAudio();
        if (!args.isEmpty() && !args[0].toString().isEmpty())
            checkPetAudioJob(args[0].toString());
    } else if (name == u"TranscodeChanged" && !args.isEmpty()) {
        emit transcodeChanged(args[0].toString());
    } else if (name == u"PetsArrangeChanged" && !args.isEmpty()) {
        setPetsArrangeValue(args[0].toBool());
    } else if (name == u"PetsHiddenChanged" && !args.isEmpty()) {
        setPetsHiddenValue(args[0].toBool());
    }
}

void DaemonClient::refreshItems()
{
    call(u"ListItems"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (doc.isArray())
            emit itemsListed(doc.array().toVariantList());
    });
}

void DaemonClient::getItem(const QString &id)
{
    call(
        u"GetItem"_s, {id},
        [this, id](const QVariant &value) {
            const QJsonDocument doc = parse(value);
            if (doc.isObject())
                emit itemLoaded(id, doc.object().toVariantMap());
            else
                emit itemLoadFailed(id, tr("Malformed item answer"));
        },
        [this, id](const DBusBackend::Reply &reply) { emit itemLoadFailed(id, reply.message); }, Silent);
}

void DaemonClient::setAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                 const QVariantMap &options)
{
    const QString json = options.isEmpty() ? QString() : QString::fromUtf8(toJson(options));
    // The daemon announces AssignmentsChanged itself; no local refresh needed.
    call(u"SetAssignment"_s, {output, desktop, itemId, json}, {}, {}, Ungated);
}

void DaemonClient::clearAssignment(const QString &output, const QString &desktop)
{
    call(u"ClearAssignment"_s, {output, desktop}, {}, {}, Ungated);
}

void DaemonClient::refreshAssignments()
{
    call(u"Assignments"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (!doc.isArray())
            return;
        const QVariantList list = doc.array().toVariantList();
        if (m_assignmentsLoaded && list == m_assignments)
            return;
        m_assignmentsLoaded = true;
        m_assignments = list;
        emit assignmentsChanged();
    });
}

QVariantList DaemonClient::overridingAssignments(const QString &output, const QString &desktop,
                                                const QStringList &knownDesktops) const
{
    const auto exists = [&](const QString &d) { return d == u"*" || knownDesktops.isEmpty() || knownDesktops.contains(d); };
    QVariantList result;
    for (const QVariant &entry : m_assignments) {
        const QVariantMap a = entry.toMap();
        const QString o = a.value(u"output"_s).toString();
        const QString d = a.value(u"desktop"_s).toString();
        if ((output == u"*" || o == output) && (desktop == u"*" || d == desktop) && !(o == output && d == desktop)
            && exists(d))
            result.append(entry);
    }
    return result;
}

QVariantList DaemonClient::orphanAssignments(const QString &output, const QString &desktop,
                                             const QStringList &knownDesktops) const
{
    QVariantList result;
    if (desktop != u"*" || knownDesktops.isEmpty())
        return result;
    for (const QVariant &entry : m_assignments) {
        const QVariantMap a = entry.toMap();
        if ((output == u"*" || a.value(u"output"_s).toString() == output)
            && !knownDesktops.contains(a.value(u"desktop"_s).toString()) && a.value(u"desktop"_s).toString() != u"*")
            result.append(entry);
    }
    return result;
}

void DaemonClient::resolveAssignment(const QString &output, const QString &desktop)
{
    call(u"ResolveAssignment"_s, {output, desktop}, [this, output, desktop](const QVariant &value) {
        const QJsonObject object = parse(value).object();
        emit assignmentResolved(output, desktop, object.value("assignment"_L1).toObject().toVariantMap(),
                                object.value("item"_L1).toObject().toVariantMap(),
                                object.value("playlist"_L1).toObject().toVariantMap());
    });
}

void DaemonClient::refreshSettings()
{
    call(u"Settings"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (!doc.isObject())
            return;
        const QVariantMap map = doc.object().toVariantMap();
        if (m_settingsLoaded && map == m_settings)
            return;
        m_settingsLoaded = true;
        m_settings = map;
        emit settingsChanged();
    });
}

void DaemonClient::setSettings(const QVariantMap &settings)
{
    call(u"SetSettings"_s, {QString::fromUtf8(toJson(settings))}, {}, {}, Ungated);
}

void DaemonClient::setItemPermission(const QString &itemId, const QString &permission, bool allowed)
{
    if (!permissionsAvailable()) {
        emit error(tr("Permissions need a newer Murasaki service; update and restart it."));
        return;
    }
    // The daemon announces the change itself (ItemsChanged, and AssignmentsChanged for a shown item).
    call(u"SetItemPermission"_s, {itemId, permission, allowed}, {}, {}, Ungated);
}

void DaemonClient::clearSteamCmd()
{
    m_steamCmdLoaded = false;
    if (!m_steamCmd.isEmpty()) {
        m_steamCmd.clear();
        emit steamCmdChanged();
    }
}

bool DaemonClient::requireSteamCmd()
{
    if (steamCmdSupported())
        return true;
    emit error(tr("Downloading with SteamCMD needs a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::refreshSteamCmd()
{
    if (!steamCmdSupported())
        return;
    call(u"SteamCmdStatus"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (!doc.isObject())
            return;
        const QVariantMap status = doc.object().toVariantMap();
        if (m_steamCmdLoaded && status == m_steamCmd)
            return;
        m_steamCmdLoaded = true;
        m_steamCmd = status;
        emit steamCmdChanged();
    });
}

void DaemonClient::steamCmdLogin(const QString &user)
{
    if (requireSteamCmd())
        call(u"SteamCmdLogin"_s, {user}, {}, {}, Ungated);
}

void DaemonClient::steamCmdSubmit(const QString &kind, const QString &secret)
{
    if (requireSteamCmd())
        call(u"SteamCmdSubmit"_s, {kind, secret}, {}, {}, Ungated);
}

void DaemonClient::steamCmdCancelLogin()
{
    if (requireSteamCmd())
        call(u"SteamCmdCancelLogin"_s, {}, {}, {}, Ungated);
}

void DaemonClient::steamCmdLogout()
{
    if (requireSteamCmd())
        call(u"SteamCmdLogout"_s, {}, {}, {}, Ungated);
}

void DaemonClient::steamCmdDownload(const QString &appId, const QString &itemId)
{
    if (requireSteamCmd())
        call(u"SteamCmdDownload"_s, {appId, itemId}, {}, {}, Ungated);
}

void DaemonClient::steamCmdCancel(const QString &itemId)
{
    if (requireSteamCmd())
        call(u"SteamCmdCancel"_s, {itemId}, {}, {}, Ungated);
}

void DaemonClient::setPlaylists(const QVariantList &playlists)
{
    if (m_playlistsLoaded && playlists == m_playlists)
        return;
    m_playlistsLoaded = true;
    m_playlists = playlists;
    emit playlistsChanged();
}

void DaemonClient::clearPlaylists()
{
    m_playlistsLoaded = false;
    if (!m_playlists.isEmpty()) {
        m_playlists.clear();
        emit playlistsChanged();
    }
}

bool DaemonClient::requirePlaylists()
{
    if (playlistsAvailable())
        return true;
    emit error(tr("Playlists need a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::refreshPlaylists()
{
    if (!playlistsAvailable())
        return;
    call(u"ListPlaylists"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (doc.isArray())
            setPlaylists(doc.array().toVariantList());
    });
}

void DaemonClient::createPlaylist(const QString &name)
{
    if (!requirePlaylists())
        return;
    call(
        u"CreatePlaylist"_s, {name}, [this, name](const QVariant &value) { emit playlistCreated(name, value.toString()); },
        {}, Ungated);
}

void DaemonClient::updatePlaylist(const QString &playlistId, const QVariantMap &changes)
{
    if (!requirePlaylists())
        return;
    call(u"UpdatePlaylist"_s, {playlistId, QString::fromUtf8(toJson(changes))}, {}, {}, Ungated);
}

void DaemonClient::addToPlaylist(const QString &playlistId, const QStringList &itemIds)
{
    if (!requirePlaylists())
        return;
    call(u"AddToPlaylist"_s, {playlistId, itemIds}, {}, {}, Ungated);
}

void DaemonClient::removeFromPlaylist(const QString &playlistId, const QStringList &itemIds)
{
    if (!requirePlaylists())
        return;
    call(u"RemoveFromPlaylist"_s, {playlistId, itemIds}, {}, {}, Ungated);
}

void DaemonClient::deletePlaylist(const QString &playlistId)
{
    if (!requirePlaylists())
        return;
    call(u"DeletePlaylist"_s, {playlistId}, {}, {}, Ungated);
}

void DaemonClient::nextInPlaylist(const QString &output, const QString &desktop)
{
    if (!requirePlaylists())
        return;
    call(u"NextInPlaylist"_s, {output, desktop}, {}, {}, Ungated);
}

void DaemonClient::previousInPlaylist(const QString &output, const QString &desktop)
{
    if (!requirePlaylists())
        return;
    call(u"PreviousInPlaylist"_s, {output, desktop}, {}, {}, Ungated);
}

void DaemonClient::setPets(const QVariantList &pets)
{
    if (m_petsLoaded && pets == m_pets)
        return;
    m_petsLoaded = true;
    m_pets = pets;
    emit petsChanged();
}

void DaemonClient::clearPets()
{
    m_petsLoaded = false;
    if (!m_pets.isEmpty()) {
        m_pets.clear();
        emit petsChanged();
    }
    setPetsArrangeValue(false);
    setPetsHiddenValue(false);
}

void DaemonClient::setPetsArrangeValue(bool arrange)
{
    if (m_petsArrange == arrange)
        return;
    m_petsArrange = arrange;
    emit petsArrangeChanged();
}

void DaemonClient::setPetsHiddenValue(bool hidden)
{
    if (m_petsHidden == hidden)
        return;
    m_petsHidden = hidden;
    emit petsHiddenChanged();
}

void DaemonClient::loadPetsHidden()
{
    const quint64 generation = m_generation;
    m_backend->property(u"PetsHidden"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation == m_generation && reply.ok)
            setPetsHiddenValue(reply.value.toBool());
    });
}

void DaemonClient::loadPetsArrange()
{
    const quint64 generation = m_generation;
    m_backend->property(u"PetsArrange"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation == m_generation && reply.ok)
            setPetsArrangeValue(reply.value.toBool());
    });
}

bool DaemonClient::requirePets()
{
    if (petsAvailable())
        return true;
    emit error(tr("Desktop pets need a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::refreshPets()
{
    if (!petsAvailable())
        return;
    call(u"ListPets"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (doc.isArray())
            setPets(doc.array().toVariantList());
    });
}

void DaemonClient::addPet(const QString &itemId, const QString &variant, const QString &output,
                          const QVariantMap &options)
{
    if (!requirePets())
        return;
    const QString json = options.isEmpty() ? QString() : QString::fromUtf8(toJson(options));
    call(
        u"AddPet"_s, {itemId, variant, output, json},
        [this, itemId](const QVariant &value) { emit petAdded(itemId, value.toString()); }, {}, Ungated);
}

void DaemonClient::updatePet(const QString &petId, const QVariantMap &changes)
{
    if (!requirePets())
        return;
    ++m_inflightPetUpdates[petId];
    notifyPendingPets();
    // Whatever the answer, the controls get the daemon's list before they follow it again: after an accepted edit
    // that is the new value, after a refused one the old value.
    const quint64 generation = m_generation;
    const auto finish = [this, petId, generation] {
        if (generation != m_generation)
            return; // the daemon restarted; the in-flight table was cleared with it
        call(
            u"ListPets"_s, {},
            [this, petId](const QVariant &value) {
                const QJsonDocument doc = parse(value);
                if (doc.isArray())
                    setPets(doc.array().toVariantList());
                releasePetUpdate(petId);
            },
            [this, petId](const DBusBackend::Reply &) { releasePetUpdate(petId); }, Silent);
    };
    call(
        u"UpdatePet"_s, {petId, QString::fromUtf8(toJson(changes))}, [finish](const QVariant &) { finish(); },
        [finish](const DBusBackend::Reply &) { finish(); }, Ungated);
}

void DaemonClient::releasePetUpdate(const QString &petId)
{
    const auto it = m_inflightPetUpdates.find(petId);
    if (it == m_inflightPetUpdates.end())
        return;
    if (--it.value() <= 0)
        m_inflightPetUpdates.erase(it);
    notifyPendingPets();
}

QStringList DaemonClient::pendingPetIds() const
{
    QStringList ids = m_pendingPetUpdates.keys();
    for (auto it = m_inflightPetUpdates.cbegin(); it != m_inflightPetUpdates.cend(); ++it) {
        if (!ids.contains(it.key()))
            ids.append(it.key());
    }
    ids.sort();
    return ids;
}

void DaemonClient::notifyPendingPets()
{
    const QStringList now = pendingPetIds();
    if (now == m_lastPendingPetIds)
        return;
    m_lastPendingPetIds = now;
    emit pendingPetUpdatesChanged();
}

void DaemonClient::queuePetUpdate(const QString &petId, const QVariantMap &changes)
{
    if (!petsAvailable())
        return;
    QVariantMap &pending = m_pendingPetUpdates[petId];
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        pending.insert(it.key(), it.value());
    m_petUpdateTimer.start();
    notifyPendingPets();
}

void DaemonClient::flushPetUpdates()
{
    m_petUpdateTimer.stop();
    const auto pending = std::exchange(m_pendingPetUpdates, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        updatePet(it.key(), it.value()); // moves the pet from queued to in flight; notifies
    notifyPendingPets();
}

void DaemonClient::removePet(const QString &petId)
{
    if (!requirePets())
        return;
    // Edits still waiting for a pet that goes away would only fail.
    m_pendingPetUpdates.remove(petId);
    notifyPendingPets();
    call(u"RemovePet"_s, {petId}, {}, {}, Ungated);
}

void DaemonClient::transcodeState(const QString &itemId)
{
    if (!petsAvailable())
        return;
    call(u"TranscodeState"_s, {itemId}, [this, itemId](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (doc.isObject())
            emit transcodeStateLoaded(itemId, doc.object().value("variants"_L1).toArray().toVariantList());
    });
}

void DaemonClient::requestTranscode(const QString &itemId, const QString &variant)
{
    if (!requirePets())
        return;
    call(u"RequestTranscode"_s, {itemId, variant}, {}, {}, Ungated);
}

void DaemonClient::setPetsHidden(bool hidden)
{
    if (!requirePets() || !petBehavioursAvailable())
        return;
    // The daemon announces PetsHiddenChanged; until then the toggle keeps showing the daemon's value.
    call(u"SetPetsHidden"_s, {hidden}, {}, {}, Ungated);
}

void DaemonClient::rescuePets()
{
    if (!requirePets() || !petBehavioursAvailable())
        return;
    call(u"RescuePets"_s, {}, {}, {}, Ungated);
}

void DaemonClient::setPetsArrange(bool arrange)
{
    if (!requirePets())
        return;
    // The daemon announces PetsArrangeChanged; until then the toggle keeps showing the daemon's value.
    call(u"SetPetsArrange"_s, {arrange}, {}, {}, Ungated);
}

void DaemonClient::addLocalPetItem(const QString &path)
{
    if (!requirePets())
        return;
    call(
        u"AddLocalPetItem"_s, {path},
        [this, path](const QVariant &value) { emit localPetAdded(path, value.toString()); },
        [this](const DBusBackend::Reply &reply) {
            // A daemon built before this call: say so instead of failing silently.
            if (reply.error == u"org.freedesktop.DBus.Error.UnknownMethod"_s)
                emit error(tr("The Murasaki service does not support adding local pets; update and restart it."));
        },
        Ungated);
}

void DaemonClient::removeLocalPetItem(const QString &itemId)
{
    if (!requirePets())
        return;
    call(u"RemoveLocalPetItem"_s, {itemId}, {}, {}, Ungated);
}

void DaemonClient::addLocalPuppetItem(const QString &folder)
{
    if (!puppetsSupported()) {
        emit error(tr("Puppets need a newer Murasaki service; update and restart it."));
        return;
    }
    call(u"AddLocalPuppetItem"_s, {folder}, [this, folder](const QVariant &value) { emit localPuppetAdded(folder, value.toString()); }, {}, Ungated);
}

void DaemonClient::removeLocalPuppetItem(const QString &itemId)
{
    if (!puppetsSupported())
        return;
    call(u"RemoveLocalPuppetItem"_s, {itemId}, {}, {}, Ungated);
}

bool DaemonClient::requireImports()
{
    if (importsSupported())
        return true;
    emit error(tr("Importing pets needs a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::refreshImports()
{
    if (!importsSupported())
        return;
    call(u"ImportState"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (!doc.isArray())
            return;
        const QVariantList jobs = doc.array().toVariantList();
        if (jobs == m_imports)
            return;
        m_imports = jobs;
        emit importsChanged();
    });
}

void DaemonClient::importPet(const QVariantMap &spec)
{
    if (!requireImports())
        return;
    const QString json = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(spec)).toJson(QJsonDocument::Compact));
    call(u"ImportPet"_s, {json}, [this](const QVariant &value) { emit importStarted(value.toString()); }, {}, Ungated);
}

void DaemonClient::previewImport(const QVariantMap &spec)
{
    if (!requireImports())
        return;
    const QString json = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(spec)).toJson(QJsonDocument::Compact));
    // A spec the daemon refuses comes back as an InvalidArgs error; the form shows it as a preview error instead of a popup.
    call(
        u"PreviewImport"_s, {json},
        [this](const QVariant &value) {
            const QJsonDocument doc = parse(value);
            if (doc.isObject())
                emit importPreviewed(doc.object().toVariantMap());
        },
        [this](const DBusBackend::Reply &reply) {
            if (reply.isInvalidArgs())
                emit importPreviewed({{u"ok"_s, false}, {u"error"_s, reply.message.left(300)}});
        },
        Silent);
}

void DaemonClient::cancelImport(const QString &jobId)
{
    if (requireImports())
        call(u"CancelImport"_s, {jobId}, {}, {}, Ungated);
}

void DaemonClient::removeImportedPet(const QString &itemId, bool deleteFiles)
{
    if (requireImports())
        call(u"RemoveImportedPet"_s, {itemId, deleteFiles}, {}, {}, Ungated);
}

bool DaemonClient::requirePetAudio()
{
    if (petAudioSupported())
        return true;
    emit error(tr("Pet sounds need a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::refreshPetAudio()
{
    if (!petAudioSupported())
        return;
    call(u"ListPetAudioAssets"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parse(value);
        if (!doc.isArray())
            return;
        const QVariantList assets = doc.array().toVariantList();
        if (assets == m_petAudioAssets)
            return;
        m_petAudioAssets = assets;
        emit petAudioAssetsChanged();
    });
}

void DaemonClient::watchPetAudioJob(const QString &jobId, int ticket)
{
    if (jobId.isEmpty())
        return;
    if (!m_watchedAudioJobs.contains(jobId))
        m_watchedAudioJobs.append(jobId);
    emit petAudioImportStarted(jobId, ticket);
    checkPetAudioJob(jobId); // (it may already be over)
}

void DaemonClient::checkPetAudioJob(const QString &jobId)
{
    if (!m_watchedAudioJobs.contains(jobId))
        return;
    call(u"PetAudioImportState"_s, {jobId}, [this, jobId](const QVariant &value) {
        const QJsonObject job = parse(value).object();
        const QString state = job.value(u"state"_s).toString();
        if (state != u"done"_s && state != u"failed"_s && state != u"cancelled"_s)
            return;
        if (!m_watchedAudioJobs.removeOne(jobId))
            return;
        emit petAudioImportFinished(jobId, state == u"done"_s, job.value(u"result"_s).toObject().value(u"asset"_s).toString(),
                                    state == u"done"_s ? QString() : job.value(u"error"_s).toString(state));
    });
}

int DaemonClient::importPetAudio(const QString &path)
{
    return requestPetAudioImport(u"ImportPetAudio"_s, path);
}

int DaemonClient::importItemAudio(const QString &itemId)
{
    return requestPetAudioImport(u"ImportItemAudio"_s, itemId);
}

int DaemonClient::requestPetAudioImport(const QString &method, const QString &argument)
{
    if (!requirePetAudio())
        return 0;
    const int ticket = ++m_audioTicket;
    call(method, {argument}, [this, ticket](const QVariant &value) { watchPetAudioJob(value.toString(), ticket); },
         [this, ticket](const DBusBackend::Reply &reply) { emit petAudioImportRefused(ticket, reply.message); }, Ungated | Silent);
    return ticket;
}

void DaemonClient::cancelPetAudioImport(const QString &jobId)
{
    if (requirePetAudio())
        call(u"CancelPetAudioImport"_s, {jobId}, {}, {}, Ungated);
}

void DaemonClient::removePetAudioAsset(const QString &asset)
{
    if (requirePetAudio())
        call(u"RemovePetAudioAsset"_s, {asset}, {}, {}, Ungated);
}

void DaemonClient::startScene(const QString &petId, int index)
{
    if (requireImports())
        call(u"StartScene"_s, {petId, index}, {}, {}, Ungated);
}

} // namespace murasaki
