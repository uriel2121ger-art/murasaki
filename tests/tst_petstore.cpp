#include "petlimits.h"
#include "petstore.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <limits>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUuid>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPetStore : public QObject
{
    Q_OBJECT

private slots:
    void validationRanges();
    void applyChangesMergesAndValidates();
    void addAssignsUuidAndPersists();
    void updateAndRemoveSignalOnlyOnChange();
    void missingFileIsEmpty();
    void corruptFileIsKeptAsBackup();
    void invalidEntryIsKeptAsBackup();
    void unsupportedVersionIsKeptAsBackup();
    void duplicateIdIsKeptAsBackup();
    void limitIsEnforced();
    void failedWritesRollBackAndStaySilent();
    void referencesItem();
    void fpsIsValidatedAndStored();
    void speedIsValidatedAndStored();
    void behaviourFieldsDefaultToTheOldBehaviour();
    void behaviourFieldsAreValidatedAndStored();
    void hoverHideAndFocusFadeExcludeEachOther();
    void rotationIsWrappedAndRounded();
    void theSoundIsOffByDefaultAndValidated();
    void theSoundSurvivesTheFileAndABadOneCostsOnlyTheSound();
    void aPetIsNotAStageByDefaultAndTheFieldIsValidated();
    void aStageSurvivesTheFileAndABadOneCostsOnlyTheStage();
    void oneStagePerOutputAndDesktopTakesThemAll();
    void aPetWithoutStatesOrTrackingWritesExactlyTheOldKeys();
    void statesAreValidatedMergedAndCleared();
    void trackingIsValidatedMergedAndCleared();
    void followExcludesDodgeAndFades();
    void nullKeysInAFileAndAStageThatKeepsFlagsAndTheDistinctVariantCap();
    void statesAndTrackingSurviveTheFileAndBadOnesCostOnlyThemselves();
    void aPetWhoseStageAFileMadeConflictCanStillBeEdited();
    void updateAllWritesOnce();
    void updateAllWithAFailingWriteChangesNothing();
    void aFileWithBothExclusiveFlagsOrAWildAngleStillLoads();
    void petsStoredWithoutFpsGetTheDefaultRate();
    void playbackSpeedFollowsTheNativeRate();

private:
    static PetPlacement make(const QString &item = u"anima:1"_s)
    {
        PetPlacement p;
        p.itemId = item;
        p.variant = u"idle"_s;
        p.output = u"DP-1"_s;
        p.x = 10;
        p.y = 20;
        p.width = 300;
        return p;
    }
    static QStringList backups(const QTemporaryDir &tmp)
    {
        return QDir(tmp.path()).entryList({u"pets.json.bad-*"_s}, QDir::Files);
    }
};

void TestPetStore::validationRanges()
{
    QVERIFY(validatePlacement(make()));
    const auto invalid = [](auto mutate) {
        PetPlacement p = make();
        mutate(p);
        return !validatePlacement(p);
    };
    QVERIFY(invalid([](PetPlacement &p) { p.itemId.clear(); }));
    QVERIFY(invalid([](PetPlacement &p) { p.variant.clear(); }));
    QVERIFY(invalid([](PetPlacement &p) { p.output.clear(); }));
    QVERIFY(invalid([](PetPlacement &p) { p.output = QString(201, u'x'); }));
    QVERIFY(invalid([](PetPlacement &p) { p.x = 100001; }));
    QVERIFY(invalid([](PetPlacement &p) { p.y = -100001; }));
    QVERIFY(invalid([](PetPlacement &p) { p.x = std::nan(""); }));
    QVERIFY(invalid([](PetPlacement &p) { p.width = 15; }));
    QVERIFY(invalid([](PetPlacement &p) { p.width = 16385; }));
    QVERIFY(invalid([](PetPlacement &p) { p.opacity = 0.05; }));
    QVERIFY(invalid([](PetPlacement &p) { p.opacity = 1.01; }));
    PetPlacement edge = make();
    edge.width = 16;
    edge.opacity = 0.1;
    edge.x = -100000;
    QVERIFY(validatePlacement(edge));
}

void TestPetStore::applyChangesMergesAndValidates()
{
    QString error;
    const auto p = applyPlacementChanges(make(), testutil::json("{\"x\": 5.5, \"paused\": true, \"bogus\": 1, \"itemId\": \"other\"}"),
                                         &error);
    QVERIFY2(p, qPrintable(error));
    QCOMPARE(p->x, 5.5);
    QCOMPARE(p->paused, true);
    QCOMPARE(p->itemId, u"anima:1"_s); // immutable
    QCOMPARE(p->y, 20.0);

    QVERIFY(!applyPlacementChanges(make(), testutil::json("{\"x\": \"5\"}"), &error));
    QVERIFY(error.contains(u"number"_s));
    QVERIFY(!applyPlacementChanges(make(), testutil::json("{\"paused\": 1}"), &error));
    QVERIFY(!applyPlacementChanges(make(), testutil::json("{\"variant\": 3}"), &error));
    QVERIFY(!applyPlacementChanges(make(), testutil::json("{\"opacity\": 2}"), &error));
}

void TestPetStore::addAssignsUuidAndPersists()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    QString id;
    {
        PetStore store(path);
        store.load();
        QSignalSpy spy(&store, &PetStore::changed);
        id = store.add(make());
        QVERIFY(!QUuid::fromString(id).isNull());
        QCOMPARE(spy.count(), 1);
        QVERIFY(store.add(make(u"anima:2"_s)) != id);
    }
    PetStore again(path);
    again.load();
    QCOMPARE(again.pets().size(), 2);
    QCOMPARE(again.pets().at(0).petId, id);
    QCOMPARE(again.pets().at(0), (PetPlacement{id, u"anima:1"_s, u"idle"_s, u"DP-1"_s, 10, 20, 300, 1, false, false}));
    QCOMPARE(again.pet(id)->itemId, u"anima:1"_s);
    QVERIFY(!again.pet(u"nope"_s));

    QString error;
    PetPlacement bad = make();
    bad.width = 1;
    QVERIFY(again.add(bad, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(again.pets().size(), 2);
}

void TestPetStore::updateAndRemoveSignalOnlyOnChange()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    const QString id = store.add(make());
    QSignalSpy spy(&store, &PetStore::changed);

    PetPlacement p = *store.pet(id);
    QVERIFY(!store.update(p)); // unchanged
    p.x = 99;
    QVERIFY(store.update(p));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(store.pet(id)->x, 99.0);

    PetPlacement unknown = p;
    unknown.petId = u"nope"_s;
    QVERIFY(!store.update(unknown));
    PetPlacement invalid = p;
    invalid.opacity = 5;
    QVERIFY(!store.update(invalid));
    QCOMPARE(spy.count(), 1);

    QVERIFY(!store.remove(u"nope"_s));
    QVERIFY(store.remove(id));
    QCOMPARE(spy.count(), 2);
    QVERIFY(store.pets().isEmpty());
}

void TestPetStore::missingFileIsEmpty()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    QVERIFY(store.pets().isEmpty());
    QVERIFY(!QFile::exists(store.filePath())); // loading never writes
}

void TestPetStore::corruptFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"pets.json"_s), "{not json");
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    QVERIFY(store.pets().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
    QVERIFY(!QFile::exists(store.filePath()));
    QVERIFY(!store.add(make()).isEmpty()); // usable afterwards
    QVERIFY(QFile::exists(store.filePath()));
}

void TestPetStore::invalidEntryIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray good = "{\"petId\": \"" + id.toUtf8() + "\", \"itemId\": \"anima:1\", \"variant\": \"a\", \"output\": \"*\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 1}";
    testutil::writeFile(tmp.filePath(u"pets.json"_s), "{\"version\": 1, \"pets\": [" + good + ", {\"petId\": \"" + id.toUtf8() + "x\", \"itemId\": \"anima:2\", \"variant\": \"a\", \"output\": \"*\", \"width\": 100, \"opacity\": 9}]}");
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    QVERIFY(store.pets().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
}

void TestPetStore::unsupportedVersionIsKeptAsBackup()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"pets.json"_s), "{\"version\": 2, \"pets\": []}");
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    QCOMPARE(backups(tmp).size(), 1);
}

void TestPetStore::duplicateIdIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QByteArray id = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    const QByteArray entry = "{\"petId\": \"" + id + "\", \"itemId\": \"anima:1\", \"variant\": \"a\", \"output\": \"*\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 1}";
    testutil::writeFile(tmp.filePath(u"pets.json"_s), "{\"version\": 1, \"pets\": [" + entry + "," + entry + "]}");
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    QVERIFY(store.pets().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
}

void TestPetStore::limitIsEnforced()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    for (int i = 0; i < kMaxPets; ++i)
        QVERIFY(!store.add(make()).isEmpty());
    QString error;
    QVERIFY(store.add(make(), &error).isEmpty());
    QVERIFY(error.contains(u"64"_s));
}

void TestPetStore::failedWritesRollBackAndStaySilent()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"cfg"_s);
    QVERIFY(QDir().mkpath(dir));
    PetStore store(dir + u"/pets.json"_s);
    store.load();
    const QString kept = store.add(make());
    QVERIFY(!kept.isEmpty());

    const QFileDevice::Permissions all = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    QVERIFY(QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(dir, all); });
    if (QFileInfo(dir).isWritable())
        QSKIP("directory permissions are not enforced (running as root?)");

    QSignalSpy changed(&store, &PetStore::changed);
    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(store.add(make(u"anima:2"_s), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.pets().size(), 1);

    PetPlacement moved = *store.pet(kept);
    moved.x = 77;
    error.clear();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(!store.update(moved, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.pet(kept)->x, 10.0);

    error.clear();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(!store.remove(kept, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.pets().size(), 1);
    QCOMPARE(changed.count(), 0);
}

void TestPetStore::fpsIsValidatedAndStored()
{
    QCOMPARE(make().fps, 0);
    QCOMPARE(defaultPetFps(u"anima:123"_s), 24); // the user's default, for every item
    QCOMPARE(defaultPetFps(u"local:abc"_s), 24);
    QCOMPARE(defaultPetFps(u"we:1"_s), 24);
    QCOMPARE(kDefaultPetFps, 24);
    for (const int fps : {0, 1, 15, 30, 60, 120}) {
        PetPlacement p = make();
        p.fps = fps;
        QVERIFY2(validatePlacement(p), qPrintable(QString::number(fps)));
    }
    for (const int fps : {-1, 121, 1000}) {
        PetPlacement p = make();
        p.fps = fps;
        QString error;
        QVERIFY(!validatePlacement(p, &error));
        QVERIFY(error.contains(u"fps"_s));
    }
    const auto set = [](const char *json, QString *error = nullptr) {
        return applyPlacementChanges(make(), testutil::json(json), error);
    };
    QCOMPARE(set("{\"fps\": 24}")->fps, 24);
    QCOMPARE(set("{\"fps\": 24.0}")->fps, 24);
    QCOMPARE(set("{\"fps\": 0}")->fps, 0);
    QString error;
    QVERIFY(!set("{\"fps\": 12.5}", &error));
    QVERIFY(error.contains(u"integer"_s));
    QVERIFY(!set("{\"fps\": \"30\"}"));
    QVERIFY(!set("{\"fps\": true}"));
    QCOMPARE(set("{\"fps\": 120}")->fps, 120);
    QVERIFY(!set("{\"fps\": 121}"));
    QVERIFY(!set("{\"fps\": -5}"));
    QVERIFY(!set("{\"fps\": null}"));
    QCOMPARE(set("{\"x\": 5}")->fps, 0); // untouched when not mentioned

    PetPlacement p = make();
    p.fps = 15;
    QCOMPARE(p.toJson().value(u"fps"_s).toInt(), 15);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    {
        PetStore store(path);
        store.load();
        PetPlacement withFps = make();
        withFps.fps = 24;
        QVERIFY(!store.add(withFps).isEmpty());
        QVERIFY(store.update([&] {
            PetPlacement edited = store.pets().first();
            edited.fps = 60;
            return edited;
        }()));
    }
    PetStore again(path);
    again.load();
    QCOMPARE(again.pets().size(), 1);
    QCOMPARE(again.pets().first().fps, 60);
}

void TestPetStore::speedIsValidatedAndStored()
{
    QCOMPARE(make().speed, 1.0);
    QCOMPARE(make().timing().mode(), PetTimingMode::File);
    const auto set = [](const char *json, QString *error = nullptr) {
        return applyPlacementChanges(make(), testutil::json(json), error);
    };
    // not a number: refused by the validation as well as by the parser
    for (const double bad : {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        PetPlacement p = make();
        p.speed = bad;
        QString nanError;
        QVERIFY(!validatePlacement(p, &nanError));
        QVERIFY(nanError.contains(u"speed"_s));
    }
    QCOMPARE(set("{\"speed\": 0.25}")->speed, 0.25);
    QCOMPARE(set("{\"speed\": 4}")->speed, 4.0);
    QCOMPARE(set("{\"speed\": 2}")->timing().mode(), PetTimingMode::Speed);
    QString error;
    for (const char *bad : {"{\"speed\": 0.24}", "{\"speed\": 4.01}", "{\"speed\": 0}", "{\"speed\": -1}",
                           "{\"speed\": \"2\"}", "{\"speed\": null}", "{\"speed\": true}"})
        QVERIFY2(!set(bad, &error), bad);
    QVERIFY(error.contains(u"speed"_s));
    // a fixed rate and a speed are two modes: asked together they are an error, asked one at a time the last wins
    QVERIFY(!set("{\"fps\": 15, \"speed\": 2}", &error));
    QVERIFY(error.contains(u"together"_s));
    QCOMPARE(set("{\"fps\": 15, \"speed\": 1}")->fps, 15);
    PetPlacement fast = make();
    fast.speed = 2;
    QCOMPARE(applyPlacementChanges(fast, testutil::json("{\"fps\": 15}"))->speed, 1.0);
    QCOMPARE(applyPlacementChanges(fast, testutil::json("{\"fps\": 15}"))->timing().mode(), PetTimingMode::Fixed);
    PetPlacement fixed = make();
    fixed.fps = 15;
    const auto back = applyPlacementChanges(fixed, testutil::json("{\"speed\": 0.5}"));
    QCOMPARE(back->fps, 0);
    QCOMPARE(back->speed, 0.5);
    QCOMPARE(applyPlacementChanges(fixed, testutil::json("{\"fps\": 0}"))->timing().mode(), PetTimingMode::File);
    QCOMPARE(applyPlacementChanges(fast, testutil::json("{\"speed\": 1}"))->timing().mode(), PetTimingMode::File);
    QCOMPARE(fast.toJson().value(u"speed"_s).toDouble(), 2.0);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    {
        PetStore store(path);
        store.load();
        QVERIFY(!store.add(fast).isEmpty());
    }
    PetStore again(path);
    again.load();
    QCOMPARE(again.pets().size(), 1);
    QCOMPARE(again.pets().first().speed, 2.0);
    QCOMPARE(again.pets().first().fps, 0);
}

void TestPetStore::behaviourFieldsDefaultToTheOldBehaviour()
{
    const PetPlacement p = make();
    QVERIFY(p.clickThrough);
    QVERIFY(p.keepAbove);
    QVERIFY(!p.locked && !p.hoverHide && !p.focusFade && !p.smartDodge && !p.flipH && !p.flipV);
    QCOMPARE(p.rotation, 0.0);
    // a pets.json written before the fields: the defaults, and the file is not a bad one
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [{\"petId\": \"11111111-1111-1111-1111-111111111111\", \"itemId\": \"anima:5\", "
                              "\"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 0.9, "
                              "\"playOriginal\": false, \"paused\": false, \"fps\": 24}]}");
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 1);
    const PetPlacement old = store.pets().first();
    QVERIFY(old.clickThrough && old.keepAbove);
    QVERIFY(!old.locked && !old.hoverHide && !old.focusFade && !old.smartDodge && !old.flipH && !old.flipV);
    QCOMPARE(old.rotation, 0.0);
    QCOMPARE(old.opacity, 0.9);
    QVERIFY(backups(tmp).isEmpty());
    // and a pet is written with all of them
    const QJsonObject json = old.toJson();
    for (const char *key : {"clickThrough", "locked", "keepAbove", "hoverHide", "focusFade", "smartDodge", "rotation", "flipH", "flipV"})
        QVERIFY2(json.contains(QLatin1String(key)), key);
    QCOMPARE(json.value(u"clickThrough"_s).toBool(), true);
}

void TestPetStore::behaviourFieldsAreValidatedAndStored()
{
    const auto set = [](const char *json, QString *error = nullptr) { return applyPlacementChanges(make(), testutil::json(json), error); };
    const auto all = set("{\"clickThrough\": false, \"locked\": true, \"keepAbove\": false, \"hoverHide\": true, \"smartDodge\": true, "
                         "\"rotation\": 33.3, \"flipH\": true, \"flipV\": true}");
    QVERIFY(all);
    QVERIFY(!all->clickThrough && all->locked && !all->keepAbove && all->hoverHide && all->smartDodge && all->flipH && all->flipV);
    QCOMPARE(all->rotation, 33.3);
    // a wrong type is an error naming the key, for each of them
    for (const char *key : {"clickThrough", "locked", "keepAbove", "hoverHide", "focusFade", "smartDodge", "flipH", "flipV"}) {
        for (const char *bad : {"1", "\"yes\"", "null", "[]"}) {
            QString error;
            const QByteArray json = QByteArray("{\"") + key + "\": " + bad + "}";
            QVERIFY2(!set(json.constData(), &error), json.constData());
            QVERIFY2(error.contains(QLatin1String(key)), qPrintable(error));
        }
    }
    for (const char *bad : {"\"90\"", "true", "null", "[]"}) {
        QString error;
        QVERIFY(!set((QByteArray("{\"rotation\": ") + bad + "}").constData(), &error));
        QVERIFY(error.contains(u"rotation"_s));
    }
    QVERIFY(set("{\"somethingNew\": true}")); // unknown keys are ignored
    // round trip through the file, then through toJson
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    {
        PetStore store(path);
        store.load();
        PetPlacement p = make();
        p.clickThrough = false;
        p.locked = true;
        p.keepAbove = false;
        p.focusFade = true;
        p.smartDodge = true;
        p.rotation = 123.45;
        p.flipV = true;
        QVERIFY(!store.add(p).isEmpty());
    }
    PetStore again(path);
    again.load();
    QCOMPARE(again.pets().size(), 1);
    const PetPlacement back = again.pets().first();
    QVERIFY(!back.clickThrough && back.locked && !back.keepAbove && back.focusFade && back.smartDodge && back.flipV);
    QVERIFY(!back.hoverHide && !back.flipH);
    QCOMPARE(back.rotation, 123.45);
}

void TestPetStore::hoverHideAndFocusFadeExcludeEachOther()
{
    const auto set = [](const PetPlacement &base, const char *json, QString *error = nullptr) {
        return applyPlacementChanges(base, testutil::json(json), error);
    };
    PetPlacement hiding = make();
    hiding.hoverHide = true;
    // choosing the other one switches this one off, in both directions
    const auto faded = set(hiding, "{\"focusFade\": true}");
    QVERIFY(faded && faded->focusFade && !faded->hoverHide);
    PetPlacement fading = make();
    fading.focusFade = true;
    const auto hid = set(fading, "{\"hoverHide\": true}");
    QVERIFY(hid && hid->hoverHide && !hid->focusFade);
    // switching one off does not touch the other
    const auto off = set(hiding, "{\"focusFade\": false}");
    QVERIFY(off && off->hoverHide && !off->focusFade);
    QVERIFY(set(hiding, "{\"hoverHide\": false}") && !set(hiding, "{\"hoverHide\": false}")->hoverHide);
    // both in one call is a contradiction; so is a placement that has both
    QString error;
    QVERIFY(!set(make(), "{\"hoverHide\": true, \"focusFade\": true}", &error));
    QVERIFY(error.contains(u"exclude"_s));
    PetPlacement both = make();
    both.hoverHide = both.focusFade = true;
    QVERIFY(!validatePlacement(both, &error));
    QVERIFY(set(make(), "{\"hoverHide\": true, \"focusFade\": false}")->hoverHide);
}

void TestPetStore::theSoundIsOffByDefaultAndValidated()
{
    const QString id(64, QLatin1Char('a'));
    const PetPlacement base = make();
    QVERIFY(base.audio.isDefault());
    QVERIFY(!base.audio.active());
    QCOMPARE(base.audio.volume, 75);
    QVERIFY2(!base.toJson().contains(u"audio"_s), "a default sound is not written");
    // a partial object merges field by field
    auto p = applyPlacementChanges(base, testutil::json(("{\"audio\": {\"asset\": \"" + id.toStdString() + "\", \"mode\": \"click\"}}").c_str()));
    QVERIFY(p);
    QVERIFY(p->audio.active());
    QCOMPARE(p->audio.volume, 75);
    QCOMPARE(p->audio.mode, PetAudioMode::Click);
    auto q = applyPlacementChanges(*p, testutil::json("{\"audio\": {\"volume\": 20, \"offsetMs\": -5000}}"));
    QVERIFY(q);
    QCOMPARE(q->audio.volume, 20);
    QCOMPARE(q->audio.offsetMs, -5000);
    QCOMPARE(q->audio.asset, id);
    QCOMPARE(q->toJson().value(u"audio"_s).toObject().value(u"mode"_s).toString(), u"click"_s);
    // null removes it; other keys leave it alone
    QVERIFY(applyPlacementChanges(*q, testutil::json("{\"audio\": null}"))->audio.isDefault());
    QCOMPARE(applyPlacementChanges(*q, testutil::json("{\"width\": 300}"))->audio, q->audio);
    // refusals
    for (const char *bad : {"{\"audio\": 3}", "{\"audio\": {\"mode\": \"loud\"}}", "{\"audio\": {\"mode\": 1}}", "{\"audio\": {\"volume\": 101}}",
                            "{\"audio\": {\"volume\": -1}}", "{\"audio\": {\"volume\": 1.5}}", "{\"audio\": {\"volume\": \"9\"}}",
                            "{\"audio\": {\"offsetMs\": 5001}}", "{\"audio\": {\"offsetMs\": -5001}}", "{\"audio\": {\"asset\": \"abc\"}}",
                            "{\"audio\": {\"asset\": 5}}", "{\"audio\": {\"asset\": \"../../etc/passwd\"}}"}) {
        QString error;
        QVERIFY2(!applyPlacementChanges(base, testutil::json(bad), &error), bad);
        QVERIFY2(error.contains(u"audio"_s), bad);
    }
    QVERIFY(!applyPlacementChanges(base, testutil::json(("{\"audio\": {\"asset\": \"" + std::string(64, 'A') + "\"}}").c_str()))); // uppercase is not an id
    QVERIFY(applyPlacementChanges(base, testutil::json("{\"audio\": {\"asset\": null, \"mode\": \"once\"}, \"width\": 200}"))); // a mode without an asset is silent, not an error
    QVERIFY(!applyPlacementChanges(base, testutil::json("{\"audio\": {\"mode\": \"once\"}, \"width\": 2}"))); // the other fields are still checked
}

void TestPetStore::theSoundSurvivesTheFileAndABadOneCostsOnlyTheSound()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const std::string id(64, 'b');
    const auto entry = [](const std::string &uuid, const std::string &audio) {
        return "{\"petId\": \"" + uuid + "\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, "
               "\"width\": 100, \"opacity\": 0.9, \"playOriginal\": false, \"paused\": false, \"fps\": 24" + (audio.empty() ? "" : ", \"audio\": " + audio) + "}";
    };
    testutil::writeFile(path, ("{\"version\": 1, \"pets\": [" + entry("11111111-1111-1111-1111-111111111111",
                                    "{\"asset\": \"" + id + "\", \"mode\": \"animation_loop\", \"volume\": 40, \"offsetMs\": 250}")
                               + "," + entry("22222222-2222-2222-2222-222222222222", "{\"mode\": \"shout\", \"volume\": 400}")
                               + "," + entry("33333333-3333-3333-3333-333333333333", "") + "]}").c_str());
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 3);
    QCOMPARE(store.pets().at(0).audio.mode, PetAudioMode::AnimationLoop);
    QCOMPARE(store.pets().at(0).audio.volume, 40);
    QCOMPARE(store.pets().at(0).audio.offsetMs, 250);
    QVERIFY(store.pets().at(1).audio.isDefault()); // dropped, the pet is kept
    QVERIFY(store.pets().at(2).audio.isDefault());
    QVERIFY(backups(tmp).isEmpty());
    QCOMPARE(applyPlacementChanges(make(), store.pets().at(0).toJson())->audio, store.pets().at(0).audio); // written and read back
}

void TestPetStore::aPetIsNotAStageByDefaultAndTheFieldIsValidated()
{
    const PetPlacement base = make();
    QVERIFY(base.stage.isDefault());
    QVERIFY(!base.stage.active());
    QVERIFY2(!base.toJson().contains(u"stage"_s), "a pet that is not a stage writes no key");
    // a partial object merges field by field, the unnamed keep their value
    auto p = applyPlacementChanges(base, testutil::json("{\"stage\": {\"mode\": \"output\"}}"));
    QVERIFY(p);
    QVERIFY(p->stage.active());
    QCOMPARE(p->stage.fit, StageFit::Contain);
    QCOMPARE(p->stage.overFullscreen, StageOverFullscreen::Inherit);
    auto q = applyPlacementChanges(*p, testutil::json("{\"stage\": {\"fit\": \"cover\", \"overFullscreen\": \"show\"}}"));
    QVERIFY(q);
    QCOMPARE(q->stage.mode, StageMode::Output);
    QCOMPARE(q->stage.fit, StageFit::Cover);
    QCOMPARE(q->stage.overFullscreen, StageOverFullscreen::Show);
    QCOMPARE(q->toJson().value(u"stage"_s).toObject().value(u"fit"_s).toString(), u"cover"_s);
    // the placement's other fields are kept untouched while it is a stage, and come back when it is turned off
    auto moved = applyPlacementChanges(base, testutil::json("{\"x\": 40, \"y\": 50, \"width\": 300, \"stage\": {\"mode\": \"desktop\"}}"));
    QVERIFY(moved);
    auto off = applyPlacementChanges(*moved, testutil::json("{\"stage\": {\"mode\": \"off\"}}"));
    QVERIFY(off);
    QCOMPARE(off->x, 40.0);
    QCOMPARE(off->y, 50.0);
    QCOMPARE(off->width, 300.0);
    QVERIFY(!off->stage.active());
    // null clears it entirely (back to the default, which is not written); other keys leave it alone
    QVERIFY(applyPlacementChanges(*q, testutil::json("{\"stage\": null}"))->stage.isDefault());
    QCOMPARE(applyPlacementChanges(*q, testutil::json("{\"width\": 300}"))->stage, q->stage);
    // unknown keys of stage are ignored
    QVERIFY(applyPlacementChanges(base, testutil::json("{\"stage\": {\"mode\": \"output\", \"grabbed\": true}}")));
    // refusals: a wrong type or an unknown value is an error, never a silent default
    for (const char *bad : {"{\"stage\": 3}", "{\"stage\": \"output\"}", "{\"stage\": []}", "{\"stage\": {\"mode\": \"full\"}}", "{\"stage\": {\"mode\": 1}}",
                            "{\"stage\": {\"mode\": null}}", "{\"stage\": {\"mode\": \"OUTPUT\"}}", "{\"stage\": {\"fit\": \"fill\"}}",
                            "{\"stage\": {\"fit\": true}}", "{\"stage\": {\"overFullscreen\": \"yes\"}}", "{\"stage\": {\"overFullscreen\": 0}}"}) {
        QString error;
        QVERIFY2(!applyPlacementChanges(base, testutil::json(bad), &error), bad);
        QVERIFY2(error.contains(u"stage"_s), bad);
    }
    QVERIFY(!applyPlacementChanges(base, testutil::json("{\"stage\": {\"mode\": \"output\"}, \"width\": 2}"))); // the other fields are still checked
}

void TestPetStore::aStageSurvivesTheFileAndABadOneCostsOnlyTheStage()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const auto entry = [](const std::string &uuid, const std::string &stage) {
        return "{\"petId\": \"" + uuid + "\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, "
               "\"width\": 100, \"opacity\": 0.9, \"playOriginal\": false, \"paused\": false, \"fps\": 24" + (stage.empty() ? "" : ", \"stage\": " + stage) + "}";
    };
    testutil::writeFile(path, ("{\"version\": 1, \"pets\": [" + entry("11111111-1111-1111-1111-111111111111", "{\"mode\": \"desktop\", \"fit\": \"stretch\"}")
                               + "," + entry("22222222-2222-2222-2222-222222222222", "{\"mode\": \"everywhere\"}")
                               + "," + entry("33333333-3333-3333-3333-333333333333", "") + "]}").c_str()); // (a revision 14 file has no stage at all)
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 3);
    QCOMPARE(store.pets().at(0).stage.mode, StageMode::Desktop);
    QCOMPARE(store.pets().at(0).stage.fit, StageFit::Stretch);
    QVERIFY(store.pets().at(1).stage.isDefault()); // dropped, the pet is kept
    QVERIFY(store.pets().at(2).stage.isDefault());
    QVERIFY(backups(tmp).isEmpty());
    QCOMPARE(applyPlacementChanges(make(), store.pets().at(0).toJson())->stage, store.pets().at(0).stage); // written and read back
}

void TestPetStore::oneStagePerOutputAndDesktopTakesThemAll()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    PetPlacement a = make();
    a.output = u"DP-1"_s;
    a.stage.mode = StageMode::Output;
    QString error;
    const QString idA = store.add(a, &error);
    QVERIFY2(!idA.isEmpty(), qPrintable(error));
    PetPlacement b = make();
    b.output = u"DP-2"_s;
    b.stage.mode = StageMode::Output;
    const QString idB = store.add(b, &error);
    QVERIFY2(!idB.isEmpty(), qPrintable(error)); // another output is free
    PetPlacement same = make();
    same.output = u"DP-1"_s;
    same.stage.mode = StageMode::Output;
    QVERIFY(store.add(same, &error).isEmpty());
    QVERIFY(error.contains(u"stage"_s));
    PetPlacement desktop = make();
    desktop.stage.mode = StageMode::Desktop;
    QVERIFY(store.add(desktop, &error).isEmpty()); // two outputs are taken
    // an ordinary pet on a stage's output is fine
    PetPlacement plain = make();
    plain.output = u"DP-1"_s;
    QVERIFY(!store.add(plain, &error).isEmpty());
    // turning a plain pet into a stage on a taken output is refused by update(), and the stage can be moved to a free output
    auto plainPet = store.pets().last();
    plainPet.stage.mode = StageMode::Output;
    QVERIFY(!store.update(plainPet, &error));
    QVERIFY(error.contains(u"stage"_s));
    auto first = *store.pet(idA);
    QVERIFY(store.update(first, &error) == false && error.isEmpty()); // (unchanged: no write, no error)
    first.stage.fit = StageFit::Cover; // a change to itself is not a conflict with itself
    QVERIFY2(store.update(first, &error), qPrintable(error));
    // after both are off, a desktop stage is possible
    for (const QString &id : {idA, idB}) {
        auto pet = *store.pet(id);
        pet.stage.mode = StageMode::Off;
        QVERIFY(store.update(pet, &error));
    }
    QVERIFY2(!store.add(desktop, &error).isEmpty(), qPrintable(error));
    // and then no other stage fits
    PetPlacement another = make();
    another.output = u"DP-3"_s;
    another.stage.mode = StageMode::Output;
    QVERIFY(store.add(another, &error).isEmpty());
}

void TestPetStore::aPetWithoutStatesOrTrackingWritesExactlyTheOldKeys()
{
    // the keys a placement had before the states existed: a pet that does not use them is byte for byte what it was
    const QStringList oldKeys{u"petId"_s, u"itemId"_s, u"variant"_s, u"output"_s, u"x"_s, u"y"_s, u"width"_s, u"opacity"_s, u"playOriginal"_s, u"paused"_s, u"fps"_s,
                              u"speed"_s, u"clickThrough"_s, u"locked"_s, u"keepAbove"_s, u"hoverHide"_s, u"focusFade"_s, u"smartDodge"_s, u"rotation"_s, u"flipH"_s,
                              u"flipV"_s};
    const PetPlacement base = make();
    QVERIFY(base.states.isDefault());
    QVERIFY(base.tracking.isDefault());
    QStringList keys = base.toJson().keys();
    QStringList expected = oldKeys;
    keys.sort();
    expected.sort();
    QCOMPARE(keys, expected);
    QCOMPARE(base.variantsUsed(), QStringList{base.variant});
    // explicit defaults are not written either
    const auto same = applyPlacementChanges(base, testutil::json("{\"states\": {}, \"tracking\": {\"mode\": \"off\", \"distance\": 160, \"speed\": 1, \"returnHome\": true}}"));
    QVERIFY(same);
    QCOMPARE(same->toJson(), base.toJson());
    // a pet that uses them writes the keys, and what they name is among the variants it can show
    const auto used = applyPlacementChanges(base, testutil::json("{\"states\": {\"grabbed\": {\"variant\": \"g\"}, \"idle\": {\"sequence\": {\"items\": [\"a\", \"g\"]}}}}"));
    QVERIFY(used);
    QVERIFY(used->toJson().contains(u"states"_s));
    QVERIFY(!used->toJson().contains(u"tracking"_s));
    QCOMPARE(used->variantsUsed(), (QStringList{base.variant, u"a"_s, u"g"_s})); // in the order of the states
}

void TestPetStore::statesAreValidatedMergedAndCleared()
{
    const PetPlacement base = make();
    QString error;
    auto p = applyPlacementChanges(base, testutil::json("{\"states\": {\"grabbed\": {\"variant\": \"g\"}, \"released\": {\"variant\": \"r\"}, \"colour\": 3}}"), &error);
    QVERIFY2(p.has_value(), qPrintable(error));
    QCOMPARE(p->states.of(PetState::Grabbed)->variant, u"g"_s);
    QVERIFY(!p->states.of(PetState::Idle)); // unknown variants are accepted (named later), absent states stay absent
    // partial merge: another state joins, one is replaced, one cleared
    auto q = applyPlacementChanges(*p, testutil::json("{\"states\": {\"moving\": {\"variant\": \"m\"}, \"grabbed\": {\"variant\": \"g2\"}, \"released\": null}}"), &error);
    QVERIFY2(q.has_value(), qPrintable(error));
    QCOMPARE(q->states.of(PetState::Grabbed)->variant, u"g2"_s);
    QCOMPARE(q->states.of(PetState::Moving)->variant, u"m"_s);
    QVERIFY(!q->states.of(PetState::Released));
    // an unrelated change leaves them; null clears all
    QCOMPARE(applyPlacementChanges(*q, testutil::json("{\"width\": 300}"))->states, q->states);
    QVERIFY(applyPlacementChanges(*q, testutil::json("{\"states\": null}"))->states.isDefault());
    // written and read back
    QCOMPARE(applyPlacementChanges(make(), q->toJson())->states, q->states);
    // 16 entries are the most
    QString items16, items17;
    for (int i = 0; i < 17; ++i)
        (i < 16 ? items16 : items17) += (items16.isEmpty() || i >= 16 ? u""_s : u","_s) + u"\"v%1\""_s.arg(i);
    const QString seq16 = u"{\"states\": {\"idle\": {\"sequence\": {\"items\": [%1]}}}}"_s.arg(items16);
    QVERIFY(applyPlacementChanges(base, QJsonDocument::fromJson(seq16.toUtf8()).object()).has_value());
    const QString seq17 = u"{\"states\": {\"idle\": {\"sequence\": {\"items\": [%1,%2]}}}}"_s.arg(items16, items17);
    QVERIFY(!applyPlacementChanges(base, QJsonDocument::fromJson(seq17.toUtf8()).object(), &error).has_value());
    QVERIFY(error.contains(u"states"_s));
    for (const char *bad : {"{\"states\": 3}", "{\"states\": []}", "{\"states\": \"idle\"}", "{\"states\": {\"idle\": 3}}", "{\"states\": {\"idle\": {}}}",
                            "{\"states\": {\"idle\": {\"variant\": 4}}}", "{\"states\": {\"idle\": {\"variant\": \"a\", \"sequence\": {\"items\": [\"b\"]}}}}",
                            "{\"states\": {\"idle\": {\"sequence\": {\"items\": []}}}}", "{\"states\": {\"idle\": {\"sequence\": {\"items\": [\"a\"], \"order\": \"x\"}}}}"}) {
        error.clear();
        QVERIFY2(!applyPlacementChanges(base, testutil::json(bad), &error), bad);
        QVERIFY2(error.contains(u"states"_s), bad);
    }
    QVERIFY(!applyPlacementChanges(base, testutil::json("{\"states\": {\"idle\": {\"variant\": \"a\"}}, \"width\": 2}"))); // the other fields are still checked
}

void TestPetStore::trackingIsValidatedMergedAndCleared()
{
    const PetPlacement base = make();
    QString error;
    auto p = applyPlacementChanges(base, testutil::json("{\"tracking\": {\"mode\": \"look\"}}"), &error);
    QVERIFY2(p.has_value(), qPrintable(error));
    QCOMPARE(p->tracking.mode, PetTrackingMode::Look);
    QCOMPARE(p->tracking.distance, 160.0);
    QVERIFY(p->tracking.returnHome);
    auto q = applyPlacementChanges(*p, testutil::json("{\"tracking\": {\"mode\": \"follow\", \"distance\": 400}}"), &error);
    QVERIFY2(q.has_value(), qPrintable(error));
    QCOMPARE(q->tracking.distance, 400.0);
    auto r = applyPlacementChanges(*q, testutil::json("{\"tracking\": {\"speed\": 2, \"returnHome\": false}}"));
    QCOMPARE(r->tracking.mode, PetTrackingMode::Follow); // key by key
    QCOMPARE(r->tracking.distance, 400.0);
    QCOMPARE(r->tracking.speed, 2.0);
    QVERIFY(!r->tracking.returnHome);
    QVERIFY(applyPlacementChanges(*r, testutil::json("{\"tracking\": null}"))->tracking.isDefault());
    QVERIFY(!applyPlacementChanges(*r, testutil::json("{\"tracking\": null}"))->toJson().contains(u"tracking"_s));
    QCOMPARE(applyPlacementChanges(make(), r->toJson())->tracking, r->tracking);
    for (const char *bad : {"{\"tracking\": 1}", "{\"tracking\": \"look\"}", "{\"tracking\": {\"mode\": \"chase\"}}", "{\"tracking\": {\"distance\": -5}}",
                            "{\"tracking\": {\"distance\": 5000}}", "{\"tracking\": {\"speed\": 0}}", "{\"tracking\": {\"speed\": 5}}", "{\"tracking\": {\"returnHome\": 1}}"}) {
        error.clear();
        QVERIFY2(!applyPlacementChanges(base, testutil::json(bad), &error), bad);
        QVERIFY2(error.contains(u"tracking"_s), bad);
    }
}

void TestPetStore::followExcludesDodgeAndFades()
{
    const PetPlacement base = make();
    QString error;
    for (const char *flag : {"smartDodge", "hoverHide", "focusFade"}) {
        // follow with the flag, in either order or in one call: refused, nothing switched off silently
        PetPlacement withFlag = base;
        auto flagged = applyPlacementChanges(base, testutil::json((std::string("{\"") + flag + "\": true}").c_str()));
        QVERIFY(flagged);
        error.clear();
        QVERIFY2(!applyPlacementChanges(*flagged, testutil::json("{\"tracking\": {\"mode\": \"follow\"}}"), &error), flag);
        QVERIFY2(error.contains(u"follow"_s), flag);
        QVERIFY(!applyPlacementChanges(base, testutil::json((std::string("{\"tracking\": {\"mode\": \"follow\"}, \"") + flag + "\": true}").c_str())));
        auto follower = applyPlacementChanges(base, testutil::json("{\"tracking\": {\"mode\": \"follow\"}}"));
        QVERIFY(follower);
        error.clear();
        QVERIFY2(!applyPlacementChanges(*follower, testutil::json((std::string("{\"") + flag + "\": true}").c_str()), &error), flag);
        QVERIFY2(error.contains(u"follow"_s), flag);
        // turning the flag off in the same call is fine
        QVERIFY(applyPlacementChanges(*flagged, testutil::json((std::string("{\"tracking\": {\"mode\": \"follow\"}, \"") + flag + "\": false}").c_str())));
    }
    // look and the dodge are compatible; so are look and the fades
    auto look = applyPlacementChanges(base, testutil::json("{\"tracking\": {\"mode\": \"look\"}, \"smartDodge\": true, \"hoverHide\": true}"));
    QVERIFY(look);
    // the store refuses it too
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    PetPlacement bad = make();
    bad.smartDodge = true;
    bad.tracking.mode = PetTrackingMode::Follow;
    QVERIFY(store.add(bad, &error).isEmpty());
    QVERIFY(error.contains(u"follow"_s));
}

void TestPetStore::nullKeysInAFileAndAStageThatKeepsFlagsAndTheDistinctVariantCap()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [{\"petId\": \"11111111-1111-1111-1111-111111111111\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", "
                              "\"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 0.9, \"playOriginal\": false, \"paused\": false, \"fps\": 24, \"states\": null, \"tracking\": null}]}");
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 1);
    QVERIFY(store.pets().at(0).states.isDefault() && store.pets().at(0).tracking.isDefault());
    QVERIFY(backups(tmp).isEmpty());
    // a stage keeps the pointer flags (they are not used while it is one), and follow still excludes them: turning the stage off would otherwise give an invalid pet
    QString error;
    const auto stage = applyPlacementChanges(make(), testutil::json("{\"smartDodge\": true, \"stage\": {\"mode\": \"output\"}}"));
    QVERIFY(stage);
    QVERIFY(!applyPlacementChanges(*stage, testutil::json("{\"tracking\": {\"mode\": \"follow\"}}"), &error));
    QVERIFY(error.contains(u"follow"_s));
    // at most 16 different animations in all, though four states of 16 would name 64
    QString first, second;
    for (int i = 0; i < 16; ++i)
        first += (i ? u","_s : QString()) + u"\"a%1\""_s.arg(i);
    for (int i = 0; i < 2; ++i)
        second += (i ? u","_s : QString()) + u"\"b%1\""_s.arg(i);
    const QString ok = u"{\"states\": {\"idle\": {\"sequence\": {\"items\": [%1]}}, \"grabbed\": {\"variant\": \"a3\"}}}"_s.arg(first);
    QVERIFY(applyPlacementChanges(make(), QJsonDocument::fromJson(ok.toUtf8()).object()).has_value()); // (repeats are one animation)
    const QString tooMany = u"{\"states\": {\"idle\": {\"sequence\": {\"items\": [%1]}}, \"grabbed\": {\"sequence\": {\"items\": [%2]}}}}"_s.arg(first, second);
    QVERIFY(!applyPlacementChanges(make(), QJsonDocument::fromJson(tooMany.toUtf8()).object(), &error).has_value());
    QVERIFY(error.contains(u"16 different"_s));
}

void TestPetStore::statesAndTrackingSurviveTheFileAndBadOnesCostOnlyThemselves()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const auto entry = [](const std::string &uuid, const std::string &extra) {
        return "{\"petId\": \"" + uuid + "\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, "
               "\"width\": 100, \"opacity\": 0.9, \"playOriginal\": false, \"paused\": false, \"fps\": 24" + extra + "}";
    };
    testutil::writeFile(path, ("{\"version\": 1, \"pets\": ["
                               + entry("11111111-1111-1111-1111-111111111111", ", \"states\": {\"grabbed\": {\"variant\": \"g\"}}, \"tracking\": {\"mode\": \"look\"}")
                               + "," + entry("22222222-2222-2222-2222-222222222222", ", \"states\": {\"grabbed\": 7}, \"tracking\": {\"mode\": \"chase\"}")
                               + "," + entry("33333333-3333-3333-3333-333333333333", ", \"smartDodge\": true, \"tracking\": {\"mode\": \"follow\"}")
                               + "," + entry("44444444-4444-4444-4444-444444444444", "") + "]}").c_str()); // (a revision 18 file has neither)
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 4);
    QCOMPARE(store.pets().at(0).states.of(PetState::Grabbed)->variant, u"g"_s);
    QCOMPARE(store.pets().at(0).tracking.mode, PetTrackingMode::Look);
    QVERIFY(store.pets().at(1).states.isDefault()); // dropped, the pet is kept
    QVERIFY(store.pets().at(1).tracking.isDefault());
    QVERIFY(store.pets().at(2).smartDodge); // a follower that also dodges: it loses the following, not the dodge nor the file
    QVERIFY(store.pets().at(2).tracking.isDefault());
    QVERIFY(store.pets().at(3).states.isDefault() && store.pets().at(3).tracking.isDefault());
    QVERIFY(backups(tmp).isEmpty());
    // an update writes them and the file reads back the same
    auto first = store.pets().at(0);
    first.opacity = 0.8;
    QVERIFY(store.update(first));
    PetStore again(path);
    again.load();
    QCOMPARE(again.pets().at(0).states, first.states);
    QCOMPARE(again.pets().at(0).tracking, first.tracking);
}

void TestPetStore::aPetWhoseStageAFileMadeConflictCanStillBeEdited()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const auto entry = [](const std::string &uuid) {
        return "{\"petId\": \"" + uuid + "\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 0.9, "
               "\"playOriginal\": false, \"paused\": false, \"fps\": 24, \"stage\": {\"mode\": \"output\"}}";
    };
    testutil::writeFile(path, ("{\"version\": 1, \"pets\": [" + entry("11111111-1111-1111-1111-111111111111") + "," + entry("22222222-2222-2222-2222-222222222222") + "]}").c_str());
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 2); // (a hand-edited file with two stages on one output loads)
    QString error;
    auto second = store.pets().at(1);
    second.opacity = 0.5;
    QVERIFY2(store.update(second, &error), qPrintable(error)); // an unrelated edit is not refused
    second.output = u"DP-2"_s;
    QVERIFY2(store.update(second, &error), qPrintable(error)); // moving it to a free output is fine
    second.output = u"DP-1"_s;
    QVERIFY(!store.update(second, &error)); // moving it onto the other's is not
    QVERIFY(error.contains(u"stage"_s));
}

void TestPetStore::rotationIsWrappedAndRounded()
{
    const auto rotationOf = [](const char *json) {
        const auto p = applyPlacementChanges(make(), testutil::json(json));
        return p ? p->rotation : -1.0;
    };
    QCOMPARE(rotationOf("{\"rotation\": 0}"), 0.0);
    QCOMPARE(rotationOf("{\"rotation\": 90}"), 90.0);
    QCOMPARE(rotationOf("{\"rotation\": 359.99}"), 359.99);
    QCOMPARE(rotationOf("{\"rotation\": 360}"), 0.0);
    QCOMPARE(rotationOf("{\"rotation\": 370}"), 10.0);
    QCOMPARE(rotationOf("{\"rotation\": -90}"), 270.0);
    QCOMPARE(rotationOf("{\"rotation\": -720}"), 0.0);
    QCOMPARE(rotationOf("{\"rotation\": 33.333}"), 33.33);
    QCOMPARE(rotationOf("{\"rotation\": 359.999}"), 0.0); // rounds up to a whole turn
    QCOMPARE(rotationOf("{\"rotation\": 1e12}"), -1.0);   // not a sane angle: refused, not wrapped
    PetPlacement p = make();
    p.rotation = 360;
    QString error;
    QVERIFY(!validatePlacement(p, &error));
    p.rotation = -1;
    QVERIFY(!validatePlacement(p, &error));
    p.rotation = std::nan("");
    QVERIFY(!validatePlacement(p, &error));
    p.rotation = std::numeric_limits<double>::infinity();
    QVERIFY(!validatePlacement(p, &error));
    QCOMPARE(normalizedPetRotation(std::nan("")), 0.0);
    QCOMPARE(normalizedPetRotation(-0.001), 0.0);
    // a stored file with an angle out of range is wrapped, not quarantined
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [{\"petId\": \"11111111-1111-1111-1111-111111111111\", \"itemId\": \"anima:5\", "
                              "\"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 1, "
                              "\"playOriginal\": false, \"paused\": false, \"rotation\": 450}]}");
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 1);
    QCOMPARE(store.pets().first().rotation, 90.0);
}

void TestPetStore::updateAllWritesOnce()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    PetPlacement a = make();
    a.locked = true;
    PetPlacement b = make();
    b.output = u"DP-2"_s;
    QVERIFY(!store.add(a).isEmpty());
    QVERIFY(!store.add(b).isEmpty());
    QSignalSpy changed(&store, &PetStore::changed);
    QVERIFY(store.updateAll([](PetPlacement &p) { p.locked = false; p.rotation = 370; }));
    QCOMPARE(changed.count(), 1); // one write, one signal, for both
    for (const PetPlacement &p : store.pets())
        QVERIFY(!p.locked && p.rotation == 10.0);
    // nothing to change: false, no signal, no error
    QString error;
    QVERIFY(!store.updateAll([](PetPlacement &) {}, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(changed.count(), 1);
    // a result that is not valid: nothing changes
    const QList<PetPlacement> before = store.pets();
    QVERIFY(!store.updateAll([](PetPlacement &p) { p.width = 1; }, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.pets(), before);
    QCOMPARE(changed.count(), 1);
    PetStore again(tmp.filePath(u"pets.json"_s));
    again.load();
    QCOMPARE(again.pets(), before);
}

void TestPetStore::updateAllWithAFailingWriteChangesNothing()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"cfg"_s);
    QVERIFY(QDir().mkpath(dir));
    PetStore store(dir + u"/pets.json"_s);
    store.load();
    QVERIFY(!store.add(make()).isEmpty());
    const QList<PetPlacement> before = store.pets();
    QSignalSpy changed(&store, &PetStore::changed);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner); // the file can not be written any more
    const auto restore = qScopeGuard([&] { QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
    if (QFileInfo(dir).isWritable())
        QSKIP("the directory can be written whatever its mode (running as root?)");
    QString error;
    QVERIFY(!store.updateAll([](PetPlacement &p) { p.locked = true; }, &error));
    QVERIFY(error.contains(u"could not save"_s));
    QCOMPARE(store.pets(), before); // as it was, in memory too
    QCOMPARE(changed.count(), 0);
}

void TestPetStore::aFileWithBothExclusiveFlagsOrAWildAngleStillLoads()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const auto entry = [](const char *id, const char *extra) {
        return QByteArray("{\"petId\": \"") + id + "\", \"itemId\": \"anima:5\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, "
               "\"width\": 100, \"opacity\": 1, \"playOriginal\": false, \"paused\": false" + extra + "}";
    };
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [" + entry("11111111-1111-1111-1111-111111111111", ", \"hoverHide\": true, \"focusFade\": true") + ", "
                                  + entry("22222222-2222-2222-2222-222222222222", ", \"rotation\": 1e30, \"flipH\": true") + ", "
                                  + entry("33333333-3333-3333-3333-333333333333", ", \"rotation\": -1e12") + "]}");
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 3); // no pet was lost to the quarantine
    QVERIFY(backups(tmp).isEmpty());
    QVERIFY(store.pets().at(0).hoverHide && !store.pets().at(0).focusFade);
    QCOMPARE(store.pets().at(1).rotation, 0.0);
    QVERIFY(store.pets().at(1).flipH);
    QCOMPARE(store.pets().at(2).rotation, 0.0);
}

void TestPetStore::petsStoredWithoutFpsGetTheDefaultRate()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"pets.json"_s);
    const auto entry = [](const char *id, const char *item, const char *extra = "") {
        return QByteArray("{\"petId\": \"") + id + "\", \"itemId\": \"" + item
            + "\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 1, \"y\": 2, \"width\": 100, \"opacity\": 1, "
              "\"playOriginal\": false, \"paused\": false" + extra + "}";
    };
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [" + entry("11111111-1111-1111-1111-111111111111", "anima:5") + ", "
                                  + entry("22222222-2222-2222-2222-222222222222", "local:abc") + ", "
                                  + entry("33333333-3333-3333-3333-333333333333", "anima:6", ", \"fps\": 0") + ", "
                                  + entry("44444444-4444-4444-4444-444444444444", "anima:7", ", \"fps\": 15") + "]}");
    PetStore store(path);
    store.load();
    QCOMPARE(store.pets().size(), 4);
    QCOMPARE(store.pets().at(0).fps, 24); // from before the field: the default rate, Anima or not
    QCOMPARE(store.pets().at(1).fps, 24);
    QCOMPARE(store.pets().at(2).fps, 0);  // an explicit choice stays
    QCOMPARE(store.pets().at(3).fps, 15);
    QVERIFY(backups(tmp).isEmpty()); // an old file is not a bad one

    // a stored speed without an fps is the speed mode, not a clash with the default rate
    testutil::writeFile(path, "{\"version\": 1, \"pets\": [" + entry("55555555-5555-5555-5555-555555555555", "anima:8", ", \"speed\": 2") + "]}");
    PetStore sped(path);
    sped.load();
    QCOMPARE(sped.pets().size(), 1);
    QCOMPARE(sped.pets().at(0).timing(), (PetTiming{0, 2}));
}

void TestPetStore::playbackSpeedFollowsTheNativeRate()
{
    // A file of 60 fps delays, played at 30 fps, takes twice as long.
    QCOMPARE(playbackSpeed(30, 60), 0.5);
    QCOMPARE(playbackSpeed(15, 30), 0.5);
    QCOMPARE(playbackSpeed(60, 30), 2.0);
    QCOMPARE(playbackSpeed(30, 30), 1.0);
    QCOMPARE(playbackSpeed(0, 60), 1.0);   // the file's own timing
    QCOMPARE(playbackSpeed(30, 0), 1.0);   // a still image has no rate
    QCOMPARE(playbackSpeed(30, -3), 1.0);
    QCOMPARE(playbackSpeed(30, std::nan("")), 1.0);
}

void TestPetStore::referencesItem()
{
    QTemporaryDir tmp;
    PetStore store(tmp.filePath(u"pets.json"_s));
    store.load();
    store.add(make());
    QVERIFY(store.referencesItem(u"anima:1"_s));
    QVERIFY(!store.referencesItem(u"anima:2"_s));
}

QTEST_GUILESS_MAIN(TestPetStore)
#include "tst_petstore.moc"
