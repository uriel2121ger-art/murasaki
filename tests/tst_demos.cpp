// The pure part of the demonstrations: the marker, the layouts for 0, 1 and 2+ pets, the tracking example and the shortcut trigger.

#include "demos.h"
#include "timeline.h"
#include "trigger.h"

#include <QJsonDocument>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QJsonObject pet(const QString &id, double x, double y, double width = 200, bool stage = false)
{
    QJsonObject o{{u"petId"_s, id}, {u"itemId"_s, u"anima:1"_s}, {u"x"_s, x}, {u"y"_s, y}, {u"width"_s, width}};
    if (stage)
        o.insert(u"stage"_s, QJsonObject{{u"mode"_s, u"output"_s}});
    return o;
}

QStringList kinds(const QJsonValue &track)
{
    QStringList k;
    for (const QJsonValue &a : track.toObject().value(u"actions"_s).toArray())
        k.append(a.toObject().value(u"kind"_s).toString());
    return k;
}

} // namespace

class TstDemos : public QObject
{
    Q_OBJECT

private slots:
    void onlyTheMarkerMakesADemo()
    {
        QVERIFY(isDemo(QJsonObject{{u"origin"_s, u"demo"_s}}));
        QVERIFY(!isDemo(QJsonObject{{u"origin"_s, u"package X"_s}}));
        QVERIFY(!isDemo(QJsonObject{{u"name"_s, u"Example: mine"_s}})); // the prefix is for people
        const QJsonArray list{QJsonObject{{u"id"_s, u"a"_s}, {u"name"_s, u"Example: x"_s}, {u"origin"_s, u"demo"_s}},
                              QJsonObject{{u"id"_s, u"b"_s}, {u"name"_s, u"Example: x"_s}},
                              QJsonObject{{u"id"_s, u"c"_s}, {u"name"_s, u"Example: y"_s}, {u"origin"_s, u"demo"_s}}};
        QCOMPARE(demoIds(list), (QStringList{u"a"_s, u"c"_s}));
        QCOMPARE(demoIdByName(list, u"Example: x"_s), u"a"_s);
        QCOMPARE(demoIdByName(list, u"Example: z"_s), QString());
    }
    void aDemoNeverTakesTheNameOfSomethingTheUserMade()
    {
        const QJsonArray list{QJsonObject{{u"name"_s, u"Example: x"_s}}, QJsonObject{{u"name"_s, u"example: x (2)"_s}},
                              QJsonObject{{u"name"_s, u"Example: y"_s}, {u"origin"_s, u"demo"_s}}};
        QCOMPARE(demoFreeName(list, u"Example: z"_s), u"Example: z"_s);
        QCOMPARE(demoFreeName(list, u"Example: x"_s), u"Example: x (3)"_s); // (any case: profile names are unique so)
        QCOMPARE(demoFreeName(list, u"Example: y"_s), u"Example: y"_s);      // its own earlier example is replaced, not doubled
    }
    void withoutPetsThereIsNoTimeline()
    {
        QVERIFY(!demoTimeline({}, QSize(1920, 1080), u"n"_s).has_value());
        QVERIFY(demoTrackingPet({}).isEmpty());
    }
    void twoPetsOneCrossesAndTheOtherAppearsInTheCorner()
    {
        const auto made = demoTimeline({pet(u"a"_s, 100, 300), pet(u"b"_s, 500, 500)}, QSize(1920, 1080), u"Example: t"_s);
        QVERIFY(made.has_value());
        QString error;
        const auto parsed = applyTimelineChanges(Timeline{}, *made, &error); // the daemon's own validation accepts it
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->name, u"Example: t"_s);
        QCOMPARE(parsed->origin, u"demo"_s);
        QVERIFY(parsed->restore);
        QCOMPARE(parsed->tracks.size(), 2);
        QCOMPARE(parsed->tracks[0].pet.petId, u"a"_s);
        QVERIFY(parsed->tracks[0].startVisible);
        QCOMPARE(parsed->tracks[1].pet.petId, u"b"_s);
        QVERIFY(!parsed->tracks[1].startVisible);
        const QJsonArray tracks = made->value(u"tracks"_s).toArray();
        QCOMPARE(kinds(tracks[0]), (QStringList{u"place"_s, u"move"_s, u"wait"_s}));
        QCOMPARE(kinds(tracks[1]), (QStringList{u"wait"_s, u"place"_s, u"show"_s, u"wait"_s, u"hide"_s}));
        const QJsonObject move = tracks[0].toObject().value(u"actions"_s).toArray().at(1).toObject();
        QCOMPARE(move.value(u"mode"_s).toString(), u"smooth"_s);
        QCOMPARE(move.value(u"y"_s).toDouble(), 300.0); // it crosses at its own height
        QVERIFY(move.value(u"x"_s).toDouble() > 1000);
        const QJsonObject corner = tracks[1].toObject().value(u"actions"_s).toArray().at(1).toObject();
        QVERIFY(corner.value(u"x"_s).toDouble() > 1000 && corner.value(u"y"_s).toDouble() > 500);
    }
    void onePetDoesBothParts()
    {
        const auto made = demoTimeline({pet(u"a"_s, 100, 300)}, QSize(1920, 1080), u"n"_s);
        QVERIFY(made.has_value());
        QString error;
        QVERIFY2(applyTimelineChanges(Timeline{}, *made, &error).has_value(), qPrintable(error));
        const QJsonArray tracks = made->value(u"tracks"_s).toArray();
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(kinds(tracks[0]), (QStringList{u"place"_s, u"move"_s, u"wait"_s, u"hide"_s, u"wait"_s, u"place"_s, u"show"_s, u"wait"_s}));
    }
    void aSmallOrUnknownScreenKeepsEverythingInside()
    {
        for (const QSize screen : {QSize(), QSize(300, 200), QSize(800, 600)}) {
            const auto made = demoTimeline({pet(u"a"_s, 5000, 5000, 900), pet(u"b"_s, -40, -40, 16)}, screen, u"n"_s);
            QVERIFY(made.has_value());
            QString error;
            QVERIFY2(applyTimelineChanges(Timeline{}, *made, &error).has_value(), qPrintable(error));
            const QSize used = screen.isValid() ? screen : kDemoDefaultScreen;
            for (const QJsonValue &track : made->value(u"tracks"_s).toArray())
                for (const QJsonValue &a : track.toObject().value(u"actions"_s).toArray()) {
                    const QJsonObject o = a.toObject();
                    if (!o.contains(u"x"_s))
                        continue;
                    QVERIFY(o.value(u"x"_s).toDouble() >= 0 && o.value(u"x"_s).toDouble() <= used.width());
                    QVERIFY(o.value(u"y"_s).toDouble() >= 0 && o.value(u"y"_s).toDouble() <= used.height());
                }
        }
    }
    void theTrackingExampleFollowsForTwentySecondsAndTheStageIsSkipped()
    {
        const QJsonArray pets{pet(u"stage"_s, 0, 0, 200, true), pet(u"walker"_s, 0, 0)};
        QCOMPARE(demoTrackingPet(pets).value(u"petId"_s).toString(), u"walker"_s);
        QCOMPARE(demoTrackingPet(QJsonArray{pets[0]}).value(u"petId"_s).toString(), u"stage"_s); // only a stage: it can still look
        QCOMPARE(demoTrackingMode(pets[0].toObject()), u"look"_s);
        QJsonObject locked = pet(u"locked"_s, 0, 0);
        locked.insert(u"locked"_s, true);
        QCOMPARE(demoTrackingPet(QJsonArray{locked, pets[1]}).value(u"petId"_s).toString(), u"walker"_s); // a locked pet does not walk
        QCOMPARE(demoTrackingPet(QJsonArray{locked}).value(u"petId"_s).toString(), u"locked"_s);
        QCOMPARE(demoTrackingTimeline(locked, u"n"_s).value(u"tracks"_s).toArray().at(0).toObject().value(u"actions"_s).toArray().at(0).toObject().value(u"mode"_s).toString(), u"look"_s);
        const QJsonObject made = demoTrackingTimeline(demoTrackingPet(pets), u"Example: follow"_s);
        QString error;
        const auto parsed = applyTimelineChanges(Timeline{}, made, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QVERIFY(parsed->restore);
        QCOMPARE(parsed->tracks.size(), 1);
        QCOMPARE(parsed->tracks[0].actions[0].kind, ActionKind::Track);
        QCOMPARE(parsed->tracks[0].actions[0].tracking, PetTrackingMode::Follow);
        QCOMPARE(parsed->lengthMs(), qint64(kDemoTrackingMs));
        QCOMPARE(parsed->origin, u"demo"_s);
    }
    void theTriggerIsAShortcutThatPlaysTheTimeline()
    {
        const QJsonObject made = demoTrigger(u"tl-1"_s, u"Example: key"_s);
        QString error;
        const auto parsed = applyTriggerChanges(Trigger{}, made, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->source.kind, TriggerKind::Shortcut);
        QCOMPARE(parsed->source.id, u"murasaki-demo"_s);
        QCOMPARE(parsed->action.type, TriggerActionType::Timeline);
        QCOMPARE(parsed->action.id, u"tl-1"_s);
        QCOMPARE(parsed->origin, u"demo"_s);
        QVERIFY(parsed->enabled);
    }
    void theScreenTriggerShipsOffWithoutPicturesAndHoldsByItsKind()
    {
        const QJsonObject made = demoScreenTrigger(u"Example: hide"_s);
        QString error;
        const auto parsed = applyTriggerChanges(Trigger{}, made, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->source.kind, TriggerKind::Screen);
        QCOMPARE(parsed->origin, u"demo"_s);
        QVERIFY(!parsed->enabled); // off until the user starts it
        QVERIFY(!parsed->hold);    // (the engine holds a screen trigger by its kind)
        QCOMPARE(parsed->action.type, TriggerActionType::HideAll);
        const QJsonObject data = parsed->source.screen;
        QCOMPARE(data.value(u"regions"_s).toArray().size(), 1);
        // nothing of the screen ships: no picture, no path, no id of a reference
        const QByteArray text = QJsonDocument(made).toJson();
        QVERIFY(!text.contains("png") && !text.contains("refs") && !text.contains("image"));
        QVERIFY(isDemo(made));
    }
    void theEnglishNamesAreTheCliDefaults()
    {
        const DemoNames names = DemoNames::english();
        for (const QString &name : {names.timeline, names.trigger, names.profile, names.tracking, names.screen})
            QVERIFY(name.startsWith(u"Example: "_s));
    }
};

QTEST_GUILESS_MAIN(TstDemos)
#include "tst_demos.moc"
