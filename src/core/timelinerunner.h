#pragma once

// Runs timelines (docs/pets/automation.md, "Timelines"). Pure: the clock is a parameter, and what a run does to the pets comes out as a list of effects that the daemon
// applies to the runtime layer. Tracks run in parallel, the actions of a track in order; `speed` divides every wait and move; a smooth move holds its track until it ends.

#include "timeline.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki {

struct TimelineEffect {
    enum class Kind {
        SetHidden,   // hidden
        SetVariant,  // variant (empty: the stored one)
        Place,       // x, y, output: the position override (a teleport)
        Move,        // x, y, durationMs: a linear motion from where the pet is at atMs
        Sound,       // the pet's own sound once
        StopSounds,  // the sounds of the pet
        SetTracking, // tracking: the mouse tracking mode of the layer
        Restore,     // the pet goes back to its stored state (the runtime entry is cleared)
        Release,     // a pet made for the run goes away
    };

    Kind kind = Kind::SetHidden;
    QString petId;
    qint64 atMs = 0; // the time the action was due (the clock of advance())
    bool hidden = false;
    double x = 0;
    double y = 0;
    QString output;
    bool centre = false; // Place, Move: x and y name the centre of the pet
    QString variant;
    PetTrackingMode tracking = PetTrackingMode::Off;
    int durationMs = 0;
    QString timelineId;

    bool operator==(const TimelineEffect &other) const = default;
};

// The pet a track names, found (or made for the run) by the daemon.
struct ResolvedPet {
    QString petId; // empty: not found (the track is skipped)
    bool ephemeral = false; // made for the run: released at its end
};

class TimelineRunner
{
public:
    using Resolver = std::function<ResolvedPet(const PetRef &)>;

    struct Started {
        bool ok = false;
        QString error;      // kTimelineTooManyRunning
        QStringList notes;  // one kTimelinePetNotFound per track that was skipped
    };

    // Starts the timeline at nowMs. Running the same timeline again restarts it: the old run is cancelled first (and restored if it restores).
    Started start(const Timeline &timeline, qint64 nowMs, const Resolver &resolve);
    // Does what is due at nowMs. Call again at nextDueMs().
    void advance(qint64 nowMs);
    // The next time something is due, or -1.
    qint64 nextDueMs() const;
    // Stops the run of the timeline (its effects up to now stand; if it restores, the touched pets go back). False when it is not running.
    bool cancel(const QString &timelineId);
    void cancelAll();
    // The pet is gone: its tracks end without restoring it.
    void petRemoved(const QString &petId);

    QList<TimelineEffect> takeEffects();
    int running() const { return int(m_runs.size()); }
    bool isRunning(const QString &timelineId) const;
    QStringList runningIds() const;

private:
    struct TrackState {
        QString petId;
        bool ephemeral = false;
        QList<TimelineAction> actions;
        int index = 0;
        qint64 dueMs = 0;
        bool done = false;
    };
    struct Run {
        QString timelineId;
        double speed = 1;
        bool restore = true;
        QList<TrackState> tracks;
        QStringList touched;
        qint64 lastMs = 0;
    };

    void finish(int index, qint64 nowMs, bool restore);
    void touch(Run &run, const QString &petId);
    void push(TimelineEffect effect);

    QList<Run> m_runs;
    QList<TimelineEffect> m_effects;
};

} // namespace murasaki
