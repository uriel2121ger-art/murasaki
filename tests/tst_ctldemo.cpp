#include "animfixtures.h"
#include "automationstore.h"
#include "constants.h"
#include "demos.h"
#include "murasakiservice.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// `murasakictl demo ...` against a daemon facade that owns the real service name (a private bus only): every kind of example, the removal that takes only what is
// marked, and a pets.json that no example changes. The example's own page buttons are tested in tst_automationpages and tst_daemonclient; they make the same things.
class TestCtlDemo : public QObject
{
    Q_OBJECT

private slots:
    void theExamplesAreMadeMarkedAndRemovedAndNeverTouchThePets();
    void withoutPetsAndWithBadArgumentsItSaysSo();
    void theScreenExampleIsOffHasNoPictureAndIsRemoved();

private:
    struct Result {
        int code = -1;
        QString out;
        QString err;
    };
    static Result ctl(const QStringList &args)
    {
        QProcess process;
        process.start(QStringLiteral(MURASAKICTL_PATH), args);
        if (!process.waitForStarted(5000))
            return {};
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 30000)) {
            process.kill();
            process.waitForFinished(2000);
            return {-2, {}, u"timed out"_s};
        }
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()), QString::fromUtf8(process.readAllStandardError())};
    }
    static QByteArray fileBytes(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
};

namespace {

struct Daemon {
    QTemporaryDir tmp;
    Library library;
    AssignmentStore assignments{tmp.filePath(u"assignments.json"_s)};
    SettingsStore settings{tmp.filePath(u"settings.json"_s)};
    PetStore pets{tmp.filePath(u"pets.json"_s)};
    TranscodeManager transcodes{tmp.filePath(u"cache/pets"_s)};
    ProfileStore profiles{tmp.filePath(u"config/profiles.json"_s)};
    TriggerStore triggers{tmp.filePath(u"config/triggers.json"_s)};
    TimelineStore timelines{tmp.filePath(u"config/timelines.json"_s)};
    std::unique_ptr<MurasakiService> service;
    QDBusConnection bus = QDBusConnection::sessionBus();

    Daemon()
    {
        testutil::writeFile(tmp.filePath(u"anima/9/idle.apng"_s), animfixtures::apng(40, 30, {{1, 10}, {1, 10}}));
        library.setAnimaDirs({tmp.filePath(u"anima"_s)});
        library.rescan();
        settings.load();
        profiles.load();
        triggers.load();
        timelines.load();
        service = std::make_unique<MurasakiService>(&library, &assignments, &settings, u"ok"_s, QString());
        service->attachPets(&pets, &transcodes);
        service->attachAutomation(&profiles, &triggers, &timelines, tmp.filePath(u"state/automation.json"_s));
    }
    ~Daemon()
    {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
    }
    bool registerOnBus() { return service->registerObject(bus) && bus.registerService(QString(kDBusService)); }
    QJsonArray list(const QString &text) const { return QJsonDocument::fromJson(text.toUtf8()).array(); }
};

QString nameOf(const QJsonValue &value)
{
    return value.toObject().value(u"name"_s).toString();
}

} // namespace

void TestCtlDemo::theExamplesAreMadeMarkedAndRemovedAndNeverTouchThePets()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Daemon d;
    QVERIFY(d.bus.isConnected());
    QVERIFY(d.registerOnBus());
    const QString a = d.service->AddPet(u"anima:9"_s, u"idle"_s, u"DP-1"_s, QString());
    const QString b = d.service->AddPet(u"anima:9"_s, u"idle"_s, u"DP-1"_s, QString());
    QVERIFY(!a.isEmpty() && !b.isEmpty());

    // things the user made, with names like the examples' and no marker
    const QString ownTimeline = d.service->SetTimeline(QString(), u"{\"name\":\"Example: pets across the screen\",\"tracks\":[{\"pet\":{\"petId\":\"%1\"},\"actions\":[{\"kind\":\"hide\"}]}]}"_s.arg(a));
    const QString ownTrigger = d.service->SetTrigger(QString(), u"{\"name\":\"Example: a key that plays the timeline\",\"source\":{\"kind\":\"fire\",\"name\":\"mine\"},\"action\":{\"type\":\"hide-all\"}}"_s);
    d.service->SaveProfile(u"Example: my desktop"_s);
    QVERIFY(!ownTimeline.isEmpty() && !ownTrigger.isEmpty());
    const QByteArray petsBefore = fileBytes(d.tmp.filePath(u"pets.json"_s));
    QVERIFY(!petsBefore.isEmpty());

    // timelines: made, marked, playing; the user's timeline of the same name is neither replaced nor adopted
    Result r = ctl({u"demo"_s, u"timelines"_s, u"--screen"_s, u"1920x1080"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"is playing"_s), qPrintable(r.out + r.err));
    QJsonArray timelines = d.list(d.service->ListTimelines());
    QCOMPARE(timelines.size(), 2);
    QJsonObject demoTimeline;
    for (const QJsonValue &t : std::as_const(timelines))
        if (isDemo(t.toObject()))
            demoTimeline = t.toObject();
    QVERIFY(!demoTimeline.isEmpty());
    QCOMPARE(demoTimeline.value(u"name"_s).toString(), u"Example: pets across the screen (2)"_s);
    QCOMPARE(demoTimeline.value(u"tracks"_s).toArray().size(), 2);
    QVERIFY(d.service->RunningTimelines().contains(demoTimeline.value(u"id"_s).toString()));
    // doing it again replaces the example, it does not double it
    QCOMPARE(ctl({u"demo"_s, u"timelines"_s}).code, 0);
    QCOMPARE(d.list(d.service->ListTimelines()).size(), 2);

    // triggers: a shortcut that plays the example timeline
    r = ctl({u"demo"_s, u"triggers"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"Give the shortcut a key"_s), qPrintable(r.out));
    QJsonObject demoTrigger;
    for (const QJsonValue &t : d.list(d.service->ListTriggers()))
        if (isDemo(t.toObject()))
            demoTrigger = t.toObject();
    QCOMPARE(demoTrigger.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"shortcut"_s);
    QCOMPARE(demoTrigger.value(u"action"_s).toObject().value(u"type"_s).toString(), u"timeline"_s);
    QCOMPARE(demoTrigger.value(u"action"_s).toObject().value(u"id"_s).toString(), demoTimeline.value(u"id"_s).toString());
    QCOMPARE(demoTrigger.value(u"name"_s).toString(), u"Example: a key that plays the timeline (2)"_s);

    // profiles: the desktop as it is, as a marked example; the active profile stays the user's and nothing is switched
    const QString activeBefore = d.service->activeProfile();
    QVERIFY(!activeBefore.isEmpty());
    r = ctl({u"demo"_s, u"profiles"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"nothing else changed"_s), qPrintable(r.out));
    QCOMPARE(d.service->activeProfile(), activeBefore);
    int marked = 0;
    for (const QJsonValue &p : d.list(d.service->ListProfiles()))
        marked += isDemo(p.toObject());
    QCOMPARE(marked, 1);
    QCOMPARE(d.list(d.service->ListProfiles()).size(), 2);

    // the user switches to the example themself; removing the examples then goes back to the profile they had
    QCOMPARE(ctl({u"profile"_s, u"switch"_s, u"Example: my desktop (2)"_s}).code, 0);
    QVERIFY(d.service->activeProfile() != activeBefore);

    // tracking: the layer follows the cursor for a while, the stored pet does not change
    r = ctl({u"demo"_s, u"tracking"_s, u"--pet"_s, b});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(b), qPrintable(r.out));
    const QJsonObject layer = QJsonDocument::fromJson(d.service->petRuntime().toUtf8()).object();
    QCOMPARE(layer.value(b).toObject().value(u"tracking"_s).toString(), u"follow"_s);
    QVERIFY(!d.list(d.service->ListPets()).isEmpty());
    for (const QJsonValue &pet : d.list(d.service->ListPets()))
        QVERIFY(pet.toObject().value(u"tracking"_s).toObject().value(u"mode"_s).toString(u"off"_s) == u"off"_s);
    QCOMPARE(ctl({u"demo"_s, u"tracking"_s, u"--pet"_s, u"nobody"_s}).code, 1);

    // not one example wrote the pets
    QCOMPARE(fileBytes(d.tmp.filePath(u"pets.json"_s)), petsBefore);

    // remove: exactly the marked ones
    r = ctl({u"demo"_s, u"remove"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.out.trimmed(), u"removed 1 trigger(s), 2 timeline(s), 1 profile(s)"_s); // (the two timelines: the crossing example and the tracking one)
    timelines = d.list(d.service->ListTimelines());
    QCOMPARE(timelines.size(), 1);
    QCOMPARE(timelines.at(0).toObject().value(u"id"_s).toString(), ownTimeline);
    QCOMPARE(nameOf(timelines.at(0)), u"Example: pets across the screen"_s);
    const QJsonArray triggers = d.list(d.service->ListTriggers());
    QCOMPARE(triggers.size(), 1);
    QCOMPARE(triggers.at(0).toObject().value(u"id"_s).toString(), ownTrigger);
    const QJsonArray profiles = d.list(d.service->ListProfiles());
    QCOMPARE(profiles.size(), 1);
    QCOMPARE(nameOf(profiles.at(0)), u"Example: my desktop"_s);
    QCOMPARE(d.service->activeProfile(), activeBefore); // (the example was the active profile: back to the user's)
    QVERIFY(d.service->RunningTimelines().isEmpty()); // (a removed timeline is stopped first, which puts the pets back)
    QVERIFY(QJsonDocument::fromJson(d.service->petRuntime().toUtf8()).object().isEmpty());
    QCOMPARE(ctl({u"demo"_s, u"remove"_s}).out.trimmed(), u"removed 0 trigger(s), 0 timeline(s), 0 profile(s)"_s);
    QCOMPARE(fileBytes(d.tmp.filePath(u"pets.json"_s)), petsBefore);

    // all: the four in order, the last once the timeline is over
    r = ctl({u"demo"_s, u"all"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"timeline:"_s) && r.out.contains(u"triggers:"_s) && r.out.contains(u"profile:"_s) && r.out.contains(u"tracking:"_s), qPrintable(r.out));
    QCOMPARE(fileBytes(d.tmp.filePath(u"pets.json"_s)), petsBefore);
    QCOMPARE(ctl({u"demo"_s, u"remove"_s}).code, 0);
    QCOMPARE(d.list(d.service->ListTimelines()).size(), 1);
}

void TestCtlDemo::withoutPetsAndWithBadArgumentsItSaysSo()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Daemon d;
    QVERIFY(d.registerOnBus());
    for (const QString &kind : {u"timelines"_s, u"triggers"_s, u"profiles"_s, u"tracking"_s, u"screen"_s, u"all"_s}) {
        const Result r = ctl({u"demo"_s, kind});
        QCOMPARE(r.code, 1);
        QVERIFY2(r.err.contains(u"add one first"_s), qPrintable(kind + u": "_s + r.err));
    }
    QVERIFY(d.list(d.service->ListTimelines()).isEmpty());
    QVERIFY(d.list(d.service->ListTriggers()).isEmpty());
    QVERIFY(d.list(d.service->ListProfiles()).isEmpty());
    QCOMPARE(ctl({u"demo"_s, u"bogus"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"timelines"_s, u"extra"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"timelines"_s, u"--screen"_s, u"wide"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"timelines"_s, u"--screen"_s, u"10x10"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"profiles"_s, u"--pet"_s, u"x"_s}).code, 2);   // (an option that the kind ignores is a mistake)
    QCOMPARE(ctl({u"demo"_s, u"tracking"_s, u"--screen"_s, u"800x600"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"screen"_s, u"--pet"_s, u"x"_s}).code, 2);
    QCOMPARE(ctl({u"demo"_s, u"screen"_s, u"--screen"_s, u"800x600"_s}).code, 2);
}

void TestCtlDemo::theScreenExampleIsOffHasNoPictureAndIsRemoved()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Daemon d;
    QVERIFY(d.registerOnBus());
    const QString pet = d.service->AddPet(u"anima:9"_s, u"idle"_s, u"DP-1"_s, QString());
    QVERIFY(!pet.isEmpty());
    // the user's own trigger with the example's name and no marker
    const QString own = d.service->SetTrigger(QString(), u"{\"name\":\"Example: hide the pets while a picture is on the screen\",\"source\":{\"kind\":\"fire\",\"name\":\"mine\"},\"action\":{\"type\":\"hide-all\"}}"_s);
    QVERIFY(!own.isEmpty());
    const QByteArray petsBefore = fileBytes(d.tmp.filePath(u"pets.json"_s));

    Result r = ctl({u"demo"_s, u"screen"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"made the screen trigger"_s) && r.out.contains(u"triggers screen start"_s) && r.out.contains(u"screen capture"_s), qPrintable(r.out));
    QJsonObject example;
    for (const QJsonValue &t : d.list(d.service->ListTriggers()))
        if (isDemo(t.toObject()))
            example = t.toObject();
    QVERIFY(!example.isEmpty());
    QCOMPARE(example.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"screen"_s);
    QCOMPARE(example.value(u"enabled"_s).toBool(), false);                 // off until the user starts it
    QCOMPARE(example.value(u"name"_s).toString(), u"Example: hide the pets while a picture is on the screen (2)"_s);
    QCOMPARE(example.value(u"source"_s).toObject().value(u"data"_s).toObject().value(u"regions"_s).toArray().size(), 1);
    // nothing is watched, nothing is kept: no portal, no reference picture
    QCOMPARE(QJsonDocument::fromJson(d.service->screenWatchState().toUtf8()).object().value(u"state"_s).toString(), u"off"_s);
    // doing it again finds the example (it may hold the user's region by now)
    r = ctl({u"demo"_s, u"screen"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"already there"_s), qPrintable(r.out));
    int marked = 0;
    for (const QJsonValue &t : d.list(d.service->ListTriggers()))
        marked += isDemo(t.toObject());
    QCOMPARE(marked, 1);
    // "all" does not make it: it needs the user at the keyboard
    // (not run here: all plays a timeline for seconds)
    QCOMPARE(fileBytes(d.tmp.filePath(u"pets.json"_s)), petsBefore);
    r = ctl({u"demo"_s, u"remove"_s});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(u"removed 1 trigger(s)"_s), qPrintable(r.out));
    const QJsonArray left = d.list(d.service->ListTriggers());
    QCOMPARE(left.size(), 1);
    QCOMPARE(left.at(0).toObject().value(u"id"_s).toString(), own);
}

QTEST_MAIN(TestCtlDemo)
#include "tst_ctldemo.moc"
