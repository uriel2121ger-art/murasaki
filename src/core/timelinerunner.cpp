#include "timelinerunner.h"

#include "automation.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

qint64 scaled(int ms, double speed)
{
    return qint64(std::llround(double(ms) / speed));
}

} // namespace

void TimelineRunner::push(TimelineEffect effect)
{
    m_effects.append(std::move(effect));
}

void TimelineRunner::touch(Run &run, const QString &petId)
{
    if (!run.touched.contains(petId))
        run.touched.append(petId);
}

TimelineRunner::Started TimelineRunner::start(const Timeline &timeline, qint64 nowMs, const Resolver &resolve)
{
    Started result;
    if (isRunning(timeline.id))
        cancel(timeline.id);
    if (m_runs.size() >= kMaxRunningTimelines) {
        result.error = kTimelineTooManyRunning;
        return result;
    }
    Run run;
    run.timelineId = timeline.id;
    run.speed = std::clamp(timeline.speed, kMinTimelineSpeed, kMaxTimelineSpeed);
    run.restore = timeline.restore;
    run.lastMs = nowMs;
    int index = -1;
    for (const TimelineTrack &track : timeline.tracks) {
        ++index;
        const ResolvedPet pet = resolve(track.pet);
        if (pet.petId.isEmpty()) {
            result.notes.append(QString(kTimelinePetNotFound) + QStringLiteral(" (track %1)").arg(index + 1));
            continue;
        }
        TrackState state;
        state.petId = pet.petId;
        state.ephemeral = pet.ephemeral;
        state.actions = track.actions;
        state.dueMs = nowMs;
        TimelineEffect e;
        e.kind = TimelineEffect::Kind::SetHidden;
        e.petId = pet.petId;
        e.atMs = nowMs;
        e.hidden = !track.startVisible;
        e.timelineId = timeline.id;
        push(e);
        touch(run, pet.petId);
        run.tracks.append(state);
    }
    result.ok = true;
    if (run.tracks.isEmpty())
        return result; // nothing to drive: nothing runs
    m_runs.append(run);
    advance(nowMs); // what is due at once (a track without waits ends here)
    return result;
}

void TimelineRunner::finish(int index, qint64 nowMs, bool restore)
{
    const Run run = m_runs.takeAt(index);
    if (restore && run.restore) {
        for (const QString &petId : run.touched) {
            TimelineEffect e;
            e.kind = TimelineEffect::Kind::Restore;
            e.petId = petId;
            e.atMs = nowMs;
            e.timelineId = run.timelineId;
            push(e);
        }
    }
    for (const TrackState &track : run.tracks) {
        if (!track.ephemeral)
            continue;
        TimelineEffect e;
        e.kind = TimelineEffect::Kind::Release;
        e.petId = track.petId;
        e.atMs = nowMs;
        e.timelineId = run.timelineId;
        push(e);
    }
}

void TimelineRunner::advance(qint64 nowMs)
{
    for (;;) {
        // The earliest due action of all the runs (ties: the run that started first, the track listed first).
        int bestRun = -1, bestTrack = -1;
        qint64 best = 0;
        for (int r = 0; r < m_runs.size(); ++r) {
            for (int t = 0; t < m_runs[r].tracks.size(); ++t) {
                const TrackState &track = m_runs[r].tracks[t];
                if (track.done || track.dueMs > nowMs)
                    continue;
                if (bestRun < 0 || track.dueMs < best) {
                    bestRun = r;
                    bestTrack = t;
                    best = track.dueMs;
                }
            }
        }
        if (bestRun < 0)
            break;
        Run &run = m_runs[bestRun];
        TrackState &track = run.tracks[bestTrack];
        run.lastMs = std::max(run.lastMs, track.dueMs);
        if (track.index >= track.actions.size()) {
            track.done = true;
        } else {
            const TimelineAction &a = track.actions.at(track.index++);
            TimelineEffect e;
            e.petId = track.petId;
            e.atMs = track.dueMs;
            e.timelineId = run.timelineId;
            qint64 delay = 0;
            switch (a.kind) {
            case ActionKind::Place:
                e.kind = TimelineEffect::Kind::Place;
                e.x = a.x;
                e.y = a.y;
                e.output = a.output;
                e.centre = a.centre;
                touch(run, track.petId);
                push(e);
                break;
            case ActionKind::Show:
                e.kind = TimelineEffect::Kind::SetHidden;
                e.hidden = false;
                push(e);
                if (!a.variant.isEmpty()) {
                    e.kind = TimelineEffect::Kind::SetVariant;
                    e.variant = a.variant;
                    push(e);
                }
                touch(run, track.petId);
                break;
            case ActionKind::Hide:
                e.kind = TimelineEffect::Kind::SetHidden;
                e.hidden = true;
                touch(run, track.petId);
                push(e);
                break;
            case ActionKind::Move:
                e.x = a.x;
                e.y = a.y;
                e.output = a.output;
                e.centre = a.centre;
                touch(run, track.petId);
                if (a.smooth && a.durationMs > 0) {
                    e.kind = TimelineEffect::Kind::Move;
                    delay = scaled(a.durationMs, run.speed);
                    e.durationMs = int(delay);
                } else {
                    e.kind = TimelineEffect::Kind::Place;
                }
                push(e);
                break;
            case ActionKind::Sound:
                e.kind = TimelineEffect::Kind::Sound;
                push(e);
                break;
            case ActionKind::StopSounds:
                e.kind = TimelineEffect::Kind::StopSounds;
                push(e);
                break;
            case ActionKind::Track:
                e.kind = TimelineEffect::Kind::SetTracking;
                e.tracking = a.tracking;
                touch(run, track.petId);
                push(e);
                break;
            case ActionKind::Wait:
                delay = scaled(a.waitMs, run.speed);
                break;
            }
            track.dueMs += delay;
            if (track.index >= track.actions.size() && delay == 0)
                track.done = true;
        }
        // A run ends when every track has finished all its actions and its last delay has passed.
        for (int r = 0; r < m_runs.size();) {
            bool all = true;
            qint64 end = 0;
            for (const TrackState &t : m_runs[r].tracks) {
                all = all && (t.done || (t.index >= t.actions.size() && t.dueMs <= nowMs));
                end = std::max(end, t.dueMs);
            }
            if (all)
                finish(r, std::max(end, m_runs[r].lastMs), true);
            else
                ++r;
        }
    }
}

qint64 TimelineRunner::nextDueMs() const
{
    qint64 next = -1;
    for (const Run &run : m_runs)
        for (const TrackState &t : run.tracks)
            if (!t.done && (next < 0 || t.dueMs < next))
                next = t.dueMs;
    return next;
}

bool TimelineRunner::cancel(const QString &timelineId)
{
    for (int r = 0; r < m_runs.size(); ++r) {
        if (m_runs[r].timelineId != timelineId)
            continue;
        finish(r, m_runs[r].lastMs, true);
        return true;
    }
    return false;
}

void TimelineRunner::cancelAll()
{
    while (!m_runs.isEmpty())
        finish(0, m_runs[0].lastMs, true);
}

void TimelineRunner::petRemoved(const QString &petId)
{
    for (int r = 0; r < m_runs.size();) {
        Run &run = m_runs[r];
        for (TrackState &t : run.tracks)
            if (t.petId == petId) {
                t.done = true;
                t.ephemeral = false; // (the pet is gone, there is nothing to release)
            }
        run.touched.removeAll(petId);
        bool all = true;
        for (const TrackState &t : run.tracks)
            all = all && t.done;
        if (all)
            finish(r, run.lastMs, true);
        else
            ++r;
    }
}

QList<TimelineEffect> TimelineRunner::takeEffects()
{
    return std::exchange(m_effects, {});
}

bool TimelineRunner::isRunning(const QString &timelineId) const
{
    return std::any_of(m_runs.begin(), m_runs.end(), [&](const Run &r) { return r.timelineId == timelineId; });
}

QStringList TimelineRunner::runningIds() const
{
    QStringList ids;
    for (const Run &r : m_runs)
        ids.append(r.timelineId);
    return ids;
}

} // namespace murasaki
