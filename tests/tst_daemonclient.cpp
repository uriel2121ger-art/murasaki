#include "daemonclient.h"
#include "fakebackend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace Qt::StringLiterals;
using murasaki::DaemonClient;

class TstDaemonClient : public QObject
{
    Q_OBJECT

private:
    static FakeBackend *makeBackend()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->setProperty(u"Status"_s, u"ok"_s);
        backend->setProperty(u"WallpaperEngineAssetsDir"_s, u"/we/assets"_s);
        backend->answerJson(u"ListItems"_s, R"([{"id":"we:1","title":"One"}])");
        backend->answerJson(u"Assignments"_s, R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        backend->answerJson(u"Settings"_s, R"({"webFpsLimit":30,"pauseWhenCovered":true})");
        return backend;
    }

private slots:
    void theExamplesUseTheExistingCallsAndMarkWhatTheyMake()
    {
        const auto scripted = [](int version, const QByteArray &pets) {
            FakeBackend *backend = makeBackend();
            backend->setProperty(u"ApiVersion"_s, quint32(version));
            backend->setProperty(u"ActiveProfile"_s, QString());
            backend->setProperty(u"ProfileModified"_s, false);
            backend->answerJson(u"ListPets"_s, pets);
            // the user's own timeline has the very name the example uses, and no marker: it must never be taken for the example
            backend->answerJson(u"ListProfiles"_s, R"([{"id":"p1","name":"Example: my desktop","pets":[]},{"id":"p2","name":"Example: mine","pets":[],"origin":"demo"}])");
            backend->answerJson(u"ListTriggers"_s, R"([{"id":"t1","name":"Example: a key","enabled":true,"source":{"kind":"fire","name":"x"},"action":{"type":"hide-all"}},
                {"id":"t2","name":"x","enabled":true,"origin":"demo","source":{"kind":"shortcut","id":"murasaki-demo"},"action":{"type":"hide-all"}}])");
            backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[]})");
            backend->answerJson(u"ListTimelines"_s, R"([{"id":"l1","name":"Example: pets across the screen","tracks":[]},{"id":"l2","name":"Example: other","tracks":[],"origin":"demo"}])");
            backend->answer(u"RunningTimelines"_s, QStringList());
            backend->answer(u"SetTimeline"_s, u"l9"_s);
            backend->answer(u"SetTrigger"_s, u"t9"_s);
            backend->answer(u"SaveExampleProfile"_s, u"p9"_s);
            backend->answerJson(u"Settings"_s, R"({"demoCardsDismissed":["triggers"]})");
            backend->answer(u"SetSettings"_s, QVariant());
            for (const char *method : {"RunTimeline", "StopTimeline", "DeleteTimeline", "DeleteTrigger", "DeleteProfile"})
                backend->answer(QString::fromLatin1(method), QVariant());
            return backend;
        };
        const QVariantMap options{{u"names"_s, QVariantMap{{u"timeline"_s, u"Example: pets across the screen"_s}, {u"trigger"_s, u"Example: a key"_s}, {u"profile"_s, u"Example: my desktop"_s}, {u"tracking"_s, u"Ejemplo: seguir"_s}}},
                                  {u"screenWidth"_s, 1920}, {u"screenHeight"_s, 1080}};
        const QByteArray twoPets = R"([{"petId":"pet1","itemId":"a","variant":"idle","x":100,"y":300,"width":200},{"petId":"pet2","itemId":"b","variant":"idle","x":500,"y":500,"width":200}])";

        // an older daemon: said once, nothing sent
        FakeBackend *old = scripted(20, twoPets);
        DaemonClient oldClient(old);
        QSignalSpy oldErrors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.automationSupported());
        QVERIFY(!oldClient.demosSupported());
        oldClient.runDemo(u"timelines"_s, options);
        oldClient.removeDemos();
        QCOMPARE(oldErrors.size(), 2);
        for (const char *method : {"SetTimeline", "SetTrigger", "SaveProfile", "DeleteTimeline"})
            QCOMPARE(old->callCount(QString::fromLatin1(method)), 0);

        FakeBackend *backend = scripted(22, twoPets);
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.demosSupported());
        QTRY_COMPARE(client.pets().size(), 2);
        QTRY_COMPARE(client.timelines().size(), 2);
        QTRY_COMPARE(client.triggers().size(), 2);
        QTRY_COMPARE(client.profiles().size(), 2);
        QVERIFY(client.hasDemos()); // the marked ones, and only they, make the buttons show
        const auto lastCall = [&](const QString &method) {
            for (int i = backend->calls.size() - 1; i >= 0; --i)
                if (backend->calls.at(i).first == method)
                    return backend->calls.at(i).second;
            return QVariantList();
        };
        QSignalSpy done(&client, &DaemonClient::demoDone);
        QSignalSpy needsPet(&client, &DaemonClient::demoNeedsPet);

        client.runDemo(u"timelines"_s, options);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.last().at(0).toString(), u"timelines"_s);
        const QVariantMap timeline = done.last().at(1).toMap().value(u"timeline"_s).toMap();
        QCOMPARE(timeline.value(u"id"_s).toString(), u"l9"_s);
        QCOMPARE(timeline.value(u"name"_s).toString(), u"Example: pets across the screen (2)"_s); // the user's own timeline of that name stays and is not adopted
        QCOMPARE(timeline.value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(lastCall(u"SetTimeline"_s).at(0).toString(), QString()); // (a new one)
        QCOMPARE(QJsonDocument::fromJson(lastCall(u"SetTimeline"_s).at(1).toString().toUtf8()).object().value(u"tracks"_s).toArray().size(), 2);
        QCOMPARE(backend->callCount(u"RunTimeline"_s), 0); // the page shows the steps first and plays on request

        client.runDemo(u"triggers"_s, options);
        QTRY_COMPARE(done.size(), 2);
        const QVariantMap made = done.last().at(1).toMap();
        QCOMPARE(made.value(u"trigger"_s).toString(), u"t9"_s);
        QCOMPARE(made.value(u"timeline"_s).toString(), u"l9"_s);
        const QJsonObject trigger = QJsonDocument::fromJson(lastCall(u"SetTrigger"_s).at(1).toString().toUtf8()).object();
        QCOMPARE(trigger.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"shortcut"_s);
        QCOMPARE(trigger.value(u"action"_s).toObject().value(u"id"_s).toString(), u"l9"_s);
        QCOMPARE(trigger.value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(lastCall(u"SetTrigger"_s).at(0).toString(), QString()); // (the user's trigger "Example: a key" has no marker: a new one, named apart)
        QCOMPARE(trigger.value(u"name"_s).toString(), u"Example: a key (2)"_s);

        client.runDemo(u"profiles"_s, options);
        QTRY_COMPARE(done.size(), 3);
        QCOMPARE(lastCall(u"SaveExampleProfile"_s), QVariantList{u"Example: my desktop (2)"_s}); // (the user's profile of that name is never taken)
        QCOMPARE(done.last().at(1).toMap().value(u"profile"_s).toString(), u"p9"_s);
        QCOMPARE(backend->callCount(u"SwitchProfile"_s), 0); // switching to it is the user's choice

        QVariantMap trackingOptions = options;
        trackingOptions.insert(u"petId"_s, u"pet2"_s);
        client.runDemo(u"tracking"_s, trackingOptions);
        QTRY_COMPARE(done.size(), 4);
        QCOMPARE(done.last().at(1).toMap().value(u"petId"_s).toString(), u"pet2"_s);
        QCOMPARE(done.last().at(1).toMap().value(u"seconds"_s).toInt(), 20);
        QCOMPARE(lastCall(u"RunTimeline"_s), QVariantList{u"l9"_s});
        const QJsonObject follow = QJsonDocument::fromJson(lastCall(u"SetTimeline"_s).at(1).toString().toUtf8()).object();
        QCOMPARE(follow.value(u"tracks"_s).toArray().at(0).toObject().value(u"pet"_s).toObject().value(u"petId"_s).toString(), u"pet2"_s);
        QCOMPARE(follow.value(u"tracks"_s).toArray().at(0).toObject().value(u"actions"_s).toArray().at(0).toObject().value(u"kind"_s).toString(), u"track"_s);

        // removing: exactly the marked ones, the user's with the same kind of name stay
        QSignalSpy removed(&client, &DaemonClient::demosRemoved);
        client.removeDemos();
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toMap(), (QVariantMap{{u"triggers"_s, 1}, {u"timelines"_s, 1}, {u"profiles"_s, 1}}));
        QCOMPARE(backend->callCount(u"DeleteTrigger"_s), 1);
        QCOMPARE(backend->callCount(u"DeleteTimeline"_s), 1);
        QCOMPARE(backend->callCount(u"DeleteProfile"_s), 1);
        QTRY_VERIFY(backend->callCount(u"StopTimeline"_s) >= 1);
        for (const auto &call : std::as_const(backend->calls)) {
            if (call.first == u"DeleteTrigger"_s)
                QCOMPARE(call.second, QVariantList{u"t2"_s});
            if (call.first == u"DeleteTimeline"_s)
                QCOMPARE(call.second, QVariantList{u"l2"_s});
            if (call.first == u"DeleteProfile"_s)
                QCOMPARE(call.second, QVariantList{u"p2"_s});
        }
        // closing a card reads the settings again and adds to what the daemon has
        client.dismissDemoCard(u"timelines"_s);
        QTRY_VERIFY(backend->callCount(u"SetSettings"_s) == 1);
        QCOMPARE(QJsonDocument::fromJson(lastCall(u"SetSettings"_s).at(0).toString().toUtf8()).object().value(u"demoCardsDismissed"_s).toArray(),
                 (QJsonArray{u"triggers"_s, u"timelines"_s}));
        // an older daemon refuses the page name "screen": no message (the page hides the card for the session)
        QSignalSpy refusals(&client, &DaemonClient::error);
        backend->fail(u"SetSettings"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"demoCardsDismissed holds timelines, triggers or profiles"_s);
        client.dismissDemoCard(u"screen"_s);
        QTRY_VERIFY(backend->callCount(u"SetSettings"_s) == 2);
        QTest::qWait(50);
        QCOMPARE(refusals.size(), 0);
        // no example ever edits the stored pets
        for (const char *method : {"SaveProfile", "SwitchProfile", "SetProfileOrigin", "AddPet", "UpdatePet", "RemovePet", "RescuePets", "SetPetsArrange", "SetAssignment"})
            QCOMPARE(backend->callCount(QString::fromLatin1(method)), 0);
    }

    void anExampleWithoutPetsSaysSoAndSendsNothing()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 22u);
        backend->setProperty(u"ActiveProfile"_s, QString());
        backend->setProperty(u"ProfileModified"_s, false);
        backend->answerJson(u"ListPets"_s, "[]");
        backend->answerJson(u"ListProfiles"_s, "[]");
        backend->answerJson(u"ListTriggers"_s, "[]");
        backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[]})");
        backend->answerJson(u"ListTimelines"_s, "[]");
        backend->answer(u"RunningTimelines"_s, QStringList());
        DaemonClient client(backend);
        QSignalSpy needsPet(&client, &DaemonClient::demoNeedsPet);
        QSignalSpy done(&client, &DaemonClient::demoDone);
        backend->setAvailable(true);
        QTRY_VERIFY(client.demosSupported());
        QVERIFY(!client.hasDemos());
        for (const QString &kind : {u"timelines"_s, u"triggers"_s, u"profiles"_s, u"tracking"_s})
            client.runDemo(kind, {});
        QCOMPARE(needsPet.size(), 4);
        QCOMPARE(done.size(), 0);
        for (const char *method : {"SetTimeline", "SetTrigger", "SaveProfile", "RunTimeline"})
            QCOMPARE(backend->callCount(QString::fromLatin1(method)), 0);
    }

    void theScreenExampleIsOffHasNoPictureAndIsNeverAnEarlierOne()
    {
        const auto scripted = [](uint api, const QByteArray &pets, const QByteArray &triggers) {
            FakeBackend *backend = makeBackend();
            backend->setProperty(u"ApiVersion"_s, api);
            backend->setProperty(u"ActiveProfile"_s, QString());
            backend->setProperty(u"ProfileModified"_s, false);
            backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"off\"}"_s);
            backend->answerJson(u"ListPets"_s, pets);
            backend->answerJson(u"ListProfiles"_s, "[]");
            backend->answerJson(u"ListTriggers"_s, triggers);
            backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[]})");
            backend->answerJson(u"ListTimelines"_s, "[]");
            backend->answer(u"RunningTimelines"_s, QStringList());
            backend->answer(u"SetTrigger"_s, u"t9"_s);
            return backend;
        };
        const QVariantMap options{{u"names"_s, QVariantMap{{u"screen"_s, u"Example: hide"_s}}}};
        const QByteArray pets = R"([{"petId":"pet1","itemId":"a","variant":"idle","x":100,"y":300,"width":200}])";
        const QByteArray noTriggers = "[]";

        // before revision 22 (the examples) and before 21 (the screen): said, nothing sent
        for (const uint api : {20u, 21u}) {
            FakeBackend *old = scripted(api, pets, noTriggers);
            DaemonClient oldClient(old);
            QSignalSpy errors(&oldClient, &DaemonClient::error);
            old->setAvailable(true);
            QTRY_VERIFY(oldClient.automationSupported());
            oldClient.runDemo(u"screen"_s, options);
            QCOMPARE(errors.size(), 1);
            QCOMPARE(old->callCount(u"SetTrigger"_s), 0);
        }

        // no pet: nothing to hide
        {
            FakeBackend *backend = scripted(22, "[]", noTriggers);
            DaemonClient client(backend);
            QSignalSpy needsPet(&client, &DaemonClient::demoNeedsPet);
            backend->setAvailable(true);
            QTRY_VERIFY(client.demosSupported());
            client.runDemo(u"screen"_s, options);
            QCOMPARE(needsPet.size(), 1);
            QCOMPARE(backend->callCount(u"SetTrigger"_s), 0);
        }

        // the example: a new screen trigger that is off, marked, with a region and without any picture; the user's own trigger of the same name stays
        const QByteArray userOwn = R"([{"id":"u1","name":"Example: hide","enabled":true,"source":{"kind":"fire","name":"x"},"action":{"type":"hide-all"}}])";
        FakeBackend *backend = scripted(22, pets, userOwn);
        DaemonClient client(backend);
        QSignalSpy done(&client, &DaemonClient::demoDone);
        backend->setAvailable(true);
        QTRY_VERIFY(client.demosSupported());
        QTRY_COMPARE(client.pets().size(), 1);
        client.runDemo(u"screen"_s, options);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.last().at(1).toMap().value(u"trigger"_s).toString(), u"t9"_s);
        QCOMPARE(done.last().at(1).toMap().value(u"existing"_s).toBool(), false);
        const QJsonObject made = QJsonDocument::fromJson(backend->calls.last().second.at(1).toString().toUtf8()).object();
        QCOMPARE(backend->calls.last().second.at(0).toString(), QString()); // (a new one)
        QCOMPARE(made.value(u"name"_s).toString(), u"Example: hide (2)"_s);
        QCOMPARE(made.value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(made.value(u"enabled"_s).toBool(), true == false);
        QCOMPARE(made.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"screen"_s);
        QVERIFY(!QJsonDocument(made).toJson().contains("png"));

        // a second "Try it": the one made before may be the user's work by now (kept, shaped): the walk makes ANOTHER one under a free name and touches nothing of it
        const int sets = backend->callCount(u"SetTrigger"_s);
        backend->answerJson(u"ListTriggers"_s, R"json([{"id":"u1","name":"Example: hide","enabled":true,"source":{"kind":"fire","name":"x"},"action":{"type":"hide-all"}},
            {"id":"t9","name":"Example: hide (2)","enabled":true,"origin":"demo","source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":1,"y":2,"w":30,"h":40}]}},"action":{"type":"hide-all"}}])json");
        backend->answer(u"SetTrigger"_s, u"t10"_s);
        client.runDemo(u"screen"_s, options);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(done.last().at(1).toMap().value(u"trigger"_s).toString(), u"t10"_s);
        QVERIFY(!done.last().at(1).toMap().value(u"existing"_s).toBool());
        QCOMPARE(backend->callCount(u"SetTrigger"_s), sets + 1);
        QCOMPARE(backend->calls.last().second.at(0).toString(), QString()); // (a new one, not t9)
        QCOMPARE(QJsonDocument::fromJson(backend->calls.last().second.at(1).toString().toUtf8()).object().value(u"name"_s).toString(), u"Example: hide (3)"_s);
    }

    // The guided walk's journal (noteScreenDemo): a GUI that dies in the middle leaves what the walk changed on, and the next GUI that connects puts exactly that back.
    void aWalkThatDiedIsUndoneAtTheNextConnection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString journal = dir.filePath(u"screen-demo.ini"_s);
        const auto scripted = [](const QByteArray &triggers) {
            FakeBackend *backend = makeBackend();
            backend->setProperty(u"ApiVersion"_s, 22u);
            backend->setProperty(u"ActiveProfile"_s, QString());
            backend->setProperty(u"ProfileModified"_s, false);
            backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"off\"}"_s);
            backend->answerJson(u"ListPets"_s, "[]");
            backend->answerJson(u"ListProfiles"_s, "[]");
            backend->answerJson(u"ListTriggers"_s, triggers);
            backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[]})");
            backend->answerJson(u"ListTimelines"_s, "[]");
            backend->answer(u"RunningTimelines"_s, QStringList());
            for (const char *method : {"StopScreenWatch", "DeleteTrigger", "SetSettings"})
                backend->answer(QString::fromLatin1(method), QVariant());
            return backend;
        };
        const auto example = [](bool demo) {
            return QByteArray(R"([{"id":"t9","name":"Example","enabled":true,)") + (demo ? R"("origin":"demo",)" : "") + R"("source":{"kind":"screen","data":{"regions":[]}},"action":{"type":"hide-all"}}])";
        };

        // the walk wrote its journal and its GUI died (no undo, no end)
        {
            DaemonClient dead(scripted("[]"));
            dead.setScreenDemoJournalPath(journal);
            dead.noteScreenDemo({{u"trigger"_s, u"t9"_s}, {u"settingTurnedOn"_s, true}, {u"watchStarted"_s, true}});
        }

        // a GUI that starts with a walk of its own (a live journal) does not touch it when the daemon reappears
        {
            FakeBackend *backend = scripted(example(true));
            DaemonClient live(backend);
            live.setScreenDemoJournalPath(journal);
            live.noteScreenDemo({{u"trigger"_s, u"t9"_s}, {u"settingTurnedOn"_s, true}, {u"watchStarted"_s, false}});
            backend->setAvailable(true);
            QTRY_VERIFY(live.screenSupported());
            QTest::qWait(30);
            QCOMPARE(backend->callCount(u"DeleteTrigger"_s), 0);
            QCOMPARE(backend->callCount(u"StopScreenWatch"_s), 0);
            live.noteScreenDemo({{u"trigger"_s, u"t9"_s}, {u"settingTurnedOn"_s, true}, {u"watchStarted"_s, true}}); // (the journal is again what the dead one left)
        }

        // the next GUI: the watch is stopped, the example (still marked) deleted, the setting turned back off; and the journal is gone
        FakeBackend *backend = scripted(example(true));
        {
            DaemonClient next(backend);
            next.setScreenDemoJournalPath(journal);
            backend->setAvailable(true);
            QTRY_COMPARE(backend->callCount(u"DeleteTrigger"_s), 1);
            QCOMPARE(backend->callCount(u"StopScreenWatch"_s), 1);
            QTRY_COMPARE(backend->callCount(u"SetSettings"_s), 1);
            QCOMPARE(backend->calls.last().second.at(0).toString(), u"{\"screenTriggersEnabled\":false}"_s);
            const auto deleted = std::find_if(backend->calls.cbegin(), backend->calls.cend(), [](const auto &c) { return c.first == u"DeleteTrigger"_s; });
            QVERIFY(deleted != backend->calls.cend());
            QCOMPARE(deleted->second.at(0).toString(), u"t9"_s);
        }
        FakeBackend *again = scripted(example(true));
        DaemonClient third(again);
        third.setScreenDemoJournalPath(journal);
        again->setAvailable(true);
        QTRY_VERIFY(third.screenSupported());
        QTest::qWait(30);
        QCOMPARE(again->callCount(u"DeleteTrigger"_s), 0);
        QCOMPARE(again->callCount(u"StopScreenWatch"_s), 0);

        // a trigger the user kept or edited (no marker any more) stays, on, and so does the setting
        {
            DaemonClient dead(scripted("[]"));
            dead.setScreenDemoJournalPath(journal);
            dead.noteScreenDemo({{u"trigger"_s, u"t9"_s}, {u"settingTurnedOn"_s, true}, {u"watchStarted"_s, false}});
        }
        FakeBackend *kept = scripted(example(false));
        DaemonClient fourth(kept);
        fourth.setScreenDemoJournalPath(journal);
        kept->setAvailable(true);
        QTRY_VERIFY(fourth.screenSupported());
        QTest::qWait(60);
        QCOMPARE(kept->callCount(u"DeleteTrigger"_s), 0);
        QCOMPARE(kept->callCount(u"SetSettings"_s), 0);
        QCOMPARE(kept->callCount(u"StopScreenWatch"_s), 0);

        // an undo asked while the daemon is away is not lost: the journal waits for the next connection
        {
            DaemonClient dead(scripted("[]"));
            dead.setScreenDemoJournalPath(journal);
            dead.noteScreenDemo({{u"trigger"_s, u"t9"_s}, {u"settingTurnedOn"_s, false}, {u"watchStarted"_s, false}});
            dead.undoScreenDemo(); // (never connected: screenSupported is false)
        }
        FakeBackend *late = scripted(example(true));
        DaemonClient fifth(late);
        fifth.setScreenDemoJournalPath(journal);
        late->setAvailable(true);
        QTRY_COMPARE(late->callCount(u"DeleteTrigger"_s), 1);
    }

    // The walk starts its watch leased (the daemon ends it if the GUI dies); a daemon without that call gets the plain start, and a refusal is said once.
    void theLeasedStartFallsBackToThePlainOneOnAnOlderBuild()
    {
        const auto scripted = [] {
            FakeBackend *backend = makeBackend();
            backend->setProperty(u"ApiVersion"_s, 22u);
            backend->setProperty(u"ActiveProfile"_s, QString());
            backend->setProperty(u"ProfileModified"_s, false);
            backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"off\"}"_s);
            backend->answerJson(u"ListPets"_s, "[]");
            backend->answerJson(u"ListProfiles"_s, "[]");
            backend->answerJson(u"ListTriggers"_s, "[]");
            backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[]})");
            backend->answerJson(u"ListTimelines"_s, "[]");
            backend->answer(u"RunningTimelines"_s, QStringList());
            return backend;
        };
        {
            FakeBackend *backend = scripted();
            backend->answer(u"StartScreenWatchLeased"_s, QVariant());
            DaemonClient client(backend);
            QSignalSpy errors(&client, &DaemonClient::error);
            backend->setAvailable(true);
            QTRY_VERIFY(client.screenSupported());
            client.startScreenWatchLeased();
            QTRY_COMPARE(backend->callCount(u"StartScreenWatchLeased"_s), 1);
            QTest::qWait(30);
            QCOMPARE(backend->callCount(u"StartScreenWatch"_s), 0);
            QCOMPARE(errors.size(), 0);
        }
        { // (no such method: the plain start)
            FakeBackend *backend = scripted();
            backend->answer(u"StartScreenWatch"_s, QVariant());
            DaemonClient client(backend);
            QSignalSpy errors(&client, &DaemonClient::error);
            backend->setAvailable(true);
            QTRY_VERIFY(client.screenSupported());
            client.startScreenWatchLeased();
            QTRY_COMPARE(backend->callCount(u"StartScreenWatch"_s), 1);
            QCOMPARE(errors.size(), 0);
        }
        { // (refused: InvalidArgs is said once, and the plain start is not tried)
            FakeBackend *backend = scripted();
            backend->fail(u"StartScreenWatchLeased"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"no screen trigger is on"_s);
            DaemonClient client(backend);
            QSignalSpy errors(&client, &DaemonClient::error);
            backend->setAvailable(true);
            QTRY_VERIFY(client.screenSupported());
            client.startScreenWatchLeased();
            QTRY_COMPARE(errors.size(), 1);
            QCOMPARE(errors.last().at(0).toString(), u"no screen trigger is on"_s);
            QCOMPARE(backend->callCount(u"StartScreenWatch"_s), 0);
        }
    }

    void screenTriggersFollowTheDaemonsRevision()
    {
        const auto scripted = [](uint api) {
            FakeBackend *backend = makeBackend();
            backend->setProperty(u"ApiVersion"_s, api);
            backend->setProperty(u"ActiveProfile"_s, QString());
            backend->setProperty(u"ProfileModified"_s, false);
            backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"off\",\"reason\":\"\",\"regions\":0,\"ticksPerSecond\":0,\"since\":0,\"paused\":false,\"notes\":[]}"_s);
            backend->answerJson(u"ListProfiles"_s, "[]");
            backend->answerJson(u"ListTriggers"_s, R"([{"id":"t1","name":"V","enabled":true,"source":{"kind":"screen","data":{"regions":[]}},"action":{"type":"hide-all"}}])");
            backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":true,"available":true,"screen":{"score":[500],"detected":false,"step":0,"active":false}}],"log":[]})");
            backend->answerJson(u"ListTimelines"_s, "[]");
            backend->answer(u"RunningTimelines"_s, QStringList());
            for (const char *method : {"StartScreenWatch", "StopScreenWatch", "CommitScreenReference", "DiscardScreenReference"})
                backend->answer(QString::fromLatin1(method), QVariant());
            backend->answer(u"CaptureScreenReference"_s, u"req-1"_s);
            return backend;
        };
        // revision 20: not supported, said once, nothing sent
        FakeBackend *old = scripted(20);
        DaemonClient oldClient(old);
        QSignalSpy oldErrors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.automationSupported());
        QVERIFY(!oldClient.screenSupported());
        oldClient.startScreenWatch();
        oldClient.captureScreenReference(u"t1"_s, 0, 1);
        oldClient.stopScreenWatch();
        oldClient.commitScreenReference(u"r"_s);
        QCOMPARE(oldErrors.size(), 2);
        for (const char *method : {"StartScreenWatch", "StopScreenWatch", "CaptureScreenReference", "CommitScreenReference"})
            QCOMPARE(old->callCount(QString::fromLatin1(method)), 0);
        QVERIFY(oldClient.screenWatch().isEmpty());

        // revision 21 and later
        FakeBackend *backend = scripted(22);
        DaemonClient client(backend);
        QSignalSpy watchChanges(&client, &DaemonClient::screenWatchChanged);
        QSignalSpy captured(&client, &DaemonClient::screenCaptured);
        QSignalSpy committed(&client, &DaemonClient::screenReferenceCommitted);
        backend->setAvailable(true);
        QTRY_VERIFY(client.screenSupported());
        QTRY_COMPARE(client.screenWatch().value(u"state"_s).toString(), u"off"_s);
        QTRY_COMPARE(client.triggers().size(), 1);
        // the live scores come with the trigger states
        QCOMPARE(client.triggerStates().value(u"states"_s).toList().at(0).toMap().value(u"screen"_s).toMap().value(u"score"_s).toList().at(0).toInt(), 500);

        // the daemon announces a change: the state is read again
        backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"watching\",\"reason\":\"\",\"regions\":2,\"ticksPerSecond\":5,\"since\":7,\"paused\":false,\"notes\":[\"screen: region outside the shared screens\"]}"_s);
        backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":true,"available":true,"screen":{"score":[910],"detected":true,"step":0,"active":true}}],"log":[]})");
        const int before = watchChanges.size();
        backend->emitSignal(u"ScreenWatchChanged"_s);
        QTRY_COMPARE(client.screenWatch().value(u"state"_s).toString(), u"watching"_s);
        QVERIFY(watchChanges.size() > before);
        QCOMPARE(client.screenWatch().value(u"regions"_s).toInt(), 2);
        QCOMPARE(client.screenWatch().value(u"notes"_s).toList().size(), 1);
        QTRY_COMPARE(client.triggerStates().value(u"states"_s).toList().at(0).toMap().value(u"screen"_s).toMap().value(u"score"_s).toList().at(0).toInt(), 910);
        // the same state again is not announced again
        const int settled = watchChanges.size();
        backend->emitSignal(u"ScreenWatchChanged"_s);
        QTest::qWait(50);
        QCOMPARE(watchChanges.size(), settled);

        // start and stop are plain calls
        client.startScreenWatch();
        client.stopScreenWatch();
        QTRY_COMPARE(backend->callCount(u"StartScreenWatch"_s), 1);
        QTRY_COMPARE(backend->callCount(u"StopScreenWatch"_s), 1);

        // a capture: the pictures come with the signal (to this client alone), whatever the order; nothing is kept until commit
        // (only the header of a PNG is read by the client: signature and IHDR)
        const auto png = [](int w, int h) {
            QByteArray head("\x89PNG\r\n\x1a\n", 8), len(4, 0), width(4, 0), height(4, 0);
            qToBigEndian<quint32>(13, len.data());
            qToBigEndian<quint32>(quint32(w), width.data());
            qToBigEndian<quint32>(quint32(h), height.data());
            return QString::fromLatin1((head + len + "IHDR" + width + height + QByteArray(5, 0) + QByteArray(16, 'x')).toBase64());
        };
        const auto result = [&](const QString &request, const QString &data) {
            return QStringLiteral(R"({"request":"%1","trigger":"t1","region":1,"steps":[{"step":0,"width":10,"height":8,"png":"%2"}]})").arg(request, data);
        };
        // nobody here asked: dropped
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-0"_s, result(u"req-0"_s, png(10, 8))});
        QCOMPARE(captured.size(), 0);
        client.captureScreenReference(u"t1"_s, 1, 3);
        QTRY_COMPARE(backend->callCount(u"CaptureScreenReference"_s), 1);
        QCOMPARE(backend->calls.last().second, (QVariantList{u"t1"_s, 1, 3}));
        QTest::qWait(30); // the reply names the request
        // another request's pictures and another request's error are not this capture's
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-9"_s, result(u"req-9"_s, png(10, 8))});
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-8"_s, u"{\"request\":\"req-8\",\"error\":\"screen: not watching\"}"_s});
        QCOMPARE(captured.size(), 0);
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, result(u"req-1"_s, png(10, 8))});
        QCOMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), u"req-1"_s);
        const QVariantMap first = captured.at(0).at(1).toMap();
        QCOMPARE(first.value(u"trigger"_s).toString(), u"t1"_s);
        QCOMPARE(first.value(u"steps"_s).toList().size(), 1);
        QCOMPARE(backend->callCount(u"CommitScreenReference"_s), 0);
        // answered once: the same request again is dropped
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, result(u"req-1"_s, png(10, 8))});
        QCOMPARE(captured.size(), 1);

        // the signal beats the reply
        backend->hold = true;
        client.captureScreenReference(u"t1"_s, 1, 1);
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, u"{\"request\":\"req-1\",\"error\":\"screen: not watching\"}"_s});
        backend->hold = false;
        backend->release();
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(captured.at(1).at(1).toMap().value(u"error"_s).toString(), u"screen: not watching"_s);

        // a capture the page gave up on is dropped when it comes late
        client.captureScreenReference(u"t1"_s, 1, 1);
        QTest::qWait(30);
        client.cancelScreenCapture();
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, result(u"req-1"_s, png(10, 8))});
        QCOMPARE(captured.size(), 2);
        QTRY_COMPARE(backend->callCount(u"DiscardScreenReference"_s), 1); // the daemon is told to drop what it holds
        QCOMPARE(backend->calls.last().second, (QVariantList{u"req-1"_s}));
        // given up before the daemon even answered: the answer's request is dropped when it comes
        backend->hold = true;
        client.captureScreenReference(u"t1"_s, 1, 1);
        client.cancelScreenCapture();
        backend->hold = false;
        backend->release();
        QTRY_COMPARE(backend->callCount(u"DiscardScreenReference"_s), 2);

        // pictures that are not what they say are refused whole (the page draws only what passed)
        const QList<QString> bad{u"AAAA"_s, png(400, 400), png(12, 9)};
        for (const QString &data : bad) {
            const int n = captured.size();
            client.captureScreenReference(u"t1"_s, 1, 1);
            QTest::qWait(30);
            backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, result(u"req-1"_s, data)});
            QCOMPARE(captured.size(), n + 1);
            const QVariantMap refused = captured.last().at(1).toMap();
            QVERIFY2(!refused.value(u"error"_s).toString().isEmpty(), qPrintable(data.left(10)));
            QVERIFY(!refused.contains(u"steps"_s));
        }
        // an error text is shortened
        client.captureScreenReference(u"t1"_s, 1, 1);
        QTest::qWait(30);
        backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, QStringLiteral(R"({"request":"req-1","error":"%1"})").arg(QString(500, u'e'))});
        QCOMPARE(captured.last().at(1).toMap().value(u"error"_s).toString().size(), 200);
        client.commitScreenReference(u"req-1"_s);
        QTRY_COMPARE(committed.size(), 1);
        QCOMPARE(backend->calls.last().first == u"ListTriggers"_s || backend->callCount(u"CommitScreenReference"_s) == 1, true);
        QCOMPARE(backend->callCount(u"CommitScreenReference"_s), 1);

        // the daemon going away clears the state, and says that the screen triggers (and the examples) are gone; back with the same revision it says so again
        QSignalSpy revisions(&client, &DaemonClient::apiVersionChanged);
        backend->setAvailable(false);
        QTRY_VERIFY(client.screenWatch().isEmpty());
        QVERIFY(!client.screenSupported());
        QVERIFY(revisions.size() >= 1);
        const int gone = revisions.size();
        backend->setAvailable(true);
        QTRY_VERIFY(client.screenSupported());
        QVERIFY(revisions.size() > gone);
    }

    void automationFollowsTheDaemon()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 17u);
        DaemonClient oldClient(old);
        QSignalSpy oldErrors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.automationSupported());
        oldClient.saveProfile(u"x"_s);
        QCOMPARE(oldErrors.size(), 1);
        QCOMPARE(old->callCount(u"SaveProfile"_s), 0);
        QCOMPARE(old->callCount(u"ListProfiles"_s), 0);

        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 18u);
        backend->setProperty(u"ActiveProfile"_s, u"p1"_s);
        backend->setProperty(u"ProfileModified"_s, true);
        backend->answerJson(u"ListProfiles"_s, R"([{"id":"p1","name":"Work","pets":[{}, {}]}])");
        backend->answerJson(u"ListTriggers"_s, R"([{"id":"t1","name":"sleepy","enabled":true,"source":{"kind":"fire","name":"sleepy"},"action":{"type":"hide-all"}}])");
        backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":true,"available":true,"outcome":"ran"}],"log":[]})");
        backend->answerJson(u"ListTimelines"_s, R"([{"id":"l1","name":"wave","tracks":[]}])");
        backend->answer(u"RunningTimelines"_s, QStringList{u"l1"_s});
        backend->answer(u"SaveProfile"_s, u"p2"_s);
        backend->answerJson(u"SwitchProfile"_s, R"({"profile":"Work","kept":1,"updated":0,"added":0,"removed":0,"skipped":[{"slot":1,"itemId":"gone","reason":"profile: item missing"}]})");
        backend->answer(u"SetTrigger"_s, u"t2"_s);
        backend->answer(u"SetTimeline"_s, u"l2"_s);
        backend->answer(u"TestTrigger"_s, true);
        backend->answerJson(u"ImportPackAutomation"_s, R"({"timeline":"l3","triggers":["t3","t4"],"notes":["a note"]})");
        backend->answer(u"TimelineFromScene"_s, u"l4"_s);
        for (const char *method : {"RenameProfile", "DeleteProfile", "DeleteTrigger", "SetTriggerEnabled", "FireTrigger", "DeleteTimeline", "RunTimeline", "StopTimeline", "ClearPetRuntime"})
            backend->answer(QString::fromLatin1(method), QVariant());
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::automationSupportedChanged);
        QSignalSpy profilesChanged(&client, &DaemonClient::profilesChanged);
        backend->setAvailable(true);
        QTRY_VERIFY(client.automationSupported());
        QCOMPARE(supported.size(), 1);
        QTRY_COMPARE(client.profiles().size(), 1);
        QCOMPARE(client.activeProfile(), u"p1"_s);
        QVERIFY(client.profileModified());
        QTRY_COMPARE(client.triggers().size(), 1);
        QTRY_COMPARE(client.triggerStates().value(u"states"_s).toList().size(), 1);
        QTRY_COMPARE(client.timelines().size(), 1);
        QCOMPARE(client.runningTimelines(), QStringList{u"l1"_s});

        QSignalSpy saved(&client, &DaemonClient::profileSaved);
        client.saveProfile(u"Play"_s);
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(saved.first().at(1).toString(), u"p2"_s);
        QSignalSpy switched(&client, &DaemonClient::profileSwitched);
        client.switchProfile(u"Work"_s);
        QTRY_COMPARE(switched.size(), 1);
        QCOMPARE(client.lastSwitchReport().value(u"kept"_s).toInt(), 1);
        QCOMPARE(client.lastSwitchReport().value(u"skipped"_s).toList().size(), 1);
        QSignalSpy triggerSaved(&client, &DaemonClient::triggerSaved);
        client.setTrigger(QString(), QVariantMap{{u"name"_s, u"n"_s}});
        QTRY_COMPARE(triggerSaved.size(), 1);
        QCOMPARE(triggerSaved.first().first().toString(), u"t2"_s);
        for (const auto &call : std::as_const(backend->calls))
            if (call.first == u"SetTrigger"_s)
                QCOMPARE(call.second.value(1).toString(), u"{\"name\":\"n\"}"_s);
        QSignalSpy timelineSaved(&client, &DaemonClient::timelineSaved);
        client.setTimeline(u"l1"_s, QVariantMap{{u"speed"_s, 2}});
        QTRY_COMPARE(timelineSaved.size(), 1);
        QSignalSpy tested(&client, &DaemonClient::triggerTested);
        client.testTrigger(u"t1"_s);
        QTRY_COMPARE(tested.size(), 1);
        QVERIFY(tested.first().at(1).toBool());
        QSignalSpy made(&client, &DaemonClient::packAutomationMade);
        client.importPackAutomation(u"local:x"_s);
        QTRY_COMPARE(made.size(), 1);
        QCOMPARE(made.first().first().toMap().value(u"triggers"_s).toList().size(), 2);
        client.timelineFromScene(u"local:x"_s, 0);
        QTRY_COMPARE(made.size(), 2);
        QCOMPARE(made.last().first().toMap().value(u"timeline"_s).toString(), u"l4"_s);
        client.renameProfile(u"p1"_s, u"New"_s);
        client.deleteProfile(u"p1"_s);
        client.deleteTrigger(u"t1"_s);
        client.setTriggerEnabled(u"t1"_s, false);
        client.fireTrigger(u"sleepy"_s);
        client.deleteTimeline(u"l1"_s);
        client.runTimeline(u"l1"_s);
        client.stopTimeline(u"l1"_s);
        client.clearPetRuntime(QString());
        QTRY_COMPARE(backend->callCount(u"StopTimeline"_s), 1);
        QCOMPARE(backend->callCount(u"RunTimeline"_s), 1);
        // the daemon's own announcements re-read the lists
        backend->answerJson(u"ListProfiles"_s, R"([])");
        const int before = backend->callCount(u"ListProfiles"_s);
        backend->emitSignal(u"ProfilesChanged"_s, {});
        QTRY_COMPARE(backend->callCount(u"ListProfiles"_s), before + 1);
        QTRY_COMPARE(client.profiles().size(), 0);
        backend->answerJson(u"TriggerState"_s, R"({"states":[],"log":[{"id":"t1"}]})");
        backend->emitSignal(u"TriggerFired"_s, {u"t1"_s, u"ran"_s});
        QTRY_COMPARE(client.triggerStates().value(u"log"_s).toList().size(), 1);
        // and the daemon going away empties them
        backend->setAvailable(false);
        QVERIFY(client.triggers().isEmpty() && client.timelines().isEmpty() && client.profiles().isEmpty() && client.runningTimelines().isEmpty());
    }

    void connectsAndFetchesWhenDaemonAppears()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy listed(&client, &DaemonClient::itemsListed);
        QSignalSpy assignmentsSpy(&client, &DaemonClient::assignmentsChanged);
        QSignalSpy settingsSpy(&client, &DaemonClient::settingsChanged);
        QVERIFY(!client.available());

        client.start();
        QCOMPARE(backend->startCount, 1);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QCOMPARE(client.apiVersion(), 4);
        QCOMPARE(client.status(), u"ok"_s);
        QCOMPARE(client.assetsDir(), u"/we/assets"_s);
        QTRY_COMPARE(listed.size(), 1);
        QCOMPARE(listed.first().first().toList().size(), 1);
        QTRY_COMPARE(assignmentsSpy.size(), 1);
        QCOMPARE(client.assignments().size(), 1);
        QTRY_COMPARE(settingsSpy.size(), 1);
        QCOMPARE(client.settings().value(u"webFpsLimit"_s).toInt(), 30);
    }

    void refetchesWhenDaemonReappears()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy availableSpy(&client, &DaemonClient::availableChanged);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        backend->setAvailable(false);
        QVERIFY(!client.available());
        QCOMPARE(client.apiVersion(), 0);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QCOMPARE(availableSpy.size(), 3);
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 2);
    }

    void oldDaemonIsNotAvailable()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 2u);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        QSignalSpy versionSpy(&client, &DaemonClient::apiVersionChanged);
        backend->setAvailable(true);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!client.available());
        // QML has to see the old version to tell the user to update the daemon.
        QCOMPARE(client.apiVersion(), 2);
        QCOMPARE(versionSpy.size(), 1);
        QCOMPARE(backend->callCount(u"ListItems"_s), 0);
    }

    void apiVersionReadFailureIsUnavailableWithOneError()
    {
        FakeBackend *backend = makeBackend();
        backend->properties.remove(u"ApiVersion"_s);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_COMPARE(errors.size(), 1);
        QTest::qWait(30);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!client.available());
        QCOMPARE(client.apiVersion(), 0);
    }

    void infoNotifiesIndividually()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy version(&client, &DaemonClient::apiVersionChanged);
        QSignalSpy status(&client, &DaemonClient::statusChanged);
        QSignalSpy dir(&client, &DaemonClient::assetsDirChanged);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QCOMPARE(version.size(), 1);
        QCOMPARE(status.size(), 1);
        QCOMPARE(dir.size(), 1);
        backend->setAvailable(false);
        QCOMPARE(version.size(), 2);
        QCOMPARE(status.size(), 2);
        QCOMPARE(dir.size(), 2);
    }

    void staleAnswersAfterFlapAreIgnored()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy listed(&client, &DaemonClient::itemsListed);
        QSignalSpy loaded(&client, &DaemonClient::itemLoaded);
        QSignalSpy settingsSpy(&client, &DaemonClient::settingsChanged);
        backend->answerJson(u"GetItem"_s, R"({"id":"we:1"})");

        backend->hold = true;
        client.getItem(u"we:1"_s);
        backend->setAvailable(true);   // first generation: ApiVersion parked
        backend->setAvailable(false);
        backend->setAvailable(true);   // second generation: ApiVersion parked
        backend->hold = false;
        backend->release();            // the first answer is stale, the second proceeds
        QTRY_VERIFY(client.available());
        QTRY_COMPARE(listed.size(), 1);
        QTest::qWait(30);
        QCOMPARE(backend->callCount(u"ListItems"_s), 1);
        QCOMPARE(loaded.size(), 0); // GetItem was asked before the flap

        // A reply to a request made in this generation but delivered after another flap is dropped too.
        QTRY_COMPARE(settingsSpy.size(), 1);
        backend->answerJson(u"Settings"_s, R"({"webFpsLimit":99})");
        backend->hold = true;
        client.refreshSettings();
        backend->setAvailable(false);
        backend->hold = false;
        backend->release();
        QTest::qWait(30);
        QVERIFY(!client.available());
        QCOMPARE(listed.size(), 1);
        QCOMPARE(settingsSpy.size(), 1);
        QCOMPARE(client.settings().value(u"webFpsLimit"_s).toInt(), 30);
    }

    void ownerReplacementRefetches()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 1);

        // What SessionBusBackend reports when another process takes over the name.
        backend->setAvailable(false);
        backend->setAvailable(true);
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 2);
        QTRY_COMPARE(backend->callCount(u"Assignments"_s), 2);
        QTRY_COMPARE(backend->callCount(u"Settings"_s), 2);
        QVERIFY(client.available());
    }

    void getItemDeliversFullItem()
    {
        FakeBackend *backend = makeBackend();
        backend->answerJson(u"GetItem"_s, R"({"id":"we:1","dir":"/x","properties":[]})");
        DaemonClient client(backend);
        QSignalSpy loaded(&client, &DaemonClient::itemLoaded);
        client.getItem(u"we:1"_s);
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().at(0).toString(), u"we:1"_s);
        QCOMPARE(loaded.first().at(1).toMap().value(u"dir"_s).toString(), u"/x"_s);
        QCOMPARE(backend->calls.last().second, QVariantList{u"we:1"_s});
    }

    void getItemFailureReportsOnlyThroughItemLoadFailed()
    {
        FakeBackend *backend = makeBackend();
        backend->fail(u"GetItem"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"unknown item"_s);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        QSignalSpy failed(&client, &DaemonClient::itemLoadFailed);
        client.getItem(u"we:9"_s);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(1).toString(), u"unknown item"_s);
        QCOMPARE(errors.size(), 0); // reported once, through itemLoadFailed only
    }

    void setAssignmentSendsOptionsAsJson()
    {
        FakeBackend *backend = makeBackend();
        backend->answer(u"SetAssignment"_s, QVariant());
        DaemonClient client(backend);
        client.setAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, {{u"muted"_s, true}});
        QCOMPARE(backend->calls.size(), 1);
        const QVariantList args = backend->calls.first().second;
        QCOMPARE(args.size(), 4);
        QCOMPARE(args[0].toString(), u"DP-1"_s);
        QCOMPARE(args[2].toString(), u"we:1"_s);
        QCOMPARE(args[3].toString(), u"{\"muted\":true}"_s);

        client.setAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s);
        QVERIFY(backend->calls.last().second[3].toString().isEmpty());
    }

    void invalidArgsFromSetAssignmentIsAnError()
    {
        FakeBackend *backend = makeBackend();
        backend->fail(u"SetAssignment"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"bad key"_s);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        client.setAssignment(u""_s, u"*"_s, u"we:1"_s);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first().toString(), u"bad key"_s);
    }

    void transportFailuresAreSilent()
    {
        FakeBackend *backend = makeBackend();
        backend->fail(u"ClearAssignment"_s, u"org.freedesktop.DBus.Error.NoReply"_s, u"timeout"_s);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        client.clearAssignment(u"DP-1"_s, u"*"_s);
        QTest::qWait(30);
        QCOMPARE(errors.size(), 0);
    }

    void assignmentsChangedSignalRefetches()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy spy(&client, &DaemonClient::assignmentsChanged);
        client.refreshAssignments();
        QTRY_COMPARE(spy.size(), 1);

        backend->answerJson(u"Assignments"_s, "[]");
        backend->emitSignal(u"AssignmentsChanged"_s);
        QTRY_COMPARE(spy.size(), 2);
        QVERIFY(client.assignments().isEmpty());

        // Same content again: no spurious notification.
        backend->emitSignal(u"AssignmentsChanged"_s);
        QTest::qWait(30);
        QCOMPARE(spy.size(), 2);
    }

    void itemsChangedSignalIsForwarded()
    {
        FakeBackend *backend = makeBackend();
        DaemonClient client(backend);
        QSignalSpy spy(&client, &DaemonClient::itemsChanged);
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{u"a"_s}, QStringList{u"b"_s, u"c"_s}, QStringList{}});
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(0).toStringList(), QStringList{u"a"_s});
        QCOMPARE(spy.first().at(1).toStringList(), (QStringList{u"b"_s, u"c"_s}));
        QVERIFY(spy.first().at(2).toStringList().isEmpty());
    }

    void importsAreGatedOnTheRevisionAndFollowTheDaemon()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 13u);
        DaemonClient oldClient(old);
        QSignalSpy errors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.importsSupported());
        oldClient.importPet({{u"kind"_s, u"gif"_s}});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(old->callCount(u"ImportPet"_s), 0);

        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 14u);
        backend->answerJson(u"ImportState"_s, R"([{"id":"imp-1","state":"running","progress":0.5,"name":"Neko"}])");
        backend->answer(u"ImportPet"_s, u"imp-2"_s);
        backend->answerJson(u"PreviewImport"_s, R"({"ok":true,"preview":{"frames":4,"width":64,"height":64}})");
        backend->answer(u"CancelImport"_s, QVariant());
        backend->answer(u"RemoveImportedPet"_s, QVariant());
        backend->answer(u"StartScene"_s, QVariant());
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::importsSupportedChanged);
        QSignalSpy changed(&client, &DaemonClient::importsChanged);
        QSignalSpy started(&client, &DaemonClient::importStarted);
        QSignalSpy previewed(&client, &DaemonClient::importPreviewed);
        backend->setAvailable(true);
        QTRY_VERIFY(client.importsSupported());
        QCOMPARE(supported.size(), 1);
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(client.imports().first().toMap().value(u"id"_s).toString(), u"imp-1"_s);

        client.importPet({{u"kind"_s, u"sheet"_s}, {u"sources"_s, QStringList{u"/a.png"_s}}});
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(started.first().first().toString(), u"imp-2"_s);
        const QJsonObject sent = QJsonDocument::fromJson(backend->calls.last().second.first().toString().toUtf8()).object();
        QCOMPARE(sent.value(u"kind"_s).toString(), u"sheet"_s);
        client.previewImport({{u"kind"_s, u"gif"_s}});
        QTRY_COMPARE(previewed.size(), 1);
        QCOMPARE(previewed.first().first().toMap().value(u"preview"_s).toMap().value(u"frames"_s).toInt(), 4);
        client.cancelImport(u"imp-1"_s);
        client.removeImportedPet(u"local:1"_s, true);
        client.startScene(u"pet-1"_s, 2);
        QTRY_COMPARE(backend->callCount(u"StartScene"_s), 1);
        QCOMPARE(backend->calls.last().second, (QVariantList{u"pet-1"_s, 2}));
        QCOMPARE(backend->callCount(u"CancelImport"_s), 1);

        // The daemon announces a change: the list is read again and replaced.
        backend->answerJson(u"ImportState"_s, R"([{"id":"imp-1","state":"done","progress":1,"itemId":"local:9"}])");
        backend->emitSignal(u"ImportChanged"_s, {u"imp-1"_s});
        QTRY_COMPARE(changed.size(), 2);
        QCOMPARE(client.imports().first().toMap().value(u"itemId"_s).toString(), u"local:9"_s);
        // Gone with the daemon.
        backend->setAvailable(false);
        QVERIFY(client.imports().isEmpty());
    }

    void theFullscreenModeIsGatedOnRevision17()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 16u);
        DaemonClient oldClient(old);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.petStageSupported());
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 17u);
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::petStageSupportedChanged);
        QVERIFY(!client.petStageSupported());
        backend->setAvailable(true);
        QTRY_VERIFY(client.petStageSupported());
        QVERIFY(supported.size() >= 1);
        backend->setAvailable(false);
        QVERIFY(!client.petStageSupported());
    }

    void thePetStatesAreGatedOnRevision19()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 18u);
        DaemonClient oldClient(old);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.petStatesSupported());
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 19u);
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::petStatesSupportedChanged);
        QVERIFY(!client.petStatesSupported());
        backend->setAvailable(true);
        QTRY_VERIFY(client.petStatesSupported());
        QVERIFY(supported.size() >= 1);
        backend->setAvailable(false);
        QVERIFY(!client.petStatesSupported());
    }

    void petSoundsAreGatedAndTheImportJobIsWatchedToItsEnd()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 14u);
        DaemonClient oldClient(old);
        QSignalSpy errors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.petAudioSupported());
        oldClient.importPetAudio(u"/a.mp3"_s);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(old->callCount(u"ImportPetAudio"_s), 0);

        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 15u);
        backend->answerJson(u"ListPetAudioAssets"_s, R"([{"asset":"aa","name":"clap.wav","durationMs":400,"refs":0}])");
        backend->answer(u"ImportPetAudio"_s, u"7"_s);
        backend->answer(u"ImportItemAudio"_s, u"8"_s);
        backend->answerJson(u"PetAudioImportState"_s, R"({"id":"7","state":"running"})");
        backend->answer(u"RemovePetAudioAsset"_s, QVariant());
        backend->answer(u"CancelPetAudioImport"_s, QVariant());
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::petAudioSupportedChanged);
        QSignalSpy assets(&client, &DaemonClient::petAudioAssetsChanged);
        QSignalSpy started(&client, &DaemonClient::petAudioImportStarted);
        QSignalSpy finished(&client, &DaemonClient::petAudioImportFinished);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petAudioSupported());
        QCOMPARE(supported.size(), 1);
        QTRY_COMPARE(assets.size(), 1);
        QCOMPARE(client.petAudioAssets().first().toMap().value(u"name"_s).toString(), u"clap.wav"_s);

        const int ticket = client.importPetAudio(u"/clap.wav"_s);
        QVERIFY(ticket > 0);
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(started.first().first().toString(), u"7"_s);
        QCOMPARE(started.first().at(1).toInt(), ticket); // the card that asked knows its answer
        QCOMPARE(backend->calls.at(backend->calls.size() - 2).second.first().toString(), u"/clap.wav"_s);
        QCOMPARE(finished.size(), 0); // still running
        // the daemon announces the end of the job: a done one carries the sound's id, once
        backend->answerJson(u"PetAudioImportState"_s, R"({"id":"7","state":"done","result":{"ok":true,"asset":"aa"}})");
        backend->emitSignal(u"PetAudioChanged"_s, {u"7"_s});
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), u"7"_s);
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toString(), u"aa"_s);
        backend->emitSignal(u"PetAudioChanged"_s, {u"7"_s});
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
        // a failed one carries its name; a job this client did not start is not reported
        backend->answerJson(u"PetAudioImportState"_s, R"({"id":"8","state":"running"})");
        client.importItemAudio(u"local:3"_s);
        QTRY_COMPARE(started.size(), 2);
        backend->answerJson(u"PetAudioImportState"_s, R"({"id":"8","state":"failed","error":"audio: no audio stream"})");
        backend->emitSignal(u"PetAudioChanged"_s, {u"8"_s});
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!finished.last().at(1).toBool());
        QCOMPARE(finished.last().at(3).toString(), u"audio: no audio stream"_s);
        backend->emitSignal(u"PetAudioChanged"_s, {u"99"_s});
        QTest::qWait(50);
        QCOMPARE(finished.size(), 2);
        client.removePetAudioAsset(u"aa"_s);
        client.cancelPetAudioImport(u"7"_s);
        QTRY_COMPARE(backend->callCount(u"CancelPetAudioImport"_s), 1);
        QCOMPARE(backend->callCount(u"RemovePetAudioAsset"_s), 1);
        // the list follows the daemon, and goes with it
        backend->answerJson(u"ListPetAudioAssets"_s, R"([])");
        backend->emitSignal(u"PetAudioChanged"_s, {QString()});
        QTRY_VERIFY(client.petAudioAssets().isEmpty());
        backend->answerJson(u"ListPetAudioAssets"_s, R"([{"asset":"bb"}])");
        backend->emitSignal(u"PetsChanged"_s);
        QTRY_COMPARE(client.petAudioAssets().size(), 1);
        backend->setAvailable(false);
        QVERIFY(client.petAudioAssets().isEmpty());
    }

    void settingsRoundTrip()
    {
        FakeBackend *backend = makeBackend();
        backend->answer(u"SetSettings"_s, QVariant());
        DaemonClient client(backend);
        client.setSettings({{u"webFpsLimit"_s, 60}});
        QCOMPARE(backend->calls.last().second.first().toString(), u"{\"webFpsLimit\":60}"_s);

        QSignalSpy spy(&client, &DaemonClient::settingsChanged);
        backend->answerJson(u"Settings"_s, R"({"webFpsLimit":60,"pauseWhenCovered":true})");
        backend->emitSignal(u"SettingsChanged"_s);
        QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(client.settings().value(u"webFpsLimit"_s).toInt(), 60);
    }

    void resolveAssignmentDeliversBoth()
    {
        FakeBackend *backend = makeBackend();
        backend->answerJson(u"ResolveAssignment"_s,
                            R"({"assignment":{"output":"*","desktop":"*","itemId":"we:1"},"item":null})");
        DaemonClient client(backend);
        QSignalSpy spy(&client, &DaemonClient::assignmentResolved);
        client.resolveAssignment(u"DP-1"_s, u"d"_s);
        QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(0).toString(), u"DP-1"_s);
        QCOMPARE(spy.first().at(2).toMap().value(u"itemId"_s).toString(), u"we:1"_s);
        QVERIFY(spy.first().at(3).toMap().isEmpty());
    }

    // Pets (API revision 5).
    static FakeBackend *makePetBackend()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 5u);
        backend->setProperty(u"PetsArrange"_s, true);
        backend->answerJson(u"ListPets"_s, R"([{"petId":"p1","itemId":"anima:1","variant":"a","x":1,"width":200}])");
        return backend;
    }

    void petsAreLoadedWithApi5()
    {
        FakeBackend *backend = makePetBackend();
        DaemonClient client(backend);
        QSignalSpy availableSpy(&client, &DaemonClient::petsAvailableChanged);
        QSignalSpy petsSpy(&client, &DaemonClient::petsChanged);
        QSignalSpy arrangeSpy(&client, &DaemonClient::petsArrangeChanged);
        QVERIFY(!client.petsAvailable());
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QCOMPARE(availableSpy.size(), 1);
        QTRY_COMPARE(petsSpy.size(), 1);
        QCOMPARE(client.pets().size(), 1);
        QCOMPARE(client.pets().first().toMap().value(u"petId"_s).toString(), u"p1"_s);
        QTRY_VERIFY(client.petsArrange());
        QCOMPARE(arrangeSpy.size(), 1);

        // The daemon going away empties them again.
        backend->setAvailable(false);
        QVERIFY(!client.petsAvailable());
        QVERIFY(client.pets().isEmpty());
        QVERIFY(!client.petsArrange());
        QCOMPARE(petsSpy.size(), 2);
    }

    void api4HasNoPetsAndPetCallsReportIt()
    {
        FakeBackend *backend = makePetBackend();
        backend->setProperty(u"ApiVersion"_s, 4u);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available()); // wallpapers still work
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 1);
        QVERIFY(!client.petsAvailable());
        QCOMPARE(backend->callCount(u"ListPets"_s), 0);
        client.refreshPets(); // quiet
        client.transcodeState(u"anima:1"_s);
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 3}});
        QCOMPARE(errors.size(), 0);
        client.addPet(u"anima:1"_s, {}, u"*"_s);
        client.setPetsArrange(true);
        client.addLocalPetItem(u"/a.gif"_s);
        QCOMPARE(errors.size(), 3);
        QTest::qWait(DaemonClient::PetUpdateDelayMs + 50);
        QVERIFY(backend->calls.isEmpty() || backend->callCount(u"UpdatePet"_s) == 0);
        QCOMPARE(backend->callCount(u"AddPet"_s), 0);
        QCOMPARE(backend->callCount(u"SetPetsArrange"_s), 0);
    }

    void petsSignalsFollowTheDaemon()
    {
        FakeBackend *backend = makePetBackend();
        DaemonClient client(backend);
        QSignalSpy petsSpy(&client, &DaemonClient::petsChanged);
        QSignalSpy transcodeSpy(&client, &DaemonClient::transcodeChanged);
        QSignalSpy arrangeSpy(&client, &DaemonClient::petsArrangeChanged);
        backend->setAvailable(true);
        QTRY_COMPARE(petsSpy.size(), 1);
        QTRY_COMPARE(arrangeSpy.size(), 1);

        backend->answerJson(u"ListPets"_s, "[]");
        backend->emitSignal(u"PetsChanged"_s);
        QTRY_COMPARE(petsSpy.size(), 2);
        QVERIFY(client.pets().isEmpty());
        backend->emitSignal(u"PetsChanged"_s); // same list again: no signal
        QTRY_COMPARE(backend->callCount(u"ListPets"_s), 3);
        QCOMPARE(petsSpy.size(), 2);

        backend->emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
        QCOMPARE(transcodeSpy.size(), 1);
        QCOMPARE(transcodeSpy.first().first().toString(), u"anima:1"_s);
        backend->emitSignal(u"PetsArrangeChanged"_s, {false});
        QVERIFY(!client.petsArrange());
        QCOMPARE(arrangeSpy.size(), 2);
        backend->emitSignal(u"PetsArrangeChanged"_s, {false});
        QCOMPARE(arrangeSpy.size(), 2);
    }

    // Pet behaviours (API revision 12): PetsHidden, SetPetsHidden and RescuePets.
    void hiddenPetsAndTheRescueNeedRevision12()
    {
        FakeBackend *backend = makePetBackend();
        backend->setProperty(u"ApiVersion"_s, 12u);
        backend->setProperty(u"PetsHidden"_s, true);
        backend->answer(u"SetPetsHidden"_s, QVariant());
        backend->answer(u"RescuePets"_s, QVariant());
        DaemonClient client(backend);
        QSignalSpy availability(&client, &DaemonClient::petBehavioursAvailableChanged);
        QSignalSpy hiddenSpy(&client, &DaemonClient::petsHiddenChanged);
        QVERIFY(!client.petBehavioursAvailable());
        backend->setAvailable(true);
        QTRY_VERIFY(client.petBehavioursAvailable());
        QVERIFY(availability.size() >= 1);
        QTRY_VERIFY(client.petsHidden()); // read when connected
        QCOMPARE(hiddenSpy.size(), 1);

        // the daemon announces the changes, the calls carry the value
        backend->emitSignal(u"PetsHiddenChanged"_s, {false});
        QVERIFY(!client.petsHidden());
        backend->emitSignal(u"PetsHiddenChanged"_s, {false});
        QCOMPARE(hiddenSpy.size(), 2);
        client.setPetsHidden(true);
        QCOMPARE(backend->calls.last().first, u"SetPetsHidden"_s);
        QCOMPARE(backend->calls.last().second, QVariantList{true});
        client.rescuePets();
        QCOMPARE(backend->calls.last().first, u"RescuePets"_s);
        QCOMPARE(backend->calls.last().second, QVariantList{});

        // going away puts it back to shown, as the daemon would start
        backend->emitSignal(u"PetsHiddenChanged"_s, {true});
        QVERIFY(client.petsHidden());
        backend->setAvailable(false);
        QVERIFY(!client.petsHidden());
        QVERIFY(!client.petBehavioursAvailable());
    }

    void aDaemonBeforeRevision12HasNoHideAndNoRescue()
    {
        FakeBackend *backend = makePetBackend(); // revision 5: pets, nothing newer
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QVERIFY(!client.petBehavioursAvailable());
        QCOMPARE(backend->callCount(u"PetsHidden"_s), 0);
        const int calls = backend->calls.size();
        client.setPetsHidden(true);
        client.rescuePets();
        QCOMPARE(backend->callCount(u"SetPetsHidden"_s), 0);
        QCOMPARE(backend->callCount(u"RescuePets"_s), 0);
        QCOMPARE(backend->calls.size(), calls);
        QVERIFY(!client.petsHidden());
        backend->emitSignal(u"PetsHiddenChanged"_s, {true}); // not subscribed to by an old daemon, but harmless
        QVERIFY(client.petsHidden());
    }

    void puppetsNeedTheNewerService()
    {
        FakeBackend *old = makeBackend();
        old->setProperty(u"ApiVersion"_s, 19u);
        DaemonClient oldClient(old);
        QSignalSpy oldErrors(&oldClient, &DaemonClient::error);
        old->setAvailable(true);
        QTRY_VERIFY(oldClient.available());
        QVERIFY(!oldClient.puppetsSupported());
        oldClient.addLocalPuppetItem(u"/some/folder"_s);
        QCOMPARE(oldErrors.size(), 1);
        QCOMPARE(old->callCount(u"AddLocalPuppetItem"_s), 0);

        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 20u);
        backend->answer(u"AddLocalPuppetItem"_s, u"local:pup"_s);
        backend->answer(u"RemoveLocalPuppetItem"_s, QVariant());
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::puppetsSupportedChanged);
        QSignalSpy added(&client, &DaemonClient::localPuppetAdded);
        backend->setAvailable(true);
        QTRY_VERIFY(client.puppetsSupported());
        QVERIFY(supported.size() >= 1);
        client.addLocalPuppetItem(u"/some/folder"_s);
        QTRY_COMPARE(added.size(), 1);
        QCOMPARE(added.first().at(0).toString(), u"/some/folder"_s);
        QCOMPARE(added.first().at(1).toString(), u"local:pup"_s);
        client.removeLocalPuppetItem(u"local:pup"_s);
        QTRY_COMPARE(backend->callCount(u"RemoveLocalPuppetItem"_s), 1);
    }

    void petCallsSendTheirArguments()
    {
        FakeBackend *backend = makePetBackend();
        backend->answer(u"AddPet"_s, u"new-id"_s);
        backend->answer(u"UpdatePet"_s, QVariant());
        backend->answer(u"RemovePet"_s, QVariant());
        backend->answer(u"RequestTranscode"_s, QVariant());
        backend->answer(u"SetPetsArrange"_s, QVariant());
        backend->answer(u"AddLocalPetItem"_s, u"local:abc"_s);
        backend->answer(u"RemoveLocalPetItem"_s, QVariant());
        backend->answerJson(u"TranscodeState"_s,
                            R"({"itemId":"anima:1","variants":[{"name":"a","transcode":{"state":"running","progress":0.5}}]})");
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QSignalSpy added(&client, &DaemonClient::petAdded);
        QSignalSpy state(&client, &DaemonClient::transcodeStateLoaded);
        QSignalSpy localAdded(&client, &DaemonClient::localPetAdded);
        const auto lastCall = [&] { return backend->calls.last(); };

        client.addPet(u"anima:1"_s, u"a"_s, u"DP-1"_s, {{u"width"_s, 300}, {u"playOriginal"_s, true}});
        QTRY_COMPARE(added.size(), 1);
        QCOMPARE(added.first().at(0).toString(), u"anima:1"_s);
        QCOMPARE(added.first().at(1).toString(), u"new-id"_s);
        QCOMPARE(lastCall().first, u"AddPet"_s);
        QCOMPARE(lastCall().second.mid(0, 3), (QVariantList{u"anima:1"_s, u"a"_s, u"DP-1"_s}));
        const QJsonObject options = QJsonDocument::fromJson(lastCall().second.at(3).toString().toUtf8()).object();
        QCOMPARE(options.value(u"width"_s).toInt(), 300);
        QCOMPARE(options.value(u"playOriginal"_s).toBool(), true);

        client.addPet(u"anima:1"_s, {}, u"*"_s);
        QVERIFY(lastCall().second.at(3).toString().isEmpty());

        client.updatePet(u"p1"_s, {{u"opacity"_s, 0.5}});
        QCOMPARE(lastCall().first, u"UpdatePet"_s);
        QCOMPARE(lastCall().second.at(1).toString(), u"{\"opacity\":0.5}"_s);
        client.removePet(u"p1"_s);
        QCOMPARE(lastCall().first, u"RemovePet"_s);
        QCOMPARE(lastCall().second, QVariantList{u"p1"_s});
        client.requestTranscode(u"anima:1"_s, QString());
        QCOMPARE(lastCall().first, u"RequestTranscode"_s);
        QCOMPARE(lastCall().second, (QVariantList{u"anima:1"_s, QString()}));
        client.setPetsArrange(true);
        QCOMPARE(lastCall().first, u"SetPetsArrange"_s);
        QCOMPARE(lastCall().second, QVariantList{true});
        client.removeLocalPetItem(u"local:abc"_s);
        QCOMPARE(lastCall().first, u"RemoveLocalPetItem"_s);

        client.addLocalPetItem(u"/home/me/cat.gif"_s);
        QTRY_COMPARE(localAdded.size(), 1);
        QCOMPARE(localAdded.first().at(0).toString(), u"/home/me/cat.gif"_s);
        QCOMPARE(localAdded.first().at(1).toString(), u"local:abc"_s);

        client.transcodeState(u"anima:1"_s);
        QTRY_COMPARE(state.size(), 1);
        QCOMPARE(state.first().at(0).toString(), u"anima:1"_s);
        const QVariantList variants = state.first().at(1).toList();
        QCOMPARE(variants.size(), 1);
        QCOMPARE(variants.first().toMap().value(u"transcode"_s).toMap().value(u"state"_s).toString(), u"running"_s);
    }

    void petErrorsAreReported()
    {
        FakeBackend *backend = makePetBackend();
        backend->fail(u"AddPet"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"Unknown item"_s);
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QSignalSpy errors(&client, &DaemonClient::error);
        QSignalSpy added(&client, &DaemonClient::petAdded);
        client.addPet(u"anima:404"_s, {}, u"*"_s);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first().toString(), u"Unknown item"_s);
        QCOMPARE(added.size(), 0);

        // A daemon without the local pets call (UnknownMethod is not scripted for it): a readable message.
        client.addLocalPetItem(u"/a.gif"_s);
        QTRY_COMPARE(errors.size(), 2);
        QVERIFY(errors.last().first().toString().contains(u"update"_s));
    }

    void pendingPetIdsFollowAnEditUntilTheDaemonListIsBack()
    {
        FakeBackend *backend = makePetBackend();
        backend->answer(u"UpdatePet"_s, QVariant());
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QSignalSpy spy(&client, &DaemonClient::pendingPetUpdatesChanged);
        QVERIFY(client.pendingPetIds().isEmpty());

        // Queued: pending at once (a control must not take the daemon's old value meanwhile).
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 5}});
        QCOMPARE(client.pendingPetIds(), QStringList{u"p1"_s});
        QCOMPARE(spy.size(), 1);
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 6}}); // same set: no signal
        QCOMPARE(spy.size(), 1);

        // Sent, but still pending until the answer and the refreshed list have arrived.
        backend->hold = true;
        client.flushPetUpdates();
        QCOMPARE(client.pendingPetIds(), QStringList{u"p1"_s});
        backend->hold = false;
        backend->release();
        QTRY_VERIFY(client.pendingPetIds().isEmpty());
        QCOMPARE(spy.size(), 2);
        QVERIFY(backend->callCount(u"ListPets"_s) >= 2);
    }

    void aRefusedPetEditIsReleasedAndReported()
    {
        FakeBackend *backend = makePetBackend();
        backend->fail(u"UpdatePet"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"opacity out of range"_s);
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        QSignalSpy errors(&client, &DaemonClient::error);
        QSignalSpy spy(&client, &DaemonClient::pendingPetUpdatesChanged);
        client.updatePet(u"p1"_s, {{u"opacity"_s, 0}});
        QCOMPARE(client.pendingPetIds(), QStringList{u"p1"_s});
        QTRY_COMPARE(errors.size(), 1);
        QTRY_VERIFY(client.pendingPetIds().isEmpty()); // the controls go back to the daemon's value
        QCOMPARE(spy.size(), 2);
    }

    void pendingPetIdsAreClearedWhenTheDaemonGoesAway()
    {
        FakeBackend *backend = makePetBackend();
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 5}});
        QVERIFY(!client.pendingPetIds().isEmpty());
        backend->setAvailable(false);
        QVERIFY(client.pendingPetIds().isEmpty());
    }

    void queuedPetUpdatesAreMergedAndSentOnce()
    {
        FakeBackend *backend = makePetBackend();
        backend->answer(u"UpdatePet"_s, QVariant());
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.petsAvailable());

        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 1}});
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 5}, {u"y"_s, 6}});
        client.queuePetUpdate(u"p2"_s, {{u"opacity"_s, 0.4}});
        QVERIFY(client.hasPendingPetUpdate(u"p1"_s));
        QVERIFY(!client.hasPendingPetUpdate(u"p3"_s));
        QCOMPARE(backend->callCount(u"UpdatePet"_s), 0);
        QTRY_COMPARE(backend->callCount(u"UpdatePet"_s), 2);
        QTRY_VERIFY(!client.hasPendingPetUpdate(u"p1"_s));
        QHash<QString, QJsonObject> sent;
        for (const auto &call : std::as_const(backend->calls)) {
            if (call.first == u"UpdatePet"_s)
                sent.insert(call.second.at(0).toString(), QJsonDocument::fromJson(call.second.at(1).toString().toUtf8()).object());
        }
        QCOMPARE(sent.value(u"p1"_s).value(u"x"_s).toInt(), 5);
        QCOMPARE(sent.value(u"p1"_s).value(u"y"_s).toInt(), 6);
        QCOMPARE(sent.value(u"p2"_s).value(u"opacity"_s).toDouble(), 0.4);

        // flush sends right away; removing the pet drops its waiting edits.
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 9}});
        client.flushPetUpdates();
        QCOMPARE(backend->callCount(u"UpdatePet"_s), 3);
        client.queuePetUpdate(u"p1"_s, {{u"x"_s, 10}});
        client.removePet(u"p1"_s);
        QTRY_VERIFY(!client.hasPendingPetUpdate(u"p1"_s)); // the queued edit is gone; the sent one is answered
        QTest::qWait(DaemonClient::PetUpdateDelayMs + 100);
        QCOMPARE(backend->callCount(u"UpdatePet"_s), 3);
    }

    // Playlists (API revision 7).
    static FakeBackend *makePlaylistBackend()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 7u);
        backend->answerJson(u"ListPlaylists"_s,
                            R"([{"id":"u1","name":"Calm","items":["we:1"],"order":"sequential","intervalMinutes":15,"changeOnLogin":false}])");
        return backend;
    }

    void playlistsAreLoadedWithApi7()
    {
        FakeBackend *backend = makePlaylistBackend();
        DaemonClient client(backend);
        QSignalSpy availableSpy(&client, &DaemonClient::playlistsAvailableChanged);
        QSignalSpy listSpy(&client, &DaemonClient::playlistsChanged);
        QVERIFY(!client.playlistsAvailable());
        backend->setAvailable(true);
        QTRY_VERIFY(client.playlistsAvailable());
        QCOMPARE(availableSpy.size(), 1);
        QTRY_COMPARE(listSpy.size(), 1);
        QCOMPARE(client.playlists().size(), 1);
        QCOMPARE(client.playlists().first().toMap().value(u"name"_s).toString(), u"Calm"_s);

        backend->answerJson(u"ListPlaylists"_s, R"([])");
        backend->emitSignal(u"PlaylistsChanged"_s);
        QTRY_COMPARE(listSpy.size(), 2);
        QVERIFY(client.playlists().isEmpty());

        backend->answerJson(u"ListPlaylists"_s, R"([{"id":"u2","name":"Night"}])");
        backend->emitSignal(u"PlaylistsChanged"_s);
        QTRY_COMPARE(listSpy.size(), 3);
        backend->emitSignal(u"PlaylistsChanged"_s); // same list: no change signal
        QTRY_COMPARE(backend->callCount(u"ListPlaylists"_s), 4);
        QCOMPARE(listSpy.size(), 3);

        backend->setAvailable(false);
        QVERIFY(!client.playlistsAvailable());
        QVERIFY(client.playlists().isEmpty());
        QCOMPARE(listSpy.size(), 4);
    }

    void api6HasNoPlaylistsAndTheCallsReportIt()
    {
        FakeBackend *backend = makePlaylistBackend();
        backend->setProperty(u"ApiVersion"_s, 6u);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 1);
        QVERIFY(!client.playlistsAvailable());
        QCOMPARE(backend->callCount(u"ListPlaylists"_s), 0);
        client.refreshPlaylists(); // quiet
        QCOMPARE(errors.size(), 0);
        client.createPlaylist(u"x"_s);
        client.nextInPlaylist(u"*"_s, u"*"_s);
        client.addToPlaylist(u"u1"_s, {u"we:1"_s});
        QCOMPARE(errors.size(), 3);
        QCOMPARE(backend->callCount(u"CreatePlaylist"_s), 0);
    }

    void playlistCallsGoToTheDaemon()
    {
        FakeBackend *backend = makePlaylistBackend();
        backend->answer(u"CreatePlaylist"_s, u"new-id"_s);
        for (const char *method : {"UpdatePlaylist", "AddToPlaylist", "RemoveFromPlaylist", "DeletePlaylist",
                                   "NextInPlaylist", "PreviousInPlaylist"})
            backend->answer(QString::fromLatin1(method), QVariant());
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.playlistsAvailable());

        QSignalSpy created(&client, &DaemonClient::playlistCreated);
        client.createPlaylist(u"Mix"_s);
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(created.first().at(0).toString(), u"Mix"_s);
        QCOMPARE(created.first().at(1).toString(), u"new-id"_s);

        client.updatePlaylist(u"u1"_s, {{u"intervalMinutes"_s, 30}, {u"order"_s, u"shuffle"_s}});
        client.addToPlaylist(u"u1"_s, {u"we:2"_s, u"we:3"_s});
        client.removeFromPlaylist(u"u1"_s, {u"we:1"_s});
        client.deletePlaylist(u"u1"_s);
        client.nextInPlaylist(u"DP-1"_s, u"*"_s);
        client.previousInPlaylist(u"DP-1"_s, u"d1"_s);
        QHash<QString, QVariantList> sent;
        for (const auto &call : std::as_const(backend->calls))
            sent.insert(call.first, call.second);
        const QJsonObject changes = QJsonDocument::fromJson(sent.value(u"UpdatePlaylist"_s).at(1).toString().toUtf8()).object();
        QCOMPARE(changes.value(u"intervalMinutes"_s).toInt(), 30);
        QCOMPARE(changes.value(u"order"_s).toString(), u"shuffle"_s);
        QCOMPARE(sent.value(u"AddToPlaylist"_s).at(1).toStringList(), (QStringList{u"we:2"_s, u"we:3"_s}));
        QCOMPARE(sent.value(u"RemoveFromPlaylist"_s).at(0).toString(), u"u1"_s);
        QCOMPARE(sent.value(u"DeletePlaylist"_s).at(0).toString(), u"u1"_s);
        QCOMPARE(sent.value(u"NextInPlaylist"_s), (QVariantList{u"DP-1"_s, u"*"_s}));
        QCOMPARE(sent.value(u"PreviousInPlaylist"_s), (QVariantList{u"DP-1"_s, u"d1"_s}));
    }

    void resolveAssignmentCarriesThePlaylist()
    {
        FakeBackend *backend = makeBackend();
        backend->answerJson(u"ResolveAssignment"_s,
                            R"({"assignment":{"output":"*","desktop":"*","itemId":"we:1"},"item":null,"playlist":{"id":"u1","index":2,"nextAt":5}})");
        DaemonClient client(backend);
        QSignalSpy spy(&client, &DaemonClient::assignmentResolved);
        client.resolveAssignment(u"DP-1"_s, u"d"_s);
        QTRY_COMPARE(spy.size(), 1);
        const QVariantMap playlist = spy.first().at(4).toMap();
        QCOMPARE(playlist.value(u"id"_s).toString(), u"u1"_s);
        QCOMPARE(playlist.value(u"index"_s).toInt(), 2);

        backend->answerJson(u"ResolveAssignment"_s, R"({"assignment":null,"item":null})");
        client.resolveAssignment(u"DP-2"_s, u"d"_s);
        QTRY_COMPARE(spy.size(), 2);
        QVERIFY(spy.at(1).at(4).toMap().isEmpty());
    }

    void itemPermissionsNeedApi8AndGoToTheDaemon()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 8u);
        backend->answer(u"SetItemPermission"_s, QVariant());
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.permissionsAvailable());
        client.setItemPermission(u"we:1"_s, u"network"_s, true);
        QTRY_COMPARE(backend->callCount(u"SetItemPermission"_s), 1);
        QCOMPARE(backend->calls.last().second, (QVariantList{u"we:1"_s, u"network"_s, true}));
        QCOMPARE(errors.size(), 0);
    }

    void api7HasNoPermissionsAndTheCallReportsIt()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 7u);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QVERIFY(!client.permissionsAvailable());
        client.setItemPermission(u"we:1"_s, u"network"_s, true);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(backend->callCount(u"SetItemPermission"_s), 0);
    }

    // Settings inside a target that keep their own wallpaper (the wallpaper page and the Workshop's "Use now" ask).
    static FakeBackend *makeOverrideBackend()
    {
        FakeBackend *backend = makeBackend();
        backend->answerJson(u"Assignments"_s,
                            R"([{"output":"*","desktop":"*","itemId":"we:1"},
                                {"output":"DP-1","desktop":"*","itemId":"we:2"},
                                {"output":"DP-1","desktop":"d1","itemId":"we:3"},
                                {"output":"HDMI-A-1","desktop":"d2","itemId":"we:4"},
                                {"output":"*","desktop":"gone","itemId":"we:5"},
                                {"output":"DP-1","desktop":"gone","itemId":"we:6"}])");
        return backend;
    }
    static QStringList keys(const QVariantList &list)
    {
        QStringList out;
        for (const QVariant &entry : list)
            out << entry.toMap().value(u"output"_s).toString() + u'/' + entry.toMap().value(u"desktop"_s).toString();
        return out;
    }

    void overridingAssignmentsAreTheNarrowerOnesInsideTheTarget()
    {
        FakeBackend *backend = makeOverrideBackend();
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_COMPARE(client.assignments().size(), 6);
        const QStringList known{u"d1"_s, u"d2"_s};
        // everything narrower than "all screens, all desktops" that still applies (not the exact key, not a vanished desktop)
        QCOMPARE(keys(client.overridingAssignments(u"*"_s, u"*"_s, known)),
                 (QStringList{u"DP-1/*"_s, u"DP-1/d1"_s, u"HDMI-A-1/d2"_s}));
        QCOMPARE(keys(client.overridingAssignments(u"DP-1"_s, u"*"_s, known)), (QStringList{u"DP-1/d1"_s}));
        QCOMPARE(keys(client.overridingAssignments(u"DP-1"_s, u"d1"_s, known)), QStringList());
        QCOMPARE(keys(client.overridingAssignments(u"*"_s, u"d1"_s, known)), (QStringList{u"DP-1/d1"_s}));
        // desktops not known (the surface is away): every desktop counts as existing
        QCOMPARE(keys(client.overridingAssignments(u"DP-1"_s, u"*"_s, {})), (QStringList{u"DP-1/d1"_s, u"DP-1/gone"_s}));
    }

    void orphanAssignmentsAreThoseOfVanishedDesktops()
    {
        FakeBackend *backend = makeOverrideBackend();
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_COMPARE(client.assignments().size(), 6);
        const QStringList known{u"d1"_s, u"d2"_s};
        QCOMPARE(keys(client.orphanAssignments(u"*"_s, u"*"_s, known)), (QStringList{u"*/gone"_s, u"DP-1/gone"_s}));
        QCOMPARE(keys(client.orphanAssignments(u"DP-1"_s, u"*"_s, known)), (QStringList{u"DP-1/gone"_s}));
        QCOMPARE(keys(client.orphanAssignments(u"*"_s, u"d1"_s, known)), QStringList()); // a desktop target reaches no other desktop
        QCOMPARE(keys(client.orphanAssignments(u"*"_s, u"*"_s, {})), QStringList());     // unknown desktops: none vanished
    }

    // SteamCMD downloads (API revision 9).
    static FakeBackend *makeSteamCmdBackend()
    {
        FakeBackend *backend = makeBackend();
        backend->setProperty(u"ApiVersion"_s, 9u);
        backend->answerJson(u"SteamCmdStatus"_s,
                            R"({"available":true,"program":"/usr/games/steamcmd","state":"logged-out","user":"","prompt":"","message":"","jobs":[]})");
        return backend;
    }

    void steamCmdStatusIsLoadedAndFollowsTheSignal()
    {
        FakeBackend *backend = makeSteamCmdBackend();
        DaemonClient client(backend);
        QSignalSpy supported(&client, &DaemonClient::steamCmdSupportedChanged);
        QSignalSpy changed(&client, &DaemonClient::steamCmdChanged);
        QVERIFY(!client.steamCmdSupported());
        backend->setAvailable(true);
        QTRY_VERIFY(client.steamCmdSupported());
        QCOMPARE(supported.size(), 1);
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(client.steamCmd().value(u"state"_s).toString(), u"logged-out"_s);
        QVERIFY(client.steamCmd().value(u"available"_s).toBool());

        backend->answerJson(u"SteamCmdStatus"_s,
                            R"({"available":true,"state":"logging-in","user":"bob","prompt":"guard","message":"","jobs":[]})");
        backend->emitSignal(u"SteamCmdChanged"_s);
        QTRY_COMPARE(changed.size(), 2);
        QCOMPARE(client.steamCmd().value(u"prompt"_s).toString(), u"guard"_s);
        backend->emitSignal(u"SteamCmdChanged"_s); // the same status: no change signal
        QTRY_COMPARE(backend->callCount(u"SteamCmdStatus"_s), 3);
        QCOMPARE(changed.size(), 2);

        backend->setAvailable(false);
        QVERIFY(!client.steamCmdSupported());
        QVERIFY(client.steamCmd().isEmpty());
    }

    void api8HasNoSteamCmdAndTheCallsReportIt()
    {
        FakeBackend *backend = makeSteamCmdBackend();
        backend->setProperty(u"ApiVersion"_s, 8u);
        DaemonClient client(backend);
        QSignalSpy errors(&client, &DaemonClient::error);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        QTRY_COMPARE(backend->callCount(u"ListItems"_s), 1);
        QVERIFY(!client.steamCmdSupported());
        QCOMPARE(backend->callCount(u"SteamCmdStatus"_s), 0);
        client.refreshSteamCmd(); // quiet
        QCOMPARE(errors.size(), 0);
        client.steamCmdLogin(u"bob"_s);
        client.steamCmdDownload(u"431960"_s, u"1"_s);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(backend->callCount(u"SteamCmdLogin"_s), 0);
    }

    void steamCmdCallsGoToTheDaemon()
    {
        FakeBackend *backend = makeSteamCmdBackend();
        for (const char *method : {"SteamCmdLogin", "SteamCmdSubmit", "SteamCmdCancelLogin", "SteamCmdLogout", "SteamCmdDownload",
                                   "SteamCmdCancel"})
            backend->answer(QString::fromLatin1(method), QVariant());
        DaemonClient client(backend);
        backend->setAvailable(true);
        QTRY_VERIFY(client.steamCmdSupported());
        client.steamCmdLogin(u"bob"_s);
        client.steamCmdSubmit(u"password"_s, u"hunter2"_s);
        client.steamCmdCancelLogin();
        client.steamCmdLogout();
        client.steamCmdDownload(u"3474900"_s, u"42"_s);
        client.steamCmdCancel(u"42"_s);
        QHash<QString, QVariantList> sent;
        for (const auto &call : std::as_const(backend->calls))
            sent.insert(call.first, call.second);
        QCOMPARE(sent.value(u"SteamCmdLogin"_s), (QVariantList{u"bob"_s}));
        QCOMPARE(sent.value(u"SteamCmdSubmit"_s), (QVariantList{u"password"_s, u"hunter2"_s}));
        QVERIFY(sent.contains(u"SteamCmdCancelLogin"_s));
        QVERIFY(sent.contains(u"SteamCmdLogout"_s));
        QCOMPARE(sent.value(u"SteamCmdDownload"_s), (QVariantList{u"3474900"_s, u"42"_s}));
        QCOMPARE(sent.value(u"SteamCmdCancel"_s), (QVariantList{u"42"_s}));
        // the client keeps no copy of the secret
        QVERIFY(!QJsonDocument::fromVariant(QVariant::fromValue(client.steamCmd())).toJson().contains("hunter2"));
    }
};

QTEST_MAIN(TstDaemonClient)
#include "tst_daemonclient.moc"
