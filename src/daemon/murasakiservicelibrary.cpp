#include "murasakiservice.h"
#include "murasakiserviceinternal.h"

#include "automationmanager.h"

#include "localpets.h"
#include "localpuppets.h"
#include "petaudiomanager.h"
#include "puppetchecker.h"
#include "importablefiles.h"

#include "constants.h"
#include "playlistmanager.h"
#include "shortcut/launchassigned.h"
#include "shortcut/shortcutpolicy.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

QString MurasakiService::ListItems() const
{
    QJsonArray array;
    for (const WallpaperItem &item : m_library->items())
        array.append(withPermissions(item.toSummaryJson(), item.id));
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString MurasakiService::GetItem(const QString &id) const
{
    const auto item = m_library->item(id);
    if (!item) {
        if (calledFromDBus())
            sendErrorReply(QDBusError::InvalidArgs, u"Unknown item: "_s + id.left(200));
        return {};
    }
    return QString::fromUtf8(QJsonDocument(itemJson(*item)).toJson(QJsonDocument::Compact));
}

void MurasakiService::Rescan()
{
    m_library->rescan();
}

void MurasakiService::SetAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                    const QString &optionsJson)
{
    QJsonObject options;
    if (!optionsJson.trimmed().isEmpty()) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(optionsJson.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            rejectCall(u"options must be a JSON object"_s);
            return;
        }
        options = doc.object();
    }
    QString problem;
    const auto assignment = makeAssignment(output, desktop, itemId, options, &problem);
    if (!assignment) {
        rejectCall(problem);
        return;
    }
    if (isPlaylistItemId(itemId)) {
        if (!playlistsAvailable()) {
            rejectCall(u"playlists are not available"_s);
            return;
        }
        if (!m_playlistStore->playlist(playlistIdOf(itemId))) {
            rejectCall(u"Unknown playlist: "_s + itemId.left(200));
            return;
        }
        m_assignments->set(*assignment);
        return;
    }
    const auto item = m_library->item(itemId);
    if (!item) {
        rejectCall(u"Unknown item: "_s + itemId.left(200));
        return;
    }
    if (item->kind == ItemKind::Pet) {
        rejectCall(u"Pets cannot be set as a wallpaper: "_s + itemId.left(200));
        return;
    }
    if (item->kind == ItemKind::Asset) {
        rejectCall(u"Asset packs are not wallpapers: "_s + itemId.left(200));
        return;
    }
    m_assignments->set(*assignment);
}

void MurasakiService::ClearAssignment(const QString &output, const QString &desktop)
{
    if (!isValidKeyPart(output) || !isValidKeyPart(desktop)) {
        rejectCall(u"output and desktop must be 1-200 characters"_s);
        return;
    }
    m_assignments->clear(output, desktop);
}

QString MurasakiService::Assignments() const
{
    QJsonArray array;
    for (const Assignment &assignment : m_assignments->assignments())
        array.append(assignment.toJson());
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString MurasakiService::ResolveAssignment(const QString &output, const QString &desktop) const
{
    if (!isValidKeyPart(output) || !isValidKeyPart(desktop)) {
        rejectCall(u"output and desktop must be 1-200 characters"_s);
        return {};
    }
    QJsonObject reply{{u"assignment"_s, QJsonValue::Null}, {u"item"_s, QJsonValue::Null}};
    if (const auto assignment = m_assignments->resolve(output, desktop)) {
        reply.insert(u"assignment"_s, assignment->toJson());
        if (isPlaylistItemId(assignment->itemId)) {
            // The assignment names the item that is showing, so that what the surface compares (and what a client
            // shows) is the wallpaper itself; "playlist" says where it comes from.
            const auto current = m_playlistManager ? m_playlistManager->current(output, desktop) : std::nullopt;
            if (current) {
                if (!current->itemId.isEmpty()) {
                    QJsonObject resolved = assignment->toJson();
                    resolved.insert(u"itemId"_s, current->itemId);
                    reply.insert(u"assignment"_s, resolved);
                    if (const auto item = m_library->item(current->itemId))
                        reply.insert(u"item"_s, itemJson(*item));
                }
                reply.insert(u"playlist"_s, QJsonObject{{u"id"_s, current->playlistId},
                                                        {u"index"_s, current->index},
                                                        {u"nextAt"_s, double(current->nextAt)}});
            }
        } else if (const auto item = m_library->item(assignment->itemId); item && item->kind != ItemKind::Pet && item->kind != ItemKind::Asset) {
            // A pet or an asset pack is not a wallpaper: an old or hand-edited assignment to one resolves like a vanished item.
            reply.insert(u"item"_s, itemJson(*item));
        }
    }
    return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
}

// A string for the log: control characters (a path or a key from a socket can hold any) become '?', and it is cut.
static QString printable(const QString &text, int limit = 64)
{
    QString out = text.left(limit);
    for (QChar &c : out) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || (c.unicode() >= 0x80 && c.unicode() <= 0x9f) || c == QChar(0x2028) || c == QChar(0x2029))
            c = QLatin1Char('?');
    }
    return out;
}

static bool hasControl(const QString &text)
{
    return std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f || (c.unicode() >= 0x80 && c.unicode() <= 0x9f); });
}

QString MurasakiService::assignedShortcut(const QString &output, const QString &desktop, const QString &itemDir, const QString &key) const
{
    using namespace shortcut;
    if (!isValidKeyPart(output) || !isValidKeyPart(desktop) || itemDir.isEmpty() || itemDir.size() > 4096 || !validKey(key.toUtf8())) {
        return {};
    }
    const QString dir = QDir::cleanPath(itemDir);
    std::optional<WallpaperItem> item;
    for (const WallpaperItem &candidate : m_library->items()) {
        if (QDir::cleanPath(candidate.dir) == dir) {
            item = candidate;
            break;
        }
    }
    if (!item) {
        return {};
    }
    const bool declared = std::any_of(item->properties.begin(), item->properties.end(),
                                      [&](const WeProperty &p) { return p.type == WeProperty::Type::UserShortcut && p.key == key; });
    if (!declared) {
        return {};
    }
    // the one assignment the surface shows on this output and desktop: (output, desktop), (output, *), (*, desktop), (*, *), in that order
    const auto assignment = m_assignments->resolve(output, desktop);
    if (!assignment) {
        return {};
    }
    QString shown = assignment->itemId;
    if (isPlaylistItemId(shown)) {
        const auto current = m_playlistManager ? m_playlistManager->current(output, desktop) : std::nullopt;
        shown = current ? current->itemId : QString();
    }
    if (shown != item->id) {
        return {};
    }
    const QJsonValue value = assignment->properties.value(key);
    return value.isString() ? value.toString() : QString();
}

// What a click on a dock shortcut of a wallpaper may start. The surface has seen the click and that the wallpaper is shown; everything else
// is read here: the item is one in the library with that usershortcut property, the application is the one the user stored for it in the
// assignment that shows that item on `output` and `desktop` (the one ResolveAssignment returns), and the switch, the rate and the
// application's own gap are the daemon's. The scene never names an application: its script gave a property key, and the surface passed it
// on after the engine's message was validated. The daemon trusts the session bus: any process of the user can call this method (it could
// as well start the application itself).
bool MurasakiService::LaunchShortcut(const QString &output, const QString &desktop, const QString &itemDir, const QString &key)
{
    using namespace shortcut;
    if (hasControl(output) || hasControl(desktop) || hasControl(itemDir) || hasControl(key) || !isValidKeyPart(output) || !isValidKeyPart(desktop)
        || itemDir.isEmpty() || itemDir.size() > 4096 || !validKey(key.toUtf8())) {
        rejectCall(u"output, desktop, itemDir and key must be valid"_s);
        return false;
    }
    const auto no = [&](const QString &why) {
        qInfo("murasakid: shortcut %s of %s not started: %s", qPrintable(printable(key)), qPrintable(printable(itemDir.right(64))), qPrintable(why));
        return false;
    };
    const QString desktopId = assignedShortcut(output, desktop, itemDir, key);
    if (desktopId.isEmpty()) {
        return no(u"no application chosen for it in what is shown there"_s);
    }
    static const QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    const LaunchOutcome outcome = launchAssigned(desktopId, m_settings->shortcutsLaunch(), m_shortcutPolicy, clock.elapsed(), applicationRoots(),
                                                 systemDesktopEnvironment(), systemFindProgram(), [](const LaunchCommand &command) { return startDetached(command); });
    if (!outcome.started) {
        return no(outcome.reason);
    }
    qInfo("murasakid: started %s for the shortcut %s", qPrintable(printable(desktopId)), qPrintable(printable(key)));
    return true;
}

QJsonArray MurasakiService::variantStates(const WallpaperItem &item) const
{
    QJsonArray array;
    for (const PetVariant &variant : item.variants) {
        QJsonObject json = variant.toJson();
        if (m_transcodes && item.format.isEmpty()) { // (a puppet's variants are not animation files: nothing is converted for them)
            // "transcode" is the state of the bucket the placements need (the one in use once it is done), at the
            // rate the variant is reported at; "buckets" lists every bucket and rate that has a state.
            json.insert(u"transcode"_s, reportedTranscode(item, variant).toJson());
            QJsonArray buckets;
            for (const TranscodeStatus &status : m_transcodes->statuses(sourceFor(item, variant, kTranscodeMaxSide, 0)))
                buckets.append(status.toJson());
            json.insert(u"buckets"_s, buckets);
        }
        array.append(json);
    }
    return array;
}

QJsonObject MurasakiService::itemJson(const WallpaperItem &item) const
{
    QJsonObject json = withPermissions(item.toJson(), item.id);
    if (item.kind == ItemKind::Pet && m_transcodes)
        json.insert(u"variants"_s, variantStates(item));
    if (item.source == u"anima-engine"_s && item.kind == ItemKind::Pet) {
        QJsonArray importable;
        for (const ImportableFile &file : findImportableFiles(item.dir))
            importable.append(QJsonObject{{u"path"_s, file.path}, {u"name"_s, file.name}, {u"kind"_s, file.kind}, {u"size"_s, file.size}});
        if (!importable.isEmpty())
            json.insert(u"importable"_s, importable);
    }
    if (isGenerated(item)) {
        json.insert(u"generated"_s, true);
        // The values the import was made with (without the source paths), so the GUI can reopen the form: "Edit import".
        {
            const QJsonObject root = readRecordFile(item.dir + u"/source.json"_s);
            QJsonObject spec = root.value(u"spec"_s).toObject();
            if (root.contains(u"mdaa"_s))
                spec.insert(u"autoScene"_s, root.value(u"mdaa"_s).toObject().value(u"autoScene"_s).toInt(-1));
            json.insert(u"importSpec"_s, spec);
        }
        if (const QJsonObject mdaa = packageRecord(item); !mdaa.isEmpty()) {
            json.insert(u"scenes"_s, mdaa.value(u"scenes"_s));
            json.insert(u"autoScene"_s, mdaa.value(u"autoScene"_s).toInt(-1));
            json.insert(u"hasPack"_s, !mdaa.value(u"pack"_s).toObject().isEmpty()); // a triggers pack whose timelines can be made (revision 18)
        }
    }
    return json;
}

QJsonObject MurasakiService::withPermissions(QJsonObject json, const QString &itemId) const
{
    json.insert(u"permissions"_s, QJsonObject{{u"network"_s, m_permissions && m_permissions->network(itemId)}});
    return json;
}

void MurasakiService::attachPermissions(ItemPermissionStore *permissions)
{
    m_permissions = permissions;
    connect(m_permissions, &ItemPermissionStore::changed, this, [this](const QString &itemId) {
        emit ItemsChanged({}, {itemId}, {});
        // The surface only listens to assignments: a page must reload under its new rules.
        if (m_assignments->referencesItem(itemId) || (m_playlistManager && m_playlistManager->showsItem(itemId)))
            emit AssignmentsChanged();
    });
}

void MurasakiService::SetItemPermission(const QString &itemId, const QString &permission, bool allowed)
{
    if (!m_permissions) {
        rejectCall(u"permissions are not available"_s);
        return;
    }
    if (permission != u"network"_s) {
        rejectCall(u"Unknown permission: "_s + permission.left(200));
        return;
    }
    // Revoking is allowed for a vanished item (its grant should not outlive it); granting needs an existing one.
    if (allowed && !m_library->item(itemId)) {
        rejectCall(u"Unknown item: "_s + itemId.left(200));
        return;
    }
    QString error;
    if (!m_permissions->setNetwork(itemId, allowed, &error))
        rejectCall(error);
}

void MurasakiService::attachPlaylists(PlaylistStore *playlists, PlaylistManager *manager)
{
    m_playlistStore = playlists;
    m_playlistManager = manager;
    connect(m_playlistStore, &PlaylistStore::changed, this, &MurasakiService::PlaylistsChanged);
    // The surface re-resolves on this signal, and a new item only reloads the wallpaper when the resolved item differs.
    connect(m_playlistManager, &PlaylistManager::rotated, this, &MurasakiService::AssignmentsChanged);
}

bool MurasakiService::playlistsAvailable() const
{
    return m_playlistStore && m_playlistManager;
}

bool MurasakiService::checkPlaylistItem(const QString &itemId) const
{
    if (!isValidKeyPart(itemId) || isPlaylistItemId(itemId)) {
        rejectCall(u"Not a wallpaper: "_s + itemId.left(200));
        return false;
    }
    const auto item = m_library->item(itemId);
    if (!item) {
        rejectCall(u"Unknown item: "_s + itemId.left(200));
        return false;
    }
    if (item->kind == ItemKind::Pet) {
        rejectCall(u"Pets cannot be in a playlist: "_s + itemId.left(200));
        return false;
    }
    if (item->kind == ItemKind::Asset) {
        rejectCall(u"Asset packs cannot be in a playlist: "_s + itemId.left(200));
        return false;
    }
    return true;
}

QString MurasakiService::ListPlaylists() const
{
    QJsonArray array;
    if (m_playlistStore) {
        for (const Playlist &playlist : m_playlistStore->playlists())
            array.append(playlist.toJson());
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString MurasakiService::CreatePlaylist(const QString &name)
{
    if (!playlistsAvailable()) {
        rejectCall(u"playlists are not available"_s);
        return {};
    }
    QString problem;
    const QString id = m_playlistStore->create(name, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

void MurasakiService::UpdatePlaylist(const QString &playlistId, const QString &changesJson)
{
    if (!playlistsAvailable()) {
        rejectCall(u"playlists are not available"_s);
        return;
    }
    const auto current = m_playlistStore->playlist(playlistId);
    if (!current) {
        rejectCall(u"Unknown playlist: "_s + playlistId.left(200));
        return;
    }
    const auto changes = parseObject(changesJson, false);
    if (!changes) {
        rejectCall(u"changes must be a JSON object"_s);
        return;
    }
    QString problem;
    const auto updated = applyPlaylistChanges(*current, *changes, &problem);
    if (!updated) {
        rejectCall(problem);
        return;
    }
    // Items that were already in the list may have left the library since; only the new ones must exist.
    for (const QString &itemId : updated->items) {
        if (!current->items.contains(itemId) && !checkPlaylistItem(itemId))
            return;
    }
    problem.clear();
    if (!m_playlistStore->update(*updated, &problem) && !problem.isEmpty())
        rejectCall(problem);
}

void MurasakiService::changePlaylistItems(const QString &playlistId, const QStringList &itemIds, bool add)
{
    if (!playlistsAvailable()) {
        rejectCall(u"playlists are not available"_s);
        return;
    }
    if (itemIds.size() > kMaxPlaylistItems) { // before any lookup: the list comes from the bus
        rejectCall(u"at most %1 items at once"_s.arg(kMaxPlaylistItems));
        return;
    }
    auto playlist = m_playlistStore->playlist(playlistId);
    if (!playlist) {
        rejectCall(u"Unknown playlist: "_s + playlistId.left(200));
        return;
    }
    for (const QString &itemId : itemIds) {
        if (add) {
            if (!checkPlaylistItem(itemId))
                return;
            if (!playlist->items.contains(itemId))
                playlist->items.append(itemId);
        } else {
            playlist->items.removeAll(itemId);
        }
    }
    QString problem;
    if (!m_playlistStore->update(*playlist, &problem) && !problem.isEmpty())
        rejectCall(problem);
}

void MurasakiService::AddToPlaylist(const QString &playlistId, const QStringList &itemIds)
{
    changePlaylistItems(playlistId, itemIds, true);
}

void MurasakiService::RemoveFromPlaylist(const QString &playlistId, const QStringList &itemIds)
{
    changePlaylistItems(playlistId, itemIds, false);
}

void MurasakiService::DeletePlaylist(const QString &playlistId)
{
    if (!playlistsAvailable()) {
        rejectCall(u"playlists are not available"_s);
        return;
    }
    if (!m_playlistStore->playlist(playlistId)) {
        rejectCall(u"Unknown playlist: "_s + playlistId.left(200));
        return;
    }
    if (m_assignments->referencesItem(playlistItemId(playlistId))) {
        rejectCall(u"Clear the assignments that play this playlist first: "_s + playlistId.left(200));
        return;
    }
    QString problem;
    if (!m_playlistStore->remove(playlistId, &problem) && !problem.isEmpty())
        rejectCall(problem);
}

void MurasakiService::movePlaylist(const QString &output, const QString &desktop, int direction)
{
    if (!playlistsAvailable()) {
        rejectCall(u"playlists are not available"_s);
        return;
    }
    if (!isValidKeyPart(output) || !isValidKeyPart(desktop)) {
        rejectCall(u"output and desktop must be 1-200 characters"_s);
        return;
    }
    QString problem;
    const bool moved = direction > 0 ? m_playlistManager->next(output, desktop, &problem)
                                     : m_playlistManager->previous(output, desktop, &problem);
    if (!moved)
        rejectCall(problem);
}

void MurasakiService::NextInPlaylist(const QString &output, const QString &desktop)
{
    movePlaylist(output, desktop, +1);
}

void MurasakiService::PreviousInPlaylist(const QString &output, const QString &desktop)
{
    movePlaylist(output, desktop, -1);
}

} // namespace murasaki
