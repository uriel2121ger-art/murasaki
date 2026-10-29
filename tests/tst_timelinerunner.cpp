// The timeline runner with a fake clock: each kind of action, parallel tracks, speed, restore, cancel, the cap of running timelines, and a property run that checks that a
// finished run leaves nothing hidden or moved behind it.

#include "automation.h"
#include "petruntime.h"
#include "timelinerunner.h"

#include <QRandomGenerator>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using Kind = TimelineEffect::Kind;

namespace {

TimelineAction act(ActionKind kind, double x = 0, double y = 0, int ms = 0, bool smooth = false, const QString &variant = {})
{
    TimelineAction a;
    a.kind = kind;
    a.x = x;
    a.y = y;
    a.waitMs = kind == ActionKind::Wait ? ms : 0;
    a.durationMs = kind == ActionKind::Move ? ms : 0;
    a.smooth = smooth;
    a.variant = variant;
    return a;
}

Timeline timeline(const QString &id, QList<QList<TimelineAction>> tracks, double speed = 1, bool restore = true)
{
    Timeline t;
    t.id = id;
    t.name = id;
    t.speed = speed;
    t.restore = restore;
    for (int i = 0; i < tracks.size(); ++i) {
        TimelineTrack track;
        track.pet.petId = u"pet%1"_s.arg(i);
        track.actions = tracks[i];
        t.tracks.append(track);
    }
    return t;
}

const TimelineRunner::Resolver known = [](const PetRef &ref) { return ResolvedPet{ref.petId, false}; };

QList<Kind> kinds(const QList<TimelineEffect> &effects)
{
    QList<Kind> k;
    for (const TimelineEffect &e : effects)
        k.append(e.kind);
    return k;
}

}

class TstTimelineRunner : public QObject
{
    Q_OBJECT

private slots:
    void everyActionKindHasItsEffectAtItsTime()
    {
        TimelineRunner runner;
        const auto t = timeline(u"t"_s, {{act(ActionKind::Place, 5, 6), act(ActionKind::Wait, 0, 0, 100), act(ActionKind::Show, 0, 0, 0, false, u"wave"_s), act(ActionKind::Sound),
                                          act(ActionKind::Move, 9, 8, 200, true), act(ActionKind::StopSounds), act(ActionKind::Hide)}});
        const auto started = runner.start(t, 1000, known);
        QVERIFY(started.ok);
        QList<TimelineEffect> all = runner.takeEffects();
        QCOMPARE(kinds(all), (QList<Kind>{Kind::SetHidden, Kind::Place})); // the start, the place; then the wait holds the track
        QCOMPARE(runner.nextDueMs(), qint64(1100));
        runner.advance(1099);
        QVERIFY(runner.takeEffects().isEmpty());
        runner.advance(1100);
        all = runner.takeEffects();
        QCOMPARE(kinds(all), (QList<Kind>{Kind::SetHidden, Kind::SetVariant, Kind::Sound, Kind::Move})); // the smooth move holds the track for 200 ms
        QCOMPARE(all[3].durationMs, 200);
        QCOMPARE(all[3].atMs, qint64(1100));
        QCOMPARE(all[1].variant, u"wave"_s);
        QCOMPARE(runner.nextDueMs(), qint64(1300));
        runner.advance(1300);
        all = runner.takeEffects();
        QCOMPARE(kinds(all), (QList<Kind>{Kind::StopSounds, Kind::SetHidden, Kind::Restore})); // the end restores the pet
        QCOMPARE(runner.running(), 0);
        QCOMPARE(runner.nextDueMs(), qint64(-1));
    }
    void aTrackActionSetsTheTrackingOfTheLayerAndTheEndRestoresIt()
    {
        TimelineRunner runner;
        TimelineAction track = act(ActionKind::Track);
        track.tracking = PetTrackingMode::Follow;
        runner.start(timeline(u"t"_s, {{track, act(ActionKind::Wait, 0, 0, 100)}}), 0, known);
        QList<TimelineEffect> all = runner.takeEffects();
        QCOMPARE(kinds(all), (QList<Kind>{Kind::SetHidden, Kind::SetTracking}));
        QCOMPARE(all[1].tracking, PetTrackingMode::Follow);
        runner.advance(100);
        QCOMPARE(kinds(runner.takeEffects()), (QList<Kind>{Kind::Restore})); // the pet was touched: the stored mode comes back
    }
    void aLateTickStillGivesTheRightTimes()
    {
        TimelineRunner runner;
        runner.start(timeline(u"t"_s, {{act(ActionKind::Hide), act(ActionKind::Wait, 0, 0, 100), act(ActionKind::Show), act(ActionKind::Wait, 0, 0, 100), act(ActionKind::Hide)}}), 0, known);
        runner.takeEffects();
        runner.advance(10'000); // the machine slept
        const auto effects = runner.takeEffects();
        QCOMPARE(effects.size(), 3);
        QCOMPARE(effects[0].atMs, qint64(100)); // each at the time it was due, not when it was noticed
        QCOMPARE(effects[1].atMs, qint64(200));
    }
    void tracksRunInParallelInTimeOrder()
    {
        TimelineRunner runner;
        const auto t = timeline(u"t"_s, {{act(ActionKind::Wait, 0, 0, 300), act(ActionKind::Place, 1, 1)}, {act(ActionKind::Wait, 0, 0, 100), act(ActionKind::Place, 2, 2)}});
        runner.start(t, 0, known);
        runner.takeEffects();
        runner.advance(1000);
        const auto effects = runner.takeEffects();
        QCOMPARE(effects[0].petId, u"pet1"_s); // the shorter wait comes first
        QCOMPARE(effects[0].atMs, qint64(100));
        QCOMPARE(effects[1].petId, u"pet0"_s);
        QCOMPARE(effects[1].atMs, qint64(300));
    }
    void speedDividesWaitsAndMoves()
    {
        for (const auto &[speed, wait, move] : {std::tuple{4.0, 25, 50}, std::tuple{0.25, 400, 800}, std::tuple{1.0, 100, 200}}) {
            TimelineRunner runner;
            runner.start(timeline(u"t"_s, {{act(ActionKind::Wait, 0, 0, 100), act(ActionKind::Move, 1, 1, 200, true), act(ActionKind::Hide)}}, speed), 0, known);
            runner.takeEffects();
            QCOMPARE(runner.nextDueMs(), qint64(wait));
            runner.advance(wait);
            const auto effects = runner.takeEffects();
            QCOMPARE(effects[0].durationMs, move);
            QCOMPARE(runner.nextDueMs(), qint64(wait + move));
        }
    }
    void aTrackThatStartsHiddenIsHiddenAtOnce()
    {
        TimelineRunner runner;
        Timeline t = timeline(u"t"_s, {{act(ActionKind::Wait, 0, 0, 50)}});
        t.tracks[0].startVisible = false;
        runner.start(t, 0, known);
        const auto effects = runner.takeEffects();
        QVERIFY(effects[0].hidden);
    }
    void restoreIsAChoice()
    {
        for (const bool restore : {true, false}) {
            TimelineRunner runner;
            runner.start(timeline(u"t"_s, {{act(ActionKind::Hide)}}, 1, restore), 0, known);
            const auto effects = runner.takeEffects();
            QCOMPARE(effects.last().kind == Kind::Restore, restore);
            QCOMPARE(runner.running(), 0);
        }
    }
    void cancelRestoresWhatWasTouchedAndOnlyThat()
    {
        TimelineRunner runner;
        runner.start(timeline(u"t"_s, {{act(ActionKind::Hide), act(ActionKind::Wait, 0, 0, 1000)}, {act(ActionKind::Sound), act(ActionKind::Wait, 0, 0, 1000)}}), 0, known);
        runner.takeEffects();
        QVERIFY(runner.isRunning(u"t"_s));
        QVERIFY(runner.cancel(u"t"_s));
        QVERIFY(!runner.cancel(u"t"_s));
        QCOMPARE(runner.running(), 0);
        const auto effects = runner.takeEffects();
        QSet<QString> restored;
        for (const TimelineEffect &e : effects)
            if (e.kind == Kind::Restore)
                restored.insert(e.petId);
        QVERIFY(restored.contains(u"pet0"_s));
        QCOMPARE(runner.nextDueMs(), qint64(-1));
    }
    void aRemovedPetEndsItsTrackWithoutRestore()
    {
        TimelineRunner runner;
        runner.start(timeline(u"t"_s, {{act(ActionKind::Hide), act(ActionKind::Wait, 0, 0, 1000)}, {act(ActionKind::Wait, 0, 0, 500)}}), 0, known);
        runner.takeEffects();
        runner.petRemoved(u"pet0"_s);
        runner.advance(2000);
        for (const TimelineEffect &e : runner.takeEffects())
            QVERIFY(!(e.kind == Kind::Restore && e.petId == u"pet0"_s));
        QCOMPARE(runner.running(), 0);
        // when it was the only track the run ends at once
        runner.start(timeline(u"u"_s, {{act(ActionKind::Wait, 0, 0, 500)}}), 0, known);
        runner.takeEffects();
        runner.petRemoved(u"pet0"_s);
        QCOMPARE(runner.running(), 0);
    }
    void atMostEightRunAndTheSameOneRestarts()
    {
        TimelineRunner runner;
        for (int i = 0; i < kMaxRunningTimelines; ++i)
            QVERIFY(runner.start(timeline(u"t%1"_s.arg(i), {{act(ActionKind::Wait, 0, 0, 1000)}}), 0, known).ok);
        const auto refused = runner.start(timeline(u"nine"_s, {{act(ActionKind::Wait, 0, 0, 1000)}}), 0, known);
        QVERIFY(!refused.ok);
        QCOMPARE(refused.error, QString(kTimelineTooManyRunning));
        QCOMPARE(runner.running(), kMaxRunningTimelines);
        // the same timeline again is a restart, not a ninth
        runner.takeEffects();
        QVERIFY(runner.start(timeline(u"t3"_s, {{act(ActionKind::Hide), act(ActionKind::Wait, 0, 0, 1000)}}), 50, known).ok);
        QCOMPARE(runner.running(), kMaxRunningTimelines);
        const auto effects = runner.takeEffects();
        QVERIFY(kinds(effects).contains(Kind::Restore)); // the old run was undone first
        QVERIFY(kinds(effects).indexOf(Kind::Restore) < kinds(effects).lastIndexOf(Kind::SetHidden));
        runner.cancelAll();
        QCOMPARE(runner.running(), 0);
    }
    void aTrackWhosePetIsNotFoundIsSkippedWithANote()
    {
        TimelineRunner runner;
        const auto resolver = [](const PetRef &ref) { return ref.petId == u"pet1" ? ResolvedPet{} : ResolvedPet{ref.petId, false}; };
        const auto started = runner.start(timeline(u"t"_s, {{act(ActionKind::Wait, 0, 0, 10)}, {act(ActionKind::Wait, 0, 0, 10)}}), 0, resolver);
        QVERIFY(started.ok);
        QCOMPARE(started.notes, QStringList{QString(kTimelinePetNotFound) + u" (track 2)"_s}); // (it says which track)
        // nothing to drive: nothing runs
        TimelineRunner none;
        QVERIFY(none.start(timeline(u"t"_s, {{act(ActionKind::Hide)}}), 0, [](const PetRef &) { return ResolvedPet{}; }).ok);
        QCOMPARE(none.running(), 0);
    }
    void aPetMadeForTheRunIsReleasedAtItsEnd()
    {
        TimelineRunner runner;
        Timeline t = timeline(u"t"_s, {{act(ActionKind::Wait, 0, 0, 10)}});
        runner.start(t, 0, [](const PetRef &) { return ResolvedPet{u"made"_s, true}; });
        runner.takeEffects();
        runner.advance(10);
        const auto kindsSeen = kinds(runner.takeEffects());
        QVERIFY(kindsSeen.contains(Kind::Release));
        // and a cancel releases it too
        runner.start(t, 0, [](const PetRef &) { return ResolvedPet{u"made"_s, true}; });
        runner.takeEffects();
        runner.cancel(u"t"_s);
        QVERIFY(kinds(runner.takeEffects()).contains(Kind::Release));
    }
    void propertyAFinishedRunLeavesNothingBehind()
    {
        QRandomGenerator rng(7);
        for (int round = 0; round < 300; ++round) {
            TimelineRunner runner;
            QHash<QString, PetRuntime> layer;
            const auto apply = [&](const QList<TimelineEffect> &effects) {
                for (const TimelineEffect &e : effects) {
                    switch (e.kind) {
                    case Kind::SetHidden:
                        layer[e.petId].hidden = e.hidden;
                        break;
                    case Kind::SetVariant:
                        layer[e.petId].variant = e.variant;
                        break;
                    case Kind::Place:
                        layer[e.petId].position = PetPosition{e.x, e.y};
                        break;
                    case Kind::Move:
                        layer[e.petId].position = PetPosition{e.x, e.y};
                        break;
                    case Kind::Restore:
                        layer.remove(e.petId);
                        break;
                    default:
                        break;
                    }
                }
            };
            QList<QList<TimelineAction>> tracks;
            for (int i = 0, n = rng.bounded(1, 4); i < n; ++i) {
                QList<TimelineAction> actions;
                for (int j = 0, m = rng.bounded(0, 10); j < m; ++j) {
                    switch (rng.bounded(7)) {
                    case 0: actions.append(act(ActionKind::Place, rng.bounded(100), rng.bounded(100))); break;
                    case 1: actions.append(act(ActionKind::Show, 0, 0, 0, false, rng.bounded(2) ? u"v"_s : QString())); break;
                    case 2: actions.append(act(ActionKind::Hide)); break;
                    case 3: actions.append(act(ActionKind::Move, 5, 5, rng.bounded(300), rng.bounded(2))); break;
                    case 4: actions.append(act(ActionKind::Sound)); break;
                    case 5: actions.append(act(ActionKind::Wait, 0, 0, rng.bounded(500))); break;
                    default: actions.append(act(ActionKind::StopSounds)); break;
                    }
                }
                tracks.append(actions);
            }
            const double speeds[] = {0.25, 1, 4};
            qint64 now = 0;
            runner.start(timeline(u"t"_s, tracks, speeds[rng.bounded(3)]), now, known);
            apply(runner.takeEffects());
            const bool cancelEarly = rng.bounded(3) == 0;
            int steps = 0;
            while (runner.running() > 0 && steps++ < 10000) {
                now = runner.nextDueMs();
                QVERIFY(now >= 0);
                runner.advance(now);
                apply(runner.takeEffects());
                if (cancelEarly && steps == 2) {
                    runner.cancel(u"t"_s);
                    apply(runner.takeEffects());
                }
            }
            QCOMPARE(runner.running(), 0);
            QVERIFY2(layer.isEmpty(), "a finished run left a pet hidden or moved");
        }
    }
};

QTEST_GUILESS_MAIN(TstTimelineRunner)
#include "tst_timelinerunner.moc"
