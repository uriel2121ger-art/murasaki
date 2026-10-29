// What an imported package becomes in the automation: scenes as one-step timelines, a Triggers pack as a timeline and two disabled triggers.

#include "automation.h"
#include "packautomation.h"
#include "testutil.h"

#include <QJsonArray>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TstPackAutomation : public QObject
{
    Q_OBJECT

private slots:
    void aSceneThatShowsIsAShowAndAPlaceAndDoesNotRestore()
    {
        QString error;
        const auto t = timelineFromScene(testutil::json(R"({"line":2,"show":true,"variant":"jump","x":300,"y":400})"), 1, u"local:abc"_s, u"Pack"_s, &error);
        QVERIFY2(t.has_value(), qPrintable(error));
        QCOMPARE(t->name, u"Pack: scene 2"_s);
        QVERIFY(!t->restore); // it would undo itself at once
        QCOMPARE(t->origin, u"package Pack [local:abc]"_s);
        QCOMPARE(t->tracks.size(), 1);
        QCOMPARE(t->tracks[0].pet.item, u"local:abc"_s);
        QCOMPARE(t->tracks[0].actions.size(), 2);
        QCOMPARE(t->tracks[0].actions[0].kind, ActionKind::Show);
        QCOMPARE(t->tracks[0].actions[0].variant, u"jump"_s);
        QCOMPARE(t->tracks[0].actions[1].kind, ActionKind::Place);
        QCOMPARE(t->tracks[0].actions[1].x, 300.0);
        const auto hide = timelineFromScene(testutil::json(R"({"line":3,"show":false})"), 2, u"local:abc"_s, u"Pack"_s);
        QVERIFY(hide.has_value());
        QCOMPARE(hide->tracks[0].actions[0].kind, ActionKind::Hide);
        QVERIFY(!timelineFromScene(testutil::json(R"({"show":true})"), 0, u"local:abc"_s, u"P"_s, &error).has_value()); // shows nothing
        QVERIFY(!timelineFromScene(QJsonObject{}, 0, QString(), u"P"_s).has_value());
    }
    void aPackBecomesOneTimelineWithTwoTracksPerAnimationAndTwoDisabledTriggers()
    {
        const QJsonObject pack = testutil::json(R"({"name":"Wave hello","speedMult":2,"match":{"cooldownMs":900},"regions":[{"id":"r","kind":"rect","x":1,"y":2,"w":3,"h":4}],"event":{"name":"e"},
            "tracks":[{"name":"Hello","variant":"hello","startVisible":false,"actions":[
              {"t":2000,"kind":"hide"},{"t":500,"kind":"show"},{"t":0,"kind":"move","x":100,"y":200,"dur":0},{"t":1000,"kind":"move","x":300,"y":200,"dur":400,"mode":"smooth"},
              {"t":1200,"kind":"sound","sound":"boom.wav"},{"t":10,"kind":"place","x":1,"y":1},{"t":20,"kind":"wait"}]},
              {"name":"Lost","variant":"","actions":[]}]})");
        const PackAutomation made = automationFromPack(pack, u"local:abc"_s, u"Pack"_s);
        QCOMPARE(made.timelines.size(), 1);
        const Timeline &t = made.timelines[0];
        QCOMPARE(t.name, u"Wave hello"_s);
        QCOMPARE(t.speed, 2.0);
        QVERIFY(t.restore);
        QVERIFY(validateTimeline(t));
        QCOMPARE(t.tracks.size(), 2); // the dropped animation has none
        QVERIFY(t.origin.contains(u"local:abc"_s)); // the replacement key names the item
        for (const TimelineTrack &track : t.tracks) {
            QCOMPARE(track.pet.item, u"local:abc"_s);
            QCOMPARE(track.pet.variant, u"hello"_s);
            QVERIFY(!track.startVisible);
        }
        // what is shown and when: show at 500, hide at 2000
        const QList<TimelineAction> &shown = t.tracks[0].actions;
        QCOMPARE(shown.size(), 4);
        QCOMPARE(shown[0].kind, ActionKind::Wait);
        QCOMPARE(shown[0].waitMs, 500);
        QCOMPARE(shown[1].kind, ActionKind::Show);
        QCOMPARE(shown[2].waitMs, 1500);
        QCOMPARE(shown[3].kind, ActionKind::Hide);
        // where it moves: a jump at 0, a wait of 1000, then a smooth move of 400 ms
        const QList<TimelineAction> &moving = t.tracks[1].actions;
        QCOMPARE(moving.size(), 3);
        QCOMPARE(moving[0].kind, ActionKind::Move);
        QVERIFY(!moving[0].smooth);
        QCOMPARE(moving[1].waitMs, 1000);
        QVERIFY(moving[2].smooth && moving[2].durationMs == 400);
        QVERIFY(moving[0].centre && moving[2].centre); // the pack's positions are centres: the daemon resolves them with the pet's size
        QCOMPARE(t.lengthMs(), qint64(2000));
        // the notes say what was not carried over
        const QString notes = made.notes.join(u'\n');
        QVERIFY(notes.contains(u"was not imported"_s));
        QVERIFY(notes.contains(u"boom.wav"_s));
        QVERIFY(notes.contains(u"centre"_s));
        QVERIFY(notes.contains(QString(kAutomationScreenInactive)));
        // two triggers, both disabled and with their origin: a fire one and the inert screen one
        QCOMPARE(made.triggers.size(), 2);
        for (const Trigger &tr : made.triggers) {
            QVERIFY(!tr.enabled);
            QCOMPARE(tr.origin, u"package Pack [local:abc]"_s);
            QCOMPARE(tr.cooldownMs, 900);
            QCOMPARE(tr.action.type, TriggerActionType::Timeline);
        }
        QCOMPARE(made.triggers[0].source.kind, TriggerKind::Fire);
        QVERIFY(made.triggers[0].source.name.startsWith(u"Wave-hello-"_s)); // plus a short hash of the item
        QCOMPARE(made.triggers[1].source.kind, TriggerKind::Screen);
        QCOMPARE(made.triggers[1].source.screen.value(u"regions"_s).toArray().size(), 1);
        // a trigger needs its timeline's id; with it, both validate
        for (Trigger tr : made.triggers) {
            tr.action.id = u"some-id"_s;
            QString error;
            QVERIFY2(validateTrigger(tr, &error), qPrintable(error));
        }
    }
    void theBasePositionIsTheFirstMoveEvenWhenItComesLate()
    {
        const PackAutomation made = automationFromPack(testutil::json(R"({"name":"P","tracks":[{"name":"a","variant":"v","actions":[{"t":1500,"kind":"move","x":50,"y":60,"dur":0}]}]})"),
                                                       u"i"_s, u"P"_s);
        const QList<TimelineAction> &moving = made.timelines[0].tracks[1].actions;
        QCOMPARE(moving.size(), 3); // the pet stands at the first move from the start: a jump there, a wait, the move itself
        QCOMPARE(moving[0].kind, ActionKind::Move);
        QCOMPARE(moving[0].x, 50.0);
        QVERIFY(!moving[0].smooth && moving[0].centre);
        QCOMPARE(moving[1].waitMs, 1500);
    }
    void twoPackagesWithOneTitleNeverShareAnOriginOrAFireName()
    {
        const QJsonObject pack = testutil::json(R"({"name":"Wave","tracks":[{"name":"a","variant":"v","actions":[{"t":0,"kind":"show"}]}]})");
        const PackAutomation a = automationFromPack(pack, u"local:one"_s, u"Same title"_s);
        const PackAutomation b = automationFromPack(pack, u"local:two"_s, u"Same title"_s);
        QVERIFY(a.timelines[0].origin != b.timelines[0].origin);
        QVERIFY(a.triggers[0].source.name != b.triggers[0].source.name); // FireTrigger("x") must not run both
        QVERIFY(a.triggers[0].origin != b.triggers[0].origin);
        QVERIFY(validateName(a.triggers[0].source.name, nullptr));
        QVERIFY(a.timelines[0].origin.size() <= 200);
        QVERIFY(packageOrigin(QString(500, u'x'), QString(500, u'y')).size() <= 200);
        // Anima's animation speed is said not to be applied, the loop is said not to be repeated
        const PackAutomation c = automationFromPack(testutil::json(R"({"name":"W","animSpeedMult":1.5,"tracks":[{"name":"a","variant":"v","actions":[]}]})"), u"i"_s, u"P"_s);
        QVERIFY(c.notes.join(u' ').contains(u"1.5"_s));
        QVERIFY(c.notes.join(u' ').contains(u"plays once"_s));
    }
    void aPackWithNothingUsableMakesNothing()
    {
        const PackAutomation none = automationFromPack(testutil::json(R"({"name":"x","tracks":[{"variant":"","actions":[]}]})"), u"i"_s, u"P"_s);
        QVERIFY(none.timelines.isEmpty() && none.triggers.isEmpty());
        QVERIFY(!none.notes.isEmpty());
        QVERIFY(automationFromPack(QJsonObject{}, u"i"_s, u"P"_s).timelines.isEmpty());
    }
    void longAndManyThingsStayWithinTheLimits()
    {
        QJsonArray tracks, actions;
        for (int i = 0; i < 400; ++i)
            actions.append(QJsonObject{{u"t"_s, i * 100}, {u"kind"_s, i % 2 ? u"show"_s : u"hide"_s}});
        actions.append(QJsonObject{{u"t"_s, 86'000'000}, {u"kind"_s, u"hide"_s}}); // a day away: split into waits
        for (int i = 0; i < 16; ++i)
            tracks.append(QJsonObject{{u"name"_s, u"a%1"_s.arg(i)}, {u"variant"_s, u"v%1"_s.arg(i)}, {u"actions"_s, actions}});
        const PackAutomation made = automationFromPack(QJsonObject{{u"name"_s, u"Big"_s}, {u"tracks"_s, tracks}, {u"speedMult"_s, 99}}, u"i"_s, u"P"_s);
        QCOMPARE(made.timelines.size(), 1);
        QVERIFY(validateTimeline(made.timelines[0]));
        QVERIFY(made.timelines[0].tracks.size() <= kMaxTimelineTracks);
        QCOMPARE(made.timelines[0].speed, kMaxTimelineSpeed);
        QVERIFY(made.notes.join(u' ').contains(u"dropped"_s));
    }
    void fireNamesAreSafe()
    {
        QCOMPARE(fireNameFrom(u"Wave hello!"_s), u"Wave-hello-"_s);
        QVERIFY(!fireNameFrom(u"../../etc"_s).contains(u'/'));
        QVERIFY(!fireNameFrom(u".hidden"_s).startsWith(u'.'));
        QCOMPARE(fireNameFrom(QString()), u"pack"_s);
        QCOMPARE(fireNameFrom(QString(200, u'a')).size(), 64);
        QString error;
        QVERIFY(validateName(fireNameFrom(u"ñandú ☃ ok"_s), &error));
    }
};

QTEST_GUILESS_MAIN(TstPackAutomation)
#include "tst_packautomation.moc"
