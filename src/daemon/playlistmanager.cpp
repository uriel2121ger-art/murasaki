#include "playlistmanager.h"

#include "jsonfile.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {
constexpr qint64 kMinuteMs = 60 * 1000;

QStringList sorted(QStringList list)
{
    list.sort();
    return list;
}
} // namespace

PlaylistManager::PlaylistManager(PlaylistStore *playlists, AssignmentStore *assignments, Library *library,
                                 QString statePath, QObject *parent)
    : QObject(parent)
    , m_playlists(playlists)
    , m_assignments(assignments)
    , m_library(library)
    , m_statePath(std::move(statePath))
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &PlaylistManager::checkDue);
    connect(m_assignments, &AssignmentStore::changed, this, &PlaylistManager::sync);
    connect(m_playlists, &PlaylistStore::changed, this, &PlaylistManager::sync);
    connect(m_library, &Library::itemsChanged, this, &PlaylistManager::sync);
}

qint64 PlaylistManager::now() const
{
    return m_clock();
}

bool PlaylistManager::playable(const QString &itemId) const
{
    const auto item = m_library->item(itemId);
    return item && item->kind != ItemKind::Pet && item->kind != ItemKind::Asset && item->status == ItemStatus::Ok;
}

void PlaylistManager::reshuffle(QStringList &bag, QString avoidFirst) // by value: it may alias an item of bag
{
    std::shuffle(bag.begin(), bag.end(), m_random);
    // The first item of a new bag is never the last one of the old bag.
    if (bag.size() > 1 && bag.first() == avoidFirst) {
        std::uniform_int_distribution<qsizetype> pick(1, bag.size() - 1);
        bag.swapItemsAt(0, pick(m_random));
    }
}

void PlaylistManager::rebuild(Target &target, const Playlist &playlist)
{
    const QString keep = target.sequence.value(target.pos);
    const int oldPos = target.pos;
    target.order = playlist.order;
    if (playlist.order == PlaylistOrder::Sequential) {
        target.sequence = playlist.items;
        const qsizetype at = playlist.items.indexOf(keep);
        target.pos = at >= 0 ? int(at) : std::clamp(oldPos, 0, std::max<int>(0, int(playlist.items.size()) - 1));
    } else {
        QStringList bag = playlist.items;
        const bool kept = bag.removeOne(keep);
        reshuffle(bag, QString());
        if (kept)
            bag.prepend(keep);
        target.sequence = bag;
        target.pos = 0;
    }
    m_dirty = true;
}

void PlaylistManager::settle(Target &target)
{
    const int n = int(target.sequence.size());
    QString shown;
    for (int i = 0; i < n; ++i) {
        const int at = (target.pos + i) % n;
        if (playable(target.sequence.at(at))) {
            if (at != target.pos) {
                target.pos = at;
                m_dirty = true;
            }
            shown = target.sequence.at(at);
            break;
        }
    }
    target.shown = shown;
}

void PlaylistManager::step(Target &target, int direction)
{
    const int n = int(target.sequence.size());
    m_dirty = true;
    for (int i = 0; i < 2 * n; ++i) {
        target.pos += direction;
        if (target.order == PlaylistOrder::Sequential) {
            target.pos = (target.pos % n + n) % n;
        } else if (target.pos >= n) {
            reshuffle(target.sequence, target.sequence.last());
            target.pos = 0;
        } else if (target.pos < 0) {
            target.pos = n - 1;
        }
        if (playable(target.sequence.at(target.pos))) {
            target.shown = target.sequence.at(target.pos);
            return;
        }
    }
    target.shown.clear();
}

void PlaylistManager::restartInterval(Target &target, const Playlist &playlist)
{
    target.nextAt = now() + qint64(playlist.intervalMinutes) * kMinuteMs;
    target.intervalMinutes = playlist.intervalMinutes;
    m_dirty = true;
}

PlaylistManager::Target *PlaylistManager::ensure(const Assignment &assignment, const Playlist &playlist)
{
    const Key key{assignment.output, assignment.desktop};
    auto it = m_targets.find(key);
    if (it != m_targets.end() && it->playlistId != playlist.id) {
        m_targets.erase(it);
        it = m_targets.end();
        m_dirty = true;
    }
    if (it == m_targets.end()) {
        Target fresh;
        fresh.playlistId = playlist.id;
        fresh.order = playlist.order;
        fresh.sequence = playlist.items;
        if (playlist.order == PlaylistOrder::Shuffle)
            reshuffle(fresh.sequence, QString());
        restartInterval(fresh, playlist);
        it = m_targets.insert(key, fresh);
        m_dirty = true;
    } else {
        const bool sameItems = it->order == PlaylistOrder::Sequential
            ? it->sequence == playlist.items
            : sorted(it->sequence) == sorted(playlist.items);
        if (it->order != playlist.order || !sameItems)
            rebuild(*it, playlist);
        // A new interval takes effect now, from this moment: a target that waits 24 h must not keep waiting after
        // the user shortened it. A saved state does not know its interval and is adopted as it is.
        if (it->intervalMinutes == 0) {
            it->intervalMinutes = playlist.intervalMinutes;
        } else if (it->intervalMinutes != playlist.intervalMinutes) {
            restartInterval(*it, playlist);
        }
    }
    settle(*it);
    return &*it;
}

PlaylistManager::Current PlaylistManager::describe(const Target &target, const Playlist &playlist) const
{
    Current current;
    current.playlistId = playlist.id;
    current.itemId = target.shown;
    current.index = target.shown.isEmpty() ? -1 : int(playlist.items.indexOf(target.shown));
    current.nextAt = target.nextAt;
    return current;
}

std::optional<PlaylistManager::Current> PlaylistManager::current(const QString &output, const QString &desktop)
{
    const auto assignment = m_assignments->resolve(output, desktop);
    if (!assignment)
        return std::nullopt;
    const QString id = playlistIdOf(assignment->itemId);
    if (id.isEmpty())
        return std::nullopt;
    const auto playlist = m_playlists->playlist(id);
    if (!playlist) {
        Current gone;
        gone.playlistId = id;
        return gone;
    }
    const Target *target = ensure(*assignment, *playlist);
    const Current result = describe(*target, *playlist);
    if (m_dirty) {
        save();
        rearm();
    }
    return result;
}

bool PlaylistManager::showsItem(const QString &itemId) const
{
    for (const Target &target : m_targets) {
        if (target.shown == itemId)
            return true;
    }
    return false;
}

bool PlaylistManager::move(const QString &output, const QString &desktop, int direction, QString *error)
{
    const auto assignment = m_assignments->resolve(output, desktop);
    const QString id = assignment ? playlistIdOf(assignment->itemId) : QString();
    const auto playlist = id.isEmpty() ? std::nullopt : m_playlists->playlist(id);
    if (!playlist) {
        if (error)
            *error = u"%1/%2 is not showing a playlist"_s.arg(output.left(200), desktop.left(200));
        return false;
    }
    Target *target = ensure(*assignment, *playlist);
    if (!target->sequence.isEmpty())
        step(*target, direction);
    restartInterval(*target, *playlist);
    save();
    rearm();
    emit rotated();
    return true;
}

bool PlaylistManager::next(const QString &output, const QString &desktop, QString *error)
{
    return move(output, desktop, +1, error);
}

bool PlaylistManager::previous(const QString &output, const QString &desktop, QString *error)
{
    return move(output, desktop, -1, error);
}

void PlaylistManager::setSessionLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    if (locked)
        m_timer.stop();
    else
        checkDue();
}

void PlaylistManager::checkDue()
{
    if (m_locked)
        return;
    const qint64 at = now();
    bool rotatedAny = false;
    for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
        if (it->nextAt > at)
            continue;
        const auto playlist = m_playlists->playlist(it->playlistId);
        if (!playlist)
            continue;
        if (!it->sequence.isEmpty())
            step(*it, +1);
        restartInterval(*it, *playlist);
        rotatedAny = true;
    }
    save();
    rearm();
    if (rotatedAny)
        emit rotated();
}

int PlaylistManager::wakeInMs() const
{
    if (m_targets.isEmpty())
        return -1;
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (const Target &target : m_targets)
        earliest = std::min(earliest, target.nextAt);
    return int(std::clamp<qint64>(earliest - now(), 0, std::numeric_limits<int>::max()));
}

void PlaylistManager::rearm()
{
    const int wake = wakeInMs();
    if (wake < 0 || m_locked)
        m_timer.stop();
    else
        m_timer.start(wake);
}

void PlaylistManager::sync()
{
    QHash<Key, QString> before;
    for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it)
        before.insert(it.key(), it->shown);

    QSet<Key> active;
    for (const Assignment &assignment : m_assignments->assignments()) {
        const QString id = playlistIdOf(assignment.itemId);
        if (id.isEmpty())
            continue;
        const auto playlist = m_playlists->playlist(id);
        if (!playlist)
            continue;
        active.insert({assignment.output, assignment.desktop});
        ensure(assignment, *playlist);
    }
    for (auto it = m_targets.begin(); it != m_targets.end();) {
        if (active.contains(it.key())) {
            ++it;
        } else {
            it = m_targets.erase(it);
            m_dirty = true;
        }
    }

    bool shownChanged = false;
    for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it) {
        if (before.contains(it.key()) && before.value(it.key()) != it->shown)
            shownChanged = true;
    }
    save();
    rearm();
    if (shownChanged)
        emit rotated();
}

void PlaylistManager::save()
{
    if (!m_dirty)
        return;
    m_dirty = false;
    if (m_saveBlocked) {
        qWarning("murasakid: not saving playlist positions over the unreadable %s", qPrintable(m_statePath));
        return;
    }
    QJsonArray targets;
    for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it) {
        targets.append(QJsonObject{{u"output"_s, it.key().first},
                                   {u"desktop"_s, it.key().second},
                                   {u"playlistId"_s, it->playlistId},
                                   {u"order"_s, playlistOrderToString(it->order)},
                                   {u"sequence"_s, QJsonArray::fromStringList(it->sequence)},
                                   {u"pos"_s, it->pos},
                                   {u"nextAt"_s, double(it->nextAt)}});
    }
    writeJsonObjectFile(m_statePath, QJsonObject{{u"version"_s, 1}, {u"targets"_s, targets}});
}

void PlaylistManager::readState()
{
    m_targets.clear();
    const JsonFileRead read = readJsonObjectFile(m_statePath);
    if (!read.exists)
        return;
    QString problem = read.problem;
    QMap<Key, Target> parsed;
    if (problem.isEmpty()) {
        if (read.root.value(u"version"_s).toInt() != 1 || !read.root.value(u"targets"_s).isArray()) {
            problem = u"unsupported format"_s;
        } else {
            for (const QJsonValue &value : read.root.value(u"targets"_s).toArray()) {
                const QJsonObject entry = value.toObject();
                Target target;
                target.playlistId = entry.value(u"playlistId"_s).toString();
                const QString order = entry.value(u"order"_s).toString();
                target.order = order == u"shuffle" ? PlaylistOrder::Shuffle : PlaylistOrder::Sequential;
                const QJsonValue sequence = entry.value(u"sequence"_s);
                bool ok = sequence.isArray() && entry.value(u"pos"_s).isDouble() && entry.value(u"nextAt"_s).isDouble()
                    && !target.playlistId.isEmpty() && (order == u"shuffle" || order == u"sequential");
                if (ok) {
                    for (const QJsonValue &item : sequence.toArray()) {
                        ok = ok && item.isString();
                        target.sequence.append(item.toString());
                    }
                }
                target.pos = entry.value(u"pos"_s).toInt(-1);
                target.nextAt = qint64(entry.value(u"nextAt"_s).toDouble());
                if (!ok || target.pos < 0 || (target.pos >= target.sequence.size() && target.pos != 0)
                    || !isValidKeyPart(entry.value(u"output"_s).toString())
                    || !isValidKeyPart(entry.value(u"desktop"_s).toString())) {
                    problem = u"invalid entry"_s;
                    break;
                }
                parsed.insert({entry.value(u"output"_s).toString(), entry.value(u"desktop"_s).toString()}, target);
            }
        }
    }
    if (problem.isEmpty()) {
        m_targets = parsed;
        return;
    }
    if (!quarantineBadFile(m_statePath, problem, u"no saved positions"_s, u"playlist positions"_s))
        m_saveBlocked = true;
}

void PlaylistManager::load()
{
    m_saveBlocked = false;
    readState();
    const qint64 at = now();
    QHash<Key, bool> restored;
    for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it)
        restored.insert(it.key(), true);

    QSet<Key> active;
    for (const Assignment &assignment : m_assignments->assignments()) {
        const QString id = playlistIdOf(assignment.itemId);
        const auto playlist = id.isEmpty() ? std::nullopt : m_playlists->playlist(id);
        if (!playlist)
            continue;
        const Key key{assignment.output, assignment.desktop};
        const bool wasSaved = restored.value(key) && m_targets.value(key).playlistId == id;
        Target *target = ensure(assignment, *playlist);
        active.insert(key);
        if (!wasSaved)
            continue;
        if (playlist->changeOnLogin && !target->sequence.isEmpty())
            step(*target, +1);
        if (playlist->changeOnLogin || target->nextAt <= at)
            restartInterval(*target, *playlist);
    }
    for (auto it = m_targets.begin(); it != m_targets.end();) {
        if (active.contains(it.key())) {
            ++it;
        } else {
            it = m_targets.erase(it);
            m_dirty = true;
        }
    }
    save();
    rearm();
}

} // namespace murasaki
