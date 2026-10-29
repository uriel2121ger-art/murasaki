// The pure models of the automation (profiles, triggers, timelines, the pets' runtime layer): every field valid and invalid, the limits, and a mutation run over
// hostile input that must never crash and never accept what validation refuses.

#include "automation.h"
#include "petruntime.h"
#include "profile.h"
#include "testutil.h"
#include "timeline.h"
#include "trigger.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QSet>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QJsonObject obj(const char *text)
{
    return testutil::json(text);
}

}

class TstAutomationModels : public QObject
{
    Q_OBJECT

private slots:
    void theMotionIsLinearAndClamped()
    {
        PetMotion m{0, 0, 100, -50, 1000, 500};
        QCOMPARE(positionAt(m, 0).x, 0.0);
        QCOMPARE(positionAt(m, 1000).x, 0.0);
        QCOMPARE(positionAt(m, 1250).x, 50.0);
        QCOMPARE(positionAt(m, 1250).y, -25.0);
        QCOMPARE(positionAt(m, 1500).x, 100.0);
        QCOMPARE(positionAt(m, 99999).y, -50.0);
        QVERIFY(!motionDone(m, 1499));
        QVERIFY(motionDone(m, 1500));
        m.durationMs = 0; // a teleport
        QVERIFY(motionDone(m, 0));
        QCOMPARE(positionAt(m, 0).x, 100.0);
    }
    void theRuntimeRoundTripsAndRefusesWhatIsOutOfRange()
    {
        PetRuntime r;
        QVERIFY(r.isDefault());
        r.hidden = true;
        r.position = PetPosition{12.5, -3};
        r.output = u"DP-1"_s;
        r.motion = PetMotion{1, 2, 3, 4, 5000, 300};
        r.opacity = 0.5;
        QVERIFY(!r.isDefault());
        QString error;
        const auto back = parsePetRuntime(r.toJson(), &error);
        QVERIFY2(back.has_value(), qPrintable(error));
        QCOMPARE(*back, r);
        for (const char *bad : {R"({"x":1})", R"({"x":"a","y":1})", R"({"x":1e9,"y":0})", R"({"hidden":"yes"})", R"({"opacity":0})", R"({"opacity":2})",
                                R"({"motion":{"fromX":0,"fromY":0,"toX":0,"toY":0,"startMs":0,"durationMs":60001}})", R"({"motion":{"fromX":0}})",
                                R"({"motion":3})", R"({"output":3})"})
            QVERIFY2(!parsePetRuntime(obj(bad), &error).has_value(), bad);
        QVERIFY(parsePetRuntime(obj(R"({"unknown":1})")).has_value()); // unknown keys are ignored
        // the mouse tracking mode of the layer: any of the three names, even off (it overrides the stored mode), nothing else
        PetRuntime tracking;
        tracking.tracking = PetTrackingMode::Follow;
        QVERIFY(!tracking.isDefault());
        QCOMPARE(parsePetRuntime(tracking.toJson()), std::optional<PetRuntime>(tracking));
        tracking.tracking = PetTrackingMode::Off;
        QVERIFY(!tracking.isDefault());
        QCOMPARE(parsePetRuntime(tracking.toJson()), std::optional<PetRuntime>(tracking));
        for (const char *bad : {R"({"tracking":"run"})", R"({"tracking":1})", R"({"tracking":null})"})
            QVERIFY2(!parsePetRuntime(obj(bad), &error).has_value(), bad);
        // the position that is drawn: the motion first, then the override, then the stored one
        const PetPosition stored{7, 8};
        QCOMPARE(effectivePosition(PetRuntime{}, stored, 0).x, 7.0);
        PetRuntime placed;
        placed.position = PetPosition{1, 2};
        QCOMPARE(effectivePosition(placed, stored, 0).y, 2.0);
        placed.motion = PetMotion{0, 0, 10, 10, 0, 100};
        QCOMPARE(effectivePosition(placed, stored, 50).x, 5.0);
    }
    void theLayerLeavesOutDefaultsAndSkipsBadEntries()
    {
        QHash<QString, PetRuntime> layer;
        PetRuntime hidden;
        hidden.hidden = true;
        layer.insert(u"a"_s, hidden);
        layer.insert(u"b"_s, PetRuntime{});
        const QJsonObject json = runtimeLayerToJson(layer);
        QCOMPARE(json.size(), 1);
        int skipped = -1;
        QJsonObject mixed = json;
        mixed.insert(u"bad"_s, QJsonObject{{u"opacity"_s, 9}});
        mixed.insert(u"worse"_s, 5);
        const auto back = runtimeLayerFromJson(mixed, &skipped);
        QCOMPARE(back.size(), 1);
        QCOMPARE(skipped, 2);
    }

    void aTimelineRoundTripsEveryActionKind()
    {
        const auto timeline = applyTimelineChanges(Timeline{}, obj(R"({"name":"hello","speed":2,"restore":false,"tracks":[
            {"pet":{"item":"local:1","variant":"v"},"startVisible":false,"actions":[
              {"kind":"place","x":10,"y":20,"output":"DP-1"},{"kind":"show","variant":"wave"},{"kind":"move","x":5,"y":6,"mode":"smooth","durationMs":1500},
              {"kind":"sound"},{"kind":"wait","ms":250},{"kind":"stopSounds"},{"kind":"hide"}]},
            {"pet":{"slot":3},"actions":[]},{"pet":{"petId":"abc"},"actions":[{"kind":"move","x":1,"y":2}]}]})"));
        QVERIFY(timeline.has_value());
        QCOMPARE(timeline->tracks.size(), 3);
        QCOMPARE(timeline->tracks[0].actions.size(), 7);
        QVERIFY(timeline->tracks[0].actions[2].smooth);
        QVERIFY(!timeline->tracks[2].actions[0].smooth); // teleport by default
        QCOMPARE(timeline->lengthMs(), qint64(1750));
        Timeline withId = *timeline;
        withId.id = u"x"_s;
        const auto again = applyTimelineChanges(withId, withId.toJson());
        QVERIFY(again.has_value());
        QCOMPARE(again->toJson(), withId.toJson());
        QCOMPARE(allActionKinds().size(), 8);
        for (const ActionKind kind : allActionKinds())
            QCOMPARE(actionKindFromName(actionKindName(kind)), std::optional<ActionKind>(kind));
        QVERIFY(!actionKindFromName(u"exec"_s));
    }
    void aTimelineRefusesWhatIsWrongAndSaysWhere()
    {
        const auto refused = [](const char *text, const QString &mention) {
            QString error;
            const auto t = applyTimelineChanges(Timeline{}, QJsonDocument::fromJson(text).object(), &error);
            QVERIFY2(!t.has_value(), text);
            QVERIFY2(error.contains(mention), qPrintable(error));
        };
        refused(R"({"name":""})", u"name"_s);
        refused(R"({"name":"x","speed":0.1})", u"speed"_s);
        refused(R"({"name":"x","speed":5})", u"speed"_s);
        refused(R"({"name":"x","restore":1})", u"restore"_s);
        refused(R"({"name":"x","tracks":3})", u"tracks"_s);
        refused(R"({"name":"x","tracks":[{"pet":{},"actions":[]}]})", u"track 0"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"petId":"a","slot":1},"actions":[]}]})", u"exactly one"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"exec","command":"ls"}]}]})", u"track 0, action 0"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"wait","ms":3600001}]}]})", u"ms"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"wait"}]}]})", u"ms"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"move","x":1,"y":2,"mode":"smooth","durationMs":60001}]}]})", u"durationMs"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"move","x":1,"y":2,"mode":"fly"}]}]})", u"mode"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"place","x":1e9,"y":2}]}]})", u"100000"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":1},"actions":[{"kind":"place","x":"a","y":2}]}]})", u"numbers"_s);
        refused(R"({"name":"x","tracks":[{"pet":{"slot":-1},"actions":[]}]})", u"slot"_s);
    }
    void anAnchorNamesTheCentreForPlacesAndMovesOnly()
    {
        QString error;
        const auto t = applyTimelineChanges(Timeline{}, obj(R"({"name":"n","tracks":[{"pet":{"slot":0},"actions":[{"kind":"place","x":1,"y":2,"anchor":"centre"},
            {"kind":"move","x":3,"y":4,"anchor":"centre","mode":"smooth","durationMs":10},{"kind":"move","x":5,"y":6,"anchor":"corner"},{"kind":"move","x":7,"y":8}]}]})"), &error);
        QVERIFY2(t.has_value(), qPrintable(error));
        const auto &a = t->tracks[0].actions;
        QVERIFY(a[0].centre && a[1].centre && !a[2].centre && !a[3].centre);
        Timeline withId = *t;
        withId.id = u"x"_s;
        QCOMPARE(applyTimelineChanges(withId, withId.toJson())->toJson(), withId.toJson()); // the anchor round trips
        QVERIFY(!withId.toJson().value(u"tracks"_s).toArray()[0].toObject().value(u"actions"_s).toArray()[2].toObject().contains(u"anchor"_s)); // (a corner is the default)
        for (const char *bad : {R"({"kind":"place","x":1,"y":1,"anchor":"middle"})", R"({"kind":"hide","anchor":"centre"})", R"({"kind":"move","x":1,"y":1,"anchor":3})"})
            QVERIFY2(!applyTimelineChanges(Timeline{}, QJsonObject{{u"name"_s, u"n"_s}, {u"tracks"_s, QJsonArray{QJsonObject{{u"pet"_s, QJsonObject{{u"slot"_s, 0}}}, {u"actions"_s, QJsonArray{obj(bad)}}}}}}).has_value(), bad);
    }
    void timelineCapsHold()
    {
        QJsonArray tracks;
        for (int i = 0; i <= kMaxTimelineTracks; ++i)
            tracks.append(QJsonObject{{u"pet"_s, QJsonObject{{u"slot"_s, i}}}, {u"actions"_s, QJsonArray()}});
        QVERIFY(!applyTimelineChanges(Timeline{}, QJsonObject{{u"name"_s, u"n"_s}, {u"tracks"_s, tracks}}).has_value());
        tracks.removeLast();
        QVERIFY(applyTimelineChanges(Timeline{}, QJsonObject{{u"name"_s, u"n"_s}, {u"tracks"_s, tracks}}).has_value());
        QJsonArray actions;
        for (int i = 0; i <= kMaxTrackActions; ++i)
            actions.append(QJsonObject{{u"kind"_s, u"hide"_s}});
        const QJsonObject track{{u"pet"_s, QJsonObject{{u"slot"_s, 0}}}, {u"actions"_s, actions}};
        QVERIFY(!applyTimelineChanges(Timeline{}, QJsonObject{{u"name"_s, u"n"_s}, {u"tracks"_s, QJsonArray{track}}}).has_value());
        actions.removeLast();
        QVERIFY(applyTimelineChanges(Timeline{}, QJsonObject{{u"name"_s, u"n"_s}, {u"tracks"_s, QJsonArray{QJsonObject{{u"pet"_s, QJsonObject{{u"slot"_s, 0}}}, {u"actions"_s, actions}}}}}).has_value());
    }

    void everyKindOfTriggerRoundTrips()
    {
        struct Case { const char *source; };
        const char *sources[] = {
            R"({"kind":"app-focus","app":"firefox*","on":"leave"})",
            R"({"kind":"app-open","app":"steam","on":"close"})",
            R"({"kind":"fullscreen","app":"mpv","output":"DP-1"})",
            R"({"kind":"fullscreen"})",
            R"({"kind":"time","at":"22:30","days":[1,2,3]})",
            R"({"kind":"interval","everyMinutes":15})",
            R"({"kind":"idle","minutes":10})",
            R"({"kind":"idle","minutes":10,"state":"back"})",
            R"({"kind":"power","state":"battery"})",
            R"({"kind":"session","state":"locked"})",
            R"({"kind":"game","state":"started"})",
            R"({"kind":"output","state":"added","name":"HDMI-A-1"})",
            R"({"kind":"shortcut","id":"wave","description":"Wave"})",
            R"({"kind":"signal-file","name":"sleepy"})",
            R"({"kind":"fire","name":"sleepy"})",
            R"({"kind":"dbus","interface":"org.example.Iface","member":"Changed","senderNamespace":"org.example"})",
            R"({"kind":"screen","data":{"regions":[1,2,3]}})",
        };
        QSet<int> kinds;
        for (const char *source : sources) {
            Trigger base;
            base.id = u"id"_s;
            QJsonObject changes{{u"name"_s, u"t"_s}, {u"source"_s, obj(source)}, {u"action"_s, QJsonObject{{u"type"_s, u"hide-all"_s}}}};
            QString error;
            const auto t = applyTriggerChanges(base, changes, &error);
            QVERIFY2(t.has_value(), qPrintable(QString::fromUtf8(source) + ": " + error));
            const auto again = applyTriggerChanges(base, t->toJson(), &error);
            QVERIFY2(again.has_value(), qPrintable(error));
            QCOMPARE(*again, *t);
            kinds.insert(int(t->source.kind));
        }
        QCOMPARE(kinds.size(), allTriggerKinds().size()); // every kind has a case
    }
    void anIdleTriggerIsAwayUnlessItSaysBack()
    {
        const auto make = [](const QString &source, QString *error = nullptr) {
            Trigger base;
            base.id = u"id"_s;
            return applyTriggerChanges(base, QJsonObject{{u"name"_s, u"t"_s}, {u"source"_s, obj(source.toUtf8().constData())}, {u"action"_s, QJsonObject{{u"type"_s, u"hide-all"_s}}}}, error);
        };
        const auto plain = make(u"{\"kind\":\"idle\",\"minutes\":10}"_s);
        const auto away = make(u"{\"kind\":\"idle\",\"minutes\":10,\"state\":\"away\"}"_s);
        const auto back = make(u"{\"kind\":\"idle\",\"minutes\":10,\"state\":\"back\"}"_s);
        QVERIFY(plain && away && back);
        QCOMPARE(*away, *plain); // "away" is the default: the same trigger, and it is written without the key
        QVERIFY(!plain->source.toJson().contains(u"state"_s));
        QVERIFY(!away->source.toJson().contains(u"state"_s));
        QCOMPARE(back->source.toJson().value(u"state"_s).toString(), u"back"_s);
        QVERIFY(*back != *plain);
        QString error;
        QVERIFY(!make(u"{\"kind\":\"idle\",\"minutes\":10,\"state\":\"later\"}"_s, &error));
        QVERIFY2(error.contains(u"state"_s), qPrintable(error));
        QVERIFY(!make(u"{\"kind\":\"idle\",\"minutes\":10,\"state\":\"locked\"}"_s, &error));
    }
    void aTriggerRefusesWhatIsWrong()
    {
        const auto refused = [](const QString &changes, const QString &mention) {
            Trigger base;
            base.id = u"id"_s;
            QString error;
            const auto t = applyTriggerChanges(base, QJsonDocument::fromJson(changes.toUtf8()).object(), &error);
            QVERIFY2(!t.has_value(), qPrintable(changes));
            QVERIFY2(error.contains(mention), qPrintable(changes + " -> " + error));
        };
        const QString act = u"\"action\":{\"type\":\"hide-all\"}"_s;
        const auto with = [&](const QString &source) { return u"{\"name\":\"t\",\"source\":%1,%2}"_s.arg(source, act); };
        refused(with(u"{\"kind\":\"exec\"}"_s), u"source.kind"_s);
        refused(with(u"{\"kind\":\"screen-grab\"}"_s), u"source.kind"_s);
        refused(with(u"{\"kind\":\"app-focus\"}"_s), u"app"_s);
        refused(with(u"{\"kind\":\"time\",\"at\":\"24:00\"}"_s), u"HH:MM"_s);
        refused(with(u"{\"kind\":\"time\",\"at\":\"9:00\"}"_s), u"HH:MM"_s);
        refused(with(u"{\"kind\":\"time\",\"at\":\"10:00\",\"days\":[7]}"_s), u"days"_s);
        refused(with(u"{\"kind\":\"interval\",\"everyMinutes\":0}"_s), u"everyMinutes"_s);
        refused(with(u"{\"kind\":\"interval\",\"everyMinutes\":1441}"_s), u"everyMinutes"_s);
        refused(with(u"{\"kind\":\"idle\",\"minutes\":1.5}"_s), u"whole"_s);
        refused(with(u"{\"kind\":\"power\",\"state\":\"locked\"}"_s), u"state"_s);
        refused(with(u"{\"kind\":\"fire\",\"name\":\"../x\"}"_s), u"name"_s);
        refused(with(u"{\"kind\":\"fire\",\"name\":\"\"}"_s), u"name"_s);
        refused(with(u"{\"kind\":\"signal-file\",\"name\":\"a/b\"}"_s), u"name"_s);
        refused(with(u"{\"kind\":\"shortcut\",\"id\":\"has space\"}"_s), u"id"_s);
        refused(with(u"{\"kind\":\"dbus\",\"interface\":\"not a name\",\"member\":\"M\"}"_s), u"D-Bus"_s);
        refused(with(u"{\"kind\":\"fire\",\"name\":\"x\",\"on\":\"leave\"}"_s), u"on is for"_s);
        refused(u"{\"name\":\"t\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"run\",\"command\":\"ls\"}}"_s, u"action.type"_s);
        refused(u"{\"name\":\"t\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"open-url\"}}"_s, u"action.type"_s);
        refused(u"{\"name\":\"t\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"timeline\"}}"_s, u"action.id"_s);
        refused(u"{\"name\":\"t\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"scene\",\"index\":1}}"_s, u"scene"_s);
        refused(u"{\"name\":\"t\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"mute\",\"mode\":\"maybe\"}}"_s, u"mute"_s);
        refused(u"{\"name\":\"t\",\"cooldownMs\":99,\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"hide-all\"}}"_s, u"cooldownMs"_s);
        refused(u"{\"name\":\"t\",\"mode\":\"hold\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"hide-all\"}}"_s, u"shortcut"_s);
        refused(u"{\"name\":\"t\",\"mode\":\"hold\",\"source\":{\"kind\":\"shortcut\",\"id\":\"k\"},\"action\":{\"type\":\"profile\",\"id\":\"p\"}}"_s, u"undone"_s);
    }
    void aHoldShortcutHasAnInverse()
    {
        TriggerAction hide;
        hide.type = TriggerActionType::HideAll;
        QCOMPARE(inverseAction(hide)->type, TriggerActionType::ShowAll);
        TriggerAction mute;
        mute.type = TriggerActionType::Mute;
        mute.mute = u"on"_s;
        QCOMPARE(inverseAction(mute)->mute, u"off"_s);
        TriggerAction profile;
        profile.type = TriggerActionType::Profile;
        QVERIFY(!inverseAction(profile));
    }
    void applicationPatternsMatchLikeThePauseRules()
    {
        QVERIFY(matchesApplication(u"firefox"_s, u"Firefox"_s));
        QVERIFY(matchesApplication(u"fire*"_s, u"firefox"_s));
        QVERIFY(matchesApplication(u"*fox"_s, u"firefox"_s));
        QVERIFY(matchesApplication(u"f?refox"_s, u"firefox"_s));
        QVERIFY(matchesApplication(u"*"_s, u"x"_s));
        QVERIFY(!matchesApplication(u"fire"_s, u"firefox"_s));
        QVERIFY(!matchesApplication(u"*a"_s, u"firefox"_s));
        QVERIFY(!matchesApplication(QString(), u"x"_s));
        QVERIFY(!matchesApplication(u"x"_s, QString()));
        // no pattern makes it slow: the matching is linear
        QVERIFY(!matchesApplication(QString(u"*a").repeated(60) + u"b", QString(u'a').repeated(2000)));
    }

    void applicationPatternsAcceptOtherSpellingsOfTheSameClass()
    {
        QVERIFY(matchesApplication(u"org.kde.dolphin"_s, u"dolphin"_s));
        QVERIFY(matchesApplication(u"dolphin"_s, u"org.kde.Dolphin"_s));
        QVERIFY(matchesApplication(u"firefox.desktop"_s, u"firefox"_s));
        QVERIFY(matchesApplication(u"org.mozilla.firefox"_s, u"Firefox.desktop"_s));
        QVERIFY(!matchesApplication(u"org.kde.dolphin"_s, u"org.gnome.dolphin"_s)); // two different ids
        QVERIFY(!matchesApplication(u"game.exe"_s, u"exe"_s));                     // two parts are not reverse-DNS
        QVERIFY(!matchesApplication(u"dolphin"_s, u"org.kde.dolphin.extra"_s));
    }

    void aProfileCarriesEveryFieldOfThePets()
    {
        QString profileError;
        const auto profile = applyProfileChanges(Profile{}, QJsonDocument::fromJson(QStringLiteral(R"({"name":"Work","pets":[
            {"itemId":"local:1","variant":"v","output":"DP-1","x":10,"y":20,"width":300,"opacity":0.8,"locked":true,"rotation":370,"flipH":true,"fps":0,"speed":2,"clickThrough":false},
            {"itemId":"local:2","variant":"w","output":"*","slot":5,"audio":{"asset":"%1","mode":"once","volume":50,"offsetMs":0}}]})").arg(QString(64, u'a')).toUtf8()).object(), &profileError);
        QVERIFY2(profile.has_value(), qPrintable(profileError));
        QCOMPARE(profile->pets.size(), 2);
        QCOMPARE(profile->pets[0].slot, 0);
        QCOMPARE(profile->pets[1].slot, 5);
        QCOMPARE(profile->pets[0].pet.rotation, 10.0);
        QVERIFY(profile->pets[0].pet.locked);
        QVERIFY(!profile->pets[0].pet.clickThrough);
        QCOMPARE(profile->pets[0].pet.speed, 2.0);
        Profile withId = *profile;
        withId.id = u"x"_s;
        const auto again = applyProfileChanges(withId, withId.toJson());
        QVERIFY(again.has_value());
        QCOMPARE(again->toJson(), withId.toJson());
        QVERIFY(!withId.toJson().value(u"pets"_s).toArray().at(0).toObject().contains(u"petId"_s));
    }
    void aProfileRefusesWhatIsWrong()
    {
        const auto refused = [](const char *text, const QString &mention) {
            QString error;
            const auto p = applyProfileChanges(Profile{}, QJsonDocument::fromJson(text).object(), &error);
            QVERIFY2(!p.has_value(), text);
            QVERIFY2(error.contains(mention), qPrintable(error));
        };
        refused(R"({"name":""})", u"name"_s);
        refused(R"({"name":"p","pets":{}})", u"pets"_s);
        refused(R"({"name":"p","pets":[3]})", u"pet 0"_s);
        refused(R"({"name":"p","pets":[{"itemId":"","variant":"v","output":"*"}]})", u"pet 0"_s);
        refused(R"({"name":"p","pets":[{"itemId":"a","variant":"v","output":"*","width":1}]})", u"width"_s);
        refused(R"({"name":"p","pets":[{"itemId":"a","variant":"v","output":"*","slot":1},{"itemId":"b","variant":"v","output":"*","slot":1}]})", u"slot"_s);
        refused(R"({"name":"p","pets":[{"itemId":"a","variant":"v","output":"*","slot":-1}]})", u"slot"_s);
        refused(R"({"name":"p","pets":[{"itemId":"a","variant":"v","output":"*","x":"far"}]})", u"x"_s);
        QJsonArray many;
        for (int i = 0; i <= kMaxProfilePets; ++i)
            many.append(QJsonObject{{u"itemId"_s, u"a"_s}, {u"variant"_s, u"v"_s}, {u"output"_s, u"*"_s}});
        QString error;
        QVERIFY(!applyProfileChanges(Profile{}, QJsonObject{{u"name"_s, u"p"_s}, {u"pets"_s, many}}, &error).has_value());
        QVERIFY(error.contains(kProfileTooManyPets));
        many.removeLast();
        QVERIFY(applyProfileChanges(Profile{}, QJsonObject{{u"name"_s, u"p"_s}, {u"pets"_s, many}}).has_value());
    }
    void entriesFromPetsNumberTheSlotsAndDropThePetIds()
    {
        PetPlacement a, b;
        a.petId = u"pa"_s;
        a.itemId = u"i"_s;
        b.petId = u"pb"_s;
        b.itemId = u"j"_s;
        const auto entries = entriesFromPets({a, b});
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[1].slot, 1);
        QVERIFY(entries[0].pet.petId.isEmpty());
    }

    void hostileInputIsRefusedOrAcceptedNeverFatal()
    {
        // A fixed seed: the same mutations on every run. Whatever is accepted must validate; whatever is not must say why.
        QRandomGenerator rng(20261022);
        const QJsonValue pool[] = {QJsonValue(), QJsonValue(true), QJsonValue(0), QJsonValue(-1), QJsonValue(1e300), QJsonValue(0.5), QJsonValue(u"x"_s),
                                   QJsonValue(QString(300, u'z')), QJsonValue(QJsonArray()), QJsonValue(QJsonObject()), QJsonValue(QJsonArray{1, 2, 3})};
        const auto mutate = [&](auto &&self, const QJsonValue &value, int depth) -> QJsonValue {
            if (depth > 6 || rng.bounded(5) == 0)
                return pool[rng.bounded(int(std::size(pool)))];
            if (value.isObject()) {
                QJsonObject o = value.toObject();
                for (auto it = o.begin(); it != o.end(); ++it)
                    if (rng.bounded(3) == 0)
                        it.value() = self(self, it.value(), depth + 1);
                return o;
            }
            if (value.isArray()) {
                QJsonArray a = value.toArray();
                for (int i = 0; i < a.size(); ++i)
                    if (rng.bounded(3) == 0)
                        a[i] = self(self, a.at(i), depth + 1);
                return a;
            }
            return value;
        };
        const QJsonObject timeline = obj(R"({"name":"t","speed":1,"restore":true,"tracks":[{"pet":{"item":"a","variant":"b"},"startVisible":true,"actions":[
            {"kind":"move","x":1,"y":2,"mode":"smooth","durationMs":100},{"kind":"wait","ms":5},{"kind":"show","variant":"v"}]}]})");
        const QJsonObject trigger = obj(R"({"name":"t","enabled":true,"cooldownMs":500,"mode":"press","source":{"kind":"time","at":"10:00","days":[1,2]},"action":{"type":"scene","petId":"p","index":1}})");
        const QJsonObject profile = obj(R"({"name":"p","pets":[{"itemId":"a","variant":"v","output":"*","x":1,"y":2,"width":100,"slot":0}]})");
        int accepted = 0, refused = 0;
        for (int i = 0; i < 4000; ++i) {
            QString error;
            const QJsonObject t = mutate(mutate, timeline, 0).toObject();
            if (const auto parsed = applyTimelineChanges(Timeline{}, t, &error)) {
                QVERIFY(validateTimeline(*parsed));
                ++accepted;
            } else {
                QVERIFY(!error.isEmpty());
                ++refused;
            }
            const QJsonObject g = mutate(mutate, trigger, 0).toObject();
            Trigger base;
            if (const auto parsed = applyTriggerChanges(base, g, &error))
                QVERIFY(validateTrigger(*parsed));
            else
                QVERIFY(!error.isEmpty());
            const QJsonObject p = mutate(mutate, profile, 0).toObject();
            if (const auto parsed = applyProfileChanges(Profile{}, p, &error))
                QVERIFY(validateProfile(*parsed));
            else
                QVERIFY(!error.isEmpty());
            int skipped = 0;
            runtimeLayerFromJson(mutate(mutate, QJsonObject{{u"a"_s, QJsonObject{{u"hidden"_s, true}, {u"x"_s, 1}, {u"y"_s, 2}}}}, 0).toObject(), &skipped);
        }
        QVERIFY(accepted > 0 && refused > 0);
    }
};

QTEST_GUILESS_MAIN(TstAutomationModels)
#include "tst_automationmodels.moc"
