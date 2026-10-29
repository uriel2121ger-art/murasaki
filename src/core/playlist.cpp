#include "playlist.h"

#include "assignment.h"
#include "jsonfile.h"

#include <QJsonArray>
#include <QSet>
#include <QUuid>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

bool isPlaylistItemId(const QString &itemId)
{
    return itemId.startsWith(kPlaylistItemPrefix);
}

QString playlistItemId(const QString &playlistId)
{
    return kPlaylistItemPrefix + playlistId;
}

QString playlistIdOf(const QString &itemId)
{
    return isPlaylistItemId(itemId) ? itemId.mid(kPlaylistItemPrefix.size()) : QString();
}

QString playlistOrderToString(PlaylistOrder order)
{
    return order == PlaylistOrder::Shuffle ? u"shuffle"_s : u"sequential"_s;
}

QJsonObject Playlist::toJson() const
{
    return QJsonObject{
        {u"id"_s, id},
        {u"name"_s, name},
        {u"items"_s, QJsonArray::fromStringList(items)},
        {u"order"_s, playlistOrderToString(order)},
        {u"intervalMinutes"_s, intervalMinutes},
        {u"changeOnLogin"_s, changeOnLogin},
    };
}

bool validatePlaylist(const Playlist &p, QString *error)
{
    if (p.name.trimmed().isEmpty() || p.name.size() > kMaxPlaylistNameLength)
        return fail(error, u"name must be 1-%1 characters"_s.arg(kMaxPlaylistNameLength));
    if (p.intervalMinutes < kMinPlaylistInterval || p.intervalMinutes > kMaxPlaylistInterval)
        return fail(error, u"intervalMinutes must be between %1 and %2"_s.arg(kMinPlaylistInterval)
                               .arg(kMaxPlaylistInterval));
    if (p.items.size() > kMaxPlaylistItems)
        return fail(error, u"at most %1 items"_s.arg(kMaxPlaylistItems));
    QSet<QString> seen;
    for (const QString &item : p.items) {
        if (!isValidKeyPart(item))
            return fail(error, u"items must be 1-200 characters"_s);
        if (isPlaylistItemId(item))
            return fail(error, u"a playlist can not contain a playlist"_s);
        if (seen.contains(item))
            return fail(error, u"duplicate item: "_s + item.left(200));
        seen.insert(item);
    }
    return true;
}

std::optional<Playlist> applyPlaylistChanges(const Playlist &base, const QJsonObject &changes, QString *error)
{
    Playlist p = base;
    const auto bad = [error](const QString &message) -> std::optional<Playlist> {
        fail(error, message);
        return std::nullopt;
    };
    if (const auto it = changes.constFind(u"name"_s); it != changes.constEnd()) {
        if (!it.value().isString())
            return bad(u"name must be a string"_s);
        p.name = it.value().toString();
    }
    if (const auto it = changes.constFind(u"items"_s); it != changes.constEnd()) {
        if (!it.value().isArray())
            return bad(u"items must be an array of strings"_s);
        QStringList items;
        for (const QJsonValue &value : it.value().toArray()) {
            if (!value.isString())
                return bad(u"items must be an array of strings"_s);
            items.append(value.toString());
        }
        p.items = items;
    }
    if (const auto it = changes.constFind(u"order"_s); it != changes.constEnd()) {
        const QString order = it.value().isString() ? it.value().toString() : QString();
        if (order == u"sequential")
            p.order = PlaylistOrder::Sequential;
        else if (order == u"shuffle")
            p.order = PlaylistOrder::Shuffle;
        else
            return bad(u"order must be sequential or shuffle"_s);
    }
    if (const auto it = changes.constFind(u"intervalMinutes"_s); it != changes.constEnd()) {
        // JSON has one number type, so an integral double such as 30.0 is accepted.
        if (!it.value().isDouble() || it.value().toDouble() != std::floor(it.value().toDouble()))
            return bad(u"intervalMinutes must be an integer"_s);
        const double minutes = it.value().toDouble();
        if (minutes < kMinPlaylistInterval || minutes > kMaxPlaylistInterval)
            return bad(u"intervalMinutes must be between %1 and %2"_s.arg(kMinPlaylistInterval)
                           .arg(kMaxPlaylistInterval));
        p.intervalMinutes = static_cast<int>(minutes);
    }
    if (const auto it = changes.constFind(u"changeOnLogin"_s); it != changes.constEnd()) {
        if (!it.value().isBool())
            return bad(u"changeOnLogin must be a boolean"_s);
        p.changeOnLogin = it.value().toBool();
    }
    if (!validatePlaylist(p, error))
        return std::nullopt;
    return p;
}

PlaylistStore::PlaylistStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void PlaylistStore::load()
{
    m_playlists.clear();
    m_saveBlocked = false;
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;

    QString problem = read.problem;
    if (problem.isEmpty()) {
        if (read.root.value(u"version"_s).toInt() != 1 || !read.root.value(u"playlists"_s).isArray()) {
            problem = u"unsupported format"_s;
        } else {
            const QJsonArray entries = read.root.value(u"playlists"_s).toArray();
            if (entries.size() > kMaxPlaylists)
                problem = u"too many playlists"_s;
            for (const QJsonValue &value : entries) {
                if (!problem.isEmpty())
                    break;
                const QJsonObject entry = value.toObject();
                Playlist base;
                base.id = entry.value(u"id"_s).toString();
                QString error;
                const auto parsed = applyPlaylistChanges(base, entry, &error);
                if (!parsed) {
                    problem = u"invalid entry: "_s + error;
                } else if (QUuid::fromString(parsed->id).isNull()) {
                    problem = u"invalid entry: id must be a uuid"_s;
                } else if (playlist(parsed->id)) {
                    problem = u"invalid entry: duplicate id"_s;
                } else {
                    m_playlists.append(*parsed);
                }
            }
        }
    }
    if (problem.isEmpty())
        return;

    m_playlists.clear();
    if (!quarantineBadFile(m_path, problem, u"no playlists"_s, u"playlists"_s))
        m_saveBlocked = true;
}

std::optional<Playlist> PlaylistStore::playlist(const QString &id) const
{
    for (const Playlist &p : m_playlists) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

bool PlaylistStore::referencesItem(const QString &itemId) const
{
    for (const Playlist &p : m_playlists) {
        if (p.items.contains(itemId))
            return true;
    }
    return false;
}

QString PlaylistStore::create(const QString &name, QString *error)
{
    if (m_playlists.size() >= kMaxPlaylists) {
        fail(error, u"at most %1 playlists"_s.arg(kMaxPlaylists));
        return {};
    }
    Playlist p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name;
    if (!validatePlaylist(p, error))
        return {};
    m_playlists.append(p);
    if (!save()) {
        m_playlists.removeLast();
        fail(error, u"could not save the playlists"_s);
        return {};
    }
    emit changed();
    return p.id;
}

bool PlaylistStore::update(const Playlist &playlist, QString *error)
{
    if (error)
        error->clear();
    if (!validatePlaylist(playlist, error))
        return false;
    for (Playlist &p : m_playlists) {
        if (p.id != playlist.id)
            continue;
        if (p == playlist)
            return false;
        const Playlist previous = p;
        p = playlist;
        if (!save()) {
            p = previous;
            fail(error, u"could not save the playlists"_s);
            return false;
        }
        emit changed();
        return true;
    }
    return false;
}

bool PlaylistStore::remove(const QString &id, QString *error)
{
    if (error)
        error->clear();
    for (qsizetype i = 0; i < m_playlists.size(); ++i) {
        if (m_playlists.at(i).id != id)
            continue;
        const Playlist removed = m_playlists.takeAt(i);
        if (!save()) {
            m_playlists.insert(i, removed);
            fail(error, u"could not save the playlists"_s);
            return false;
        }
        emit changed();
        return true;
    }
    return false;
}

bool PlaylistStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving playlists over the unreadable %s", qPrintable(m_path));
        return false;
    }
    QJsonArray array;
    for (const Playlist &p : m_playlists)
        array.append(p.toJson());
    return writeJsonObjectFile(m_path, QJsonObject{{u"version"_s, 1}, {u"playlists"_s, array}});
}

} // namespace murasaki
