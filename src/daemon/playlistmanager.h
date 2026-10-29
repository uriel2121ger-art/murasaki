#pragma once

#include "assignment.h"
#include "library.h"
#include "playlist.h"

#include <QMap>
#include <QObject>
#include <QTimer>

#include <functional>
#include <optional>
#include <random>
#include <utility>

namespace murasaki {

// Plays playlists: for every assignment of "playlist:<id>" (a target, keyed like the assignment: output and
// desktop, wildcards included) it knows which item of the playlist is showing and when the next one is due. The
// position per target survives restarts ($XDG_STATE_HOME/murasaki/playlist-state.json).
//
// One timer sleeps until the earliest deadline of any target. A deadline that passed while the session was locked
// (or the machine asleep) rotates once, not once per elapsed interval.
class PlaylistManager : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>; // milliseconds since the epoch

    struct Current {
        QString playlistId;
        QString itemId; // empty when no item of the playlist can be shown
        int index = -1; // position of itemId in the playlist's list, -1 when none
        qint64 nextAt = 0;
    };

    PlaylistManager(PlaylistStore *playlists, AssignmentStore *assignments, Library *library, QString statePath,
                    QObject *parent = nullptr);

    // Tests drive the manager with a fake clock and a fixed shuffle seed.
    void setClock(Clock clock) { m_clock = std::move(clock); }
    void setSeed(quint32 seed) { m_random.seed(seed); }

    // Reads the saved positions and reconciles them with the assignments. Call once, after the stores are loaded.
    // A target that was saved gets a new deadline instead of a catch-up rotation; when its playlist asks for
    // changeOnLogin it advances once. A new playlist assignment starts at the first item.
    void load();

    // What the assignment that serves (output, desktop) plays, or nothing when it is not a playlist.
    std::optional<Current> current(const QString &output, const QString &desktop);
    // Whether the item is what some target shows right now.
    bool showsItem(const QString &itemId) const;

    // Move the target that serves (output, desktop) and restart its interval. False with *error when that is not
    // a playlist assignment.
    bool next(const QString &output, const QString &desktop, QString *error = nullptr);
    bool previous(const QString &output, const QString &desktop, QString *error = nullptr);

    // While locked nothing rotates; unlocking runs the rotations that came due.
    void setSessionLocked(bool locked);

    // Rotates every target whose deadline passed (once each), unless the session is locked.
    void checkDue();

    // Milliseconds until the earliest deadline, -1 when no target exists.
    int wakeInMs() const;

signals:
    // The item some target shows changed (rotation, next/previous, an edit or a library change).
    void rotated();

private:
    struct Target {
        QString playlistId;
        PlaylistOrder order = PlaylistOrder::Sequential;
        QStringList sequence; // the play order: the playlist's list, or the current shuffled bag
        int pos = 0;
        qint64 nextAt = 0;
        int intervalMinutes = 0; // the playlist's interval this deadline was set with; 0 for a saved state (unknown)
        QString shown; // what the target showed when last settled (not saved)
    };
    using Key = std::pair<QString, QString>;

    qint64 now() const;
    bool playable(const QString &itemId) const;
    Target *ensure(const Assignment &assignment, const Playlist &playlist);
    void rebuild(Target &target, const Playlist &playlist);
    void reshuffle(QStringList &bag, QString avoidFirst);
    void settle(Target &target);
    void step(Target &target, int direction);
    void restartInterval(Target &target, const Playlist &playlist);
    bool move(const QString &output, const QString &desktop, int direction, QString *error);
    void sync();
    void rearm();
    void save();
    void readState();
    Current describe(const Target &target, const Playlist &playlist) const;

    PlaylistStore *m_playlists;
    AssignmentStore *m_assignments;
    Library *m_library;
    QString m_statePath;
    Clock m_clock;
    std::mt19937 m_random{std::random_device{}()};
    QMap<Key, Target> m_targets;
    QTimer m_timer;
    bool m_locked = false;
    bool m_dirty = false;
    bool m_saveBlocked = false;
};

} // namespace murasaki
