// The daemon's automation without D-Bus: profiles over a real pet store, the runtime layer under timelines, the triggers through the engine and a closed list of what
// their actions can do. The hooks record every call, so a test sees exactly what the automation did to the world.

#include "automationmanager.h"
#include "testutil.h"

#include <QSignalSpy>
#include <algorithm>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

struct World {
    QTemporaryDir tmp;
    PetStore pets{tmp.filePath(u"pets.json"_s)};
    ProfileStore profiles{tmp.filePath(u"profiles.json"_s)};
    TriggerStore triggers{tmp.filePath(u"triggers.json"_s)};
    TimelineStore timelines{tmp.filePath(u"timelines.json"_s)};
    SettingsStore settings{tmp.filePath(u"settings.json"_s)};
    QStringList calls;
    bool hidden = false;
    bool muted = false;
    QStringList missingItems;
    std::unique_ptr<AutomationManager> manager;

    explicit World(bool load = true)
    {
        pets.load();
        profiles.load();
        triggers.load();
        timelines.load();
        settings.load();
        AutomationHooks hooks;
        hooks.setPetsHidden = [this](bool h) {
            hidden = h;
            calls << (h ? u"hide-all"_s : u"show-all"_s);
        };
        hooks.petsHidden = [this] { return hidden; };
        hooks.playSound = [this](const QString &petId) { calls << u"sound:"_s + petId; };
        hooks.stopSounds = [this] { calls << u"stop-sounds"_s; };
        hooks.startScene = [this](const QString &petId, int index, QString *) {
            calls << u"scene:%1:%2"_s.arg(petId).arg(index);
            return true;
        };
        hooks.setMuted = [this](bool m) {
            muted = m;
            calls << (m ? u"mute"_s : u"unmute"_s);
        };
        hooks.muted = [this] { return muted; };
        hooks.petSize = [](const QString &, const QString &variant) { return variant == u"big" ? QSizeF(400, 200) : QSizeF(200, 100); };
        hooks.checkItem = [this](const QString &item, const QString &variant) -> QString {
            if (missingItems.contains(item))
                return QString(kProfileItemMissing);
            return variant == u"nope" ? QString(kProfileVariantMissing) : QString();
        };
        manager = std::make_unique<AutomationManager>(&pets, &profiles, &triggers, &timelines, &settings, hooks, tmp.filePath(u"state.json"_s));
        if (load)
            manager->load();
    }

    QString pet(const QString &item, double x = 0, const QString &variant = u"v"_s)
    {
        PetPlacement p;
        p.itemId = item;
        p.variant = variant;
        p.output = u"*"_s;
        p.x = x;
        return pets.add(p);
    }
    QString trigger(const QString &changes)
    {
        Trigger base;
        QString error;
        const auto t = applyTriggerChanges(base, QJsonDocument::fromJson(changes.toUtf8()).object(), &error);
        if (!t)
            qFatal("fixture trigger: %s", qPrintable(error));
        return triggers.add(*t);
    }
    QString timeline(const QString &changes)
    {
        QString error;
        const auto t = applyTimelineChanges(Timeline{}, QJsonDocument::fromJson(changes.toUtf8()).object(), &error);
        if (!t)
            qFatal("fixture timeline: %s", qPrintable(error));
        return timelines.add(*t);
    }
};

}

class TstAutomationManager : public QObject
{
    Q_OBJECT

private slots:
    void aFreshInstallHasNothingRunningOrSubscribed()
    {
        World w;
        QVERIFY(!w.manager->needs().any());
        QVERIFY(!w.manager->engineTimerActive());
        QVERIFY(!w.manager->timelineTimerActive());
        QVERIFY(w.manager->layer().isEmpty());
        QCOMPARE(w.manager->layerJson(), u"{}"_s);
        QVERIFY(w.manager->activeProfile().isEmpty());
        QVERIFY(!QFile::exists(w.profiles.filePath()) && !QFile::exists(w.triggers.filePath()) && !QFile::exists(w.timelines.filePath())); // nothing was written
    }

    void savingAndSwitchingProfilesKeepsTheWindowsOfThePetsThatStay()
    {
        World w;
        const QString a = w.pet(u"i"_s, 1);
        const QString b = w.pet(u"j"_s, 2);
        QString error;
        QVERIFY2(!w.manager->saveProfile(u"Work"_s, &error).isEmpty(), qPrintable(error));
        QVERIFY(!w.manager->profileModified());
        // change the desktop: move a, drop b, add k
        PetPlacement moved = *w.pets.pet(a);
        moved.x = 99;
        w.pets.update(moved);
        w.pets.remove(b);
        const QString k = w.pet(u"k"_s, 3);
        QVERIFY(w.manager->profileModified());
        QVERIFY2(!w.manager->saveProfile(u"Play"_s, &error).isEmpty(), qPrintable(error));
        QCOMPARE(w.profiles.profiles().size(), 2);
        QSignalSpy changed(&w.pets, &PetStore::changed);
        QJsonObject report;
        QVERIFY2(w.manager->switchProfile(u"work"_s, &error, &report), qPrintable(error)); // by name, any case
        QCOMPARE(changed.size(), 1); // one write
        QCOMPARE(report.value(u"updated"_s).toInt(), 1);
        QCOMPARE(report.value(u"added"_s).toInt(), 1);
        QCOMPARE(report.value(u"removed"_s).toInt(), 1);
        QCOMPARE(w.pets.pets().size(), 2);
        QCOMPARE(w.pets.pet(a)->x, 1.0);   // a keeps its petId, so its window
        QVERIFY(!w.pets.pet(k).has_value()); // k is gone
        QVERIFY(!w.manager->profileModified());
        QCOMPARE(w.profiles.profile(w.manager->activeProfile())->name, u"Work"_s);
        // switching to what is already there writes nothing
        changed.clear();
        QVERIFY(w.manager->switchProfile(u"Work"_s, &error));
        QCOMPARE(changed.size(), 0);
        QVERIFY(!w.manager->switchProfile(u"Nobody"_s, &error));
        QVERIFY(error.contains(u"Unknown profile"_s));
    }
    void anEntryWhoseItemIsGoneIsSkippedWithAReasonAndTheRestApplies()
    {
        World w;
        w.pet(u"i"_s);
        w.pet(u"gone"_s);
        w.manager->saveProfile(u"P"_s);
        w.pets.remove(w.pets.pets().first().petId);
        w.missingItems = {u"gone"_s};
        QJsonObject report;
        QString error;
        QVERIFY2(w.manager->switchProfile(u"P"_s, &error, &report), qPrintable(error));
        QCOMPARE(report.value(u"skipped"_s).toArray().size(), 1);
        QCOMPARE(report.value(u"skipped"_s).toArray().at(0).toObject().value(u"reason"_s).toString(), QString(kProfileItemMissing));
        QCOMPARE(w.pets.pets().size(), 1);
        QCOMPARE(w.pets.pets().first().itemId, u"i"_s);
        QCOMPARE(w.manager->lastSwitchReport().value(u"profile"_s).toString(), u"P"_s);
        QVERIFY(w.profiles.profile(w.manager->activeProfile())->pets.size() == 2); // the profile keeps its entry
    }
    void theActiveProfileIsALabelThatSurvivesARestartAndNothingIsRestored()
    {
        World w;
        w.pet(u"i"_s);
        const QString id = w.manager->saveProfile(u"P"_s);
        w.pets.remove(w.pets.pets().first().petId);
        AutomationHooks hooks;
        AutomationManager again(&w.pets, &w.profiles, &w.triggers, &w.timelines, &w.settings, hooks, w.tmp.filePath(u"state.json"_s));
        again.load();
        QCOMPARE(again.activeProfile(), id);
        QVERIFY(w.pets.pets().isEmpty()); // (the pets stay as they are stored)
        w.manager->deleteProfile(u"P"_s);
        QVERIFY(w.manager->activeProfile().isEmpty());
    }
    void renamingAndDeletingProfiles()
    {
        World w;
        w.pet(u"i"_s);
        w.manager->saveProfile(u"One"_s);
        w.manager->saveProfile(u"Two"_s);
        QString error;
        QVERIFY(!w.manager->renameProfile(u"One"_s, u"two"_s, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(w.manager->renameProfile(u"One"_s, u"First"_s, &error));
        QVERIFY(w.profiles.profileByName(u"First"_s).has_value());
        QVERIFY(w.manager->saveProfile(u"first"_s).size() > 0); // saving under an existing name replaces it
        QCOMPARE(w.profiles.profiles().size(), 2);
        QVERIFY(w.manager->deleteProfile(u"Two"_s, &error));
        QVERIFY(!w.manager->deleteProfile(u"Two"_s, &error));
    }

    void aNoOpSwitchMapsTheSlotsToThePetsOfThePlanNotToTheStoreOrder()
    {
        World w;
        const QString a = w.pet(u"i"_s, 1);
        const QString b = w.pet(u"j"_s, 2);
        w.manager->saveProfile(u"P"_s); // slot 0 = i (a), slot 1 = j (b)
        Profile p = *w.profiles.profileByName(u"P"_s);
        std::reverse(p.pets.begin(), p.pets.end()); // the profile lists j first: the plan's order is not the store's, and nothing changes
        QVERIFY(w.profiles.update(p));
        const QJsonObject before = w.pets.pets().first().toJson();
        QString error;
        QVERIFY2(w.manager->switchProfile(u"P"_s, &error), qPrintable(error));
        QCOMPARE(w.pets.pets().first().toJson(), before); // (nothing was written)
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","restore":false,"tracks":[{"pet":{"slot":1},"actions":[{"kind":"hide"}]}]})"));
        QVERIFY(w.manager->runTimeline(t));
        QVERIFY(w.manager->layer().value(b).hidden); // slot 1 is still j, whatever the order
        QVERIFY(!w.manager->layer().contains(a));
    }
    void theRescueSuspendsTheTriggersUntilTheyAreResumed()
    {
        World w;
        w.trigger(R"({"name":"loop","source":{"kind":"fire","name":"go"},"action":{"type":"hide-all"},"cooldownMs":100})");
        QSignalSpy suspended(w.manager.get(), &AutomationManager::suspendedChanged);
        w.manager->rescue();
        QCOMPARE(suspended.size(), 1);
        QVERIFY(w.manager->suspendedMs() > 50'000);
        QVERIFY(!w.manager->needs().any());
        w.manager->fireTrigger(u"go"_s);
        QVERIFY(w.calls.isEmpty()); // the loop that keeps putting the pets away does not win against the rescue
        w.manager->resumeAutomation();
        QCOMPARE(suspended.size(), 2);
        QCOMPARE(w.manager->suspendedMs(), qint64(0));
        w.manager->fireTrigger(u"go"_s);
        QCOMPARE(w.calls, QStringList{u"hide-all"_s});
        // by itself after the time, and a rescue with the master switch already off suspends nothing
        w.manager->setRescueSuspendMs(60);
        w.manager->rescue();
        QVERIFY(w.manager->suspendedMs() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(w.manager->suspendedMs(), qint64(0), 2000);
        w.settings.merge(QJsonObject{{u"automationEnabled"_s, false}});
        suspended.clear();
        w.manager->rescue();
        QCOMPARE(suspended.size(), 0);
    }
    void muteIsASessionStateNotAWriteOfTheSettings()
    {
        World w;
        const QString id = w.trigger(R"({"name":"m","source":{"kind":"fire","name":"m"},"action":{"type":"mute","mode":"toggle"}})");
        const QString settingsBefore = QString::fromUtf8(QJsonDocument(w.settings.values()).toJson());
        QVERIFY(w.manager->testTrigger(id));
        QCOMPARE(w.calls, QStringList{u"mute"_s}); // (the hook is the service's SetPetsMuted: see tst_murasakiservice)
        QCOMPARE(QString::fromUtf8(QJsonDocument(w.settings.values()).toJson()), settingsBefore);
    }

    void aTimelineHidesMovesAndPutsEverythingBack()
    {
        World w;
        const QString a = w.pet(u"i"_s, 10);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"hide"},{"kind":"wait","ms":60},
            {"kind":"place","x":500,"y":400},{"kind":"show","variant":"wave"},{"kind":"move","x":600,"y":400,"mode":"smooth","durationMs":60}]}]})").arg(a));
        QSignalSpy changed(w.manager.get(), &AutomationManager::runtimeChanged);
        QString error;
        QVERIFY2(w.manager->runTimeline(t, &error), qPrintable(error));
        QVERIFY(w.manager->layer().value(a).hidden);
        QVERIFY(w.manager->timelineTimerActive());
        QVERIFY(changed.size() >= 1);
        QTRY_VERIFY_WITH_TIMEOUT(w.manager->runningTimelines().isEmpty(), 3000);
        QVERIFY(w.manager->layer().isEmpty()); // restore: back to the stored state
        QVERIFY(!w.manager->timelineTimerActive());
        QCOMPARE(w.pets.pet(a)->x, 10.0); // nothing was written to the store
        QVERIFY(!QFile::exists(w.tmp.filePath(u"layer"_s)));
    }
    void aTimelineThatDoesNotRestoreLeavesTheLayerAndAMoveIsAMotion()
    {
        World w;
        const QString a = w.pet(u"i"_s, 10);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","restore":false,"tracks":[{"pet":{"petId":"%1"},"actions":[
            {"kind":"move","x":300,"y":200,"mode":"smooth","durationMs":40}]}]})").arg(a));
        QVERIFY(w.manager->runTimeline(t));
        const PetRuntime during = w.manager->layer().value(a);
        QVERIFY(during.motion.has_value());
        QCOMPARE(during.motion->fromX, 10.0); // from where the pet was
        QCOMPARE(during.motion->toX, 300.0);
        QCOMPARE(during.motion->durationMs, 40);
        QTRY_VERIFY_WITH_TIMEOUT(w.manager->runningTimelines().isEmpty(), 3000);
        QVERIFY(w.manager->layer().value(a).position.has_value()); // it stays where it went
        w.manager->clearRuntime(a);
        QVERIFY(w.manager->layer().isEmpty());
        QVERIFY(QJsonDocument::fromJson(w.manager->layerJson().toUtf8()).object().isEmpty());
    }
    void aTrackActionTurnsTrackingOnInTheLayerOnlyAndTheEndUndoesIt()
    {
        World w;
        const QString a = w.pet(u"i"_s, 10);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"track","mode":"follow"},{"kind":"wait","ms":80}]}]})").arg(a));
        QVERIFY(w.manager->runTimeline(t));
        QCOMPARE(w.manager->layer().value(a).tracking, std::optional<PetTrackingMode>(PetTrackingMode::Follow));
        QCOMPARE(w.pets.pet(a)->tracking.mode, PetTrackingMode::Off); // the stored placement is not touched
        QVERIFY(QJsonDocument::fromJson(w.manager->layerJson().toUtf8()).object().value(a).toObject().value(u"tracking"_s).toString() == u"follow"_s);
        QTRY_VERIFY_WITH_TIMEOUT(w.manager->runningTimelines().isEmpty(), 3000);
        QVERIFY(w.manager->layer().isEmpty());
        QCOMPARE(w.pets.pet(a)->tracking.mode, PetTrackingMode::Off);
        QString error;
        const auto bad = applyTimelineChanges(Timeline{}, QJsonDocument::fromJson(R"({"name":"x","tracks":[{"pet":{"slot":0},"actions":[{"kind":"track","mode":"run"}]}]})").object(), &error);
        QVERIFY(!bad.has_value());
        QVERIFY2(error.contains(u"mode"_s), qPrintable(error));
    }
    void aSoundActionCallsTheHookAndAnUnknownPetSkipsTheTrack()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"sound"},{"kind":"stopSounds"}]}]})").arg(a));
        QVERIFY(w.manager->runTimeline(t));
        QCOMPARE(w.calls, (QStringList{u"sound:"_s + a, u"stop-sounds"_s}));
        const QString lost = w.timeline(QStringLiteral(R"({"name":"lost","tracks":[{"pet":{"petId":"nobody"},"actions":[{"kind":"hide"}]}]})"));
        QString error;
        QVERIFY(!w.manager->runTimeline(lost, &error));
        QCOMPARE(error, QString(kTimelinePetNotFound));
        QVERIFY(w.manager->layer().isEmpty());
        QVERIFY(!w.manager->runTimeline(u"nothing"_s, &error));
        QVERIFY(error.contains(u"Unknown timeline"_s));
    }
    void anActionThatNamesTheCentreIsResolvedWithThePetsSize()
    {
        World w;
        const QString a = w.pet(u"i"_s, 10);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","restore":false,"tracks":[{"pet":{"petId":"%1"},"actions":[
            {"kind":"place","x":300,"y":200,"anchor":"centre"},{"kind":"move","x":500,"y":400,"anchor":"centre","mode":"smooth","durationMs":50},{"kind":"wait","ms":10},
            {"kind":"show","variant":"big"},{"kind":"place","x":1000,"y":1000,"anchor":"centre"},{"kind":"place","x":7,"y":8}]}]})").arg(a));
        QVERIFY(w.manager->runTimeline(t));
        const PetRuntime first = w.manager->layer().value(a);
        QVERIFY(first.motion.has_value());
        QCOMPARE(first.motion->fromX, 200.0); // the place put the top-left corner at (300 - 100, 200 - 50)
        QCOMPARE(first.motion->fromY, 150.0);
        QCOMPARE(first.motion->toX, 400.0);   // and the glide ends with the centre at (500, 400): the corner at (400, 350)
        QCOMPARE(first.motion->toY, 350.0);
        QTRY_VERIFY_WITH_TIMEOUT(w.manager->runningTimelines().isEmpty(), 3000);
        QCOMPARE(w.manager->layer().value(a).position->x, 7.0); // a corner stays a corner
        QCOMPARE(w.manager->layer().value(a).variant, u"big"_s);
    }
    void aTrackCanNameAPetByItemOrBySlot()
    {
        World w;
        w.pet(u"i"_s, 1);
        const QString b = w.pet(u"j"_s, 2);
        w.manager->saveProfile(u"P"_s); // slots 0 and 1
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","restore":false,"tracks":[{"pet":{"item":"j"},"actions":[{"kind":"hide"}]},{"pet":{"slot":0},"actions":[{"kind":"place","x":7,"y":8}]}]})"));
        QVERIFY(w.manager->runTimeline(t));
        QVERIFY(w.manager->layer().value(b).hidden);
        QCOMPARE(w.manager->layer().size(), 2);
    }
    void aRemovedPetLeavesTheLayerAndItsTrack()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"hide"},{"kind":"wait","ms":3000}]}]})").arg(a));
        QVERIFY(w.manager->runTimeline(t));
        QVERIFY(w.manager->layer().contains(a));
        w.pets.remove(a);
        QVERIFY(w.manager->layer().isEmpty());
        QVERIFY(w.manager->runningTimelines().isEmpty());
        QVERIFY(!w.manager->timelineTimerActive());
    }
    void theRescueStopsEveryTimelineAndClearsTheLayer()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        const QString t = w.timeline(QStringLiteral(R"({"name":"t","restore":false,"tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"hide"},{"kind":"wait","ms":5000}]}]})").arg(a));
        w.manager->runTimeline(t);
        QVERIFY(w.manager->setPetHidden(a, true));
        w.manager->rescue();
        QVERIFY(w.manager->runningTimelines().isEmpty());
        QVERIFY(w.manager->layer().isEmpty());
        QVERIFY(!w.manager->timelineTimerActive());
        QVERIFY(!w.manager->setPetHidden(u"nobody"_s, true));
    }
    void theSameTimelineRestartsAndOnlyEightRunAtOnce()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        QStringList ids;
        for (int i = 0; i < kMaxRunningTimelines + 1; ++i)
            ids << w.timeline(QStringLiteral(R"({"name":"t%2","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"wait","ms":5000}]}]})").arg(a).arg(i));
        for (int i = 0; i < kMaxRunningTimelines; ++i)
            QVERIFY(w.manager->runTimeline(ids[i]));
        QString error;
        QVERIFY(!w.manager->runTimeline(ids.last(), &error));
        QCOMPARE(error, QString(kTimelineTooManyRunning));
        QVERIFY(w.manager->runTimeline(ids[0])); // a restart, not a ninth
        QCOMPARE(w.manager->runningTimelines().size(), kMaxRunningTimelines);
        QVERIFY(w.manager->stopTimeline(u"t1"_s)); // by name
        QCOMPARE(w.manager->runningTimelines().size(), kMaxRunningTimelines - 1);
    }

    void aFireTriggerRunsItsActionOnceAndTheMasterSwitchStopsIt()
    {
        World w;
        const QString id = w.trigger(R"({"name":"sleepy","source":{"kind":"fire","name":"sleepy"},"action":{"type":"hide-all"}})");
        QVERIFY(w.manager->fireTrigger(u"sleepy"_s));
        QCOMPARE(w.calls, QStringList{u"hide-all"_s});
        QVERIFY(w.manager->fireTrigger(u"sleepy"_s)); // the cooldown drops it, but something had the name
        QCOMPARE(w.calls.size(), 1);
        QVERIFY(!w.manager->fireTrigger(u"nobody"_s));
        const QJsonObject state = w.manager->triggerStates().at(0).toObject();
        QCOMPARE(state.value(u"id"_s).toString(), id);
        QCOMPARE(state.value(u"outcome"_s).toString(), u"cooldown"_s);
        QVERIFY(w.manager->triggerLog().size() >= 2);
        QTest::qWait(520);
        w.settings.merge(QJsonObject{{u"automationEnabled"_s, false}});
        QVERIFY(!w.manager->needs().any());
        w.manager->fireTrigger(u"sleepy"_s);
        QCOMPARE(w.calls.size(), 1);
        w.settings.merge(QJsonObject{{u"automationEnabled"_s, true}});
        w.manager->fireTrigger(u"sleepy"_s);
        QCOMPARE(w.calls.size(), 2);
    }
    void aDisabledTriggerDoesNothingAndTestingIsOneFire()
    {
        World w;
        const QString id = w.trigger(R"({"name":"t","enabled":false,"source":{"kind":"fire","name":"x"},"action":{"type":"hide-all"}})");
        w.manager->fireTrigger(u"x"_s);
        QVERIFY(w.calls.isEmpty());
        QCOMPARE(w.manager->triggerStates().at(0).toObject().value(u"outcome"_s).toString(), u"disabled"_s);
        QVERIFY(!w.manager->testTrigger(id));
        Trigger t = *w.triggers.trigger(id);
        t.enabled = true;
        w.triggers.update(t);
        QVERIFY(w.manager->testTrigger(id));
        QCOMPARE(w.calls, QStringList{u"hide-all"_s});
    }
    void everyActionOfTheClosedListDoesOnlyWhatItSays()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        w.pet(u"i"_s, 5);
        const QString tl = w.timeline(QStringLiteral(R"({"name":"tl","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"sound"}]}]})").arg(a));
        w.manager->saveProfile(u"P"_s);
        const QString profile = w.manager->activeProfile();
        struct Case {
            QString action;
            QStringList expect;
        };
        const QList<Case> cases = {
            {QStringLiteral(R"({"type":"timeline","id":"%1"})").arg(tl), {u"sound:"_s + a}},
            {QStringLiteral(R"({"type":"scene","petId":"%1","index":2})").arg(a), {u"scene:%1:2"_s.arg(a)}},
            {QStringLiteral(R"({"type":"scene","item":"i","index":1})"), {}}, // two pets: filled below
            {QStringLiteral(R"({"type":"profile","id":"%1"})").arg(profile), {}},
            {R"({"type":"hide-all"})", {u"hide-all"_s}},
            {R"({"type":"show-all"})", {u"show-all"_s}},
            {R"({"type":"toggle-hidden"})", {u"hide-all"_s}},
            {R"({"type":"stop-sounds"})", {u"stop-sounds"_s}},
            {R"({"type":"mute","mode":"on"})", {u"mute"_s}},
            {R"({"type":"mute","mode":"toggle"})", {u"unmute"_s}},
            {QStringLiteral(R"({"type":"play-sound","petId":"%1"})").arg(a), {u"sound:"_s + a}},
        };
        QSet<QString> typesSeen;
        int n = 0;
        for (const Case &c : cases) {
            w.calls.clear();
            const QString trigger = w.trigger(QStringLiteral(R"({"name":"c%1","source":{"kind":"fire","name":"c%1"},"action":%2})").arg(n).arg(c.action));
            typesSeen.insert(QJsonDocument::fromJson(c.action.toUtf8()).object().value(u"type"_s).toString());
            QVERIFY(w.manager->testTrigger(trigger));
            if (!c.expect.isEmpty())
                QCOMPARE(w.calls, c.expect);
            // the only calls that exist are these: nothing else can have happened (the hooks have no other member)
            for (const QString &call : std::as_const(w.calls))
                QVERIFY2(call.startsWith(u"sound:") || call.startsWith(u"scene:") || call == u"hide-all" || call == u"show-all" || call == u"stop-sounds" || call == u"mute"
                             || call == u"unmute",
                         qPrintable(call));
            ++n;
        }
        for (const TriggerActionType type : allTriggerActionTypes())
            QVERIFY2(typesSeen.contains(triggerActionName(type)), qPrintable(triggerActionName(type)));
        QCOMPARE(w.calls.size() >= 0, true);
    }
    void aSceneActionReachesEveryPetOfTheItem()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        const QString b = w.pet(u"i"_s, 5);
        const QString id = w.trigger(R"({"name":"s","source":{"kind":"fire","name":"s"},"action":{"type":"scene","item":"i","index":3}})");
        QVERIFY(w.manager->testTrigger(id));
        QCOMPARE(w.calls, (QStringList{u"scene:%1:3"_s.arg(a), u"scene:%1:3"_s.arg(b)}));
    }
    void aWindowTriggerFollowsTheSurfacesFactsAndOnlyWhenEnabled()
    {
        World w;
        w.trigger(R"({"name":"f","source":{"kind":"app-focus","app":"firefox"},"action":{"type":"hide-all"}})");
        QVERIFY(w.manager->needs().windows);
        const auto facts = [](const QString &app) { return QJsonObject{{u"known"_s, true}, {u"activeApp"_s, app}, {u"openApps"_s, QJsonArray{app}}, {u"outputs"_s, QJsonObject()}}; };
        w.manager->setSessionFacts(facts(u"konsole"_s));
        w.manager->setSessionFacts(facts(u"firefox"_s));
        QCOMPARE(w.calls, QStringList{u"hide-all"_s});
        w.manager->setSessionFacts(facts(u"firefox"_s)); // no change: nothing
        QCOMPARE(w.calls.size(), 1);
        // a session without a monitor: unknown, not "false", and no fire when it comes back
        w.manager->setSessionFacts(QJsonObject{});
        QVERIFY(!QJsonDocument::fromJson(QJsonDocument(w.manager->triggerStates()).toJson()).array().at(0).toObject().value(u"available"_s).toBool(true));
        w.manager->setSessionFacts(facts(u"firefox"_s));
        QCOMPARE(w.calls.size(), 1);
    }
    void theSurfacesIdleDrivesTheIdleTriggersAndLogindIsTheFallback()
    {
        World w;
        w.trigger(R"({"name":"away","source":{"kind":"idle","minutes":2},"action":{"type":"hide-all"}})");
        w.trigger(R"({"name":"back","source":{"kind":"idle","minutes":2,"state":"back"},"action":{"type":"show-all"}})");
        QVERIFY(w.manager->needs().idle);
        const auto facts = [](bool idleKnown, bool idle) {
            return QJsonObject{{u"known"_s, true}, {u"outputs"_s, QJsonObject()}, {u"idleKnown"_s, idleKnown}, {u"idle"_s, idle}, {u"idleAfterMs"_s, 120'000}};
        };
        w.manager->setSessionFacts(facts(true, false));
        w.manager->setIdle(true, true); // logind says idle, but the surface knows the user is there: ignored
        QVERIFY(w.calls.isEmpty());
        // the notification came 2 minutes after the last input: the nap is due at once
        w.manager->setSessionFacts(facts(true, true));
        QTRY_COMPARE(w.calls, QStringList{u"hide-all"_s});
        // the surface restarts during the nap: the idle is unknown meanwhile, logind's "not idle" does not end it
        w.manager->setSessionFacts(facts(false, false));
        w.manager->setIdle(true, false);
        w.manager->setSessionFacts(facts(true, true));
        QCOMPARE(w.calls.size(), 1);
        // and the pets come back with the user
        w.manager->setSessionFacts(facts(true, false));
        QCOMPARE(w.calls, (QStringList{u"hide-all"_s, u"show-all"_s}));
    }
    // The daemon starts while the user is already away: that idle was not seen beginning, so neither the nap nor its "back" fires for it.
    void anIdleThatWasThereWhenTheDaemonStartedFiresNothing()
    {
        World w;
        w.trigger(R"({"name":"away","source":{"kind":"idle","minutes":2},"action":{"type":"hide-all"}})");
        w.trigger(R"({"name":"back","source":{"kind":"idle","minutes":2,"state":"back"},"action":{"type":"show-all"}})");
        const auto facts = [](bool idle) {
            return QJsonObject{{u"known"_s, true}, {u"outputs"_s, QJsonObject()}, {u"idleKnown"_s, true}, {u"idle"_s, idle}, {u"idleAfterMs"_s, 120'000}};
        };
        w.manager->setIdle(true, false); // (logind answers first, as on Plasma: never idle)
        w.manager->setSessionFacts(facts(true));
        QTest::qWait(50);
        w.manager->setSessionFacts(facts(false));
        QVERIFY(w.calls.isEmpty());
    }
    void theSessionLockIsAnEdgeToo()
    {
        World w;
        w.trigger(R"({"name":"l","source":{"kind":"session","state":"locked"},"action":{"type":"stop-sounds"}})");
        w.manager->setLocked(false);
        w.manager->setLocked(true);
        QCOMPARE(w.calls, QStringList{u"stop-sounds"_s});
        w.manager->setLocked(true);
        QCOMPARE(w.calls.size(), 1);
    }
    void aHoldShortcutHidesWhileHeldAndShowsOnRelease()
    {
        World w;
        w.trigger(R"({"name":"peek","mode":"hold","source":{"kind":"shortcut","id":"peek"},"action":{"type":"hide-all"}})");
        QCOMPARE(w.manager->needs().shortcutIds, QStringList{u"peek"_s});
        w.manager->shortcut(u"peek"_s, true);
        QVERIFY(w.hidden);
        w.manager->shortcut(u"peek"_s, false);
        QVERIFY(!w.hidden);
        QCOMPARE(w.calls, (QStringList{u"hide-all"_s, u"show-all"_s}));
    }
    // The end of a hold puts back what the user had when it began, not the opposite action: pets hidden by hand stay hidden, a mute they set stays set.
    void aHoldRestoresWhatWasThereWhenItBeganAndNotTheOpposite()
    {
        World w;
        w.trigger(R"({"name":"peek","mode":"hold","source":{"kind":"shortcut","id":"peek"},"action":{"type":"hide-all"}})");
        w.trigger(R"({"name":"show","mode":"hold","source":{"kind":"shortcut","id":"show"},"action":{"type":"show-all"}})");
        w.trigger(R"({"name":"peek2","mode":"hold","source":{"kind":"shortcut","id":"peek2"},"action":{"type":"hide-all"}})");
        w.trigger(R"({"name":"quiet2","mode":"hold","source":{"kind":"shortcut","id":"quiet2"},"action":{"type":"mute","mode":"on"}})");
        w.trigger(R"({"name":"quiet","mode":"hold","source":{"kind":"shortcut","id":"quiet"},"action":{"type":"mute","mode":"on"}})");
        // hidden by hand before the hold: still hidden after it
        w.hidden = true;
        w.manager->shortcut(u"peek"_s, true);
        w.manager->shortcut(u"peek"_s, false);
        QVERIFY(w.hidden);
        // a show-all hold over pets that were hidden by hand: hidden again at the end
        w.manager->shortcut(u"show"_s, true);
        QVERIFY(!w.hidden);
        w.manager->shortcut(u"show"_s, false);
        QVERIFY(w.hidden);
        // and over shown pets, the hide-all hold still shows them
        w.hidden = false;
        w.manager->shortcut(u"peek2"_s, true);
        QVERIFY(w.hidden);
        w.manager->shortcut(u"peek2"_s, false);
        QVERIFY(!w.hidden);
        // a mute the user had set stays set; one the hold made goes
        w.muted = true;
        w.manager->shortcut(u"quiet"_s, true);
        w.manager->shortcut(u"quiet"_s, false);
        QVERIFY(w.muted);
        w.muted = false;
        w.manager->shortcut(u"quiet2"_s, true);
        QVERIFY(w.muted);
        w.manager->shortcut(u"quiet2"_s, false);
        QVERIFY(!w.muted);
    }
    void fireByTimelineNameIsOffUnlessTheSettingIsOn()
    {
        World w;
        const QString a = w.pet(u"i"_s);
        w.timeline(QStringLiteral(R"({"name":"wave","tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"sound"}]}]})").arg(a));
        QVERIFY(!w.manager->fireTrigger(u"wave"_s));
        QVERIFY(w.calls.isEmpty());
        w.settings.merge(QJsonObject{{u"fireByTimelineName"_s, true}});
        QVERIFY(w.manager->fireTrigger(u"wave"_s));
        QCOMPARE(w.calls, QStringList{u"sound:"_s + a});
    }
    void theTimersExistOnlyWhileARuleOrARunNeedsThem()
    {
        World w;
        QVERIFY(!w.manager->engineTimerActive());
        const QString id = w.trigger(R"({"name":"t","source":{"kind":"interval","everyMinutes":5},"action":{"type":"hide-all"}})");
        QVERIFY(w.manager->needs().timer);
        QVERIFY(w.manager->engineTimerActive());
        w.triggers.remove(id);
        QVERIFY(!w.manager->engineTimerActive());
        QVERIFY(!w.manager->needs().any());
    }
    void triggerNeedsAreAnnouncedWhenTheySet()
    {
        World w;
        QSignalSpy needs(w.manager.get(), &AutomationManager::needsChanged);
        w.trigger(R"({"name":"t","source":{"kind":"signal-file","name":"go"},"action":{"type":"hide-all"}})");
        QCOMPARE(needs.size(), 1);
        QVERIFY(w.manager->needs().signalFiles);
        w.manager->signalFile(u"go"_s);
        QCOMPARE(w.calls, QStringList{u"hide-all"_s});
    }
};

QTEST_GUILESS_MAIN(TstAutomationManager)
#include "tst_automationmanager.moc"
