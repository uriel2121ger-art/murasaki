// What the daemon tells the helper (src/screenmatch/plan.*): the settings of a screen trigger as the store keeps them, the regions mapped into the pixels of the monitor streams, the pets, and the
// list of commands. Pure: no bus, no process.

#include "screenmatch/plan.h"

#include <QJsonArray>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace murasaki::screenmatch;

namespace {

Trigger screenTrigger(const QString &id, const QJsonObject &screen)
{
    Trigger t;
    t.id = id;
    t.name = u"t"_s;
    t.source.kind = TriggerKind::Screen;
    t.source.screen = screen;
    return t;
}

QJsonObject region(double x, double y, double w, double h)
{
    return {{u"kind"_s, u"rect"_s}, {u"x"_s, x}, {u"y"_s, y}, {u"w"_s, w}, {u"h"_s, h}};
}

const QByteArray kGrid(kGridBytes, '\x40');

PlanInput input(const QList<Trigger> &triggers, const QList<StreamGeometry> &streams)
{
    PlanInput in;
    in.triggers = triggers;
    in.streams = streams;
    in.grid = [](const QString &, int, int) { return kGrid; };
    in.steps = [](const QString &, int) { return QList<int>{0}; };
    return in;
}

} // namespace

class TstScreenMatchPlan : public QObject
{
    Q_OBJECT

private slots:
    void aPackTriggerGivesItsSettingsWithAnimasDefaults()
    {
        const TriggerSpec spec = specOf(screenTrigger(u"abc"_s, {{u"regions"_s, QJsonArray{region(10, 20, 30, 40)}}}));
        QCOMPARE(spec.mode, ScreenMatchMode::Hash); // no mode: ORB_HASH, the average hash
        QCOMPARE(spec.match.agg, ScreenAgg::Any);
        QCOMPARE(spec.match.startFrames, 3);
        QCOMPARE(spec.match.graceMs, qint64(250));
        QCOMPARE(spec.match.cooldownMs, qint64(500));
        QVERIFY(spec.match.stopOnMismatch);
        QVERIFY(!spec.match.sequencer);
        QCOMPARE(spec.regionCount, 1);
        QCOMPARE(spec.regions.at(0).w, 30.0);
        QVERIFY(spec.notes.isEmpty());
    }
    void featureModesAreMappedToStructureWithANote()
    {
        const TriggerSpec sift = specOf(screenTrigger(u"abc"_s, {{u"match"_s, QJsonObject{{u"mode"_s, u"SIFT_LIB"_s}, {u"threshold"_s, 0.9}, {u"agg"_s, u"min"_s}}}}));
        QCOMPARE(sift.mode, ScreenMatchMode::Structure);
        QCOMPARE(sift.match.threshold, 0.9);
        QCOMPARE(sift.match.agg, ScreenAgg::All);
        QVERIFY(sift.notes.at(0).contains(u"matched by structure, not SIFT"_s));
        const TriggerSpec own = specOf(screenTrigger(u"abc"_s, {{u"match"_s, QJsonObject{{u"mode"_s, u"structure"_s}}}}));
        QCOMPARE(own.mode, ScreenMatchMode::Structure);
        QVERIFY(own.notes.isEmpty()); // our own name: nothing to say
    }
    void theSequencerAndTheNumbersAreClamped()
    {
        const TriggerSpec spec = specOf(screenTrigger(u"abc"_s, {{u"match"_s, QJsonObject{{u"refMode"_s, u"sequencer"_s}, {u"seqLen"_s, 500}, {u"threshold"_s, 7}, {u"startFramesToCheck"_s, 0},
                                                                                           {u"stopGraceMs"_s, -5}, {u"cooldownMs"_s, 1e12}, {u"stopOnMismatch"_s, false}, {u"cooldownFrom"_s, u"deactivate"_s}}}}));
        QVERIFY(spec.match.sequencer);
        QCOMPARE(spec.match.seqLen, 64);
        QCOMPARE(spec.match.threshold, 1.0);
        QCOMPARE(spec.match.startFrames, 1);
        QCOMPARE(spec.match.graceMs, qint64(0));
        QCOMPARE(spec.match.cooldownMs, kScreenMaxDurationMs);
        QVERIFY(!spec.match.stopOnMismatch);
        QCOMPARE(spec.match.cooldownFrom, ScreenCooldownFrom::Deactivate);
    }
    void hostileRegionsNeverBreakAnything()
    {
        QJsonArray many;
        for (int i = 0; i < 20; ++i)
            many.append(region(i, i, 10, 10));
        const TriggerSpec spec = specOf(screenTrigger(u"abc"_s, {{u"regions"_s, many}}));
        QCOMPARE(spec.regionCount, kMaxRegionsPerTrigger);
        QVERIFY(spec.notes.at(0).contains(u"more than 8 regions"_s));
        const TriggerSpec odd = specOf(screenTrigger(u"abc"_s, {{u"regions"_s, QJsonArray{QJsonValue(5), QJsonObject{{u"x"_s, u"a"_s}, {u"w"_s, -4}}, region(1e30, 0, 1e30, 10)}}}));
        QCOMPARE(odd.regionCount, 3);
        QCOMPARE(odd.regions.at(1).w, 0.0);
        QVERIFY(odd.regions.at(2).x <= kMaxCoordinate);
    }
    void aRegionIsMappedToTheStreamUnderItsCentreInThatStreamsPixels()
    {
        const QList<StreamGeometry> streams{{41, 0, 0, 1920, 1080, 1.0}, {42, 1920, -200, 2560, 1440, 2.0}};
        const Mapped first = mapRectangle(100, 200, 300, 120, streams);
        QVERIFY(first.onScreen);
        QCOMPARE(first.stream, 0);
        QCOMPARE(QList<int>({first.x, first.y, first.w, first.h}), (QList<int>{100, 200, 300, 120}));
        // on the second monitor, which is at 2x: logical (2000, 0) is 80 px in and 200 px down, twice that in pixels
        const Mapped second = mapRectangle(2000, 0, 50, 25, streams);
        QVERIFY(second.onScreen);
        QCOMPARE(second.stream, 1);
        QCOMPARE(QList<int>({second.x, second.y, second.w, second.h}), (QList<int>{160, 400, 100, 50}));
        // the stream under the CENTRE: a region that starts on the first and mostly lies on the second
        const Mapped straddle = mapRectangle(1900, 100, 400, 100, streams);
        QCOMPARE(straddle.stream, 1);
        QVERIFY(straddle.x == 0);                    // held inside the stream
        QCOMPARE(straddle.w, int(std::ceil((1900 + 400 - 1920) * 2.0)));
    }
    void aRegionOffTheScreensOrTooSmallIsNotOnScreen()
    {
        const QList<StreamGeometry> streams{{41, 0, 0, 1920, 1080, 1.0}};
        QVERIFY(!mapRectangle(5000, 5000, 100, 100, streams).onScreen);
        QVERIFY(!mapRectangle(-500, 10, 100, 100, streams).onScreen);
        QVERIFY(!mapRectangle(10, 10, 1, 1, streams).onScreen);       // under 2 pixels
        QVERIFY(!mapRectangle(10, 10, 0, 50, streams).onScreen);
        QVERIFY(!mapRectangle(10, 10, 50, -5, streams).onScreen);
        QVERIFY(!mapRectangle(std::nan(""), 10, 50, 50, streams).onScreen);
        QVERIFY(!mapRectangle(10, 10, 50, 50, {}).onScreen);
        QVERIFY(!mapRectangle(10, 10, 50, 50, {{1, 0, 0, 0, 0, 1.0}}).onScreen);
        QVERIFY(!mapRectangle(10, 10, 50, 50, {{1, 0, 0, 100, 100, 0.0}}).onScreen); // a scale of 0 is nonsense
        // the edge: the last pixel is on the stream
        QVERIFY(mapRectangle(1918, 1078, 2, 2, streams).onScreen);
        QVERIFY(!mapRectangle(1920, 10, 50, 50, streams).onScreen);
    }
    void aStreamWhoseScaleIsNotKnownIsNamedNotMapped()
    {
        const QList<StreamGeometry> streams{{41, 0, 0, 1920, 1080, 1.0, true}, {42, 1920, 0, 1920, 1080, 1.0, false}};
        QVERIFY(mapRectangle(10, 10, 50, 50, streams).onScreen);
        const Mapped unknown = mapRectangle(2000, 10, 50, 50, streams);
        QVERIFY(!unknown.onScreen);
        QVERIFY(!unknown.scaleKnown);
        QVERIFY(mapPets({{2000, 10, 50, 50}}, streams).isEmpty());
        const Trigger t = screenTrigger(u"abc"_s, {{u"regions"_s, QJsonArray{region(2000, 10, 50, 50)}}});
        const Plan plan = makePlan(input({t}, streams));
        QCOMPARE(plan.triggers.at(0).regions.at(0).problem, QString(kScaleUnknown));
        QCOMPARE(plan.declaredRegions, 0);
    }
    void petsAreMappedAndBounded()
    {
        const QList<StreamGeometry> streams{{41, 0, 0, 1920, 1080, 1.0}};
        QList<LogicalRect> pets{{10, 10, 100, 100}, {9000, 9000, 10, 10}, {1700, 900, 300, 300}};
        const QList<PetRect> mapped = mapPets(pets, streams);
        QCOMPARE(mapped.size(), 2); // the one off the screens is dropped
        QCOMPARE(mapped.at(0).w, 100);
        QList<LogicalRect> crowd;
        for (int i = 0; i < 200; ++i)
            crowd.append({double(i), 10, 20, 20});
        QCOMPARE(mapPets(crowd, streams).size(), kMaxPets);
    }
    void thePlanSetsUpTheHelper()
    {
        const Trigger t = screenTrigger(u"abc-1"_s, {{u"regions"_s, QJsonArray{region(100, 200, 300, 120), region(9000, 0, 50, 50)}}, {u"match"_s, QJsonObject{{u"mode"_s, u"structure"_s}}}});
        PlanInput in = input({t}, {{41, 0, 0, 1920, 1080, 1.0}});
        in.ticksPerSecond = 99;
        const Plan plan = makePlan(in);
        QCOMPARE(plan.commands.first().type, CommandType::Clear);
        QCOMPARE(plan.commands.at(1).type, CommandType::Trigger);
        QCOMPARE(plan.commands.at(1).trigger, u"abc-1"_s);
        QCOMPARE(plan.commands.at(1).mode, ScreenMatchMode::Structure);
        QCOMPARE(plan.commands.at(2).type, CommandType::Region);
        QCOMPARE(plan.commands.at(2).region, 0);
        QCOMPARE(plan.commands.at(2).w, 300);
        QCOMPARE(plan.commands.at(3).type, CommandType::Ref);
        QCOMPARE(plan.commands.at(3).grid, kGrid);
        QCOMPARE(plan.commands.last().type, CommandType::Rate);
        QCOMPARE(plan.commands.last().rate, kMaxRate); // clamped
        QCOMPARE(plan.commands.size(), 5);             // the region off the screens is not declared
        QCOMPARE(plan.declaredRegions, 1);
        QVERIFY(plan.triggers.at(0).regions.at(0).declared);
        QVERIFY(!plan.triggers.at(0).regions.at(1).declared);
        QCOMPARE(plan.triggers.at(0).regions.at(1).problem, QString(kRegionOutside));
        QVERIFY(plan.notes.at(0).contains(u"outside the shared screens"_s));
        // every line the plan makes is one the helper's grammar takes
        for (const Command &c : plan.commands) {
            QString error;
            QVERIFY2(parseCommand(formatCommand(c), &error).has_value(), qPrintable(error));
        }
    }
    void aRegionWithoutAReferenceIsNotDeclared()
    {
        const Trigger t = screenTrigger(u"abc"_s, {{u"regions"_s, QJsonArray{region(100, 200, 300, 120)}}});
        PlanInput in = input({t}, {{41, 0, 0, 1920, 1080, 1.0}});
        in.steps = [](const QString &, int) { return QList<int>(); };
        Plan plan = makePlan(in);
        QCOMPARE(plan.declaredRegions, 0);
        QCOMPARE(plan.triggers.at(0).regions.at(0).problem, QString(kRegionNoReference));
        // steps exist but their pictures can not be read
        in.steps = [](const QString &, int) { return QList<int>{0, 1}; };
        in.grid = [](const QString &, int, int) { return QByteArray("short"); };
        plan = makePlan(in);
        QCOMPARE(plan.declaredRegions, 0);
        QCOMPARE(plan.triggers.at(0).regions.at(0).problem, QString(kRegionNoReference));
    }
    void aSequencerSendsItsStepsAndNoMoreThanItWalks()
    {
        const Trigger t = screenTrigger(u"abc"_s, {{u"regions"_s, QJsonArray{region(100, 200, 300, 120)}}, {u"match"_s, QJsonObject{{u"refMode"_s, u"sequencer"_s}, {u"seqLen"_s, 3}}}});
        PlanInput in = input({t}, {{41, 0, 0, 1920, 1080, 1.0}});
        in.steps = [](const QString &, int) { return QList<int>{0, 1, 2, 3, 4, 70}; };
        const Plan plan = makePlan(in);
        int refs = 0;
        for (const Command &c : plan.commands)
            refs += c.type == CommandType::Ref;
        QCOMPARE(refs, 3);
        QCOMPARE(plan.triggers.at(0).steps, 3);
    }
    void theLimitsOfTheHelperAreKept()
    {
        QList<Trigger> triggers;
        for (int i = 0; i < 6; ++i)
            triggers.append(screenTrigger(u"t%1"_s.arg(i), {{u"regions"_s, QJsonArray{region(10, 10, 20, 20), region(40, 10, 20, 20), region(70, 10, 20, 20), region(100, 10, 20, 20),
                                                                                           region(130, 10, 20, 20)}}}));
        const Plan plan = makePlan(input(triggers, {{41, 0, 0, 1920, 1080, 1.0}}));
        QCOMPARE(plan.triggers.size(), kMaxEnabledScreenTriggers);
        QVERIFY(plan.notes.contains(QString(kTooManyTriggers)));
        QVERIFY(plan.declaredRegions <= kMaxRegionsTotal);
        QCOMPARE(plan.declaredRegions, kMaxRegionsTotal); // 4 triggers of 5 regions: 20 asked, 16 kept
        QVERIFY(plan.notes.contains(QString(kTooManyRegions)));
        int declaredInPlan = 0;
        for (const PlannedTrigger &p : plan.triggers)
            for (const PlannedRegion &r : p.regions)
                declaredInPlan += r.declared;
        QCOMPARE(declaredInPlan, kMaxRegionsTotal);
    }
    void aTriggerThatIsNotAScreenOrHasABadIdIsLeftOut()
    {
        Trigger fire;
        fire.id = u"fire"_s;
        fire.source.kind = TriggerKind::Fire;
        const Trigger bad = screenTrigger(u"../etc"_s, {{u"regions"_s, QJsonArray{region(10, 10, 50, 50)}}});
        const Plan plan = makePlan(input({fire, bad}, {{41, 0, 0, 1920, 1080, 1.0}}));
        QVERIFY(plan.triggers.isEmpty());
        QCOMPARE(plan.commands.size(), 2); // clear and rate
    }
};

QTEST_GUILESS_MAIN(TstScreenMatchPlan)
#include "tst_screenmatchplan.moc"
