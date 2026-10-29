#include "importsettings.h"
#include "settings.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSettingsStore : public QObject
{
    Q_OBJECT

private slots:
    void defaultsWhenFileMissing();
    void importSettingsAreValidatedAndPersisted();
    void roundTripsAcrossInstances();
    void mergeRejectsInvalidWithoutPartialApply_data();
    void mergeRejectsInvalidWithoutPartialApply();
    void integralDoubleIsAccepted();
    void boundariesAreInclusive();
    void unchangedMergeIsSilent();
    void eachChangingMergeEmitsOnce();
    void unknownKeysInFileAreDropped();
    void corruptFileIsKeptAsBackup();
    void invalidValueInFileIsKeptAsBackup();
    void nonObjectFileIsKeptAsBackup();
    void presenterIsValidatedAndPersisted();
    void invalidPresenterInFileIsQuarantined();
    void sceneEngineIsValidatedAndPersisted();
    void aFileWithoutSceneEngineMeansAutoAndAnExplicitChoiceStays();
    void invalidSceneEngineInFileIsQuarantined();
    void pauseRulesAreValidatedAndPersisted();
    void invalidPauseRulesInTheFileAreQuarantined();
    void aFileFromBeforePauseRulesGetsTheirDefaults();
    void sandboxEnginesIsValidatedAndPersisted();
    void requireSandboxIsValidatedAndPersisted();
    void mediaRemoteArtIsValidatedAndPersisted();
    void mediaShareIsValidatedAndPersisted();
    void petsSoundSettingsAreValidatedAndPersisted();
    void shortcutsLaunchIsValidatedAndPersisted();
    void stageSettingsAreValidatedAndPersisted();
    void puppetSettingsAreValidatedAndPersisted();
    void aCoreFileMustBeSafeToLoad();
    void petLimitsAreValidatedAndPersisted();
    void sceneRenderScaleIsValidatedAndPersisted();
    void aFileFromBeforeTheSandboxMeansSandboxed();
    void failedSaveRollsBack();
    void blockedSaveRollsBack();
    void petsHideOnGameIsABooleanOffByDefault();
};

static // The import defaults (importsettings.h) and the stage settings every settings object now carries.
QJsonObject withImports(QJsonObject o)
{
    ImportSettings{}.write(o);
    AutomationSettings{}.write(o);
    PuppetSettings{}.write(o);
    if (!o.contains(u"stageHideOverFullscreen"_s))
        o.insert(u"stageHideOverFullscreen"_s, true);
    if (!o.contains(u"stageMaxPixels"_s))
        o.insert(u"stageMaxPixels"_s, 8294400);
    if (!o.contains(u"petsHideOnGame"_s))
        o.insert(u"petsHideOnGame"_s, false);
    return o;
}

QJsonObject defaults()
{
    return withImports(QJsonObject{{u"webFpsLimit"_s, 30}, {u"pauseWhenCovered"_s, true}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}});
}

static QJsonObject readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void TestSettingsStore::importSettingsAreValidatedAndPersisted()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"s.json"_s);
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.importSettings().format, u"apng"_s);
    QCOMPARE(store.importSettings().sizePolicy, u"fit"_s);
    QCOMPARE(store.importSettings().fps, 24);
    QCOMPARE(store.importSettings().workshopFiles, u"ask"_s);
    QVERIFY(!store.importSettings().convertGifOnImport && !store.importSettings().keepSource);
    QString error;
    for (const QJsonObject &bad : {QJsonObject{{u"importFormat"_s, u"gif"_s}}, QJsonObject{{u"importFps"_s, 0}}, QJsonObject{{u"importFps"_s, 24.5}},
                                   QJsonObject{{u"importVideoMaxSide"_s, 99999}}, QJsonObject{{u"importMaxOutputMiB"_s, 5}}, QJsonObject{{u"importWorkshopMdaa"_s, true}},
                                   QJsonObject{{u"convertGifOnImport"_s, u"yes"_s}}, QJsonObject{{u"importSizePolicy"_s, u"stretch"_s}}}) {
        QVERIFY2(!store.merge(bad, &error), qPrintable(bad.keys().first()));
        QVERIFY(error.contains(bad.keys().first()));
    }
    QVERIFY(store.merge({{u"importFormat"_s, u"webp"_s}, {u"importFps"_s, 12}, {u"convertGifOnImport"_s, true}, {u"importWorkshopFiles"_s, u"auto"_s}, {u"importVideoMaxFps"_s, 60}}));
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.importSettings().format, u"webp"_s);
    QCOMPARE(again.importSettings().fps, 12);
    QVERIFY(again.importSettings().convertGifOnImport);
    QCOMPARE(again.importSettings().workshopFiles, u"auto"_s);
    QCOMPARE(again.values().value(u"importVideoMaxFps"_s).toInt(), 60);
}

void TestSettingsStore::defaultsWhenFileMissing()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    store.load();
    QCOMPARE(store.values(), defaults());
    QVERIFY(!QFile::exists(store.filePath()));
}

void TestSettingsStore::roundTripsAcrossInstances()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"sub/settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QVERIFY(store.merge({{u"webFpsLimit"_s, 144}, {u"pauseWhenCovered"_s, false}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values(), (withImports(QJsonObject{{u"webFpsLimit"_s, 144}, {u"pauseWhenCovered"_s, false}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}})));
}

void TestSettingsStore::mergeRejectsInvalidWithoutPartialApply_data()
{
    QTest::addColumn<QJsonObject>("changes");
    QTest::newRow("unknown key") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"bogus"_s, 1}};
    QTest::newRow("fps too low") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"webFpsLimit"_s, 9}};
    QTest::newRow("fps too high") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"webFpsLimit"_s, 145}};
    QTest::newRow("fps string") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"webFpsLimit"_s, u"60"_s}};
    QTest::newRow("fps fractional") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"webFpsLimit"_s, 30.5}};
    QTest::newRow("fps negative") << QJsonObject{{u"webFpsLimit"_s, -1}};
    QTest::newRow("fps huge") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"webFpsLimit"_s, 1e300}};
    QTest::newRow("fps bool") << QJsonObject{{u"webFpsLimit"_s, true}};
    QTest::newRow("pause int") << QJsonObject{{u"webFpsLimit"_s, 60}, {u"pauseWhenCovered"_s, 1}};
    QTest::newRow("presenter unknown") << QJsonObject{{u"presenter"_s, u"x11"_s}};
    QTest::newRow("presenter case") << QJsonObject{{u"presenter"_s, u"Plasma"_s}};
    QTest::newRow("presenter empty") << QJsonObject{{u"presenter"_s, QString()}};
    QTest::newRow("presenter int") << QJsonObject{{u"presenter"_s, 1}};
    QTest::newRow("presenter partial apply") << QJsonObject{{u"pauseWhenCovered"_s, false}, {u"presenter"_s, u"gnome"_s}};
    QTest::newRow("pause null") << QJsonObject{{u"pauseWhenCovered"_s, QJsonValue::Null}};
}

void TestSettingsStore::mergeRejectsInvalidWithoutPartialApply()
{
    QFETCH(QJsonObject, changes);
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    store.load();
    QSignalSpy spy(&store, &SettingsStore::changed);

    QString error;
    QVERIFY(!store.merge(changes, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.values(), defaults());
    QCOMPARE(spy.count(), 0);
    QVERIFY(!QFile::exists(store.filePath()));
}

void TestSettingsStore::integralDoubleIsAccepted()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    QVERIFY(store.merge({{u"webFpsLimit"_s, 60.0}}));
    QCOMPARE(store.values().value(u"webFpsLimit"_s).toInt(), 60);
}

void TestSettingsStore::presenterIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"presenter"_s).toString(), u"auto"_s);
        QSignalSpy spy(&store, &SettingsStore::changed);
        for (const QString &value : {u"plasma"_s, u"layer-shell"_s, u"auto"_s}) {
            QVERIFY(store.merge({{u"presenter"_s, value}}));
            QCOMPARE(store.values().value(u"presenter"_s).toString(), value);
        }
        QCOMPARE(spy.count(), 3);
        QVERIFY(store.merge({{u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}})); // unchanged: silent
        QCOMPARE(spy.count(), 3);
        QVERIFY(store.merge({{u"presenter"_s, u"plasma"_s}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"presenter"_s).toString(), u"plasma"_s);
}

void TestSettingsStore::invalidPresenterInFileIsQuarantined()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{\"webFpsLimit\": 45, \"presenter\": \"wayland\"}");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values(), defaults());
    QVERIFY(!QFileInfo::exists(path));
}

void TestSettingsStore::boundariesAreInclusive()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    QVERIFY(store.merge({{u"webFpsLimit"_s, 10}}));
    QVERIFY(store.merge({{u"webFpsLimit"_s, 144}}));
    QVERIFY(!store.merge({{u"webFpsLimit"_s, 0}}));
}

void TestSettingsStore::unchangedMergeIsSilent()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    store.load();
    QSignalSpy spy(&store, &SettingsStore::changed);

    QVERIFY(store.merge({}));
    QVERIFY(store.merge({{u"webFpsLimit"_s, 30}, {u"pauseWhenCovered"_s, true}}));
    QVERIFY(store.merge({{u"webFpsLimit"_s, 30.0}}));
    QCOMPARE(spy.count(), 0);
    QVERIFY(!QFile::exists(store.filePath()));
}

void TestSettingsStore::eachChangingMergeEmitsOnce()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    QSignalSpy spy(&store, &SettingsStore::changed);
    QVERIFY(store.merge({{u"webFpsLimit"_s, 60}, {u"pauseWhenCovered"_s, false}}));
    QCOMPARE(spy.count(), 1);
    QVERIFY(store.merge({{u"webFpsLimit"_s, 60}, {u"pauseWhenCovered"_s, true}}));
    QCOMPARE(spy.count(), 2);
}

void TestSettingsStore::unknownKeysInFileAreDropped()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{\"webFpsLimit\": 45, \"futureKey\": [1, 2]}");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values(), (withImports(QJsonObject{{u"webFpsLimit"_s, 45}, {u"pauseWhenCovered"_s, true}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}})));
    QVERIFY(QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}, QDir::Files).isEmpty());

    QVERIFY(store.merge({{u"pauseWhenCovered"_s, false}}));
    QCOMPARE(readFile(path), (withImports(QJsonObject{{u"webFpsLimit"_s, 45}, {u"pauseWhenCovered"_s, false}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}})));
}

void TestSettingsStore::corruptFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{not json");
    SettingsStore store(path);
    store.load();

    QCOMPARE(store.values(), defaults());
    QVERIFY(!QFile::exists(path));
    const QStringList backups = QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    QFile backup(tmp.filePath(backups.first()));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), QByteArray("{not json"));

    QVERIFY(store.merge({{u"webFpsLimit"_s, 50}}));
    QVERIFY(QFile::exists(path));
}

void TestSettingsStore::invalidValueInFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{\"webFpsLimit\": 5000, \"pauseWhenCovered\": false}");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values(), defaults());
    QCOMPARE(QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}, QDir::Files).size(), 1);
}

void TestSettingsStore::nonObjectFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "[1, 2]");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values(), defaults());
    QCOMPARE(QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}, QDir::Files).size(), 1);
}

namespace {

// Makes a dir read-only for the scope; root ignores permissions, so tests skip then.
struct ReadOnlyDir {
    explicit ReadOnlyDir(const QString &path) : path(path)
    {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }
    ~ReadOnlyDir() { QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); }
    bool effective() const { return !QFileInfo(path).isWritable(); }
    QString path;
};

} // namespace

void TestSettingsStore::sceneEngineIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    QString chosen = u"vulkan"_s;
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"sceneEngine"_s).toString(), u"auto"_s); // the default since milestone 6
        QSignalSpy spy(&store, &SettingsStore::changed);
        const QStringList values{
            u"vulkan"_s,
            u"auto"_s,
        };
        for (const QString &value : values) {
            QVERIFY(store.merge({{u"sceneEngine"_s, value}}));
            QCOMPARE(store.values().value(u"sceneEngine"_s).toString(), value);
        }
        QVERIFY(store.merge({{u"sceneEngine"_s, u"auto"_s}})); // unchanged: silent
        QCOMPARE(spy.count(), values.size()); // each value differs from the one before
        for (const QJsonValue &bad : {QJsonValue(u"Vulkan"_s), QJsonValue(u"gl"_s), QJsonValue(QString()), QJsonValue(1), QJsonValue(true)}) {
            QString error;
            QVERIFY(!store.merge({{u"pauseWhenCovered"_s, false}, {u"sceneEngine"_s, bad}}, &error));
            QVERIFY(error.contains(u"sceneEngine"_s));
            QVERIFY(store.values().value(u"pauseWhenCovered"_s).toBool()); // nothing applied
        }
        QVERIFY(store.merge({{u"sceneEngine"_s, chosen}})); // a user's explicit choice
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"sceneEngine"_s).toString(), chosen);
}

void TestSettingsStore::aFileWithoutSceneEngineMeansAutoAndAnExplicitChoiceStays()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    const auto write = [&](const QByteArray &json) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(json);
    };
    // a settings file from before the engine setting existed
    write(R"({"webFpsLimit":30,"pauseWhenCovered":true,"presenter":"auto"})");
    SettingsStore older(path);
    older.load();
    QCOMPARE(older.values().value(u"sceneEngine"_s).toString(), u"auto"_s);
    // someone who chose an engine keeps it
    QString engine = u"vulkan"_s;
    write(R"({"webFpsLimit":30,"pauseWhenCovered":true,"presenter":"auto","sceneEngine":")" + engine.toUtf8() + R"("})");
    SettingsStore chose(path);
    chose.load();
    QCOMPARE(chose.values().value(u"sceneEngine"_s).toString(), engine);
}

void TestSettingsStore::invalidSceneEngineInFileIsQuarantined()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{\"webFpsLimit\": 45, \"sceneEngine\": \"metal\"}");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values(), defaults()); // the whole file is set aside, every key is a default
    QVERIFY(!QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}).isEmpty());
}

void TestSettingsStore::pauseRulesAreValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"pauseOnGame"_s).toBool(), true);
        QCOMPARE(store.values().value(u"batteryMode"_s).toString(), u"off"_s);
        QVERIFY(store.values().value(u"pauseApps"_s).toArray().isEmpty());
        QSignalSpy spy(&store, &SettingsStore::changed);
        QVERIFY(store.merge({{u"pauseOnGame"_s, false}}));
        for (const QString &mode : {u"pause"_s, u"lowfps"_s, u"off"_s})
            QVERIFY(store.merge({{u"batteryMode"_s, mode}}));
        QVERIFY(store.merge({{u"pauseApps"_s, QJsonArray{u"obs"_s, u"  steam_app_*  "_s}}}));
        QCOMPARE(spy.count(), 5);
        // trimmed on the way in, order kept
        QCOMPARE(store.values().value(u"pauseApps"_s).toArray(), (QJsonArray{u"obs"_s, u"steam_app_*"_s}));
        QVERIFY(store.merge({{u"pauseApps"_s, QJsonArray{u"obs"_s, u"steam_app_*"_s}}})); // same after trimming: silent
        QCOMPARE(spy.count(), 5);
        QVERIFY(store.merge({{u"batteryMode"_s, u"pause"_s}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"pauseOnGame"_s).toBool(), false);
    QCOMPARE(again.values().value(u"batteryMode"_s).toString(), u"pause"_s);
    QCOMPARE(again.values().value(u"pauseApps"_s).toArray(), (QJsonArray{u"obs"_s, u"steam_app_*"_s}));

    QJsonArray tooMany;
    for (int i = 0; i < 65; ++i)
        tooMany.append(u"app%1"_s.arg(i));
    QJsonArray sixtyFour;
    for (int i = 0; i < 64; ++i)
        sixtyFour.append(u"app%1"_s.arg(i));
    QVERIFY(again.merge({{u"pauseApps"_s, sixtyFour}}));
    for (const QJsonValue &bad : {QJsonValue(u"obs"_s), QJsonValue(1), QJsonValue(QJsonArray{1}), QJsonValue(QJsonArray{u"  "_s}),
                                  QJsonValue(QJsonArray{QString()}), QJsonValue(QJsonArray{QString(101, u'x')}),
                                  QJsonValue(QJsonArray{u"ok"_s, QJsonValue::Null}), QJsonValue(tooMany)}) {
        QString error;
        QVERIFY(!again.merge({{u"pauseOnGame"_s, true}, {u"pauseApps"_s, bad}}, &error));
        QVERIFY(error.contains(u"pauseApps"_s));
        QCOMPARE(again.values().value(u"pauseOnGame"_s).toBool(), false); // nothing applied
    }
    QVERIFY(again.merge({{u"pauseApps"_s, QJsonArray{QString(100, u'x')}}})); // the limit itself
    for (const QJsonObject &bad : {QJsonObject{{u"batteryMode"_s, u"Pause"_s}}, QJsonObject{{u"batteryMode"_s, u"slow"_s}},
                                   QJsonObject{{u"batteryMode"_s, 1}}, QJsonObject{{u"pauseOnGame"_s, u"yes"_s}},
                                   QJsonObject{{u"pauseOnGame"_s, 1}}}) {
        QString error;
        QVERIFY(!again.merge(bad, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestSettingsStore::invalidPauseRulesInTheFileAreQuarantined()
{
    for (const char *json : {"{\"batteryMode\": \"slow\"}", "{\"pauseOnGame\": 3}", "{\"pauseApps\": \"obs\"}",
                             "{\"pauseApps\": [1]}"}) {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(u"settings.json"_s);
        testutil::writeFile(path, json);
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values(), defaults());
        QVERIFY2(!QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}).isEmpty(), json);
    }
}

void TestSettingsStore::aFileFromBeforePauseRulesGetsTheirDefaults()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, R"({"webFpsLimit":60,"pauseWhenCovered":false,"presenter":"plasma","sceneEngine":"vulkan"})");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values().value(u"webFpsLimit"_s).toInt(), 60);
    QCOMPARE(store.values().value(u"pauseOnGame"_s).toBool(), true);
    QCOMPARE(store.values().value(u"batteryMode"_s).toString(), u"off"_s);
    QVERIFY(store.values().value(u"pauseApps"_s).toArray().isEmpty());
    QVERIFY(QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}).isEmpty());
}

void TestSettingsStore::sandboxEnginesIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"sandboxEngines"_s), QJsonValue(true)); // on by default
        QString error;
        QVERIFY(!store.merge({{u"sandboxEngines"_s, u"no"_s}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(!store.merge({{u"sandboxEngines"_s, 0}}, &error));
        QCOMPARE(store.values().value(u"sandboxEngines"_s), QJsonValue(true));
        QSignalSpy spy(&store, &SettingsStore::changed);
        QVERIFY(store.merge({{u"sandboxEngines"_s, false}}));
        QCOMPARE(spy.size(), 1);
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"sandboxEngines"_s), QJsonValue(false));
}

void TestSettingsStore::requireSandboxIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"requireSandbox"_s), QJsonValue(false)); // off by default
        QString error;
        QVERIFY(!store.merge({{u"requireSandbox"_s, u"yes"_s}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(store.merge({{u"requireSandbox"_s, true}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"requireSandbox"_s), QJsonValue(true));
}

void TestSettingsStore::mediaRemoteArtIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"mediaRemoteArt"_s), QJsonValue(true)); // on by default
        QString error;
        QVERIFY(!store.merge({{u"mediaRemoteArt"_s, 1}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(store.merge({{u"mediaRemoteArt"_s, false}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"mediaRemoteArt"_s), QJsonValue(false));
}

void TestSettingsStore::mediaShareIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"mediaShare"_s), QJsonValue(true));
        QString error;
        QVERIFY(!store.merge({{u"mediaShare"_s, u"no"_s}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(store.merge({{u"mediaShare"_s, false}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"mediaShare"_s), QJsonValue(false));
}

void TestSettingsStore::petLimitsAreValidatedAndPersisted()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"petMaxFps"_s), QJsonValue(0));         // unlimited by default
        QCOMPARE(store.values().value(u"petSaveEnergy"_s), QJsonValue(true)); // and saving energy
        QString error;
        for (const QJsonValue &bad : {QJsonValue(-1), QJsonValue(121), QJsonValue(30.5), QJsonValue(u"30"_s), QJsonValue(true)}) {
            QVERIFY(!store.merge({{u"petMaxFps"_s, bad}}, &error));
            QVERIFY(error.contains(u"petMaxFps"_s));
        }
        QVERIFY(!store.merge({{u"petSaveEnergy"_s, 1}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(store.merge({{u"petMaxFps"_s, 30}, {u"petSaveEnergy"_s, false}}));
        QVERIFY(store.merge({{u"petMaxFps"_s, 120.0}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"petMaxFps"_s), QJsonValue(120));
    QCOMPARE(again.values().value(u"petSaveEnergy"_s), QJsonValue(false));
}

void TestSettingsStore::petsSoundSettingsAreValidatedAndPersisted()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"petsVolume"_s), QJsonValue(100));
        QCOMPARE(store.values().value(u"petsMuted"_s), QJsonValue(false));
        QCOMPARE(store.values().value(u"petsMuteWhenFullscreen"_s), QJsonValue(true));
        QString error;
        for (const QJsonValue &bad : {QJsonValue(-1), QJsonValue(101), QJsonValue(50.5), QJsonValue(u"50"_s), QJsonValue(true)}) {
            QVERIFY(!store.merge({{u"petsVolume"_s, bad}}, &error));
            QVERIFY(error.contains(u"petsVolume"_s));
        }
        QVERIFY(!store.merge({{u"petsMuted"_s, 1}}, &error));
        QVERIFY(!store.merge({{u"petsMuteWhenFullscreen"_s, u"yes"_s}}, &error));
        QVERIFY(store.merge({{u"petsVolume"_s, 35}, {u"petsMuted"_s, true}, {u"petsMuteWhenFullscreen"_s, false}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"petsVolume"_s), QJsonValue(35));
    QCOMPARE(again.values().value(u"petsMuted"_s), QJsonValue(true));
    QCOMPARE(again.values().value(u"petsMuteWhenFullscreen"_s), QJsonValue(false));
}

void TestSettingsStore::puppetSettingsAreValidatedAndPersisted()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"puppetMaxPixels"_s), QJsonValue(4194304)); // safe defaults: a modest picture, no Core
        QCOMPARE(store.values().value(u"puppetCorePath"_s), QJsonValue(QString()));
        QString error;
        for (const QJsonValue &bad : {QJsonValue(999999), QJsonValue(16000001), QJsonValue(0), QJsonValue(-5), QJsonValue(2000000.5), QJsonValue(u"4194304"_s), QJsonValue(true)}) {
            QVERIFY(!store.merge({{u"puppetMaxPixels"_s, bad}}, &error));
            QVERIFY(error.contains(u"puppetMaxPixels"_s));
        }
        for (const QJsonValue &bad : {QJsonValue(5), QJsonValue(true), QJsonValue(u"relative/libCubismCore.so"_s), QJsonValue(u"/a\nb"_s), QJsonValue(QString(5000, QLatin1Char('x')))}) {
            QVERIFY2(!store.merge({{u"puppetCorePath"_s, bad}}, &error), qPrintable(bad.toString()));
            QVERIFY(error.contains(u"puppetCorePath"_s));
        }
        QCOMPARE(store.puppetSettings().maxPixels, 4194304LL);
        QVERIFY(store.merge({{u"puppetMaxPixels"_s, 1000000}, {u"puppetCorePath"_s, u"/opt/live2d/libCubismCore.so"_s}}, &error));
        QVERIFY(store.merge({{u"puppetMaxPixels"_s, 16000000}}, &error));
        QCOMPARE(store.puppetSettings().corePath, u"/opt/live2d/libCubismCore.so"_s);
        QVERIFY(store.merge({{u"puppetCorePath"_s, u""_s}}, &error)); // empty: none
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.puppetSettings().maxPixels, 16000000LL); // persisted
    QCOMPARE(again.puppetSettings().corePath, QString());
}

void TestSettingsStore::aCoreFileMustBeSafeToLoad()
{
    QTemporaryDir dir;
    const QString good = dir.filePath(u"libCubismCore.so"_s);
    testutil::writeFile(good, "x");
    QFile::setPermissions(good, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ReadOther);
    QCOMPARE(PuppetSettings::coreFileProblem(good), QString());
    QCOMPARE(PuppetSettings::coreFileProblem(QString()), QString()); // none is fine
    QVERIFY(!PuppetSettings::coreFileProblem(dir.filePath(u"missing.so"_s)).isEmpty());
    QVERIFY(!PuppetSettings::coreFileProblem(u"relative.so"_s).isEmpty());
    QVERIFY(!PuppetSettings::coreFileProblem(dir.path()).isEmpty()); // a folder
    QVERIFY(QFile::link(good, dir.filePath(u"link.so"_s)));
    QVERIFY(!PuppetSettings::coreFileProblem(dir.filePath(u"link.so"_s)).isEmpty()); // a link is not followed
    QFile::setPermissions(good, QFile::permissions(good) | QFile::WriteGroup);
    QVERIFY(PuppetSettings::coreFileProblem(good).contains(u"written"_s));
    QFile::setPermissions(good, QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther);
    QVERIFY(!PuppetSettings::coreFileProblem(good).isEmpty());
    // the settings file keeps a path whose library has since gone: it is only checked when the user sets it
    SettingsStore store(dir.filePath(u"settings.json"_s));
    store.load();
    QString error;
    QVERIFY(store.merge({{u"puppetCorePath"_s, dir.filePath(u"gone.so"_s)}}, &error)); // (the store reads text; the service looks at the file)
}

void TestSettingsStore::stageSettingsAreValidatedAndPersisted()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"stageHideOverFullscreen"_s), QJsonValue(true));
        QCOMPARE(store.values().value(u"stageMaxPixels"_s), QJsonValue(8294400));
        QString error;
        QVERIFY(!store.merge({{u"stageHideOverFullscreen"_s, 1}}, &error));
        QVERIFY(error.contains(u"stageHideOverFullscreen"_s));
        for (const QJsonValue &bad : {QJsonValue(999999), QJsonValue(33177601), QJsonValue(0), QJsonValue(-5), QJsonValue(2000000.5), QJsonValue(u"8294400"_s), QJsonValue(true)}) {
            QVERIFY(!store.merge({{u"stageMaxPixels"_s, bad}}, &error));
            QVERIFY(error.contains(u"stageMaxPixels"_s));
        }
        QCOMPARE(store.values().value(u"stageMaxPixels"_s), QJsonValue(8294400)); // (nothing applied)
        QVERIFY(store.merge({{u"stageHideOverFullscreen"_s, false}, {u"stageMaxPixels"_s, 1000000}}));
        QVERIFY(store.merge({{u"stageMaxPixels"_s, 33177600}}));
        QVERIFY(store.merge({{u"stageMaxPixels"_s, 2073600}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"stageHideOverFullscreen"_s), QJsonValue(false));
    QCOMPARE(again.values().value(u"stageMaxPixels"_s), QJsonValue(2073600));
    // a file with a value out of range is quarantined whole, as for every key
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{\"stageMaxPixels\": 5}");
    file.close();
    SettingsStore bad(path);
    bad.load();
    QCOMPARE(bad.values().value(u"stageMaxPixels"_s), QJsonValue(8294400));
}

void TestSettingsStore::shortcutsLaunchIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"shortcutsLaunch"_s), QJsonValue(true)); // on by default
        QVERIFY(store.shortcutsLaunch());
        QString error;
        QVERIFY(!store.merge({{u"shortcutsLaunch"_s, 1}}, &error));
        QVERIFY(error.contains(u"boolean"_s));
        QVERIFY(store.merge({{u"shortcutsLaunch"_s, false}}));
        QVERIFY(!store.shortcutsLaunch());
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"shortcutsLaunch"_s), QJsonValue(false));
}

void TestSettingsStore::sceneRenderScaleIsValidatedAndPersisted()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"sceneRenderScale"_s), QJsonValue(0.0)); // automatic by default
        QString error;
        QVERIFY(!store.merge({{u"sceneRenderScale"_s, u"half"_s}}, &error));
        QVERIFY(error.contains(u"number"_s));
        QVERIFY(!store.merge({{u"sceneRenderScale"_s, 0.1}}, &error));
        QVERIFY(error.contains(u"between"_s));
        QVERIFY(!store.merge({{u"sceneRenderScale"_s, 1.5}}, &error));
        QVERIFY(store.merge({{u"sceneRenderScale"_s, 1.0}}));
        QVERIFY(store.merge({{u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}}));
        QVERIFY(store.merge({{u"sceneRenderScale"_s, 0.75}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"sceneRenderScale"_s), QJsonValue(0.75));
}

void TestSettingsStore::aFileFromBeforeTheSandboxMeansSandboxed()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, R"({"webFpsLimit": 60, "pauseWhenCovered": true})");
    SettingsStore store(path);
    store.load();
    QCOMPARE(store.values().value(u"webFpsLimit"_s).toInt(), 60);
    QCOMPARE(store.values().value(u"sandboxEngines"_s), QJsonValue(true));
    QVERIFY(QDir(tmp.path()).entryList({u"settings.json.bad-*"_s}).isEmpty()); // not quarantined
}

void TestSettingsStore::failedSaveRollsBack()
{
    QTemporaryDir tmp;
    SettingsStore store(tmp.filePath(u"settings.json"_s));
    store.load();
    QSignalSpy spy(&store, &SettingsStore::changed);
    ReadOnlyDir guard(tmp.path());
    if (!guard.effective())
        QSKIP("directory permissions are not enforced (running as root?)");

    QString error;
    bool saveFailed = false;
    QVERIFY(!store.merge({{u"webFpsLimit"_s, 60}}, &error, &saveFailed));
    QVERIFY(saveFailed);
    QCOMPARE(error, u"could not save settings"_s);
    QCOMPARE(store.values(), defaults());
    QCOMPARE(spy.count(), 0);

    // Validation errors are not save failures.
    QVERIFY(!store.merge({{u"webFpsLimit"_s, 1}}, &error, &saveFailed));
    QVERIFY(!saveFailed);
}

void TestSettingsStore::blockedSaveRollsBack()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"settings.json"_s);
    testutil::writeFile(path, "{not json");
    ReadOnlyDir guard(tmp.path());
    if (!guard.effective())
        QSKIP("directory permissions are not enforced (running as root?)");

    // The rename fails in a read-only dir, so saving stays blocked and the bad file is never overwritten.
    SettingsStore store(path);
    store.load();
    QSignalSpy spy(&store, &SettingsStore::changed);
    bool saveFailed = false;
    QVERIFY(!store.merge({{u"webFpsLimit"_s, 60}}, nullptr, &saveFailed));
    QVERIFY(saveFailed);
    QCOMPARE(store.values(), defaults());
    QCOMPARE(spy.count(), 0);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{not json"));
}

void TestSettingsStore::petsHideOnGameIsABooleanOffByDefault()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"settings.json"_s);
    {
        SettingsStore store(path);
        store.load();
        QCOMPARE(store.values().value(u"petsHideOnGame"_s), QJsonValue(false));
        QString error;
        QVERIFY(!store.merge({{u"petsHideOnGame"_s, u"yes"_s}}, &error));
        QVERIFY(error.contains(u"petsHideOnGame"_s));
        QVERIFY(store.merge({{u"petsHideOnGame"_s, true}}));
    }
    SettingsStore again(path);
    again.load();
    QCOMPARE(again.values().value(u"petsHideOnGame"_s), QJsonValue(true));
}

QTEST_GUILESS_MAIN(TestSettingsStore)
#include "tst_settingsstore.moc"
