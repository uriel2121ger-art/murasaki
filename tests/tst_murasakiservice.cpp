#include "animfixtures.h"
#include "fakesteamcmd.h"
#include "assignment.h"
#include "automationmanager.h"
#include "automationstore.h"
#include "constants.h"
#include "itempermissions.h"
#include "localpets.h"
#include "localpuppets.h"
#include "murasakiservice.h"
#include "screenwatch.h"
#include "screenrefstore.h"
#include "petaudiomanager.h"
#include "petstore.h"
#include "playlist.h"
#include "playlistmanager.h"
#include "settings.h"
#include "transcodemanager.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QScopeGuard>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QMetaClassInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

// Every settings object carries the import defaults (importsettings.h).
static QJsonObject withImportDefaults(QJsonObject o)
{
    murasaki::ImportSettings{}.write(o);
    murasaki::AutomationSettings{}.write(o);
    if (!o.contains(u"stageHideOverFullscreen"_s))
        o.insert(u"stageHideOverFullscreen"_s, true);
    if (!o.contains(u"stageMaxPixels"_s))
        o.insert(u"stageMaxPixels"_s, 8294400);
    if (!o.contains(u"petsHideOnGame"_s))
        o.insert(u"petsHideOnGame"_s, false);
    if (!o.contains(u"puppetMaxPixels"_s))
        o.insert(u"puppetMaxPixels"_s, 4194304);
    if (!o.contains(u"puppetCorePath"_s))
        o.insert(u"puppetCorePath"_s, QString());
    return o;
}

using namespace murasaki;

// Counts SettingsChanged signals as they arrive over the bus.
class BusSignalCounter : public QObject
{
    Q_OBJECT

public:
    int count = 0;

public slots:
    void onSettingsChanged() { ++count; }
};

class TestMurasakiService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void listItemsReturnsSummaries();
    void getItemReturnsFullItemOrEmpty();
    void statusReflectsSteamInstall();
    void forwardsItemsChanged();
    void classInfoMatchesConstant();
    void dbusRoundTrip();
    void setResolveAndClearAssignments();
    void rejectsInvalidAssignments();
    void aShortcutLaunchNeedsAnItemAnAssignmentAndTheSwitch();
    void petsAreListedButNotAssignable();
    void assetPacksAreListedButNotAssignable();
    void assignedItemChangesEmitAssignmentsChanged();
    void identicalAssignmentEmitsOnce();
    void removedAssignedItemEmitsAssignmentsChanged();
    void assignmentDBusErrors();
    void settingsOverDBus();
    void petsAddUpdateRemove();
    void petsRejectBadInput();
    void petsWithoutAttachFail();
    void heavyPetsAreTranscodedWhenAdded();
    void transcodeRequestsAndState();
    void transcodeSizeFollowsTheWidthAndTheOutputScale();
    void aStageIsConvertedToTheSizeOfTheScreenUpToTheSource();
    void everyVariantAStateNamesIsConvertedLikeTheSelectedOne();
    void petsWithDisjointStatesKeepEachOthersCopies();
    void aResizeIsDebouncedAndUnusedBucketsAreRemoved();
    void everyPlacementKeepsItsBucket();
    void outputScalesAreValidated();
    void aCopyFromBeforeBucketsServesSmallerPets();
    void startupQueueWaitsForTheFirstScales();
    void startupQueueGoesAheadAfterTheGracePeriod();
    void switchingToTheOriginalPrunesTheBuckets();
    void aQueuedBucketNobodyNeedsAnymoreIsCancelled();
    void newPetsStartAtTheDefaultRate();
    void petBehaviourFieldsTravelThroughTheApi();
    void thePetSoundTravelsThroughTheApiAndTheAssetCalls();
    void theKeptAudioOfAnImportedVideoBecomesASound();
    void rescuePetsChangesExactlyTheListedFieldsAndWritesOnce();
    void petsHiddenSignalsOnlyOnChangeAndIsNeverStored();
    void aFailingWriteMakesRescuePetsAllOrNothing();
    void rescuePetsTurnsEveryStageOffInTheSameWrite();
    void theStageOfAPetIsSetAndRefusedThroughTheService();
    void statesAndTrackingAreSetAndRefusedThroughTheService();
    void rescuePetsTurnsTrackingOff();
    void choosingHoverHideThroughUpdatePetClearsFocusFade();
    void everyRateSharesOneCopy();
    void changingTheRateOrTheScaleStartsNoConversion();
    void aCopyRetimedBeforeServesEveryFixedRate();
    void theFilesOwnTimingIsNotRetimed();
    void arrangeModeAndPetsOverDBus();
    void overlayUnitFollowsThePets();
    void wallpaperInteractIsASessionState();
    void interactKeepsTheOverlayRunningWithoutPets();
    void interactEndsByItselfAfterTheCap();
    void localPetsAreAddedListedAndRemoved();
    void importedPetsAreAddedMarkedGeneratedAndDeletedOnRequest();
    void packageScenesStartOnPets();
    void profilesSaveAndSwitchInOneWrite();
    void aProfileOriginIsSetAndKept();
    void anExampleProfileLeavesTheActiveProfileAndTheSlotsAlone();
    void anExplicitEmptyOriginClearsTheMarkerOfATimelineOrATrigger();
    void triggersAreMadeFiredAndRefusedByName();
    void timelinesHideThePetsInTheRuntimeLayerAndTheRescueBringsThemBack();
    void aSceneThatHidesThePetHidesItInTheRuntimeLayer();
    void theAutomationCallsFailCleanlyWithoutTheAutomation();
    void aPackAndItsScenesBecomeDisabledAutomationOnRequest();
    void screenTriggersDoNothingUntilTheUserAsksAndHaveTheirSettings();
    void aPackBringsItsReferencePicturesAndADeletedTriggerTakesThemAway();
    void muteIsASessionStateAndTheActionsOfACallDoNotFailTheCall();
    void twoPackagesWithOneTitleCoexistAndAreReplacedEachByItself();
    void aPreviewOverDBusDoesNotBlockTheDaemon();
    void aHelperThatNamesAFileOutsideTheImportFolderMakesTheJobFail();
    void workshopFilesAreImportedOnlyWhenTheSettingSaysAuto();
    void localPetsRejectBadFiles();
    void puppetsAreAddedListedShownAndRemoved();
    void puppetsRejectBadFolders();
    void aCoreFileIsCheckedBeforeItIsStored();
    void playlistsAreCreatedEditedAndDeleted();
    void aPlaylistCanBeAssignedAndResolves();
    void playlistsRejectBadInput();
    void nextAndPreviousRotateThePlaylist();
    void rotationAndEditsAnnounceThemselves();
    void playlistsWithoutAttachFail();
    void playlistsOverDBus();
    void steamCmdWithoutAManagerReportsUnavailable();
    void steamCmdRejectsBadInput();
    void steamCmdOverDBus();
    void itemsReportPermissionsAndDefaultToNoNetwork();
    void setItemPermissionGrantsAndRevokes();
    void setItemPermissionRejectsBadInput();
    void setItemPermissionWithoutAttachFails();
    void permissionChangeReloadsAssignedItem();

private:
    static QDBusMessage callAndWait(QDBusConnection &client, const QDBusMessage &message);
};

QDBusMessage TestMurasakiService::callAndWait(QDBusConnection &client, const QDBusMessage &message)
{
    // A second connection plus a spinning event loop: a blocking call would starve the server side.
    QDBusPendingCall pending = client.asyncCall(message);
    if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
        qWarning("D-Bus call timed out");
    return pending.reply();
}

void TestMurasakiService::initTestCase()
{
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
}

void TestMurasakiService::listItemsReturnsSummaries()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Beta"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, u"/assets"_s);

    const QJsonArray items = QJsonDocument::fromJson(service.ListItems().toUtf8()).array();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].toObject().value(u"title"_s).toString(), u"Alpha"_s);
    QVERIFY(!items[0].toObject().contains(u"properties"_s));
}

void TestMurasakiService::getItemReturnsFullItemOrEmpty()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});

    const QJsonObject item = QJsonDocument::fromJson(service.GetItem(u"we:1"_s).toUtf8()).object();
    QCOMPARE(item.value(u"id"_s).toString(), u"we:1"_s);
    QVERIFY(item.contains(u"properties"_s));
    QVERIFY(service.GetItem(u"we:404"_s).isEmpty());
}

void TestMurasakiService::statusReflectsSteamInstall()
{
    QTemporaryDir tmp;
    QCOMPARE(MurasakiService::statusFor(SteamLocator({tmp.filePath(u"none"_s)})), u"steam-not-found"_s);

    const QString root = tmp.filePath(u"steam"_s);
    QVERIFY(QDir().mkpath(root + u"/steamapps"_s));
    QCOMPARE(MurasakiService::statusFor(SteamLocator({root})), u"wallpaper-engine-not-installed"_s);

    testutil::writeFile(root + u"/steamapps/appmanifest_431960.acf"_s,
                        "\"AppState\" { \"installdir\" \"wallpaper_engine\" }");
    QVERIFY(QDir().mkpath(root + u"/steamapps/common/wallpaper_engine"_s));
    QCOMPARE(MurasakiService::statusFor(SteamLocator({root})), u"ok"_s);
}

void TestMurasakiService::forwardsItemsChanged()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QSignalSpy spy(&service, &MurasakiService::ItemsChanged);

    service.Rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"we:1"_s}));
}

void TestMurasakiService::classInfoMatchesConstant()
{
    const QMetaObject *meta = &MurasakiService::staticMetaObject;
    const int index = meta->indexOfClassInfo("D-Bus Interface");
    QVERIFY(index >= 0);
    QCOMPARE(QString::fromLatin1(meta->classInfo(index).value()), QString(kDBusInterface));
    QCOMPARE(kDBusApiVersion, kPetsGameDBusApiVersion); // (the newest feature's revision: renumbered in the constants only)
    QCOMPARE(kMinDBusApiVersion, 4u);
}

void TestMurasakiService::dbusRoundTrip()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");

    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, u"/assets"_s);

    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.p"_s
        + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    // Teardown must run even when a QVERIFY/QCOMPARE below returns early.
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client"_s);
    });
    QVERIFY(service.registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client"_s);
    QVERIFY(client.isConnected());

    const QString iface = kDBusInterface;

    const QDBusMessage list = callAndWait(client, QDBusMessage::createMethodCall(name, path, iface, u"ListItems"_s));
    QCOMPARE(list.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(QJsonDocument::fromJson(list.arguments().at(0).toString().toUtf8()).array().size(), 1);

    QDBusMessage getVersion = QDBusMessage::createMethodCall(name, path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    getVersion.setArguments({iface, u"ApiVersion"_s});
    const QDBusMessage version = callAndWait(client, getVersion);
    QCOMPARE(version.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(version.arguments().at(0).value<QDBusVariant>().variant().toUInt(), kDBusApiVersion);

    QDBusMessage getMissing = QDBusMessage::createMethodCall(name, path, iface, u"GetItem"_s);
    getMissing.setArguments({u"we:404"_s});
    const QDBusMessage missing = callAndWait(client, getMissing);
    QCOMPARE(missing.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(missing.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);

    const QDBusMessage rescan = callAndWait(client, QDBusMessage::createMethodCall(name, path, iface, u"Rescan"_s));
    QCOMPARE(rescan.type(), QDBusMessage::ReplyMessage);

    // Long ids are truncated in the error text.
    QDBusMessage getLong = QDBusMessage::createMethodCall(name, path, iface, u"GetItem"_s);
    getLong.setArguments({QString(5000, u'x')});
    const QDBusMessage longReply = callAndWait(client, getLong);
    QCOMPARE(longReply.type(), QDBusMessage::ErrorMessage);
    QVERIFY(longReply.errorMessage().size() < 300);
}

void TestMurasakiService::setResolveAndClearAssignments()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);

    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"{\"fill\": \"fit\"}"_s);
    QCOMPARE(spy.count(), 1);

    const QJsonArray all = QJsonDocument::fromJson(service.Assignments().toUtf8()).array();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all[0].toObject().value(u"fill"_s).toString(), u"fit"_s);

    const QJsonObject resolved = QJsonDocument::fromJson(service.ResolveAssignment(u"DP-1"_s, u"d1"_s).toUtf8()).object();
    QCOMPARE(resolved.value(u"assignment"_s).toObject().value(u"itemId"_s).toString(), u"we:1"_s);
    QCOMPARE(resolved.value(u"item"_s).toObject().value(u"id"_s).toString(), u"we:1"_s);
    QVERIFY(resolved.value(u"item"_s).toObject().contains(u"properties"_s)); // the full item

    service.ClearAssignment(u"*"_s, u"*"_s);
    QCOMPARE(spy.count(), 2);
    const QJsonObject empty = QJsonDocument::fromJson(service.ResolveAssignment(u"DP-1"_s, u"d1"_s).toUtf8()).object();
    QVERIFY(empty.value(u"assignment"_s).isNull());
    QVERIFY(empty.value(u"item"_s).isNull());
}

void TestMurasakiService::aShortcutLaunchNeedsAnItemAnAssignmentAndTheSwitch()
{
    QTemporaryDir tmp;
    // two items with two usershortcut properties each
    const auto makeItem = [&](const QString &id) {
        const QString dir = tmp.filePath(id);
        testutil::writeProject(dir, QJsonObject{{"type", "video"}, {"file", "clip.mp4"}, {"title", "Docks " + id},
                                                {"general", QJsonObject{{"properties", QJsonObject{{"launcher1", QJsonObject{{"type", "usershortcut"}, {"value", ""}}},
                                                                                                  {"launcher2", QJsonObject{{"type", "usershortcut"}, {"value", ""}}},
                                                                                                  {"plain", QJsonObject{{"type", "textinput"}, {"value", "x"}}}}}}}});
        testutil::writeFile(dir + "/clip.mp4", "fake video");
        return dir;
    };
    const QString dir = makeItem(u"1"_s);
    const QString other = makeItem(u"2"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    // competing assignments: a wildcard one shows item 1 with one application, the one for DP-1 on desktop d1 shows item 2, and the one for DP-1
    // on any desktop shows item 1 with another
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"{\"properties\": {\"launcher1\": \"wild.desktop\", \"launcher2\": \"wild2.desktop\"}}"_s);
    service.SetAssignment(u"DP-1"_s, u"d1"_s, u"we:2"_s, u"{\"properties\": {\"launcher1\": \"specific.desktop\"}}"_s);
    service.SetAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, u"{\"properties\": {\"launcher1\": \"output.desktop\", \"plain\": \"x\"}}"_s);
    // what is shown on DP-1 / d1 is item 2: item 1's applications (wildcard or not) do not count there
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d1"_s, dir, u"launcher1"_s), QString());
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d1"_s, other, u"launcher1"_s), u"specific.desktop"_s);
    // another desktop of DP-1: the output's assignment, not the wildcard one
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, dir, u"launcher1"_s), u"output.desktop"_s);
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, dir, u"launcher2"_s), QString()); // not stored there: the wildcard's is not borrowed
    // another output: the wildcard assignment
    QCOMPARE(service.assignedShortcut(u"HDMI-1"_s, u"d1"_s, dir, u"launcher2"_s), u"wild2.desktop"_s);
    QCOMPARE(service.assignedShortcut(u"HDMI-1"_s, u"d1"_s, other, u"launcher1"_s), QString()); // the wildcard shows item 1, not 2
    // not a usershortcut property, not a property, not an item, bad input
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, dir, u"plain"_s), QString());
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, dir, u"nope"_s), QString());
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, tmp.filePath(u"elsewhere"_s), u"launcher1"_s), QString());
    QCOMPARE(service.assignedShortcut(u""_s, u"d2"_s, dir, u"launcher1"_s), QString());
    QCOMPARE(service.assignedShortcut(u"DP-1"_s, u"d2"_s, dir, u"bad\nkey"_s), QString());
    // none of the applications is installed, so nothing starts whatever the rest says; and with the switch off nothing can
    QVERIFY(!service.LaunchShortcut(u"DP-1"_s, u"d2"_s, dir, u"launcher1"_s));
    QVERIFY(!service.LaunchShortcut(u"DP-1"_s, u"d1"_s, dir, u"launcher1"_s)); // not what is shown there
    QVERIFY(!service.LaunchShortcut(u"DP-1"_s, u"d2"_s, dir, u"bad\nkey"_s));
    QVERIFY(!service.LaunchShortcut(u"DP-1"_s, u"d2"_s, u"/x\n/y"_s, u"launcher1"_s)); // a control character in the path is refused, not logged
    QVERIFY(!service.LaunchShortcut(u""_s, u"d2"_s, dir, u"launcher1"_s));
    QString error;
    QVERIFY(settings.merge({{u"shortcutsLaunch"_s, false}}, &error));
    QVERIFY(!service.LaunchShortcut(u"DP-1"_s, u"d2"_s, dir, u"launcher1"_s));
}

void TestMurasakiService::rejectsInvalidAssignments()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});

    service.SetAssignment(u"*"_s, u"*"_s, u"we:404"_s, QString());           // unknown item
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"[1, 2]"_s);           // options not an object
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"{\"fill\": \"x\"}"_s); // bad fill
    service.SetAssignment(QString(), u"*"_s, u"we:1"_s, QString());          // empty output
    QVERIFY(store.assignments().isEmpty());
}

void TestMurasakiService::assetPacksAreListedButNotAssignable()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.filePath(u"we"_s), u"1"_s, u"Alpha"_s);
    testutil::writeProject(tmp.filePath(u"we/2"_s), testutil::json(R"({"category": "Asset", "file": "assets.json", "title": "Clock pack"})"));
    Library library;
    library.setWorkshopDirs({tmp.filePath(u"we"_s)});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});

    const QJsonArray listed = QJsonDocument::fromJson(service.ListItems().toUtf8()).array();
    QCOMPARE(listed.size(), 2);
    for (const QJsonValue &entry : listed) {
        if (entry.toObject().value(u"id"_s).toString() == u"we:2"_s) {
            QCOMPARE(entry.toObject().value(u"kind"_s).toString(), u"asset"_s);
            QCOMPARE(entry.toObject().value(u"status"_s).toString(), u"ok"_s);
        }
    }
    service.SetAssignment(u"*"_s, u"*"_s, u"we:2"_s, QString());
    QVERIFY(store.assignments().isEmpty());

    // One that got into the store anyway resolves like a vanished item.
    Assignment stray;
    stray.output = u"*"_s;
    stray.desktop = u"*"_s;
    stray.itemId = u"we:2"_s;
    QVERIFY(store.set(stray));
    const QJsonObject resolved = QJsonDocument::fromJson(service.ResolveAssignment(u"DP-1"_s, u"d"_s).toUtf8()).object();
    QVERIFY(resolved.value(u"item"_s).isNull());
}

void TestMurasakiService::petsAreListedButNotAssignable()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.filePath(u"we"_s), u"1"_s, u"Alpha"_s);
    testutil::writeFile(tmp.filePath(u"anima/9/pet.apng"_s), animfixtures::apng(40, 30, {{1, 60}, {1, 60}}));
    Library library;
    library.setWorkshopDirs({tmp.filePath(u"we"_s)});
    library.setAnimaDirs({tmp.filePath(u"anima"_s)});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);

    const QJsonArray listed = QJsonDocument::fromJson(service.ListItems().toUtf8()).array();
    QCOMPARE(listed.size(), 2);
    QJsonObject petSummary;
    for (const QJsonValue &entry : listed) {
        if (entry.toObject().value(u"id"_s).toString() == u"anima:9"_s)
            petSummary = entry.toObject();
    }
    QCOMPARE(petSummary.value(u"kind"_s).toString(), u"pet"_s);
    QCOMPARE(petSummary.value(u"variantCount"_s).toInt(), 1);
    const QJsonObject full = QJsonDocument::fromJson(service.GetItem(u"anima:9"_s).toUtf8()).object();
    QCOMPARE(full.value(u"variants"_s).toArray().at(0).toObject().value(u"width"_s).toInt(), 40);

    service.SetAssignment(u"*"_s, u"*"_s, u"anima:9"_s, QString());
    QVERIFY(store.assignments().isEmpty());
    QCOMPARE(spy.count(), 0);

    // An assignment to a pet that got into the store anyway (old or hand-edited file) resolves like a vanished
    // item: the assignment is reported, the item is null, so no surface tries to play it.
    Assignment stray;
    stray.output = u"*"_s;
    stray.desktop = u"*"_s;
    stray.itemId = u"anima:9"_s;
    QVERIFY(store.set(stray));
    const QJsonObject resolved = QJsonDocument::fromJson(service.ResolveAssignment(u"DP-1"_s, u"d"_s).toUtf8()).object();
    QCOMPARE(resolved.value(u"assignment"_s).toObject().value(u"itemId"_s).toString(), u"anima:9"_s);
    QVERIFY(resolved.value(u"item"_s).isNull());
    store.clear(u"*"_s, u"*"_s);
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, QString()); // wallpapers still work
    QCOMPARE(store.assignments().size(), 1);
}

void TestMurasakiService::assignedItemChangesEmitAssignmentsChanged()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, QString());
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);

    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Beta"_s); // an unassigned item appears
    service.Rescan();
    QCOMPARE(spy.count(), 0);

    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha renamed"_s); // the assigned item changes
    service.Rescan();
    QCOMPARE(spy.count(), 1);
}

void TestMurasakiService::identicalAssignmentEmitsOnce()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);

    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"{\"fill\": \"fit\"}"_s);
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, u"{\"fill\": \"fit\"}"_s);
    QCOMPARE(spy.count(), 1);
}

void TestMurasakiService::removedAssignedItemEmitsAssignmentsChanged()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, QString());
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);

    QVERIFY(QDir(tmp.filePath(u"1"_s)).removeRecursively());
    service.Rescan();
    QCOMPARE(spy.count(), 1);
}

void TestMurasakiService::assignmentDBusErrors()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");

    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    testutil::writeFile(tmp.filePath(u"anima/9/pet.apng"_s), animfixtures::apng(8, 8, {{1, 60}, {1, 60}}));
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.setAnimaDirs({tmp.filePath(u"anima"_s)});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});

    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.a"_s
        + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-a"_s);
    });
    QVERIFY(service.registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-a"_s);
    QVERIFY(client.isConnected());
    const QString iface = kDBusInterface;

    QDBusMessage unknown = QDBusMessage::createMethodCall(name, path, iface, u"SetAssignment"_s);
    unknown.setArguments({u"*"_s, u"*"_s, u"we:404"_s, QString()});
    const QDBusMessage unknownReply = callAndWait(client, unknown);
    QCOMPARE(unknownReply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(unknownReply.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);

    QDBusMessage pet = QDBusMessage::createMethodCall(name, path, iface, u"SetAssignment"_s);
    pet.setArguments({u"*"_s, u"*"_s, u"anima:9"_s, QString()});
    const QDBusMessage petReply = callAndWait(client, pet);
    QCOMPARE(petReply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(petReply.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QVERIFY(store.assignments().isEmpty());

    QDBusMessage ok = QDBusMessage::createMethodCall(name, path, iface, u"SetAssignment"_s);
    ok.setArguments({u"DP-1"_s, u"*"_s, u"we:1"_s, u"{\"muted\": true}"_s});
    QCOMPARE(callAndWait(client, ok).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(store.resolve(u"DP-1"_s, u"x"_s)->muted, true);

    QDBusMessage resolveLong = QDBusMessage::createMethodCall(name, path, iface, u"ResolveAssignment"_s);
    resolveLong.setArguments({QString(500, u'x'), u"*"_s});
    const QDBusMessage longReply = callAndWait(client, resolveLong);
    QCOMPARE(longReply.type(), QDBusMessage::ErrorMessage);
    QVERIFY(longReply.errorMessage().size() < 300);

    QDBusMessage clearEmpty = QDBusMessage::createMethodCall(name, path, iface, u"ClearAssignment"_s);
    clearEmpty.setArguments({QString(), u"*"_s});
    QCOMPARE(callAndWait(client, clearEmpty).type(), QDBusMessage::ErrorMessage);
}

void TestMurasakiService::settingsOverDBus()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");

    QTemporaryDir tmp;
    Library library;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});

    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.s"_s
        + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-s"_s);
    });
    QVERIFY(service.registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-s"_s);
    QVERIFY(client.isConnected());
    const QString iface = kDBusInterface;
    BusSignalCounter onBus;
    QVERIFY(client.connect(name, path, iface, u"SettingsChanged"_s, &onBus, SLOT(onSettingsChanged())));

    const auto getSettings = [&] {
        const QDBusMessage reply = callAndWait(client, QDBusMessage::createMethodCall(name, path, iface, u"Settings"_s));
        return QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
    };
    const auto setSettings = [&](const QString &json) {
        QDBusMessage message = QDBusMessage::createMethodCall(name, path, iface, u"SetSettings"_s);
        message.setArguments({json});
        return callAndWait(client, message);
    };

    QCOMPARE(getSettings(), (withImportDefaults(QJsonObject{{u"webFpsLimit"_s, 30}, {u"pauseWhenCovered"_s, true}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}})));

    QCOMPARE(setSettings(u"{\"webFpsLimit\": 60}"_s).type(), QDBusMessage::ReplyMessage);
    QVERIFY(QTest::qWaitFor([&] { return onBus.count == 1; }, 5000));
    QCOMPARE(getSettings(), (withImportDefaults(QJsonObject{{u"webFpsLimit"_s, 60}, {u"pauseWhenCovered"_s, true}, {u"presenter"_s, u"auto"_s}, {u"sceneEngine"_s, u"auto"_s}, {u"pauseOnGame"_s, true}, {u"batteryMode"_s, u"off"_s}, {u"pauseApps"_s, QJsonArray()}, {u"sandboxEngines"_s, true}, {u"requireSandbox"_s, false}, {u"sceneRenderScale"_s, 0.0}, {u"mediaRemoteArt"_s, true}, {u"mediaShare"_s, true}, {u"shortcutsLaunch"_s, true}, {u"petMaxFps"_s, 0}, {u"petSaveEnergy"_s, true}, {u"petsVolume"_s, 100}, {u"petsMuted"_s, false}, {u"petsMuteWhenFullscreen"_s, true}})));

    // No-ops do not signal.
    QCOMPARE(setSettings(u"{\"webFpsLimit\": 60}"_s).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(setSettings(u"{}"_s).type(), QDBusMessage::ReplyMessage);

    for (const QString &bad : {u"{\"webFpsLimit\": 5}"_s, u"{\"webFpsLimit\": \"fast\"}"_s,
                               u"{\"bogus\": 1}"_s, u"not json"_s, u"[1]"_s,
                               u"{\"pauseWhenCovered\": false, \"webFpsLimit\": 999}"_s}) {
        const QDBusMessage reply = setSettings(bad);
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
        QCOMPARE(reply.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    }
    QCOMPARE(getSettings().value(u"pauseWhenCovered"_s).toBool(), true);
    // Let any stray signal arrive before asserting there was none beyond the first.
    QTest::qWait(100);
    QCOMPARE(onBus.count, 1);
}

namespace {

// A service with two pet items: "anima:9" (variants "big" 900x20 and "small" 40x30) and a wallpaper "we:1".
struct PetFixture {
    QTemporaryDir tmp;
    QByteArray oldPath = qgetenv("PATH");
    Library library;
    AssignmentStore assignments{tmp.filePath(u"assignments.json"_s)};
    SettingsStore settings{tmp.filePath(u"settings.json"_s)};
    PetStore pets{tmp.filePath(u"config/pets.json"_s)};
    TranscodeManager transcodes{tmp.filePath(u"cache/pets"_s)};
    LocalPetStore localPets{tmp.filePath(u"config/local-pets.json"_s)};
    LocalPetStore localPuppets{tmp.filePath(u"config/local-puppets.json"_s)};
    MurasakiService service{&library, &assignments, &settings, u"ok"_s, {}};

    // attach = false leaves the pet API detached, for tests that prepare stored pets first.
    explicit PetFixture(bool attach = true)
    {
        testutil::writeVideoItem(tmp.filePath(u"we"_s), u"1"_s, u"Alpha"_s);
        testutil::writeFile(tmp.filePath(u"anima/9/big.apng"_s), animfixtures::apng(900, 20, {{1, 60}, {1, 60}}));
        testutil::writeFile(tmp.filePath(u"anima/9/small.apng"_s), animfixtures::apng(40, 30, {{1, 10}, {1, 10}}));
        library.setWorkshopDirs({tmp.filePath(u"we"_s)});
        library.setAnimaDirs({tmp.filePath(u"anima"_s)});
        library.rescan();
        // A fake ffmpeg that just writes its output, so nothing real ever runs.
        const QString script = tmp.filePath(u"bin/ffmpeg"_s);
        testutil::writeFile(script, "#!/bin/sh\nfor l; do :; done\necho fake > \"$l\"\n");
        QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("PATH", tmp.filePath(u"bin"_s).toUtf8() + ":" + oldPath);
        pets.load();
        localPets.load();
        service.attachLocalPets(&localPets);
        localPuppets.setItemIdFunction(&LocalPuppetImporter::itemIdFor);
        localPuppets.load();
        service.attachLocalPuppets(&localPuppets, nullptr);
        if (attach)
            service.attachPets(&pets, &transcodes);
    }
    ~PetFixture() { qputenv("PATH", oldPath); }
    QJsonObject item(const QString &id) const
    {
        return QJsonDocument::fromJson(service.GetItem(id).toUtf8()).object();
    }
    QJsonArray listed() const { return QJsonDocument::fromJson(service.ListPets().toUtf8()).array(); }
};

// Replaces the fixture's ffmpeg with one that also logs the scale filter of every run to <tmp>/filters.txt.
void logFilters(PetFixture &f)
{
    const QString script = f.tmp.filePath(u"bin/ffmpeg"_s);
    testutil::writeFile(script, "#!/bin/sh\nprev=\nfor l; do\n[ \"$prev\" = -vf ] && echo \"$l\" >> '"
                                    + f.tmp.filePath(u"filters.txt"_s).toUtf8() + "'\nprev=$l\ndone\necho fake > \"$l\"\n");
    QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

QString filtersLog(const PetFixture &f)
{
    QFile file(f.tmp.filePath(u"filters.txt"_s));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// The "big" variant's transcode object (the bucket the placements need) and the buckets that have a state.
QJsonObject bigTranscode(const PetFixture &f)
{
    return f.item(u"anima:9"_s).value(u"variants"_s).toArray().at(0).toObject().value(u"transcode"_s).toObject();
}

QList<int> bigBuckets(const PetFixture &f, const QString &state)
{
    QList<int> buckets;
    for (const QJsonValue &value : f.item(u"anima:9"_s).value(u"variants"_s).toArray().at(0).toObject().value(u"buckets"_s).toArray()) {
        if (value.toObject().value(u"state"_s).toString() == state)
            buckets.append(value.toObject().value(u"bucket"_s).toInt());
    }
    std::sort(buckets.begin(), buckets.end());
    return buckets;
}

QStringList cachedBuckets(const PetFixture &f)
{
    QStringList names;
    const QDir dir(f.tmp.filePath(u"cache/pets/anima_9"_s));
    for (const QString &name : dir.entryList({u"big-*.webm"_s}, QDir::Files)) {
        static const QRegularExpression bucket(u"-([0-9]+)-[0-9]+-[0-9a-f]{12}\\.webm$"_s);
        names << bucket.match(name).captured(1);
    }
    names.sort();
    return names;
}

} // namespace

void TestMurasakiService::petsAddUpdateRemove()
{
    PetFixture f;
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    QVERIFY(f.listed().isEmpty());

    // The first variant (by name) and a width capped at the transcode size are the defaults.
    const QString id = f.service.AddPet(u"anima:9"_s, QString(), u"DP-1"_s, QString());
    QVERIFY(!id.isEmpty());
    QCOMPARE(changed.count(), 1);
    QJsonObject pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"petId"_s).toString(), id);
    QCOMPARE(pet.value(u"variant"_s).toString(), u"big"_s);
    QCOMPARE(pet.value(u"output"_s).toString(), u"DP-1"_s);
    QCOMPARE(pet.value(u"width"_s).toDouble(), 650.0);
    QCOMPARE(pet.value(u"opacity"_s).toDouble(), 1.0);
    QCOMPARE(pet.value(u"paused"_s).toBool(), false);

    const QString second = f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s,
                                            u"{\"x\": 5, \"y\": 6, \"width\": 120, \"opacity\": 0.5, \"paused\": true}"_s);
    QCOMPARE(f.listed().size(), 2);
    pet = f.listed().at(1).toObject();
    QCOMPARE(pet.value(u"x"_s).toDouble(), 5.0);
    QCOMPARE(pet.value(u"width"_s).toDouble(), 120.0);
    QCOMPARE(pet.value(u"opacity"_s).toDouble(), 0.5);
    QCOMPARE(pet.value(u"paused"_s).toBool(), true);

    f.service.UpdatePet(second, u"{\"x\": 77, \"variant\": \"big\"}"_s);
    QCOMPARE(changed.count(), 3);
    pet = f.listed().at(1).toObject();
    QCOMPARE(pet.value(u"x"_s).toDouble(), 77.0);
    QCOMPARE(pet.value(u"variant"_s).toString(), u"big"_s);
    f.service.UpdatePet(second, u"{\"x\": 77}"_s); // no change, no signal
    QCOMPARE(changed.count(), 3);

    // Persisted: a new store reads it back.
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QCOMPARE(reread.pets().size(), 2);

    f.service.RemovePet(id);
    QCOMPARE(f.listed().size(), 1);
    QCOMPARE(changed.count(), 4);
}

void TestMurasakiService::petsRejectBadInput()
{
    PetFixture f;
    const auto rejected = [&](const QString &id, const QString &variant, const QString &output, const QString &options) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        return f.service.AddPet(id, variant, output, options).isEmpty();
    };
    QVERIFY(rejected(u"anima:404"_s, QString(), u"*"_s, QString()));          // unknown item
    QVERIFY(rejected(u"we:1"_s, QString(), u"*"_s, QString()));               // not a pet
    QVERIFY(rejected(u"anima:9"_s, u"nope"_s, u"*"_s, QString()));            // unknown variant
    QVERIFY(rejected(u"anima:9"_s, QString(), QString(), QString()));         // empty output
    QVERIFY(rejected(u"anima:9"_s, QString(), u"*"_s, u"[1]"_s));             // options not an object
    QVERIFY(rejected(u"anima:9"_s, QString(), u"*"_s, u"{\"width\": 1}"_s));  // out of range
    QVERIFY(rejected(u"anima:9"_s, QString(), u"*"_s, u"{\"x\": \"1\"}"_s)); // wrong type
    QVERIFY(f.listed().isEmpty());

    const QString id = f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    QVERIFY(!id.isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.UpdatePet(u"nope"_s, u"{}"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.UpdatePet(id, u"{\"variant\": \"nope\"}"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.UpdatePet(id, u"{\"opacity\": 0}"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.UpdatePet(id, u"nope"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemovePet(u"nope"_s);
    QCOMPARE(f.listed().size(), 1);
    QCOMPARE(f.listed().at(0).toObject().value(u"variant"_s).toString(), u"small"_s);
}

void TestMurasakiService::petsWithoutAttachFail()
{
    QTemporaryDir tmp;
    Library library;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QCOMPARE(service.ListPets(), u"[]"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(service.AddPet(u"anima:1"_s, QString(), u"*"_s, QString()).isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    service.RemovePet(u"x"_s);
    service.SetPetsArrange(true); // arrange mode needs no store
    QVERIFY(service.petsArrange());
}

void TestMurasakiService::heavyPetsAreTranscodedWhenAdded()
{
    PetFixture f;
    QSignalSpy spy(&f.service, &MurasakiService::TranscodeChanged);

    // A light variant is not queued.
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    QCOMPARE(f.item(u"anima:9"_s).value(u"variants"_s).toArray().at(1).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString(),
             u"none"_s);

    // A heavy one is, and ends up done in the cache.
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"*"_s, QString());
    QVERIFY(QTest::qWaitFor([&] {
        return f.item(u"anima:9"_s).value(u"variants"_s).toArray().at(0).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString() == u"done"_s;
    }, 10000));
    const QJsonObject big = f.item(u"anima:9"_s).value(u"variants"_s).toArray().at(0).toObject();
    QCOMPARE(big.value(u"heavy"_s).toBool(), true);
    const QString file = big.value(u"transcode"_s).toObject().value(u"file"_s).toString();
    QVERIFY(file.startsWith(f.tmp.filePath(u"cache/pets/"_s)));
    QVERIFY(QFile::exists(file));
    QVERIFY(spy.count() >= 2);
    QCOMPARE(spy.at(0).at(0).toString(), u"anima:9"_s);

    // A pet that plays the original never starts a conversion.
    PetFixture g;
    g.service.AddPet(u"anima:9"_s, u"big"_s, u"*"_s, u"{\"playOriginal\": true}"_s);
    QTest::qWait(300);
    QCOMPARE(g.item(u"anima:9"_s).value(u"variants"_s).toArray().at(0).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString(),
             u"none"_s);
}

void TestMurasakiService::transcodeSizeFollowsTheWidthAndTheOutputScale()
{
    PetFixture f;
    logFilters(f);
    f.service.setTranscodeDebounceMs(150);
    // "big" is 900 px wide; shown 300 px wide at scale 1 it needs 300 px: bucket 384.
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 384);
    QVERIFY(bigTranscode(f).value(u"file"_s).toString().contains(u"-384-"_s));
    QVERIFY(filtersLog(f).contains(u"min(384,iw)"_s));
    QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{384}));

    // A 2x output needs 600 px: the 650 bucket. It is the new "transcode", and starts after the debounce.
    f.service.SetOutputScales(u"{\"DP-1\": 2.0}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"none"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QVERIFY(bigTranscode(f).value(u"file"_s).toString().contains(u"-650-"_s));
    QVERIFY(filtersLog(f).contains(u"min(650,iw)"_s));
    QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{384, 650})); // at most two copies are kept

    // "*" is the primary output, which the daemon does not know: it takes the largest scale.
    const QString star = f.service.AddPet(u"anima:9"_s, u"big"_s, u"*"_s, u"{\"width\": 100}"_s);
    QVERIFY(!star.isEmpty());
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
}

void TestMurasakiService::aStageIsConvertedToTheSizeOfTheScreenUpToTheSource()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(150);
    // The outputs say where they are (revision 17); "big" is 900 px on its long side, so a stage on a 1920x1080 output wants it whole: the 960 bucket, which a
    // pet never gets (it stops at 650).
    f.service.SetOutputScales(u"{\"DP-1\": {\"scale\": 1, \"x\": 0, \"y\": 0, \"width\": 1920, \"height\": 1080}, \"DP-2\": 1}"_s);
    const QString id = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"stage\": {\"mode\": \"output\"}}"_s);
    QVERIFY(!id.isEmpty());
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 960);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QVERIFY(bigTranscode(f).value(u"file"_s).toString().contains(u"-960-"_s));
    // turned off, it is a pet again: the finished copy is bigger than it needs and serves it (scaled down), so nothing is converted
    f.service.UpdatePet(id, u"{\"stage\": {\"mode\": \"off\"}, \"width\": 256}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 960);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"done"_s);
    // a geometry that is not valid is refused as a whole: the places told before stay
    f.service.SetOutputScales(u"{\"DP-1\": {\"scale\": 1, \"x\": 0, \"y\": 0, \"width\": 0, \"height\": 5}}"_s);
}

void TestMurasakiService::everyVariantAStateNamesIsConvertedLikeTheSelectedOne()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(150);
    // a pet on "small" (light: never converted) shows nothing of "big": no copy of it is made
    const QString plain = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"width\": 200}"_s);
    QVERIFY(!plain.isEmpty());
    QTest::qWait(400);
    QVERIFY(bigBuckets(f, u"done"_s).isEmpty() && bigBuckets(f, u"queued"_s).isEmpty() && bigBuckets(f, u"running"_s).isEmpty());
    // a pet that grabs as "big" gets it converted at its own size, though "big" is not its selected variant
    f.service.UpdatePet(plain, u"{\"states\": {\"grabbed\": {\"variant\": \"big\"}}}"_s);
    QVERIFY(QTest::qWaitFor([&] { return !bigBuckets(f, u"done"_s).isEmpty(); }, 10000));
    QCOMPARE(bigBuckets(f, u"done"_s), QList<int>{256}); // 200 px wide: the 256 bucket
    // and at birth too
    PetFixture g;
    g.service.setTranscodeDebounceMs(150);
    const QString born = g.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"width\": 200, \"states\": {\"released\": {\"sequence\": {\"items\": [\"small\", \"big\"]}}}}"_s);
    QVERIFY(!born.isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return !bigBuckets(g, u"done"_s).isEmpty(); }, 10000));
    // taking the states away leaves the copy to the cleanup that follows a change (nothing shows "big" now)
    g.service.UpdatePet(born, u"{\"states\": null}"_s);
    QVERIFY(QTest::qWaitFor([&] { return !g.listed().at(0).toObject().contains(u"states"_s); }, 2000));
}

void TestMurasakiService::petsWithDisjointStatesKeepEachOthersCopies()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(150);
    // pet A is on "small" and grabs as "big" at 200 px (the 256 bucket); pet B shows "big" at 650 px (the 650 bucket): each needs its own copy
    const QString a = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"width\": 200, \"states\": {\"grabbed\": {\"variant\": \"big\"}}}"_s);
    const QString b = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-2"_s, u"{\"width\": 650}"_s);
    QVERIFY(!a.isEmpty() && !b.isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return bigBuckets(f, u"done"_s) == (QList<int>{256, 650}); }, 15000));
    // pet A drops its states: the copy B needs stays, and so does the biggest one for A's former need (nothing is evicted while a pet shows the variant)
    f.service.UpdatePet(a, u"{\"states\": null}"_s);
    QTest::qWait(600);
    QVERIFY(bigBuckets(f, u"done"_s).contains(650));
    QCOMPARE(f.listed().size(), 2);
}

void TestMurasakiService::aResizeIsDebouncedAndUnusedBucketsAreRemoved()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(400);
    const QString id = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigBuckets(f, u"done"_s) == QList<int>{384}; }, 10000));

    // Shrinking: the 384 copy serves the smaller pet (scaled down), so nothing is converted and the copy in use is the one reported.
    f.service.UpdatePet(id, u"{\"width\": 100}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 384);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"done"_s);
    f.service.UpdatePet(id, u"{\"width\": 90}"_s);
    QTest::qWait(900);
    QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{384}));
    QVERIFY(bigBuckets(f, u"queued"_s).isEmpty() && bigBuckets(f, u"running"_s).isEmpty());

    // Growing past the biggest copy: one conversion, after the debounce; the biggest copy stays besides the newest other one.
    f.service.UpdatePet(id, u"{\"width\": 600}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"none"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigBuckets(f, u"done"_s).contains(650); }, 10000));
    QTRY_COMPARE_WITH_TIMEOUT(cachedBuckets(f), (QStringList{u"384"_s, u"650"_s}), 5000);

    // Small again and removed: the biggest copy stays (and the newest other one: never above two)
    f.service.UpdatePet(id, u"{\"width\": 100}"_s);
    f.service.RemovePet(id);
    QTest::qWait(900);
    QCOMPARE(cachedBuckets(f).size(), 2);
    QVERIFY(cachedBuckets(f).contains(u"650"_s));
    // Placed again, as small as before or as big: the copies are reused, nothing is converted.
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 600}"_s);
    QTest::qWait(900);
    QVERIFY(bigBuckets(f, u"queued"_s).isEmpty() && bigBuckets(f, u"running"_s).isEmpty());
    QCOMPARE(cachedBuckets(f), (QStringList{u"384"_s, u"650"_s}));
}

void TestMurasakiService::everyPlacementKeepsItsBucket()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(100);
    const auto big = [&](int bucket) {
        const auto item = f.library.item(u"anima:9"_s);
        const PetVariant &variant = item->variants.first();
        return TranscodeSource{item->id, variant.name, variant.file, variant.fps, variant.frames, bucket};
    };
    // A small and a big pet of the same variant: 256 and 650 are each the best for one of them (650 also because it is the biggest).
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 100}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 600}"_s);
    // the service converts the two it needs; a third one nobody needs (made by hand) does not stay
    QTRY_COMPARE_WITH_TIMEOUT(cachedBuckets(f), (QStringList{u"256"_s, u"650"_s}), 15000);
    f.transcodes.request(big(384));
    QVERIFY(QTest::qWaitFor([&] { return f.transcodes.status(big(384)).state != TranscodeStatus::Done && f.transcodes.status(big(384)).state != TranscodeStatus::Queued && f.transcodes.status(big(384)).state != TranscodeStatus::Running; }, 15000));
    QTRY_COMPARE_WITH_TIMEOUT(cachedBuckets(f), (QStringList{u"256"_s, u"650"_s}), 5000);
    QTest::qWait(300);
    QCOMPARE(cachedBuckets(f), (QStringList{u"256"_s, u"650"_s}));

    // The startup prune is told the same two.
    QList<int> expected;
    for (const TranscodeSource &source : f.service.expectedTranscodes()) {
        if (source.variant == u"big"_s)
            expected.append(source.bucket);
    }
    std::sort(expected.begin(), expected.end());
    QCOMPARE(expected, (QList<int>{256, 650}));
}

void TestMurasakiService::outputScalesAreValidated()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(60000); // nothing may start by itself in this test
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 384);

    const auto rejected = [&](const QString &json) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.SetOutputScales(json);
        return bigTranscode(f).value(u"bucket"_s).toInt() == 384; // nothing changed
    };
    QVERIFY(rejected(u"{\"DP-1\": 9}"_s));
    QVERIFY(rejected(u"{\"DP-1\": 0.1}"_s));
    QVERIFY(rejected(u"{\"DP-1\": \"2\"}"_s));
    QVERIFY(rejected(u"{\"\": 2}"_s));
    QVERIFY(rejected(u"[2]"_s));
    QVERIFY(rejected(u"nope"_s));
    QStringList many;
    for (int i = 0; i < 33; ++i)
        many << u"\"out-%1\": 2"_s.arg(i);
    QVERIFY(rejected(u"{"_s + many.join(u',') + u"}"_s));

    f.service.SetOutputScales(u"{\"DP-1\": 2.0}"_s);
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
    f.service.SetOutputScales(u"{}"_s); // forgotten: back to 1
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 384);
}

void TestMurasakiService::aCopyFromBeforeBucketsServesSmallerPets()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(100);
    const auto item = f.library.item(u"anima:9"_s);
    const PetVariant &variant = item->variants.first();
    TranscodeSource src{item->id, variant.name, variant.file, variant.fps, variant.frames, 650};
    f.transcodes.request(src);
    QVERIFY(QTest::qWaitFor([&] { return f.transcodes.status(src).state == TranscodeStatus::Done; }, 10000));
    // Make it look like the cache of the version before buckets (at the file's own timing): <variant>-<key>.webm.
    QString legacy = f.transcodes.outputPath(src);
    legacy.replace(u"650-0-"_s, QString()); // <variant>-<key>.webm, from before buckets
    QVERIFY(QFile::rename(f.transcodes.outputPath(src), legacy));
    QCOMPARE(f.transcodes.status(src).state, TranscodeStatus::Done);
    QCOMPARE(f.transcodes.status(src).file, legacy);

    // The pet needs 384: the old copy is the 650 one and serves it, so nothing is converted and the copy stays.
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 0}"_s);
    QVERIFY(bigBuckets(f, u"done"_s).contains(650));
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"done"_s);
    QTest::qWait(700);
    QVERIFY(bigBuckets(f, u"queued"_s).isEmpty() && bigBuckets(f, u"running"_s).isEmpty());
    QVERIFY(QFile::exists(legacy));
    QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{650}));
}

void TestMurasakiService::startupQueueWaitsForTheFirstScales()
{
    PetFixture f(false);
    f.service.setScalesGraceMs(60000);
    f.service.setTranscodeDebounceMs(60000);
    PetPlacement placement;
    placement.itemId = u"anima:9"_s;
    placement.variant = u"big"_s;
    placement.output = u"DP-1"_s;
    placement.width = 300;
    QString problem;
    QVERIFY(!f.pets.add(placement, &problem).isEmpty());
    f.service.attachPets(&f.pets, &f.transcodes);
    QTest::qWait(300);
    QCOMPARE(bigBuckets(f, u"queued"_s) + bigBuckets(f, u"running"_s) + bigBuckets(f, u"done"_s), QList<int>()); // held

    f.service.SetOutputScales(u"{\"DP-1\": 2.0}"_s); // HiDPI: 600 px, not the 384 of scale 1
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 650);
    QCOMPARE(cachedBuckets(f), (QStringList{u"650"_s}));
}

void TestMurasakiService::startupQueueGoesAheadAfterTheGracePeriod()
{
    PetFixture f(false);
    f.service.setScalesGraceMs(200);
    PetPlacement placement;
    placement.itemId = u"anima:9"_s;
    placement.variant = u"big"_s;
    placement.output = u"DP-1"_s;
    placement.width = 300;
    QString problem;
    QVERIFY(!f.pets.add(placement, &problem).isEmpty());
    f.service.attachPets(&f.pets, &f.transcodes);
    QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"none"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(bigTranscode(f).value(u"bucket"_s).toInt(), 384); // no overlay came: scale 1
}

void TestMurasakiService::switchingToTheOriginalPrunesTheBuckets()
{
    PetFixture f;
    f.service.setTranscodeDebounceMs(100);
    const auto item = f.library.item(u"anima:9"_s);
    const PetVariant &variant = item->variants.first();
    const auto source = [&](int bucket) {
        return TranscodeSource{item->id, variant.name, variant.file, variant.fps, variant.frames, bucket};
    };
    // Three pets, each the user of one bucket: all three copies stay.
    const QString small = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 100, \"fps\": 0}"_s);
    const QString middle = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 0}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 600, \"fps\": 0}"_s);
    for (const int bucket : {256, 384, 650}) {
        f.transcodes.request(source(bucket));
        QVERIFY(QTest::qWaitFor([&] { return f.transcodes.status(source(bucket)).state == TranscodeStatus::Done; }, 10000));
    }
    QTest::qWait(300);
    QCOMPARE(cachedBuckets(f), (QStringList{u"256"_s, u"384"_s, u"650"_s}));

    f.service.UpdatePet(small, u"{\"playOriginal\": true}"_s);
    f.service.UpdatePet(middle, u"{\"playOriginal\": true}"_s);
    QTRY_COMPARE_WITH_TIMEOUT(cachedBuckets(f).size(), 2, 5000);
    QVERIFY(cachedBuckets(f).contains(u"650"_s)); // the one pet that still plays a copy keeps its bucket
}

void TestMurasakiService::aQueuedBucketNobodyNeedsAnymoreIsCancelled()
{
    PetFixture f;
    logFilters(f);
    // Slow conversions, so that the next one waits in the queue.
    const QString script = f.tmp.filePath(u"bin/ffmpeg"_s);
    testutil::writeFile(script, "#!/bin/sh\nprev=\nfor l; do\n[ \"$prev\" = -vf ] && echo \"$l\" >> '"
                                    + f.tmp.filePath(u"filters.txt"_s).toUtf8() + "'\nprev=$l\ndone\nsleep 1.5\necho fake > \"$l\"\n");
    QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    f.service.setTranscodeDebounceMs(100);
    const QString id = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300}"_s); // 384 runs
    QTRY_COMPARE_WITH_TIMEOUT(bigBuckets(f, u"running"_s), (QList<int>{384}), 5000);
    f.service.UpdatePet(id, u"{\"width\": 100}"_s);
    QTRY_COMPARE_WITH_TIMEOUT(bigBuckets(f, u"queued"_s), (QList<int>{256}), 3000); // waits behind 384
    f.service.UpdatePet(id, u"{\"width\": 600}"_s);
    QTRY_COMPARE_WITH_TIMEOUT(bigBuckets(f, u"queued"_s), (QList<int>{650}), 3000); // 256 was dropped, 650 queued
    QVERIFY(QTest::qWaitFor([&] { return bigBuckets(f, u"done"_s).contains(650); }, 15000));
    QVERIFY(!filtersLog(f).contains(u"min(256,iw)"_s));
}

void TestMurasakiService::newPetsStartAtTheDefaultRate()
{
    PetFixture f;
    const auto fpsOf = [&](const QString &petId) {
        for (const QJsonValue &value : f.listed()) {
            if (value.toObject().value(u"petId"_s).toString() == petId)
                return value.toObject().value(u"fps"_s).toInt(-1);
        }
        return -2;
    };
    // 24 fps for every source (the user's decision; the file's own timing and Anima Engine's 15 are one choice away).
    const QString anima = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    QCOMPARE(fpsOf(anima), 24);
    const QString gif = f.tmp.filePath(u"mine/Dancer.gif"_s);
    testutil::writeFile(gif, animfixtures::gif(32, 24, {5, 5, 5}));
    const QString local = f.service.AddPet(f.service.AddLocalPetItem(gif), QString(), u"DP-1"_s, QString());
    QCOMPARE(fpsOf(local), 24);
    // Chosen on adding, and changed later.
    const QString chosen = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"fps\": 15}"_s);
    QCOMPARE(fpsOf(chosen), 15);
    f.service.UpdatePet(chosen, u"{\"fps\": 0}"_s);
    QCOMPARE(fpsOf(chosen), 0);
    f.service.UpdatePet(chosen, u"{\"fps\": 60}"_s);
    QCOMPARE(fpsOf(chosen), 60);

    // Out of range, not whole, not a number: refused, nothing changes.
    const auto rejected = [&](const QString &changes) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.UpdatePet(chosen, changes);
        return fpsOf(chosen) == 60;
    };
    QVERIFY(rejected(u"{\"fps\": 121}"_s));
    QVERIFY(rejected(u"{\"fps\": -1}"_s));
    QVERIFY(rejected(u"{\"fps\": 12.5}"_s));
    QVERIFY(rejected(u"{\"fps\": \"30\"}"_s));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"fps\": 200}"_s).isEmpty());
}

void TestMurasakiService::petBehaviourFieldsTravelThroughTheApi()
{
    PetFixture f;
    // the defaults of a new pet are what every pet did before the fields
    const QString id = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    QJsonObject pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"clickThrough"_s).toBool(), true);
    QCOMPARE(pet.value(u"keepAbove"_s).toBool(), true);
    QCOMPARE(pet.value(u"locked"_s).toBool(), false);
    QCOMPARE(pet.value(u"rotation"_s).toDouble(), 0.0);
    // chosen on adding, changed later, wrapped, persisted
    const QString second = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s,
                                            u"{\"clickThrough\": false, \"locked\": true, \"rotation\": 450, \"flipH\": true, \"hoverHide\": true}"_s);
    QVERIFY(!second.isEmpty());
    pet = f.listed().at(1).toObject();
    QCOMPARE(pet.value(u"clickThrough"_s).toBool(), false);
    QCOMPARE(pet.value(u"locked"_s).toBool(), true);
    QCOMPARE(pet.value(u"rotation"_s).toDouble(), 90.0);
    QCOMPARE(pet.value(u"flipH"_s).toBool(), true);
    f.service.UpdatePet(second, u"{\"focusFade\": true, \"keepAbove\": false, \"smartDodge\": true, \"rotation\": -45.678}"_s);
    pet = f.listed().at(1).toObject();
    QCOMPARE(pet.value(u"focusFade"_s).toBool(), true);
    QCOMPARE(pet.value(u"hoverHide"_s).toBool(), false); // excluded by the new one
    QCOMPARE(pet.value(u"keepAbove"_s).toBool(), false);
    QCOMPARE(pet.value(u"smartDodge"_s).toBool(), true);
    QCOMPARE(pet.value(u"rotation"_s).toDouble(), 314.32);
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QCOMPARE(reread.pets().at(1).rotation, 314.32);
    QVERIFY(reread.pets().at(1).focusFade && reread.pets().at(1).smartDodge && !reread.pets().at(1).keepAbove);
    // refused: wrong type, both exclusive flags at once; nothing changes
    const auto rejected = [&](const QString &changes) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.UpdatePet(second, changes);
        return f.listed().at(1).toObject().value(u"focusFade"_s).toBool() && f.listed().at(1).toObject().value(u"rotation"_s).toDouble() == 314.32;
    };
    QVERIFY(rejected(u"{\"rotation\": \"90\"}"_s));
    QVERIFY(rejected(u"{\"locked\": 1}"_s));
    QVERIFY(rejected(u"{\"hoverHide\": true, \"focusFade\": true}"_s));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"flipV\": \"yes\"}"_s).isEmpty());
    Q_UNUSED(id);
}

// A WAV of 100 ms of a quiet tone, mono, 16-bit, 22.05 kHz: written by hand so that no tool is needed.
static QByteArray tinyWav()
{
    const int frames = 2205;
    QByteArray data;
    for (int i = 0; i < frames; ++i) {
        const qint16 v = qint16(3000 * ((i / 25) % 2 ? 1 : -1));
        data.append(char(v & 0xff));
        data.append(char((v >> 8) & 0xff));
    }
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    return "RIFF" + le32(36 + quint32(data.size())) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(22050) + le32(44100) + le16(2) + le16(16) + "data"
        + le32(quint32(data.size())) + data;
}

void TestMurasakiService::thePetSoundTravelsThroughTheApiAndTheAssetCalls()
{
    PetFixture f;
    const QString asset(64, QLatin1Char('c'));
    // The sound is a placement key: off by default, chosen on adding or later, merged, removed with null, validated.
    const QString id = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    QVERIFY(!f.listed().at(0).toObject().contains(u"audio"_s));
    f.service.UpdatePet(id, u"{\"audio\": {\"asset\": \"%1\", \"mode\": \"click\", \"volume\": 40}}"_s.arg(asset));
    QJsonObject audio = f.listed().at(0).toObject().value(u"audio"_s).toObject();
    QCOMPARE(audio.value(u"asset"_s).toString(), asset);
    QCOMPARE(audio.value(u"mode"_s).toString(), u"click"_s);
    QCOMPARE(audio.value(u"volume"_s).toInt(), 40);
    QCOMPARE(audio.value(u"offsetMs"_s).toInt(), 0);
    f.service.UpdatePet(id, u"{\"audio\": {\"offsetMs\": -300}}"_s);
    QCOMPARE(f.listed().at(0).toObject().value(u"audio"_s).toObject().value(u"mode"_s).toString(), u"click"_s);
    QCOMPARE(f.listed().at(0).toObject().value(u"audio"_s).toObject().value(u"offsetMs"_s).toInt(), -300);
    for (const char *bad : {"{\"audio\": {\"mode\": \"loud\"}}", "{\"audio\": {\"volume\": 101}}", "{\"audio\": 3}", "{\"audio\": {\"asset\": \"../x\"}}"}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.UpdatePet(id, QString::fromLatin1(bad));
        QCOMPARE(f.listed().at(0).toObject().value(u"audio"_s).toObject().value(u"volume"_s).toInt(), 40);
    }
    // the asset calls need the manager
    QCOMPARE(f.service.ListPetAudioAssets(), u"[]"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.ImportPetAudio(f.tmp.filePath(u"x.wav"_s)).isEmpty());
    PetAudioManager manager(f.tmp.filePath(u"data/pets/audio"_s), QStringLiteral(MURASAKI_PETAUDIO_BIN), QString());
    manager.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
    f.service.attachPetAudio(&manager);
    QSignalSpy changed(&f.service, &MurasakiService::PetAudioChanged);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.ImportPetAudio(u"relative.wav"_s).isEmpty());
    testutil::writeFile(f.tmp.filePath(u"tone.wav"_s), tinyWav());
    const QString job = f.service.ImportPetAudio(f.tmp.filePath(u"tone.wav"_s));
    QVERIFY(!job.isEmpty());
    QString imported;
    QVERIFY(QTest::qWaitFor(
        [&] {
            const QJsonObject state = QJsonDocument::fromJson(f.service.PetAudioImportState(job).toUtf8()).object();
            if (state.value(u"state"_s).toString() == u"done"_s)
                imported = state.value(u"result"_s).toObject().value(u"asset"_s).toString();
            return !imported.isEmpty();
        },
        10000));
    QVERIFY(changed.count() >= 2);
    QCOMPARE(f.service.PetAudioImportState(u"nope"_s), u"{}"_s);
    QJsonArray assets = QJsonDocument::fromJson(f.service.ListPetAudioAssets().toUtf8()).array();
    QCOMPARE(assets.size(), 1);
    QCOMPARE(assets.at(0).toObject().value(u"asset"_s).toString(), imported);
    QCOMPARE(assets.at(0).toObject().value(u"refs"_s).toInt(), 0);
    QCOMPARE(assets.at(0).toObject().value(u"name"_s).toString(), u"tone.wav"_s);
    // a pet that uses it keeps it: the asset can not be removed or collected
    f.service.UpdatePet(id, u"{\"audio\": {\"asset\": \"%1\"}}"_s.arg(imported));
    QCOMPARE(QJsonDocument::fromJson(f.service.ListPetAudioAssets().toUtf8()).array().at(0).toObject().value(u"refs"_s).toInt(), 1);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemovePetAudioAsset(imported);
    QCOMPARE(f.service.CollectPetAudio(), 0);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListPetAudioAssets().toUtf8()).array().size(), 1);
    // null takes the sound off the pet; then the asset can go
    f.service.UpdatePet(id, u"{\"audio\": null}"_s);
    QVERIFY(!f.listed().at(0).toObject().contains(u"audio"_s));
    f.service.RemovePetAudioAsset(imported);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListPetAudioAssets().toUtf8()).array().size(), 0);
    // and the sound survives the store
    f.service.UpdatePet(id, u"{\"audio\": {\"asset\": \"%1\", \"mode\": \"once\"}}"_s.arg(asset));
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QCOMPARE(reread.pets().at(0).audio.mode, PetAudioMode::Once);
    QCOMPARE(reread.pets().at(0).audio.asset, asset);
}

void TestMurasakiService::theKeptAudioOfAnImportedVideoBecomesASound()
{
    PetFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    const QString gif = f.tmp.filePath(u"fixture.gif"_s);
    testutil::writeFile(gif, animfixtures::gif(32, 24, {5, 5, 5}));
    // Three job folders as the import leaves them: audio kept, audio not kept, a record that points out of the folder.
    const auto job = [&](const QString &name, const QString &record) {
        testutil::writeFile(root + u"/"_s + name + u"/pet.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
        testutil::writeFile(root + u"/"_s + name + u"/source.json"_s, record.toUtf8());
        return f.service.AddLocalPetItem(root + u"/"_s + name + u"/pet.gif"_s);
    };
    testutil::writeFile(root + u"/keeps/source/clip.wav"_s, tinyWav());
    const QString kept = job(u"keeps"_s, u"{\"audio\":{\"hasAudio\":true,\"kept\":true,\"file\":\"source/clip.wav\",\"played\":false}}"_s);
    const QString notKept = job(u"drops"_s, u"{\"audio\":{\"hasAudio\":true,\"kept\":false,\"file\":\"\",\"played\":false}}"_s);
    const QString outside = job(u"escapes"_s, u"{\"audio\":{\"hasAudio\":true,\"kept\":true,\"file\":\"../keeps/source/clip.wav\"}}"_s);
    testutil::writeFile(root + u"/links/source/real.wav"_s, tinyWav());
    QVERIFY(QFile::link(root + u"/links/source/real.wav"_s, root + u"/links/source/link.wav"_s));
    const QString linked = job(u"links"_s, u"{\"audio\":{\"hasAudio\":true,\"kept\":true,\"file\":\"source/link.wav\"}}"_s);
    const QString plain = f.service.AddLocalPetItem(gif);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    PetAudioManager manager(f.tmp.filePath(u"data/pets/audio"_s), QStringLiteral(MURASAKI_PETAUDIO_BIN), QString());
    manager.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
    f.service.attachPetAudio(&manager);
    for (const QString &bad : {notKept, outside, linked, plain, u"local:nothing"_s}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        QVERIFY2(f.service.ImportItemAudio(bad).isEmpty(), qPrintable(bad));
    }
    const QString jobId = f.service.ImportItemAudio(kept);
    QVERIFY(!jobId.isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return manager.job(jobId).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(QJsonDocument::fromJson(f.service.ListPetAudioAssets().toUtf8()).array().size(), 1);
}

void TestMurasakiService::rescuePetsChangesExactlyTheListedFieldsAndWritesOnce()
{
    PetFixture f;
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s,
                     u"{\"clickThrough\": true, \"locked\": true, \"keepAbove\": false, \"smartDodge\": true, \"focusFade\": true, \"opacity\": 0.2, "
                     "\"rotation\": 33, \"flipH\": true, \"x\": 11, \"y\": 22, \"width\": 120, \"paused\": true}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-2"_s, u"{\"hoverHide\": true, \"opacity\": 0.6, \"flipV\": true, \"rotation\": 270}"_s);
    f.service.SetPetsHidden(true);
    const QJsonArray before = f.listed();
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    QSignalSpy hidden(&f.service, &MurasakiService::PetsHiddenChanged);
    f.service.RescuePets();
    QCOMPARE(changed.count(), 1); // one write for both pets
    QCOMPARE(hidden.count(), 1);
    QVERIFY(!f.service.petsHidden());
    const QJsonArray after = f.listed();
    QCOMPARE(after.size(), 2);
    for (int i = 0; i < 2; ++i) {
        const QJsonObject was = before.at(i).toObject();
        QJsonObject now = after.at(i).toObject();
        // the listed fields
        QCOMPARE(now.value(u"clickThrough"_s).toBool(), false);
        QCOMPARE(now.value(u"keepAbove"_s).toBool(), true);
        for (const char *key : {"locked", "hoverHide", "focusFade", "smartDodge"})
            QCOMPARE(now.value(QLatin1String(key)).toBool(), false);
        QCOMPARE(now.value(u"opacity"_s).toDouble(), was.value(u"opacity"_s).toDouble() < 0.5 ? 1.0 : was.value(u"opacity"_s).toDouble());
        // and nothing else
        for (const char *key : {"clickThrough", "keepAbove", "locked", "hoverHide", "focusFade", "smartDodge", "opacity"}) {
            now.remove(QLatin1String(key));
        }
        QJsonObject rest = was;
        for (const char *key : {"clickThrough", "keepAbove", "locked", "hoverHide", "focusFade", "smartDodge", "opacity"})
            rest.remove(QLatin1String(key));
        QCOMPARE(now, rest);
    }
    QCOMPARE(after.at(0).toObject().value(u"opacity"_s).toDouble(), 1.0);
    QCOMPARE(after.at(1).toObject().value(u"opacity"_s).toDouble(), 0.6);
    QCOMPARE(after.at(0).toObject().value(u"rotation"_s).toDouble(), 33.0);
    QVERIFY(!f.service.petsArrange()); // never turns the arrange mode on
    // a second rescue finds nothing to change: no write, no signal
    f.service.RescuePets();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(hidden.count(), 1);
    // persisted
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QVERIFY(!reread.pets().at(0).clickThrough && reread.pets().at(0).keepAbove && !reread.pets().at(1).hoverHide);
    // without the pet API it is refused
    Library library;
    AssignmentStore store(f.tmp.filePath(u"a2.json"_s));
    SettingsStore settings(f.tmp.filePath(u"s2.json"_s));
    MurasakiService bare(&library, &store, &settings, u"ok"_s, {});
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    bare.RescuePets();
}

void TestMurasakiService::petsHiddenSignalsOnlyOnChangeAndIsNeverStored()
{
    PetFixture f;
    QVERIFY(!f.service.petsHidden());
    QSignalSpy hidden(&f.service, &MurasakiService::PetsHiddenChanged);
    f.service.SetPetsHidden(false);
    QCOMPARE(hidden.count(), 0);
    f.service.SetPetsHidden(true);
    f.service.SetPetsHidden(true);
    QCOMPARE(hidden.count(), 1);
    QCOMPARE(hidden.at(0).at(0).toBool(), true);
    QVERIFY(f.service.petsHidden());
    f.service.SetPetsHidden(false);
    QCOMPARE(hidden.count(), 2);
    // a daemon that starts again knows nothing of it, and nothing of it is in the pets file
    f.service.SetPetsHidden(true);
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    QFile file(f.tmp.filePath(u"config/pets.json"_s));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("idden"));
    MurasakiService restarted(&f.library, &f.assignments, &f.settings, u"ok"_s, {});
    QVERIFY(!restarted.petsHidden());
}

void TestMurasakiService::rescuePetsTurnsEveryStageOffInTheSameWrite()
{
    PetFixture f;
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"stage\": {\"mode\": \"output\", \"fit\": \"cover\"}, \"x\": 11, \"y\": 22, \"width\": 120}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-2"_s, u"{\"hoverHide\": true}"_s);
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    f.service.RescuePets();
    QCOMPARE(changed.count(), 1); // one write: the stage and the other flags together
    const QJsonArray after = f.listed();
    const QJsonObject first = after.at(0).toObject();
    QCOMPARE(first.value(u"stage"_s).toObject().value(u"mode"_s).toString(), u"off"_s);
    QCOMPARE(first.value(u"stage"_s).toObject().value(u"fit"_s).toString(), u"cover"_s); // (its other choices are kept)
    QCOMPARE(first.value(u"x"_s).toDouble(), 11.0); // and the place and size it had
    QCOMPARE(first.value(u"width"_s).toDouble(), 120.0);
    QVERIFY(!after.at(1).toObject().contains(u"stage"_s));
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QVERIFY(!reread.pets().at(0).stage.active());
}

void TestMurasakiService::theStageOfAPetIsSetAndRefusedThroughTheService()
{
    PetFixture f;
    const QString a = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"stage\": {\"mode\": \"output\"}}"_s);
    QVERIFY(!a.isEmpty());
    QCOMPARE(f.listed().at(0).toObject().value(u"stage"_s).toObject().value(u"mode"_s).toString(), u"output"_s);
    // a second stage on that output is refused with a message, and a wrong value too; nothing changes
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"stage\": {\"mode\": \"output\"}}"_s);
    QCOMPARE(f.listed().size(), 1);
    const QString b = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-2"_s, QString());
    QVERIFY(!b.isEmpty());
    f.service.UpdatePet(b, u"{\"stage\": {\"mode\": \"sideways\"}}"_s);
    QVERIFY(!f.listed().at(1).toObject().contains(u"stage"_s));
    f.service.UpdatePet(b, u"{\"stage\": {\"mode\": \"desktop\"}}"_s);
    QVERIFY(!f.listed().at(1).toObject().contains(u"stage"_s)); // desktop takes DP-1 too
    f.service.UpdatePet(a, u"{\"stage\": null}"_s);
    QVERIFY(!f.listed().at(0).toObject().contains(u"stage"_s));
    f.service.UpdatePet(b, u"{\"stage\": {\"mode\": \"desktop\"}}"_s);
    QCOMPARE(f.listed().at(1).toObject().value(u"stage"_s).toObject().value(u"mode"_s).toString(), u"desktop"_s);
}

void TestMurasakiService::statesAndTrackingAreSetAndRefusedThroughTheService()
{
    PetFixture f;
    const QString a = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    QVERIFY(!a.isEmpty());
    QVERIFY(!f.listed().at(0).toObject().contains(u"states"_s)); // a pet that does not use them has neither key
    QVERIFY(!f.listed().at(0).toObject().contains(u"tracking"_s));
    f.service.UpdatePet(a, u"{\"states\": {\"grabbed\": {\"variant\": \"big\"}, \"idle\": {\"sequence\": {\"items\": [\"small\", \"big\"], \"order\": \"random\"}}}, \"tracking\": {\"mode\": \"look\"}}"_s);
    QJsonObject pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"states"_s).toObject().value(u"grabbed"_s).toObject().value(u"variant"_s).toString(), u"big"_s);
    QCOMPARE(pet.value(u"states"_s).toObject().value(u"idle"_s).toObject().value(u"sequence"_s).toObject().value(u"items"_s).toArray().size(), 2);
    QCOMPARE(pet.value(u"tracking"_s).toObject().value(u"mode"_s).toString(), u"look"_s);
    // a wrong value is an error and changes nothing; so does a follower that also dodges
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    f.service.UpdatePet(a, u"{\"states\": {\"idle\": {\"sequence\": {\"items\": []}}}}"_s);
    f.service.UpdatePet(a, u"{\"tracking\": {\"mode\": \"chase\"}}"_s);
    f.service.UpdatePet(a, u"{\"tracking\": {\"mode\": \"follow\"}, \"smartDodge\": true}"_s);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(f.listed().at(0).toObject(), pet);
    // partial merge and clearing
    f.service.UpdatePet(a, u"{\"tracking\": {\"mode\": \"follow\", \"distance\": 300}, \"states\": {\"grabbed\": null}}"_s);
    pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"tracking"_s).toObject().value(u"distance"_s).toDouble(), 300.0);
    QVERIFY(!pet.value(u"states"_s).toObject().contains(u"grabbed"_s));
    QVERIFY(pet.value(u"states"_s).toObject().contains(u"idle"_s));
    f.service.UpdatePet(a, u"{\"smartDodge\": true}"_s); // the follower refuses a dodge
    QVERIFY(!f.listed().at(0).toObject().value(u"smartDodge"_s).toBool());
    f.service.UpdatePet(a, u"{\"states\": null, \"tracking\": null}"_s);
    QVERIFY(!f.listed().at(0).toObject().contains(u"states"_s));
    QVERIFY(!f.listed().at(0).toObject().contains(u"tracking"_s));
}

void TestMurasakiService::rescuePetsTurnsTrackingOff()
{
    PetFixture f;
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"tracking\": {\"mode\": \"follow\", \"distance\": 250}, \"states\": {\"grabbed\": {\"variant\": \"big\"}}}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-2"_s, QString());
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    f.service.RescuePets();
    QCOMPARE(changed.count(), 1); // one write with the other rescue effects
    const QJsonObject first = f.listed().at(0).toObject();
    QCOMPARE(first.value(u"tracking"_s).toObject().value(u"mode"_s).toString(), u"off"_s);
    QCOMPARE(first.value(u"tracking"_s).toObject().value(u"distance"_s).toDouble(), 250.0); // (its other choices stay)
    QVERIFY(first.value(u"states"_s).toObject().contains(u"grabbed"_s)); // states are not a hazard: kept
    QVERIFY(!f.listed().at(1).toObject().contains(u"tracking"_s));
    PetStore reread(f.tmp.filePath(u"config/pets.json"_s));
    reread.load();
    QCOMPARE(reread.pets().at(0).tracking.mode, PetTrackingMode::Off);
}

void TestMurasakiService::aFailingWriteMakesRescuePetsAllOrNothing()
{
    PetFixture f;
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"locked\": true, \"clickThrough\": true}"_s);
    f.service.SetPetsHidden(true);
    const QJsonArray before = f.listed();
    QSignalSpy hidden(&f.service, &MurasakiService::PetsHiddenChanged);
    QSignalSpy changed(&f.service, &MurasakiService::PetsChanged);
    const QString dir = f.tmp.filePath(u"config"_s);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
    if (QFileInfo(dir).isWritable())
        QSKIP("the directory can be written whatever its mode (running as root?)");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RescuePets();
    QCOMPARE(f.listed(), before);          // nothing changed
    QVERIFY(f.service.petsHidden());       // and the pets were not shown: all or nothing
    QCOMPARE(hidden.count(), 0);
    QCOMPARE(changed.count(), 0);
}

void TestMurasakiService::choosingHoverHideThroughUpdatePetClearsFocusFade()
{
    PetFixture f;
    const QString id = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"focusFade\": true}"_s);
    QCOMPARE(f.listed().at(0).toObject().value(u"focusFade"_s).toBool(), true);
    f.service.UpdatePet(id, u"{\"hoverHide\": true}"_s);
    QJsonObject pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"hoverHide"_s).toBool(), true);
    QCOMPARE(pet.value(u"focusFade"_s).toBool(), false); // the earlier one is cleared
    f.service.UpdatePet(id, u"{\"focusFade\": true}"_s);
    pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"focusFade"_s).toBool(), true);
    QCOMPARE(pet.value(u"hoverHide"_s).toBool(), false);
}

void TestMurasakiService::everyRateSharesOneCopy()
{
    // The copy does not depend on the playback rate (it is applied when the pet plays): pets at three different rates share one conversion.
    PetFixture f;
    logFilters(f);
    f.service.setTranscodeDebounceMs(100);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 30}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 15}"_s);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"speed\": 2}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 15000));
    QTest::qWait(500);
    QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{384}));
    QVERIFY(!filtersLog(f).contains(u"setpts"_s)); // nothing is retimed in the copy
    QCOMPARE(filtersLog(f).count(u"scale="_s), 1);  // and there was one conversion
    QCOMPARE(bigTranscode(f).value(u"fps"_s).toInt(), 0);
}

void TestMurasakiService::changingTheRateOrTheScaleStartsNoConversion()
{
    PetFixture f;
    logFilters(f);
    f.service.setTranscodeDebounceMs(200);
    const QString id = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 30}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    const QString file = bigTranscode(f).value(u"file"_s).toString();
    for (const char *change : {"{\"fps\": 24}", "{\"fps\": 120}", "{\"fps\": 0}", "{\"speed\": 0.5}", "{\"width\": 200}", "{\"width\": 380}"}) {
        f.service.UpdatePet(id, QString::fromLatin1(change));
        QTest::qWait(500);
        QVERIFY2(bigBuckets(f, u"queued"_s).isEmpty() && bigBuckets(f, u"running"_s).isEmpty(), change);
        QCOMPARE(bigBuckets(f, u"done"_s), (QList<int>{384}));
        QCOMPARE(bigTranscode(f).value(u"file"_s).toString(), file);
    }
    QCOMPARE(filtersLog(f).count(u"scale="_s), 1);
}

void TestMurasakiService::aCopyRetimedBeforeServesEveryFixedRate()
{
    // The user's shape: pets at a fixed 24 fps and copies <bucket>-30-<key>.webm from the versions that retimed them. Such a copy has uniform
    // spacing, so it serves a pet at any fixed rate (the overlay sets mpv's speed to the ratio): nothing is converted when the rate or the
    // scale change, and the copy stays until one at the file's timing is as big.
    PetFixture f;
    logFilters(f);
    f.service.setTranscodeDebounceMs(100);
    const auto item = f.library.item(u"anima:9"_s);
    const PetVariant &variant = item->variants.first();
    TranscodeSource retimed{item->id, variant.name, variant.file, variant.fps, variant.frames, 384, 30};
    f.transcodes.request(retimed);
    QVERIFY(QTest::qWaitFor([&] { return f.transcodes.status(retimed).state == TranscodeStatus::Done; }, 10000));
    const int before = filtersLog(f).count(u"scale="_s);
    const QString id = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 24}"_s);
    for (const char *change : {"{}", "{\"fps\": 60}", "{\"fps\": 30}", "{\"width\": 200}", "{\"fps\": 1}"}) {
        f.service.UpdatePet(id, QString::fromLatin1(change));
        QTest::qWait(500);
        QCOMPARE(filtersLog(f).count(u"scale="_s), before);
        QCOMPARE(bigTranscode(f).value(u"fps"_s).toInt(), 30); // the copy in use
        QCOMPARE(bigTranscode(f).value(u"state"_s).toString(), u"done"_s);
        QCOMPARE(cachedBuckets(f), (QStringList{u"384"_s}));
    }
    // A pet at the file's own timing cannot use it (the file's delays are gone from it): one conversion at the file's timing, then the retimed copy goes
    f.service.UpdatePet(id, u"{\"fps\": 0}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"fps"_s).toInt() == 0 && bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(filtersLog(f).count(u"scale="_s), before + 1);
    QTRY_COMPARE_WITH_TIMEOUT(cachedBuckets(f).size(), 1, 5000);
    // Back to a fixed rate: the copy at the file's timing serves it, nothing more is converted
    f.service.UpdatePet(id, u"{\"fps\": 24}"_s);
    QTest::qWait(500);
    QCOMPARE(filtersLog(f).count(u"scale="_s), before + 1);
}

void TestMurasakiService::theFilesOwnTimingIsNotRetimed()
{
    PetFixture f;
    logFilters(f);
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, u"{\"width\": 300, \"fps\": 0}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bigTranscode(f).value(u"state"_s).toString() == u"done"_s; }, 10000));
    QCOMPARE(bigTranscode(f).value(u"fps"_s).toInt(), 0);
    QVERIFY(!filtersLog(f).contains(u"setpts"_s));
    QVERIFY(filtersLog(f).contains(u",fps=30"_s)); // the 60 fps source is halved, as before
}

void TestMurasakiService::transcodeRequestsAndState()
{
    PetFixture f;
    const auto states = [&] {
        return QJsonDocument::fromJson(f.service.TranscodeState(u"anima:9"_s).toUtf8()).object().value(u"variants"_s).toArray();
    };
    QCOMPARE(states().size(), 2);
    QCOMPARE(states().at(1).toObject().value(u"name"_s).toString(), u"small"_s);
    QCOMPARE(states().at(1).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString(), u"none"_s);

    f.service.RequestTranscode(u"anima:9"_s, u"small"_s);
    QVERIFY(QTest::qWaitFor([&] {
        return states().at(1).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString() == u"done"_s;
    }, 10000));
    QCOMPARE(states().at(0).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString(), u"none"_s);

    f.service.RequestTranscode(u"anima:9"_s, QString()); // every variant
    QVERIFY(QTest::qWaitFor([&] {
        return states().at(0).toObject().value(u"transcode"_s).toObject().value(u"state"_s).toString() == u"done"_s;
    }, 10000));

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RequestTranscode(u"anima:9"_s, u"nope"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RequestTranscode(u"we:1"_s, QString());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.TranscodeState(u"we:1"_s).isEmpty());
}

namespace {

class FakeUnit : public OverlayUnit
{
public:
    void start() override { calls << u"start"_s; }
    void stop() override { calls << u"stop"_s; }
    QStringList calls;
};

} // namespace

void TestMurasakiService::overlayUnitFollowsThePets()
{
    PetFixture f;
    FakeUnit unit;
    f.service.setOverlayUnit(&unit);
    QVERIFY2(unit.calls.isEmpty(), "no pets: nothing to start, and nothing to stop either");
    const QString first = f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    QCOMPARE(unit.calls, QStringList{u"start"_s});
    const QString second = f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    QCOMPARE(unit.calls, QStringList{u"start"_s}); // already asked: more pets and edits do not hammer the unit
    f.service.UpdatePet(second, u"{\"x\": 9}"_s);
    f.service.RemovePet(first);
    QCOMPARE(unit.calls, QStringList{u"start"_s}); // one pet left: still wanted
    // A start that failed is retried by a later change once the retry interval has passed.
    f.service.setOverlayRetryMs(0);
    f.service.UpdatePet(second, u"{\"x\": 10}"_s);
    QCOMPARE(unit.calls, (QStringList{u"start"_s, u"start"_s}));
    f.service.RemovePet(second);
    QCOMPARE(unit.calls.size(), 3);
    QCOMPARE(unit.calls.last(), u"stop"_s);
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    QCOMPARE(unit.calls.last(), u"start"_s);
    QCOMPARE(unit.calls.size(), 4);

    // A daemon restarted with pets on disk starts the overlay as soon as the unit is wired.
    PetFixture g;
    g.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    FakeUnit late;
    g.service.setOverlayUnit(&late);
    QCOMPARE(late.calls, QStringList{u"start"_s});
}

void TestMurasakiService::wallpaperInteractIsASessionState()
{
    PetFixture f;
    QSignalSpy interact(&f.service, &MurasakiService::WallpaperInteractChanged);
    QVERIFY(!f.service.wallpaperInteract()); // off at start

    f.service.SetWallpaperInteract(true);
    QVERIFY(f.service.wallpaperInteract());
    f.service.SetWallpaperInteract(true); // no change, no signal
    QCOMPARE(interact.count(), 1);
    QCOMPARE(interact.at(0).at(0).toBool(), true);
    f.service.SetWallpaperInteract(false);
    f.service.SetWallpaperInteract(false);
    QCOMPARE(interact.count(), 2);
    QCOMPARE(interact.at(1).at(0).toBool(), false);

    // The arrange mode wins: turning it on ends the interaction, and the interaction cannot start while it lasts.
    f.service.SetWallpaperInteract(true);
    f.service.SetPetsArrange(true);
    QVERIFY(!f.service.wallpaperInteract());
    QCOMPARE(interact.count(), 4);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.SetWallpaperInteract(true);
    QVERIFY(!f.service.wallpaperInteract());
    QCOMPARE(interact.count(), 4);
    f.service.SetWallpaperInteract(false); // off is always fine
    f.service.SetPetsArrange(false);
    f.service.SetWallpaperInteract(true);
    QVERIFY(f.service.wallpaperInteract());
    QCOMPARE(interact.count(), 5);
}

void TestMurasakiService::interactKeepsTheOverlayRunningWithoutPets()
{
    PetFixture f;
    FakeUnit unit;
    f.service.setOverlayUnit(&unit);
    QVERIFY(unit.calls.isEmpty());
    f.service.SetWallpaperInteract(true); // the catcher windows are the overlay's
    QCOMPARE(unit.calls, QStringList{u"start"_s});
    f.service.SetWallpaperInteract(false);
    QCOMPARE(unit.calls, (QStringList{u"start"_s, u"stop"_s}));
    // With a pet the overlay stays.
    f.service.AddPet(u"anima:9"_s, u"small"_s, u"*"_s, QString());
    f.service.setOverlayRetryMs(1'000'000);
    f.service.SetWallpaperInteract(true);
    f.service.SetWallpaperInteract(false);
    QCOMPARE(unit.calls.last(), u"start"_s);
    QVERIFY(!unit.calls.contains(u"stop"_s, Qt::CaseSensitive) || unit.calls.count(u"stop"_s) == 1);
}

void TestMurasakiService::interactEndsByItselfAfterTheCap()
{
    PetFixture f;
    f.service.setInteractCapMs(60);
    QSignalSpy interact(&f.service, &MurasakiService::WallpaperInteractChanged);
    f.service.SetWallpaperInteract(true);
    QVERIFY(f.service.wallpaperInteract());
    QTRY_VERIFY(!f.service.wallpaperInteract());
    QCOMPARE(interact.count(), 2);
    // Turned off by hand first: the timer does not fire a second signal later.
    f.service.SetWallpaperInteract(true);
    f.service.SetWallpaperInteract(false);
    QTest::qWait(150);
    QCOMPARE(interact.count(), 4);
}

void TestMurasakiService::arrangeModeAndPetsOverDBus()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");

    PetFixture f;
    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.pets"_s + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-pets"_s);
    });
    QVERIFY(f.service.registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-pets"_s);
    QVERIFY(client.isConnected());
    const QString iface = kDBusInterface;
    const auto call = [&](const QString &method, const QVariantList &args) {
        QDBusMessage message = QDBusMessage::createMethodCall(name, path, iface, method);
        message.setArguments(args);
        return callAndWait(client, message);
    };
    const auto arrange = [&] {
        QDBusMessage get = QDBusMessage::createMethodCall(name, path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get.setArguments({iface, u"PetsArrange"_s});
        return callAndWait(client, get).arguments().value(0).value<QDBusVariant>().variant().toBool();
    };

    // Arrange mode: a property, a setter and a signal that fires only on change.
    QSignalSpy local(&f.service, &MurasakiService::PetsArrangeChanged);
    QVERIFY(!arrange());
    QCOMPARE(call(u"SetPetsArrange"_s, {true}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(arrange());
    QCOMPARE(call(u"SetPetsArrange"_s, {true}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(local.count(), 1);
    QCOMPARE(local.at(0).at(0).toBool(), true);
    QCOMPARE(call(u"SetPetsArrange"_s, {false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!arrange());
    QCOMPARE(local.count(), 2);

    // Pet calls and their errors.
    const QDBusMessage added = call(u"AddPet"_s, {u"anima:9"_s, u"small"_s, u"DP-1"_s, u"{\"x\": 3}"_s});
    QCOMPARE(added.type(), QDBusMessage::ReplyMessage);
    const QString petId = added.arguments().at(0).toString();
    QVERIFY(!petId.isEmpty());
    const QDBusMessage list = call(u"ListPets"_s, {});
    QCOMPARE(QJsonDocument::fromJson(list.arguments().at(0).toString().toUtf8()).array().at(0).toObject().value(u"x"_s).toDouble(), 3.0);
    QCOMPARE(call(u"UpdatePet"_s, {petId, u"{\"y\": 9}"_s}).type(), QDBusMessage::ReplyMessage);
    const QDBusMessage bad = call(u"AddPet"_s, {u"we:1"_s, QString(), u"*"_s, QString()});
    QCOMPARE(bad.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(bad.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"RemovePet"_s, {u"nope"_s}).errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"RemovePet"_s, {petId}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(u"TranscodeState"_s, {u"anima:9"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(u"RequestTranscode"_s, {u"anima:9"_s, u"nope"_s}).errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"SetOutputScales"_s, {u"{\"DP-1\": 1.5}"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(u"SetOutputScales"_s, {u"{\"DP-1\": 40}"_s}).errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"SetOutputScales"_s, {u"[1]"_s}).errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
}

void TestMurasakiService::importedPetsAreAddedMarkedGeneratedAndDeletedOnRequest()
{
    PetFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    // A fake helper that "imports" by dropping a GIF and a record in a job folder, and reports it like the real one.
    const QString gif = f.tmp.filePath(u"fixture.gif"_s);
    testutil::writeFile(gif, animfixtures::gif(32, 24, {5, 5, 5}));
    const QString helper = f.tmp.filePath(u"bin/fake-petimport"_s);
    testutil::writeFile(helper, "#!/bin/sh\nR=$4\nmkdir -p \"$R/job1\"\ncp '" + gif.toUtf8() + "' \"$R/job1/pet.gif\"\necho '{}' > \"$R/job1/source.json\"\n"
                                "echo \"{\\\"done\\\":true,\\\"file\\\":\\\"$R/job1/pet.gif\\\",\\\"frames\\\":3}\"\n");
    QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    PetImportManager imports(root);
    imports.setHelper(helper, {}, false);
    f.service.attachImports(&imports);
    QSignalSpy changed(&f.service, &MurasakiService::ImportChanged);

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s)); // rejectCall for the bad spec, in-process
    QVERIFY(f.service.ImportPet(u"{}"_s).isEmpty());
    const QString jobId = f.service.ImportPet(u"{\"kind\":\"gif\",\"sources\":[\"/x.gif\"]}"_s);
    QVERIFY(!jobId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!imports.job(jobId).value(u"itemId"_s).toString().isEmpty(), 10000);
    QVERIFY(changed.count() >= 2);
    const QString itemId = imports.job(jobId).value(u"itemId"_s).toString();
    const QJsonObject item = f.item(itemId);
    QCOMPARE(item.value(u"generated"_s).toBool(), true);
    const QJsonArray state = QJsonDocument::fromJson(f.service.ImportState().toUtf8()).array();
    QCOMPARE(state.at(0).toObject().value(u"state"_s).toString(), u"done"_s);

    // A plain local pet is not generated.
    const QString plain = f.service.AddLocalPetItem(gif);
    QVERIFY(!f.item(plain).value(u"generated"_s).toBool());

    // Removing without deleteFiles keeps the folder; with it the job folder goes, and nothing else does.
    f.service.RemoveImportedPet(plain, true);
    QVERIFY(QFileInfo::exists(gif));
    f.service.RemoveImportedPet(itemId, false);
    QVERIFY(QFileInfo::exists(root + u"/job1/pet.gif"_s));
    QCOMPARE(f.service.AddLocalPetItem(root + u"/job1/pet.gif"_s), itemId);
    f.service.RemoveImportedPet(itemId, true);
    QVERIFY(!QFileInfo::exists(root + u"/job1"_s));
}

void TestMurasakiService::packageScenesStartOnPets()
{
    PetFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    // A package folder as the importer leaves it.
    const QString dir = root + u"/pack1"_s;
    testutil::writeFile(dir + u"/idle.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
    testutil::writeFile(dir + u"/jump.gif"_s, animfixtures::gif(40, 30, {5, 5, 5}));
    testutil::writeFile(dir + u"/source.json"_s,
                        R"j({"version":1,"spec":{"name":"Pack"},"mdaa":{"variants":[{"name":"idle","file":"idle.gif"},{"name":"jump","file":"jump.gif"}],
                        "autoScene":1,"scenes":[{"line":1,"show":true,"variant":"idle"},{"line":2,"show":true,"variant":"jump","x":300,"y":400},{"line":3,"show":false}]}})j");
    const QString itemId = f.service.AddLocalPetItem(dir);
    QVERIFY(itemId.startsWith(u"local:"_s));
    const QJsonObject item = f.item(itemId);
    QCOMPARE(item.value(u"title"_s).toString(), u"Pack"_s);
    QCOMPARE(item.value(u"variants"_s).toArray().size(), 2);
    QVERIFY(item.value(u"generated"_s).toBool());
    QCOMPARE(item.value(u"scenes"_s).toArray().size(), 3);
    QCOMPARE(item.value(u"autoScene"_s).toInt(), 1);

    // A new pet starts in the scene the package was imported with; naming a variant overrides it.
    const QString petId = f.service.AddPet(itemId, QString(), u"DP-1"_s, QString());
    QJsonObject pet = f.listed().at(0).toObject();
    QCOMPARE(pet.value(u"variant"_s).toString(), u"jump"_s);
    QCOMPARE(pet.value(u"x"_s).toDouble(), 300.0);
    QCOMPARE(pet.value(u"y"_s).toDouble(), 400.0);
    const QString plain = f.service.AddPet(itemId, u"idle"_s, u"DP-1"_s, QString());
    QVERIFY(!plain.isEmpty());

    f.service.StartScene(petId, 0);
    for (const QJsonValue &v : f.listed())
        if (v.toObject().value(u"petId"_s).toString() == petId)
            QCOMPARE(v.toObject().value(u"variant"_s).toString(), u"idle"_s);
    f.service.StartScene(petId, 1);
    for (const QJsonValue &v : f.listed())
        if (v.toObject().value(u"petId"_s).toString() == petId) {
            QCOMPARE(v.toObject().value(u"variant"_s).toString(), u"jump"_s);
            QCOMPARE(v.toObject().value(u"x"_s).toDouble(), 300.0);
        }
    for (const QString &why : {u"scene"_s, u"scene"_s, u"pet"_s}) {
        Q_UNUSED(why);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    }
    f.service.StartScene(petId, 2);   // hides the pet
    f.service.StartScene(petId, 9);   // no such scene
    f.service.StartScene(u"nope"_s, 0);

    // A package IS its folder: removing it with deleteFiles deletes the folder (and nothing outside the import root).
    f.service.RemovePet(petId);
    f.service.RemovePet(plain);
    f.service.RemoveImportedPet(itemId, true);
    QVERIFY(!QFileInfo::exists(dir));
}

void TestMurasakiService::workshopFilesAreImportedOnlyWhenTheSettingSaysAuto()
{
    PetFixture f;
    PetImportManager imports(f.tmp.filePath(u"data/imported"_s));
    const QString helper = f.tmp.filePath(u"bin/fake-petimport2"_s);
    testutil::writeFile(helper, "#!/bin/sh\necho '{\"error\":\"not an image\"}'\nexit 1\n");
    QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    imports.setHelper(helper, {}, false);
    f.service.attachImports(&imports);
    // An Anima item that carries a ZIP, a package and a video next to its animation.
    testutil::writeFile(f.tmp.filePath(u"anima/10/pet.apng"_s), animfixtures::apng(40, 30, {{1, 10}, {1, 10}}));
    testutil::writeFile(f.tmp.filePath(u"anima/10/frames.zip"_s), "PK not really a zip, but a name is all the listing looks at");
    testutil::writeFile(f.tmp.filePath(u"anima/10/pack/extra.mdaa"_s), "PK also not a zip");
    testutil::writeFile(f.tmp.filePath(u"anima/10/clip.mp4"_s), "video");
    // A file over the automatic limit (sparse: it costs nothing) stays for the user to import by hand.
    QFile huge(f.tmp.filePath(u"anima/10/huge.zip"_s));
    QVERIFY(huge.open(QIODevice::WriteOnly) && huge.resize(300LL << 20));
    huge.close();
    f.library.rescan();

    // Listed in the item JSON either way; nothing is imported while the settings say ask.
    const QJsonArray importable = f.item(u"anima:10"_s).value(u"importable"_s).toArray();
    QStringList kinds;
    for (const QJsonValue &v : importable)
        kinds.append(v.toObject().value(u"kind"_s).toString());
    kinds.sort();
    QCOMPARE(kinds, (QStringList{u"mdaa"_s, u"video"_s, u"zip"_s, u"zip"_s}));
    QTest::qWait(100);
    QVERIFY(imports.jobs().isEmpty());

    // Auto for files: the ZIP and the video (the package has its own setting), each once; the 300 MiB zip is left for the user.
    f.settings.merge({{u"importWorkshopFiles"_s, u"auto"_s}});
    QTRY_COMPARE_WITH_TIMEOUT(imports.jobs().size(), 2, 5000);
    QStringList kinds2;
    for (const QJsonValue &j : imports.jobs())
        kinds2.append(j.toObject().value(u"kind"_s).toString());
    kinds2.sort();
    QCOMPARE(kinds2, (QStringList{u"video"_s, u"zip"_s}));
    f.library.rescan();
    f.settings.merge({{u"importWorkshopMdaa"_s, u"auto"_s}});
    QTRY_COMPARE_WITH_TIMEOUT(imports.jobs().size(), 3, 5000);
    QCOMPARE(imports.jobs().at(2).toObject().value(u"kind"_s).toString(), u"mdaa"_s);
    f.library.rescan();
    QTest::qWait(200);
    QCOMPARE(imports.jobs().size(), 3);
    QVERIFY(QFileInfo::exists(f.tmp.filePath(u"data/auto-imported.json"_s)));
}

void TestMurasakiService::aPreviewOverDBusDoesNotBlockTheDaemon()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    PetFixture f;
    PetImportManager imports(f.tmp.filePath(u"data/imported"_s));
    // A helper that takes a second to answer a preview.
    const QString helper = f.tmp.filePath(u"bin/slow-preview"_s);
    testutil::writeFile(helper, "#!/bin/sh\nsleep 1\necho '{\"preview\":{\"frames\":3,\"width\":8,\"height\":8,\"totalDelayMs\":300,\"estimatedBytes\":1536,\"degraded\":[]}}'\n");
    QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    imports.setHelper(helper, {}, false);
    f.service.attachImports(&imports);
    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.pv"_s + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-pv"_s);
    });
    QVERIFY(f.service.registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-pv"_s);
    const QString iface = kDBusInterface;
    const QString spec = u"{\"kind\":\"gif\",\"sources\":[\"/a.gif\"]}"_s;
    const auto preview = [&](const QString &json) {
        QDBusMessage m = QDBusMessage::createMethodCall(name, path, iface, u"PreviewImport"_s);
        m.setArguments({json});
        return client.asyncCall(m);
    };
    QElapsedTimer clock;
    clock.start();
    QDBusPendingCall first = preview(spec), same = preview(spec), other = preview(u"{\"kind\":\"gif\",\"sources\":[\"/b.gif\"]}"_s);
    // While the helper sleeps the daemon still answers other calls at once.
    QDBusPendingCall state = client.asyncCall(QDBusMessage::createMethodCall(name, path, iface, u"ImportState"_s));
    QVERIFY(QTest::qWaitFor([&] { return state.isFinished(); }, 800));
    QVERIFY(!first.isFinished());
    QVERIFY(QTest::qWaitFor([&] { return first.isFinished() && same.isFinished() && other.isFinished(); }, 8000));
    const QJsonObject answer = QJsonDocument::fromJson(first.reply().arguments().value(0).toString().toUtf8()).object();
    QVERIFY(answer.value(u"ok"_s).toBool());
    QCOMPARE(answer.value(u"preview"_s).toObject().value(u"frames"_s).toInt(), 3);
    QCOMPARE(same.reply().arguments().value(0).toString(), first.reply().arguments().value(0).toString());
    QVERIFY(clock.elapsed() < 3500); // the same spec shared one helper run: two runs in turn, not three
    // A spec that is not valid is an error reply, not a hang.
    QDBusPendingCall bad = preview(u"{}"_s);
    QVERIFY(QTest::qWaitFor([&] { return bad.isFinished(); }, 3000));
    QVERIFY(bad.reply().type() == QDBusMessage::ErrorMessage);
}

void TestMurasakiService::aHelperThatNamesAFileOutsideTheImportFolderMakesTheJobFail()
{
    PetFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    const QString outside = f.tmp.filePath(u"elsewhere/pet.gif"_s);
    testutil::writeFile(outside, animfixtures::gif(32, 24, {5, 5, 5}));
    const QString helper = f.tmp.filePath(u"bin/liar"_s);
    testutil::writeFile(helper, "#!/bin/sh\necho '{\"done\":true,\"file\":\"" + outside.toUtf8() + "\",\"frames\":3}'\n");
    QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    imports.setHelper(helper, {}, false);
    f.service.attachImports(&imports);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"cannot be added"_s));
    const QString jobId = f.service.ImportPet(u"{\"kind\":\"gif\",\"sources\":[\"/x.gif\"]}"_s);
    QTRY_COMPARE_WITH_TIMEOUT(imports.job(jobId).value(u"state"_s).toString(), u"failed"_s, 8000);
    QVERIFY(imports.job(jobId).value(u"error"_s).toString().contains(u"cannot be added"_s));
    QVERIFY(f.localPets.files().isEmpty());
    // Cancelling a job that has ended is not an error.
    f.service.CancelImport(jobId);
}

void TestMurasakiService::localPetsAreAddedListedAndRemoved()
{
    PetFixture f;
    const QString gif = f.tmp.filePath(u"mine/Dancer.GIF"_s);
    testutil::writeFile(gif, animfixtures::gif(32, 24, {5, 5, 5}));
    QSignalSpy itemsChanged(&f.library, &Library::itemsChanged);

    const QString id = f.service.AddLocalPetItem(gif);
    QVERIFY(id.startsWith(u"local:"_s));
    QCOMPARE(id, LocalPetImporter::itemIdFor(testutil::canonical(gif)));
    QCOMPARE(itemsChanged.count(), 1);
    QCOMPARE(itemsChanged.at(0).at(0).toStringList(), QStringList{id});

    // A normal pet item, the file itself being the only variant. Adding it again changes nothing.
    const QJsonObject item = f.item(id);
    QCOMPARE(item.value(u"kind"_s).toString(), u"pet"_s);
    QCOMPARE(item.value(u"source"_s).toString(), u"local"_s);
    QCOMPARE(item.value(u"title"_s).toString(), u"Dancer"_s);
    const QJsonArray variants = item.value(u"variants"_s).toArray();
    QCOMPARE(variants.size(), 1);
    QCOMPARE(variants.at(0).toObject().value(u"width"_s).toInt(), 32);
    QCOMPARE(variants.at(0).toObject().value(u"file"_s).toString(), testutil::canonical(gif));
    QVERIFY(variants.at(0).toObject().contains(u"transcode"_s));
    QCOMPARE(f.service.AddLocalPetItem(gif), id);
    QCOMPARE(itemsChanged.count(), 1);

    // It can be shown, like any pet, and it is remembered across a restart.
    const QString petId = f.service.AddPet(id, QString(), u"*"_s, QString());
    QVERIFY(!petId.isEmpty());
    LocalPetStore reread(f.tmp.filePath(u"config/local-pets.json"_s));
    reread.load();
    QCOMPARE(reread.files(), QStringList{testutil::canonical(gif)});

    // Not while a pet shows it; afterwards the item is gone.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemoveLocalPetItem(id);
    QVERIFY(f.library.item(id));
    f.service.RemovePet(petId);
    f.service.RemoveLocalPetItem(id);
    QVERIFY(!f.library.item(id));
    QCOMPARE(itemsChanged.count(), 2);
    QCOMPARE(itemsChanged.at(1).at(2).toStringList(), QStringList{id});
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemoveLocalPetItem(id);
}

void TestMurasakiService::localPetsRejectBadFiles()
{
    PetFixture f;
    testutil::writeFile(f.tmp.filePath(u"mine/notes.txt"_s), "hello");
    testutil::writeFile(f.tmp.filePath(u"mine/broken.gif"_s), "GIF89a not really");
    for (const QString &path : {u"relative.gif"_s, f.tmp.filePath(u"mine/missing.gif"_s), f.tmp.filePath(u"mine/notes.txt"_s),
                                f.tmp.filePath(u"mine/broken.gif"_s), f.tmp.filePath(u"mine"_s)}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        QVERIFY2(f.service.AddLocalPetItem(path).isEmpty(), qPrintable(path));
    }
    QVERIFY(f.localPets.files().isEmpty());

    // Without the store attached the call fails too.
    QTemporaryDir tmp;
    Library library;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(service.AddLocalPetItem(f.tmp.filePath(u"mine/missing.gif"_s)).isEmpty());
}

void TestMurasakiService::puppetsAreAddedListedShownAndRemoved()
{
    PetFixture f;
    const QString folder = f.tmp.filePath(u"puppets/haru"_s);
    testutil::writeFile(folder + u"/model.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    QSignalSpy itemsChanged(&f.library, &Library::itemsChanged);

    const QString id = f.service.AddLocalPuppetItem(folder);
    QCOMPARE(id, LocalPuppetImporter::itemIdFor(testutil::canonical(folder)));
    QVERIFY(id.startsWith(u"local:puppet:"_s));
    QCOMPARE(itemsChanged.count(), 1);
    QCOMPARE(itemsChanged.at(0).at(0).toStringList(), QStringList{id});

    // a pet item that says it is a puppet; its one base variant is the model, which is not an animation (no frames)
    const QJsonObject item = f.item(id);
    QCOMPARE(item.value(u"kind"_s).toString(), u"pet"_s);
    QCOMPARE(item.value(u"format"_s).toString(), u"puppet"_s);
    QCOMPARE(item.value(u"source"_s).toString(), u"local"_s);
    QCOMPARE(item.value(u"status"_s).toString(), u"ok"_s);
    QCOMPARE(item.value(u"puppet"_s).toObject().value(u"backend"_s).toString(), u"open"_s);
    const QJsonArray variants = item.value(u"variants"_s).toArray();
    QCOMPARE(variants.size(), 1);
    QCOMPARE(variants.at(0).toObject().value(u"name"_s).toString(), u"model"_s);
    QCOMPARE(variants.at(0).toObject().value(u"frames"_s).toInt(), 0);
    QCOMPARE(f.service.AddLocalPuppetItem(folder), id); // again: the same id, nothing new
    QCOMPARE(itemsChanged.count(), 1);
    // a puppet is not a local pet file, and a pet file's id does not remove it
    QCOMPARE(f.localPets.files().size(), 0);

    // it can be shown like any pet, and nothing is converted for it
    const QString petId = f.service.AddPet(id, QString(), u"*"_s, QString());
    QVERIFY(!petId.isEmpty());
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(); // (a conversion would have been queued by now)
    QVERIFY(f.transcodes.statuses(TranscodeSource{id, u"model"_s, testutil::canonical(folder) + u"/model.inp"_s, 60, 0, 256, 0}).isEmpty());
    QCOMPARE(f.item(id).value(u"variants"_s).toArray().at(0).toObject().contains(u"transcode"_s), false);

    // remembered across a restart
    LocalPetStore reread(f.tmp.filePath(u"config/local-puppets.json"_s));
    reread.setItemIdFunction(&LocalPuppetImporter::itemIdFor);
    reread.load();
    QCOMPARE(reread.files(), QStringList{testutil::canonical(folder)});

    // not while a pet shows it; afterwards it is gone (the folder is left alone)
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemoveLocalPuppetItem(id);
    QVERIFY(f.library.item(id));
    f.service.RemovePet(petId);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemoveLocalPetItem(id); // (the pet call does not know a puppet)
    QVERIFY(f.library.item(id));
    f.service.RemoveLocalPuppetItem(id);
    QVERIFY(!f.library.item(id));
    QVERIFY(QFileInfo::exists(folder + u"/model.inp"_s));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.RemoveLocalPuppetItem(id);
}

void TestMurasakiService::aCoreFileIsCheckedBeforeItIsStored()
{
    PetFixture f;
    const QString good = f.tmp.filePath(u"core/libCubismCore.so"_s);
    testutil::writeFile(good, "not a library; it is only named");
    QFile::setPermissions(good, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    const QString unsafe = f.tmp.filePath(u"core/writable.so"_s);
    testutil::writeFile(unsafe, "x");
    QFile::setPermissions(unsafe, QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther);
    for (const QString &bad : {f.tmp.filePath(u"core/missing.so"_s), unsafe, f.tmp.filePath(u"core"_s)}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.SetSettings(QString::fromUtf8(QJsonDocument(QJsonObject{{u"puppetCorePath"_s, bad}}).toJson()));
        QCOMPARE(f.settings.puppetSettings().corePath, QString());
    }
    f.service.SetSettings(QString::fromUtf8(QJsonDocument(QJsonObject{{u"puppetCorePath"_s, good}}).toJson()));
    QCOMPARE(f.settings.puppetSettings().corePath, good);
    f.service.SetSettings(QString::fromUtf8(QJsonDocument(QJsonObject{{u"puppetCorePath"_s, QString()}}).toJson()));
    QCOMPARE(f.settings.puppetSettings().corePath, QString());
}

void TestMurasakiService::puppetsRejectBadFolders()
{
    PetFixture f;
    testutil::writeFile(f.tmp.filePath(u"puppets/file.inp"_s), "TRNSRTS2");
    QVERIFY(QDir().mkpath(f.tmp.filePath(u"puppets/empty"_s)));
    testutil::writeFile(f.tmp.filePath(u"puppets/linked/model.inp"_s), QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    QVERIFY(QFile::link(u"/etc/passwd"_s, f.tmp.filePath(u"puppets/linked/passwd.png"_s)));
    testutil::writeFile(f.tmp.filePath(u"puppets/two/a.inp"_s), QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    testutil::writeFile(f.tmp.filePath(u"puppets/two/b.inp"_s), QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    for (const QString &path : {u"relative"_s, QString(), f.tmp.filePath(u"puppets/missing"_s), f.tmp.filePath(u"puppets/file.inp"_s), f.tmp.filePath(u"puppets/empty"_s),
                                f.tmp.filePath(u"puppets/linked"_s), f.tmp.filePath(u"puppets/two"_s)}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        QVERIFY2(f.service.AddLocalPuppetItem(path).isEmpty(), qPrintable(path));
    }
    QVERIFY(f.localPuppets.files().isEmpty());

    // without the store attached the calls fail too
    QTemporaryDir tmp;
    Library library;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(service.AddLocalPuppetItem(f.tmp.filePath(u"puppets/empty"_s)).isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    service.RemoveLocalPuppetItem(u"local:puppet:abc"_s);
}

namespace {

// Three videos, one pet, and the playlist machinery with a fake clock.
struct PlaylistFixture {
    QTemporaryDir tmp;
    qint64 now = 2'000'000'000;
    Library library;
    AssignmentStore assignments{tmp.filePath(u"assignments.json"_s)};
    SettingsStore settings{tmp.filePath(u"settings.json"_s)};
    PlaylistStore playlists{tmp.filePath(u"playlists.json"_s)};
    std::unique_ptr<PlaylistManager> manager;
    std::unique_ptr<MurasakiService> service;

    PlaylistFixture()
    {
        for (const QString &id : {u"1"_s, u"2"_s, u"3"_s})
            testutil::writeVideoItem(tmp.filePath(u"we"_s), id, u"Item "_s + id);
        testutil::writeFile(tmp.filePath(u"anima/9/pet.apng"_s), animfixtures::apng(40, 30, {{1, 60}, {1, 60}}));
        library.setWorkshopDirs({tmp.filePath(u"we"_s)});
        library.setAnimaDirs({tmp.filePath(u"anima"_s)});
        library.rescan();
        playlists.load();
        manager = std::make_unique<PlaylistManager>(&playlists, &assignments, &library,
                                                    tmp.filePath(u"state/playlist-state.json"_s));
        manager->setClock([this] { return now; });
        manager->setSeed(1);
        manager->load();
        service = std::make_unique<MurasakiService>(&library, &assignments, &settings, u"ok"_s, QString());
        service->attachPlaylists(&playlists, manager.get());
    }
    QJsonObject resolve(const QString &output = u"DP-1"_s, const QString &desktop = u"1"_s) const
    {
        return QJsonDocument::fromJson(service->ResolveAssignment(output, desktop).toUtf8()).object();
    }
    QString create(const QString &name, const QStringList &items = {})
    {
        const QString id = service->CreatePlaylist(name);
        if (!items.isEmpty())
            service->AddToPlaylist(id, items);
        return id;
    }
    static QString shown(const QJsonObject &reply)
    {
        return reply.value(u"item"_s).toObject().value(u"id"_s).toString();
    }
};

} // namespace

void TestMurasakiService::playlistsAreCreatedEditedAndDeleted()
{
    PlaylistFixture f;
    QSignalSpy changed(f.service.get(), &MurasakiService::PlaylistsChanged);
    const QString id = f.service->CreatePlaylist(u"Calm"_s);
    QVERIFY(!id.isEmpty());
    QCOMPARE(changed.count(), 1);

    f.service->AddToPlaylist(id, {u"we:1"_s, u"we:2"_s, u"we:3"_s});
    f.service->RemoveFromPlaylist(id, {u"we:2"_s, u"we:404"_s});
    f.service->UpdatePlaylist(id, u"{\"name\":\"Calmer\",\"order\":\"shuffle\",\"intervalMinutes\":45,\"changeOnLogin\":true}"_s);
    f.service->UpdatePlaylist(id, u"{\"items\":[\"we:3\",\"we:1\"]}"_s); // reorder
    QJsonArray all = QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array();
    QCOMPARE(all.size(), 1);
    const QJsonObject p = all.at(0).toObject();
    QCOMPARE(p.value(u"id"_s).toString(), id);
    QCOMPARE(p.value(u"name"_s).toString(), u"Calmer"_s);
    QCOMPARE(p.value(u"items"_s).toArray(), (QJsonArray{u"we:3"_s, u"we:1"_s}));
    QCOMPARE(p.value(u"order"_s).toString(), u"shuffle"_s);
    QCOMPARE(p.value(u"intervalMinutes"_s).toInt(), 45);
    QVERIFY(p.value(u"changeOnLogin"_s).toBool());
    const int before = changed.count();
    f.service->UpdatePlaylist(id, u"{\"name\":\"Calmer\"}"_s); // no change
    QCOMPARE(changed.count(), before);

    f.service->DeletePlaylist(id);
    QCOMPARE(changed.count(), before + 1);
    QVERIFY(QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array().isEmpty());
}

void TestMurasakiService::aPlaylistCanBeAssignedAndResolves()
{
    PlaylistFixture f;
    const QString id = f.create(u"Mix"_s, {u"we:2"_s, u"we:1"_s});
    f.service->UpdatePlaylist(id, u"{\"intervalMinutes\":20}"_s);
    f.service->SetAssignment(u"*"_s, u"*"_s, playlistItemId(id), u"{\"fill\":\"fit\",\"muted\":true}"_s);

    // The stored assignment keeps the playlist, the resolved one names the item that is showing.
    const QJsonArray stored = QJsonDocument::fromJson(f.service->Assignments().toUtf8()).array();
    QCOMPARE(stored.at(0).toObject().value(u"itemId"_s).toString(), playlistItemId(id));
    const QJsonObject reply = f.resolve();
    QCOMPARE(PlaylistFixture::shown(reply), u"we:2"_s);
    QVERIFY(reply.value(u"item"_s).toObject().contains(u"properties"_s));
    const QJsonObject assignment = reply.value(u"assignment"_s).toObject();
    QCOMPARE(assignment.value(u"itemId"_s).toString(), u"we:2"_s);
    QCOMPARE(assignment.value(u"fill"_s).toString(), u"fit"_s);
    QVERIFY(assignment.value(u"muted"_s).toBool());
    const QJsonObject playlist = reply.value(u"playlist"_s).toObject();
    QCOMPARE(playlist.value(u"id"_s).toString(), id);
    QCOMPARE(playlist.value(u"index"_s).toInt(), 0);
    QCOMPARE(qint64(playlist.value(u"nextAt"_s).toDouble()), f.now + 20 * 60 * 1000);

    // Rotation changes the resolved item, and so the content key the surface compares.
    f.now += 20 * 60 * 1000;
    f.manager->checkDue();
    const QJsonObject later = f.resolve();
    QCOMPARE(PlaylistFixture::shown(later), u"we:1"_s);
    QCOMPARE(later.value(u"playlist"_s).toObject().value(u"index"_s).toInt(), 1);

    // An assignment to a plain item has no playlist key.
    f.service->SetAssignment(u"DP-2"_s, u"*"_s, u"we:3"_s, QString());
    QVERIFY(!f.resolve(u"DP-2"_s).contains(u"playlist"_s));

    // An emptied playlist resolves to nothing but keeps the assignment.
    f.service->UpdatePlaylist(id, u"{\"items\":[]}"_s);
    const QJsonObject empty = f.resolve();
    QVERIFY(empty.value(u"item"_s).isNull());
    QCOMPARE(empty.value(u"assignment"_s).toObject().value(u"itemId"_s).toString(), playlistItemId(id));
    QCOMPARE(empty.value(u"playlist"_s).toObject().value(u"index"_s).toInt(), -1);
}

void TestMurasakiService::playlistsRejectBadInput()
{
    PlaylistFixture f;
    const QString id = f.create(u"Mix"_s, {u"we:1"_s});
    const auto items = [&] {
        return QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array().at(0).toObject()
            .value(u"items"_s).toArray();
    };

    f.service->AddToPlaylist(id, {u"anima:9"_s}); // pets are not wallpapers
    f.service->AddToPlaylist(id, {u"we:404"_s});
    f.service->AddToPlaylist(id, {u"playlist:x"_s});
    f.service->AddToPlaylist(u"nope"_s, {u"we:2"_s});
    QStringList many;
    for (int i = 0; i <= kMaxPlaylistItems; ++i)
        many.append(u"we:%1"_s.arg(i));
    f.service->AddToPlaylist(id, many); // more ids than a playlist can hold: refused before any lookup
    f.service->RemoveFromPlaylist(id, many);
    f.service->UpdatePlaylist(id, u"{\"items\":[\"we:1\",\"anima:9\"]}"_s);
    f.service->UpdatePlaylist(id, u"{\"intervalMinutes\":0}"_s);
    f.service->UpdatePlaylist(id, u"{\"order\":\"random\"}"_s);
    f.service->UpdatePlaylist(id, u"not json"_s);
    f.service->UpdatePlaylist(u"nope"_s, u"{}"_s);
    f.service->RemoveFromPlaylist(u"nope"_s, {u"we:1"_s});
    f.service->DeletePlaylist(u"nope"_s);
    QCOMPARE(items(), (QJsonArray{u"we:1"_s}));
    QCOMPARE(f.service->CreatePlaylist(u"   "_s), QString());
    QCOMPARE(QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array().size(), 1);

    // The existing items of a playlist may vanish from the library; only new ones are checked.
    f.service->AddToPlaylist(id, {u"we:2"_s});
    QDir(f.tmp.filePath(u"we/2"_s)).removeRecursively();
    f.library.rescan();
    f.service->UpdatePlaylist(id, u"{\"items\":[\"we:2\",\"we:1\"]}"_s);
    QCOMPARE(items(), (QJsonArray{u"we:2"_s, u"we:1"_s}));

    // Assignments to playlists
    f.service->SetAssignment(u"*"_s, u"*"_s, u"playlist:nope"_s, QString());
    QVERIFY(f.assignments.assignments().isEmpty());
    f.service->SetAssignment(u"*"_s, u"*"_s, playlistItemId(id), QString());
    QCOMPARE(f.assignments.assignments().size(), 1);
    f.service->DeletePlaylist(id); // still assigned
    QCOMPARE(QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array().size(), 1);
    f.service->ClearAssignment(u"*"_s, u"*"_s);
    f.service->DeletePlaylist(id);
    QVERIFY(QJsonDocument::fromJson(f.service->ListPlaylists().toUtf8()).array().isEmpty());
}

void TestMurasakiService::nextAndPreviousRotateThePlaylist()
{
    PlaylistFixture f;
    const QString id = f.create(u"Mix"_s, {u"we:1"_s, u"we:2"_s, u"we:3"_s});
    f.service->SetAssignment(u"DP-1"_s, u"*"_s, playlistItemId(id), QString());
    QCOMPARE(PlaylistFixture::shown(f.resolve(u"DP-1"_s)), u"we:1"_s);
    f.service->NextInPlaylist(u"DP-1"_s, u"3"_s);
    QCOMPARE(PlaylistFixture::shown(f.resolve(u"DP-1"_s)), u"we:2"_s);
    f.service->PreviousInPlaylist(u"DP-1"_s, u"3"_s);
    f.service->PreviousInPlaylist(u"DP-1"_s, u"3"_s);
    QCOMPARE(PlaylistFixture::shown(f.resolve(u"DP-1"_s)), u"we:3"_s);

    // not a playlist: nothing moves
    f.service->NextInPlaylist(u"DP-9"_s, u"1"_s);
    f.service->SetAssignment(u"DP-2"_s, u"*"_s, u"we:1"_s, QString());
    f.service->NextInPlaylist(u"DP-2"_s, u"1"_s);
    QCOMPARE(PlaylistFixture::shown(f.resolve(u"DP-2"_s)), u"we:1"_s);
    QCOMPARE(PlaylistFixture::shown(f.resolve(u"DP-1"_s)), u"we:3"_s);
}

void TestMurasakiService::rotationAndEditsAnnounceThemselves()
{
    PlaylistFixture f;
    const QString id = f.create(u"Mix"_s, {u"we:1"_s, u"we:2"_s});
    f.service->SetAssignment(u"*"_s, u"*"_s, playlistItemId(id), QString());
    QSignalSpy assignmentsChanged(f.service.get(), &MurasakiService::AssignmentsChanged);

    f.now += 15 * 60 * 1000;
    f.manager->checkDue();
    QCOMPARE(assignmentsChanged.count(), 1);
    f.service->NextInPlaylist(u"*"_s, u"*"_s);
    QCOMPARE(assignmentsChanged.count(), 2);
    // removing the item that is showing moves on, and says so
    f.service->RemoveFromPlaylist(id, {u"we:1"_s});
    QCOMPARE(PlaylistFixture::shown(f.resolve()), u"we:2"_s);
    QVERIFY(assignmentsChanged.count() >= 3);
    // a changed item that is showing is reloaded by the surface
    const int count = assignmentsChanged.count();
    testutil::writeProject(f.tmp.filePath(u"we/2"_s), QJsonObject{{"type", "video"}, {"file", "clip.mp4"}, {"title", "Renamed"}});
    f.library.rescan();
    QVERIFY(assignmentsChanged.count() > count);
}

void TestMurasakiService::playlistsWithoutAttachFail()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    QCOMPARE(service.CreatePlaylist(u"x"_s), QString());
    QCOMPARE(service.ListPlaylists(), u"[]"_s);
    service.SetAssignment(u"*"_s, u"*"_s, u"playlist:x"_s, QString());
    QVERIFY(store.assignments().isEmpty());
    service.NextInPlaylist(u"*"_s, u"*"_s); // only warns in process
}

void TestMurasakiService::playlistsOverDBus()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    PlaylistFixture f;
    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.pl"_s
        + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-pl"_s);
    });
    QVERIFY(f.service->registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-pl"_s);
    QVERIFY(client.isConnected());
    const QString iface = kDBusInterface;
    const auto call = [&](const QString &method, const QVariantList &args) {
        QDBusMessage message = QDBusMessage::createMethodCall(name, path, iface, method);
        message.setArguments(args);
        return callAndWait(client, message);
    };

    const QDBusMessage created = call(u"CreatePlaylist"_s, {u"Over the bus"_s});
    QCOMPARE(created.type(), QDBusMessage::ReplyMessage);
    const QString id = created.arguments().at(0).toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(call(u"AddToPlaylist"_s, {id, QStringList{u"we:1"_s, u"we:2"_s}}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(u"SetAssignment"_s, {u"*"_s, u"*"_s, playlistItemId(id), QString()}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(u"NextInPlaylist"_s, {u"DP-1"_s, u"1"_s}).type(), QDBusMessage::ReplyMessage);
    const QDBusMessage resolved = call(u"ResolveAssignment"_s, {u"DP-1"_s, u"1"_s});
    const QJsonObject reply = QJsonDocument::fromJson(resolved.arguments().at(0).toString().toUtf8()).object();
    QCOMPARE(PlaylistFixture::shown(reply), u"we:2"_s);
    QCOMPARE(reply.value(u"playlist"_s).toObject().value(u"index"_s).toInt(), 1);

    const QDBusMessage notAPlaylist = call(u"UpdatePlaylist"_s, {u"nope"_s, u"{}"_s});
    QCOMPARE(notAPlaylist.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(notAPlaylist.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"ClearAssignment"_s, {u"*"_s, u"*"_s}).type(), QDBusMessage::ReplyMessage);
    const QDBusMessage afterClear = call(u"NextInPlaylist"_s, {u"DP-1"_s, u"1"_s});
    QCOMPARE(afterClear.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(afterClear.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
}

void TestMurasakiService::itemsReportPermissionsAndDefaultToNoNetwork()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    ItemPermissionStore permissions(tmp.filePath(u"permissions.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.attachPermissions(&permissions);
    QVERIFY(kDBusApiVersion >= 8);

    const auto network = [](const QJsonObject &item) {
        return item.value(u"permissions"_s).toObject().value(u"network"_s);
    };
    QCOMPARE(network(QJsonDocument::fromJson(service.GetItem(u"we:1"_s).toUtf8()).object()), QJsonValue(false));
    QCOMPARE(network(QJsonDocument::fromJson(service.ListItems().toUtf8()).array().at(0).toObject()), QJsonValue(false));

    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, QString());
    service.SetItemPermission(u"we:1"_s, u"network"_s, true);
    QCOMPARE(network(QJsonDocument::fromJson(service.GetItem(u"we:1"_s).toUtf8()).object()), QJsonValue(true));
    QCOMPARE(network(QJsonDocument::fromJson(service.ListItems().toUtf8()).array().at(0).toObject()), QJsonValue(true));
    const QJsonObject resolved = QJsonDocument::fromJson(service.ResolveAssignment(u"*"_s, u"*"_s).toUtf8()).object();
    QCOMPARE(network(resolved.value(u"item"_s).toObject()), QJsonValue(true));
}

void TestMurasakiService::setItemPermissionGrantsAndRevokes()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    const QString path = tmp.filePath(u"permissions.json"_s);
    ItemPermissionStore permissions(path);
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.attachPermissions(&permissions);
    QSignalSpy items(&service, &MurasakiService::ItemsChanged);

    service.SetItemPermission(u"we:1"_s, u"network"_s, true);
    QVERIFY(permissions.network(u"we:1"_s));
    QCOMPARE(items.count(), 1);
    QCOMPARE(items.last().at(1).toStringList(), QStringList{u"we:1"_s});
    service.SetItemPermission(u"we:1"_s, u"network"_s, true); // same state: no signal
    QCOMPARE(items.count(), 1);
    service.SetItemPermission(u"we:1"_s, u"network"_s, false);
    QVERIFY(!permissions.network(u"we:1"_s));
    QCOMPARE(items.count(), 2);
}

void TestMurasakiService::setItemPermissionRejectsBadInput()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    ItemPermissionStore permissions(tmp.filePath(u"permissions.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.attachPermissions(&permissions);
    service.SetItemPermission(u"we:missing"_s, u"network"_s, true); // unknown item
    service.SetItemPermission(u"we:1"_s, u"camera"_s, true); // unknown permission
    service.SetItemPermission(QString(), u"network"_s, true);
    QVERIFY(permissions.networkItems().isEmpty());
}

void TestMurasakiService::setItemPermissionWithoutAttachFails()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.SetItemPermission(u"we:1"_s, u"network"_s, true); // only warns in process
    const QJsonObject item = QJsonDocument::fromJson(service.GetItem(u"we:1"_s).toUtf8()).object();
    QCOMPARE(item.value(u"permissions"_s).toObject().value(u"network"_s), QJsonValue(false));
}

void TestMurasakiService::permissionChangeReloadsAssignedItem()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Beta"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    ItemPermissionStore permissions(tmp.filePath(u"permissions.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    service.attachPermissions(&permissions);
    service.SetAssignment(u"*"_s, u"*"_s, u"we:1"_s, QString());
    QSignalSpy spy(&service, &MurasakiService::AssignmentsChanged);
    service.SetItemPermission(u"we:2"_s, u"network"_s, true); // not on screen
    QCOMPARE(spy.count(), 0);
    service.SetItemPermission(u"we:1"_s, u"network"_s, true); // the surface must reload the page
    QCOMPARE(spy.count(), 1);
}

namespace {

// A service with a SteamCMD manager that runs the fake script.
struct SteamCmdFixture {
    QTemporaryDir tmp;
    Library library;
    AssignmentStore assignments{tmp.filePath(u"assignments.json"_s)};
    SettingsStore settings{tmp.filePath(u"settings.json"_s)};
    std::unique_ptr<SteamCmdManager> manager;
    std::unique_ptr<MurasakiService> service;

    SteamCmdFixture()
    {
        qputenv("FAKE_STEAMCMD_DIR", tmp.filePath(u"fake"_s).toUtf8());
        QDir().mkpath(tmp.filePath(u"fake"_s));
        for (const char *name : {"FAKE_GUARD", "FAKE_MOBILE", "FAKE_PASSWORD", "FAKE_FAIL_ITEM", "FAKE_SLOW", "FAKE_EXPIRED"})
            qunsetenv(name);
        manager = std::make_unique<SteamCmdManager>(tmp.filePath(u"install"_s), tmp.filePath(u"steamcmd.json"_s));
        manager->setProgram(fakesteamcmd::write(tmp.path()));
        service = std::make_unique<MurasakiService>(&library, &assignments, &settings, u"ok"_s, QString());
        service->attachSteamCmd(manager.get());
    }
    QJsonObject status() const { return QJsonDocument::fromJson(service->SteamCmdStatus().toUtf8()).object(); }
};

} // namespace

void TestMurasakiService::steamCmdWithoutAManagerReportsUnavailable()
{
    QTemporaryDir tmp;
    Library library;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    MurasakiService service(&library, &store, &settings, u"ok"_s, {});
    const QJsonObject status = QJsonDocument::fromJson(service.SteamCmdStatus().toUtf8()).object();
    QCOMPARE(status.value(u"state"_s).toString(), u"unavailable"_s);
    QVERIFY(!status.value(u"available"_s).toBool());
    QVERIFY(status.value(u"jobs"_s).toArray().isEmpty());
    service.SteamCmdLogin(u"bob"_s); // only warns in process
    service.SteamCmdDownload(u"431960"_s, u"1"_s);
    service.SteamCmdCancel(QString());
    service.SteamCmdLogout();
}

void TestMurasakiService::steamCmdRejectsBadInput()
{
    SteamCmdFixture f;
    f.service->SteamCmdLogin(u"+quit"_s);
    f.service->SteamCmdLogin(QString());
    QCOMPARE(f.status().value(u"state"_s).toString(), u"logged-out"_s);
    f.service->SteamCmdDownload(u"431960"_s, u"1"_s); // signed out
    f.service->SteamCmdSubmit(u"password"_s, u"x"_s);   // nothing asked
    f.service->SteamCmdSubmit(u"bogus"_s, u"x"_s);
    QVERIFY(f.status().value(u"jobs"_s).toArray().isEmpty());
}

void TestMurasakiService::steamCmdOverDBus()
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    SteamCmdFixture f;
    const QString name = u"io.github.uriel2121ger_art.MurasakiTest.sc"_s + QString::number(QCoreApplication::applicationPid());
    const QString path = kDBusPath;
    const auto cleanup = qScopeGuard([&] {
        server.unregisterService(name);
        server.unregisterObject(path);
        QDBusConnection::disconnectFromBus(u"murasaki-test-client-sc"_s);
    });
    QVERIFY(f.service->registerObject(server));
    QVERIFY(server.registerService(name));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-client-sc"_s);
    QVERIFY(client.isConnected());
    const QString iface = kDBusInterface;
    const auto call = [&](const QString &method, const QVariantList &args = {}) {
        QDBusMessage message = QDBusMessage::createMethodCall(name, path, iface, method);
        message.setArguments(args);
        return callAndWait(client, message);
    };
    const auto status = [&] {
        return QJsonDocument::fromJson(call(u"SteamCmdStatus"_s).arguments().value(0).toString().toUtf8()).object();
    };

    QCOMPARE(status().value(u"state"_s).toString(), u"logged-out"_s);
    QCOMPARE(call(u"SteamCmdLogin"_s, {u"bob"_s}).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE_WITH_TIMEOUT(status().value(u"prompt"_s).toString(), u"password"_s, 10000);
    // a wrong kind is an InvalidArgs error and changes nothing
    const QDBusMessage wrong = call(u"SteamCmdSubmit"_s, {u"guard"_s, u"1"_s});
    QCOMPARE(wrong.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(wrong.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"SteamCmdSubmit"_s, {u"password"_s, u"secret"_s}).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE_WITH_TIMEOUT(status().value(u"state"_s).toString(), u"logged-in"_s, 10000);
    QCOMPARE(status().value(u"user"_s).toString(), u"bob"_s);
    QVERIFY(!QJsonDocument(status()).toJson().contains("secret"));

    QCOMPARE(call(u"SteamCmdDownload"_s, {u"431960"_s, u"77"_s}).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE_WITH_TIMEOUT(status().value(u"jobs"_s).toArray().at(0).toObject().value(u"state"_s).toString(), u"done"_s, 10000);
    const QDBusMessage bad = call(u"SteamCmdDownload"_s, {u"431960"_s, u"x y"_s});
    QCOMPARE(bad.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(bad.errorName(), u"org.freedesktop.DBus.Error.InvalidArgs"_s);
    QCOMPARE(call(u"SteamCmdLogout"_s).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(status().value(u"state"_s).toString(), u"logged-out"_s);
}

namespace {

// The pet fixture with the automation attached.
struct AutomationFixture : PetFixture {
    ProfileStore profiles{tmp.filePath(u"config/profiles.json"_s)};
    TriggerStore triggers{tmp.filePath(u"config/triggers.json"_s)};
    TimelineStore timelines{tmp.filePath(u"config/timelines.json"_s)};
    AutomationFixture()
    {
        settings.load();
        profiles.load();
        triggers.load();
        timelines.load();
        service.attachAutomation(&profiles, &triggers, &timelines, tmp.filePath(u"state/automation.json"_s));
    }
    QJsonObject runtime() const { return QJsonDocument::fromJson(service.petRuntime().toUtf8()).object(); }
};

} // namespace

void TestMurasakiService::profilesSaveAndSwitchInOneWrite()
{
    AutomationFixture f;
    const QString a = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, QString());
    const QString b = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    const QString id = f.service.SaveProfile(u"Work"_s);
    QVERIFY(!id.isEmpty());
    QCOMPARE(f.service.activeProfile(), id);
    QVERIFY(!f.service.profileModified());
    const QJsonArray listed = QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array();
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.at(0).toObject().value(u"pets"_s).toArray().size(), 2);

    f.service.UpdatePet(a, u"{\"x\": 77}"_s);
    f.service.RemovePet(b);
    QVERIFY(f.service.profileModified());
    QSignalSpy petsChanged(&f.service, &MurasakiService::PetsChanged);
    const QJsonObject report = QJsonDocument::fromJson(f.service.SwitchProfile(u"work"_s).toUtf8()).object();
    QCOMPARE(report.value(u"updated"_s).toInt(), 1);
    QCOMPARE(report.value(u"added"_s).toInt(), 1);
    QCOMPARE(petsChanged.count(), 1); // one write, one signal
    QVERIFY(!f.service.profileModified());
    QCOMPARE(f.listed().size(), 2);
    QCOMPARE(f.listed().at(0).toObject().value(u"petId"_s).toString(), a); // the pet that stays keeps its id
    QCOMPARE(f.listed().at(0).toObject().value(u"x"_s).toDouble(), 100.0); // (back to what the profile says)

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.SwitchProfile(u"Nobody"_s).isEmpty());
    f.service.RenameProfile(u"Work"_s, u"Home"_s);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array().at(0).toObject().value(u"name"_s).toString(), u"Home"_s);
    f.service.DeleteProfile(u"Home"_s);
    QVERIFY(f.service.activeProfile().isEmpty());
}

void TestMurasakiService::aProfileOriginIsSetAndKept()
{
    AutomationFixture f;
    f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, QString());
    const QString id = f.service.SaveProfile(u"Mine"_s);
    QVERIFY(!f.service.ListProfiles().contains(u"origin"_s));
    QSignalSpy changed(&f.service, &MurasakiService::ProfilesChanged);
    f.service.SetProfileOrigin(id, u"demo"_s);
    QVERIFY(changed.count() >= 1);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array().at(0).toObject().value(u"origin"_s).toString(), u"demo"_s);
    f.service.SaveProfile(u"Mine"_s); // saving again under the name keeps the marker
    QCOMPARE(QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array().at(0).toObject().value(u"origin"_s).toString(), u"demo"_s);
    f.service.SetProfileOrigin(u"Mine"_s, QString()); // (by name; empty clears it)
    QVERIFY(!f.service.ListProfiles().contains(u"origin"_s));
    for (const QString &bad : {u"package X"_s, QString(201, u'x')}) { // only "" and "demo": a package's origin comes with its import
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        f.service.SetProfileOrigin(id, bad);
    }
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.SetProfileOrigin(u"nobody"_s, u"demo"_s);
    QVERIFY(!f.service.ListProfiles().contains(u"origin"_s));
}

void TestMurasakiService::anExampleProfileLeavesTheActiveProfileAndTheSlotsAlone()
{
    AutomationFixture f;
    const QString a = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, QString());
    const QString b = f.service.AddPet(u"anima:9"_s, u"small"_s, u"DP-1"_s, QString());
    const QString mine = f.service.SaveProfile(u"Mine"_s); // slot 0 is a, slot 1 is b; Mine is active
    // a timeline of the user that drives slot 1
    const QString slotTimeline = f.service.SetTimeline(QString(), u"{\"name\":\"s\",\"tracks\":[{\"pet\":{\"slot\":1},\"actions\":[{\"kind\":\"hide\"},{\"kind\":\"wait\",\"ms\":5000}]}]}"_s);
    f.service.RemovePet(a); // (the order of the pets now differs from the slots)
    const QString before = f.service.activeProfile();
    const QString example = f.service.SaveExampleProfile(u"Example: my desktop"_s);
    QVERIFY(!example.isEmpty());
    QCOMPARE(f.service.activeProfile(), before); // not the active profile
    QCOMPARE(before, mine);
    const QJsonArray listed = QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array();
    QCOMPARE(listed.size(), 2);
    QCOMPARE(listed.at(1).toObject().value(u"origin"_s).toString(), u"demo"_s);
    f.service.RunTimeline(slotTimeline); // slot 1 is still b, not whatever pet is first now
    QVERIFY(f.runtime().value(b).toObject().value(u"hidden"_s).toBool());
    f.service.ClearPetRuntime(QString());
    // saving again replaces the example; a profile of the user with that name is refused
    QCOMPARE(f.service.SaveExampleProfile(u"Example: my desktop"_s), example);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array().size(), 2);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.SaveExampleProfile(u"Mine"_s).isEmpty());
    // switching to the example and deleting it goes back to the profile that was active before
    f.service.SwitchProfile(example);
    QCOMPARE(f.service.activeProfile(), example);
    f.service.DeleteProfile(example);
    QCOMPARE(f.service.activeProfile(), mine);
    // renaming an example makes it the user's
    const QString again = f.service.SaveExampleProfile(u"Example: other"_s);
    f.service.RenameProfile(again, u"Keep me"_s);
    QVERIFY(!QJsonDocument::fromJson(f.service.ListProfiles().toUtf8()).array().at(1).toObject().contains(u"origin"_s));
}

void TestMurasakiService::anExplicitEmptyOriginClearsTheMarkerOfATimelineOrATrigger()
{
    AutomationFixture f;
    const QString t = f.service.SetTimeline(QString(), u"{\"name\":\"e\",\"origin\":\"demo\",\"tracks\":[]}"_s);
    QVERIFY(f.service.ListTimelines().contains(u"demo"_s));
    f.service.SetTimeline(t, u"{\"name\":\"e2\"}"_s); // an edit that says nothing about the origin keeps it
    QVERIFY(f.service.ListTimelines().contains(u"demo"_s));
    f.service.SetTimeline(t, u"{\"name\":\"e3\",\"origin\":\"\"}"_s);
    QVERIFY(!f.service.ListTimelines().contains(u"origin"_s));
    const QString g = f.service.SetTrigger(QString(), u"{\"name\":\"x\",\"origin\":\"demo\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"hide-all\"}}"_s);
    QVERIFY(f.service.ListTriggers().contains(u"demo"_s));
    f.service.SetTrigger(g, u"{\"origin\":\"\"}"_s);
    QVERIFY(!f.service.ListTriggers().contains(u"origin"_s));
}

void TestMurasakiService::triggersAreMadeFiredAndRefusedByName()
{
    AutomationFixture f;
    QSignalSpy triggersChanged(&f.service, &MurasakiService::TriggersChanged);
    const QString id = f.service.SetTrigger(QString(), u"{\"name\":\"sleepy\",\"source\":{\"kind\":\"fire\",\"name\":\"sleepy\"},\"action\":{\"type\":\"hide-all\"}}"_s);
    QVERIFY(!id.isEmpty());
    QCOMPARE(triggersChanged.count(), 1);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array().at(0).toObject().value(u"cooldownMs"_s).toInt(), 500); // the setting's default
    QVERIFY(!f.service.petsHidden());
    QVERIFY(f.service.FireTrigger(u"sleepy"_s));
    QVERIFY(f.service.petsHidden());
    QVERIFY(!f.service.FireTrigger(u"nobody"_s));
    const QJsonObject state = QJsonDocument::fromJson(f.service.TriggerState().toUtf8()).object();
    QCOMPARE(state.value(u"states"_s).toArray().size(), 1);
    QCOMPARE(state.value(u"log"_s).toArray().at(0).toObject().value(u"outcome"_s).toString(), u"ran"_s);
    f.service.SetTriggerEnabled(id, false);
    QVERIFY(!QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array().at(0).toObject().value(u"enabled"_s).toBool());
    // an action that is not in the closed list, a source that reads the screen, a bad name: refused, nothing stored
    for (const QString &bad : {u"{\"name\":\"x\",\"source\":{\"kind\":\"fire\",\"name\":\"x\"},\"action\":{\"type\":\"run\",\"command\":\"ls\"}}"_s,
                               u"{\"name\":\"x\",\"source\":{\"kind\":\"screen-grab\"},\"action\":{\"type\":\"hide-all\"}}"_s,
                               u"{\"name\":\"x\",\"source\":{\"kind\":\"fire\",\"name\":\"../../x\"},\"action\":{\"type\":\"hide-all\"}}"_s, u"[1]"_s}) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
        QVERIFY(f.service.SetTrigger(QString(), bad).isEmpty());
    }
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array().size(), 1);
    // the shortcut ids the overlay registers are the enabled shortcut triggers only
    QVERIFY(!f.service.SetTrigger(QString(), u"{\"name\":\"k\",\"source\":{\"kind\":\"shortcut\",\"id\":\"peek\"},\"action\":{\"type\":\"hide-all\"}}"_s).isEmpty());
    QCOMPARE(QJsonDocument::fromJson(f.service.shortcutTriggerIds().toUtf8()).array().at(0).toObject().value(u"id"_s).toString(), u"peek"_s);
    f.service.TriggerShortcut(u"peek"_s, true);
    f.service.DeleteTrigger(id);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array().size(), 1);
}

void TestMurasakiService::timelinesHideThePetsInTheRuntimeLayerAndTheRescueBringsThemBack()
{
    AutomationFixture f;
    const QString a = f.service.AddPet(u"anima:9"_s, u"big"_s, u"DP-1"_s, QString());
    QSignalSpy runtimeChanged(&f.service, &MurasakiService::PetRuntimeChanged);
    const QString id = f.service.SetTimeline(QString(), QStringLiteral(R"({"name":"nap","restore":false,"tracks":[{"pet":{"petId":"%1"},"actions":[{"kind":"hide"},{"kind":"wait","ms":60000}]}]})").arg(a));
    QVERIFY(!id.isEmpty());
    f.service.RunTimeline(u"nap"_s);
    QVERIFY(f.runtime().value(a).toObject().value(u"hidden"_s).toBool());
    QVERIFY(runtimeChanged.count() >= 1);
    QCOMPARE(f.service.RunningTimelines().size(), 1);
    QCOMPARE(f.listed().at(0).toObject().value(u"x"_s).toDouble(), 100.0); // the stored placement is untouched
    f.service.RescuePets();
    QVERIFY(f.runtime().isEmpty());
    QVERIFY(f.service.RunningTimelines().isEmpty());
    // ClearPetRuntime does the same for one pet
    f.service.RunTimeline(id);
    QVERIFY(!f.runtime().isEmpty());
    f.service.ClearPetRuntime(a);
    QVERIFY(f.runtime().isEmpty());
    f.service.StopTimeline(u"nap"_s);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTimelines().toUtf8()).array().size(), 1);
    f.service.DeleteTimeline(u"nap"_s);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTimelines().toUtf8()).array().size(), 0);
}

void TestMurasakiService::aSceneThatHidesThePetHidesItInTheRuntimeLayer()
{
    AutomationFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    const QString dir = root + u"/pack1"_s;
    testutil::writeFile(dir + u"/idle.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
    testutil::writeFile(dir + u"/source.json"_s,
                        R"j({"version":1,"spec":{"name":"Pack"},"mdaa":{"variants":[{"name":"idle","file":"idle.gif"}],
                        "scenes":[{"line":1,"show":true,"variant":"idle"},{"line":2,"show":false}]}})j");
    const QString itemId = f.service.AddLocalPetItem(dir);
    const QString petId = f.service.AddPet(itemId, QString(), u"DP-1"_s, QString());
    f.service.StartScene(petId, 1); // hides the pet
    QVERIFY(f.runtime().value(petId).toObject().value(u"hidden"_s).toBool());
    f.service.StartScene(petId, 0); // a scene that shows it undoes that
    QVERIFY(f.runtime().isEmpty());
}

void TestMurasakiService::theAutomationCallsFailCleanlyWithoutTheAutomation()
{
    PetFixture f;
    QCOMPARE(f.service.activeProfile(), QString());
    QCOMPARE(f.service.petRuntime(), u"{}"_s);
    QCOMPARE(f.service.shortcutTriggerIds(), u"[]"_s);
    QVERIFY(f.service.ListProfiles() == u"[]"_s && f.service.ListTriggers() == u"[]"_s && f.service.ListTimelines() == u"[]"_s);
    for (int i = 0; i < 3; ++i)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.SaveProfile(u"x"_s).isEmpty());
    QVERIFY(f.service.SetTrigger(QString(), u"{}"_s).isEmpty());
    QVERIFY(!f.service.FireTrigger(u"x"_s));
}

void TestMurasakiService::screenTriggersDoNothingUntilTheUserAsksAndHaveTheirSettings()
{
    QVERIFY(kScreenDBusApiVersion == 21 && kDBusApiVersion >= kScreenDBusApiVersion);
    AutomationFixture f;
    // not attached: every call says so, the state is "off"
    QCOMPARE(QJsonDocument::fromJson(f.service.screenWatchState().toUtf8()).object().value(u"state"_s).toString(), u"off"_s);
    for (int i = 0; i < 4; ++i)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    f.service.StartScreenWatch();
    f.service.StartScreenWatchLeased(); // (the leased start says the same)
    QVERIFY(f.service.CaptureScreenReference(u"x"_s, 0, 1).isEmpty());
    f.service.CommitScreenReference(u"x"_s);
    f.service.StopScreenWatch(); // (always harmless)

    ScreenRefStore refs(f.tmp.filePath(u"refs"_s));
    f.service.attachScreen(&refs, QString(), QString(), false, QString(), QString());
    QVERIFY(f.service.screenWatch());
    QVERIFY(f.service.screenWatch()->idle());
    const auto refused = [&](const QString &mention) {
        QSignalSpy errors(&f.service, &MurasakiService::ScreenWatchChanged);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid: .*"_s + mention));
        f.service.StartScreenWatch();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid: .*"_s + mention));
        f.service.StartScreenWatchLeased(); // (a refused start leases nothing)
        QCOMPARE(errors.count(), 0);
        QVERIFY(f.service.screenWatch()->idle()); // nothing was opened: no portal, no helper, no timer
    };
    refused(u"screenTriggersEnabled"_s);
    QVERIFY(f.settings.merge(QJsonObject{{u"screenTriggersEnabled"_s, true}}));
    refused(u"no screen trigger"_s);
    const QString id = f.service.SetTrigger(QString(), u"{\"name\":\"s\",\"enabled\":true,\"source\":{\"kind\":\"screen\",\"data\":{\"regions\":[{\"kind\":\"rect\",\"x\":1,\"y\":1,\"w\":50,\"h\":50}]}},\"action\":{\"type\":\"hide-all\"}}"_s);
    QVERIFY(!id.isEmpty());
    refused(u"murasaki-screenmatch was not found"_s);
    QVERIFY(f.service.screenWatch()->idle());
    // the engine knows a screen trigger exists, and only then
    QVERIFY(f.service.automation()->needs().screen);
    f.service.DeleteTrigger(id);
    QVERIFY(!f.service.automation()->needs().screen);
    // before watching, a screen trigger says it is not active
    const QString again = f.service.SetTrigger(QString(), u"{\"name\":\"s\",\"enabled\":true,\"source\":{\"kind\":\"screen\",\"data\":{}},\"action\":{\"type\":\"hide-all\"}}"_s);
    const QJsonArray states = QJsonDocument::fromJson(f.service.TriggerState().toUtf8()).object().value(u"states"_s).toArray();
    QCOMPARE(states.at(0).toObject().value(u"note"_s).toString(), QString(kAutomationScreenInactive));
    Q_UNUSED(again);
    // the settings: closed lists of keys and ranges
    QVERIFY(f.settings.merge(QJsonObject{{u"screenTicksPerSecond"_s, 3}, {u"screenRemember"_s, true}, {u"screenWatchAtLogin"_s, false}, {u"screenIgnorePets"_s, false}}));
    QCOMPARE(f.settings.automationSettings().screenTicksPerSecond, 3);
    QString error;
    for (const QJsonObject &bad : {QJsonObject{{u"screenTicksPerSecond"_s, 17}}, QJsonObject{{u"screenTicksPerSecond"_s, 0}}, QJsonObject{{u"screenTicksPerSecond"_s, 2.5}}, QJsonObject{{u"screenTriggersEnabled"_s, 1}}}) {
        QVERIFY(!f.settings.merge(bad, &error));
        QVERIFY(!error.isEmpty());
    }
    QCOMPARE(f.settings.automationSettings().screenTicksPerSecond, 3);
    // the first-visit card of the screen section can be closed for good too (and no other page name is taken)
    QVERIFY(f.settings.merge(QJsonObject{{u"demoCardsDismissed"_s, QJsonArray{u"screen"_s, u"triggers"_s}}}, &error));
    QCOMPARE(f.settings.automationSettings().demoCardsDismissed, (QStringList{u"screen"_s, u"triggers"_s}));
    QVERIFY(!f.settings.merge(QJsonObject{{u"demoCardsDismissed"_s, QJsonArray{u"bogus"_s}}}, &error));
    const QJsonObject values = f.settings.values();
    QCOMPARE(values.value(u"screenTriggersEnabled"_s).toBool(), true);
    QCOMPARE(values.value(u"screenIgnorePets"_s).toBool(), false);
}

void TestMurasakiService::aPackBringsItsReferencePicturesAndADeletedTriggerTakesThemAway()
{
    AutomationFixture f;
    ScreenRefStore refs(f.tmp.filePath(u"refs"_s));
    f.service.attachScreen(&refs, QString(), QString(), false, QString(), QString());
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    const QString dir = root + u"/pack2"_s;
    testutil::writeFile(dir + u"/hello.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
    QImage picture(40, 30, QImage::Format_RGB888);
    picture.fill(QColor(200, 30, 30));
    QDir().mkpath(dir + u"/screenrefs/region0"_s);
    QVERIFY(picture.save(dir + u"/screenrefs/region0/step0.png"_s, "PNG"));
    testutil::writeFile(dir + u"/source.json"_s,
                        R"j({"version":1,"spec":{"name":"Pack"},"mdaa":{"variants":[{"name":"hello","file":"hello.gif"}],"scenes":[],
                        "screenRefs":{"status":"ready","sourceRegions":1,"regions":[{"region":0,"status":"ready","sourceFrames":1,"frames":[{"file":"screenrefs/region0/step0.png","clipIndex":0}]}]},
                        "pack":{"name":"Wave","speedMult":1,"match":{"cooldownMs":600},"regions":[{"kind":"rect","x":1,"y":1,"w":20,"h":20}],
                          "tracks":[{"name":"Hello","variant":"hello","startVisible":true,"actions":[{"t":0,"kind":"show"},{"t":300,"kind":"hide"}]}]}}})j");
    const QString itemId = f.service.AddLocalPetItem(dir);
    QVERIFY(itemId.startsWith(u"local:"_s));
    const QJsonObject made = QJsonDocument::fromJson(f.service.ImportPackAutomation(itemId).toUtf8()).object();
    QString screenId;
    for (const QJsonValue &t : QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array())
        if (t.toObject().value(u"source"_s).toObject().value(u"kind"_s).toString() == u"screen")
            screenId = t.toObject().value(u"id"_s).toString();
    QVERIFY(!screenId.isEmpty());
    QStringList notes;
    for (const QJsonValue &n : made.value(u"notes"_s).toArray())
        notes << n.toString();
    QVERIFY2(notes.join(u"|"_s).contains(u"screen: ready (1 reference pictures kept)"_s), qPrintable(notes.join(u"|"_s)));
    QCOMPARE(refs.steps(screenId, 0), (QList<int>{0}));
    QVERIFY(refs.frame(screenId, 0, 0).startsWith("\x89PNG"));
    QVERIFY(QFileInfo::exists(refs.triggerFolder(screenId)));
    for (const QJsonValue &st : QJsonDocument::fromJson(f.service.TriggerState().toUtf8()).object().value(u"states"_s).toArray())
        if (st.toObject().value(u"id"_s).toString() == screenId)
            QCOMPARE(st.toObject().value(u"refs"_s).toArray(), QJsonArray{1}); // one picture for the one region, watching or not
    // made again, the pictures are replaced (not doubled) and the trigger keeps its id
    f.service.ImportPackAutomation(itemId);
    QCOMPARE(refs.steps(screenId, 0), (QList<int>{0}));
    // the trigger goes, its folder goes with it (and nothing else's)
    QVERIFY(refs.putFrame(u"other"_s, 0, 0, refs.frame(screenId, 0, 0)));
    f.service.DeleteTrigger(screenId);
    QVERIFY(!QFileInfo::exists(refs.triggerFolder(screenId)));
    QVERIFY(QFileInfo::exists(refs.triggerFolder(u"other"_s)));
}

void TestMurasakiService::aPackAndItsScenesBecomeDisabledAutomationOnRequest()
{
    AutomationFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    const QString dir = root + u"/pack1"_s;
    testutil::writeFile(dir + u"/hello.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
    testutil::writeFile(dir + u"/source.json"_s,
                        R"j({"version":1,"spec":{"name":"Pack"},"mdaa":{"variants":[{"name":"hello","file":"hello.gif"}],
                        "scenes":[{"line":1,"show":true,"variant":"hello","x":5,"y":6},{"line":2,"show":false}],
                        "pack":{"name":"Wave","speedMult":1,"match":{"cooldownMs":600},"regions":[{"kind":"rect","x":1,"y":1,"w":2,"h":2}],
                          "tracks":[{"name":"Hello","variant":"hello","startVisible":true,"actions":[{"t":0,"kind":"show"},{"t":300,"kind":"hide"},{"t":100,"kind":"sound","sound":"x.wav"}]}]}}})j");
    const QString itemId = f.service.AddLocalPetItem(dir);
    QVERIFY(itemId.startsWith(u"local:"_s));
    // a scene becomes a one-step timeline, once (made again it is replaced, not doubled)
    const QString scene = f.service.TimelineFromScene(itemId, 0);
    QVERIFY(!scene.isEmpty());
    QCOMPARE(f.service.TimelineFromScene(itemId, 0), scene);
    QVERIFY(!f.service.TimelineFromScene(itemId, 1).isEmpty());
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTimelines().toUtf8()).array().size(), 2);
    // the pack: a timeline and two triggers, all disabled
    const QJsonObject made = QJsonDocument::fromJson(f.service.ImportPackAutomation(itemId).toUtf8()).object();
    QVERIFY(!made.value(u"timeline"_s).toString().isEmpty());
    QCOMPARE(made.value(u"triggers"_s).toArray().size(), 2);
    QVERIFY(made.value(u"notes"_s).toArray().size() >= 2);
    const QJsonArray triggers = QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array();
    QCOMPARE(triggers.size(), 2);
    for (const QJsonValue &t : triggers) {
        QVERIFY(!t.toObject().value(u"enabled"_s).toBool());
        QCOMPARE(t.toObject().value(u"action"_s).toObject().value(u"id"_s).toString(), made.value(u"timeline"_s).toString());
        QVERIFY(t.toObject().value(u"origin"_s).toString().startsWith(u"package"_s));
    }
    // it does nothing until the user enables it: fired, nothing runs
    QString fireName;
    for (const QJsonValue &t : triggers)
        if (t.toObject().value(u"source"_s).toObject().value(u"kind"_s).toString() == u"fire")
            fireName = t.toObject().value(u"source"_s).toObject().value(u"name"_s).toString();
    QVERIFY(fireName.startsWith(u"Wave-"_s));
    QVERIFY(f.service.FireTrigger(fireName)); // (a trigger has the name, it is disabled)
    QVERIFY(f.service.RunningTimelines().isEmpty());
    // made again, it replaces what it made and keeps a trigger the user switched on
    const QString fireId = triggers.at(0).toObject().value(u"id"_s).toString();
    f.service.SetTriggerEnabled(fireId, true);
    f.service.ImportPackAutomation(itemId);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array().size(), 2);
    QCOMPARE(QJsonDocument::fromJson(f.service.ListTimelines().toUtf8()).array().size(), 3);
    bool on = false;
    for (const QJsonValue &t : QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array())
        on = on || t.toObject().value(u"enabled"_s).toBool();
    QVERIFY(on);
    // an item without a pack, and an unknown one, are refused
    const QString plain = f.service.AddLocalPetItem(f.tmp.filePath(u"data/imported/pack1/hello.gif"_s));
    Q_UNUSED(plain);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.ImportPackAutomation(u"anima:9"_s).isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(f.service.TimelineFromScene(itemId, 9).isEmpty());
}

void TestMurasakiService::muteIsASessionStateAndTheActionsOfACallDoNotFailTheCall()
{
    AutomationFixture f;
    QSignalSpy muted(&f.service, &MurasakiService::PetsMutedChanged);
    QVERIFY(!f.service.petsMuted());
    const QString id = f.service.SetTrigger(QString(), u"{\"name\":\"m\",\"source\":{\"kind\":\"fire\",\"name\":\"m\"},\"action\":{\"type\":\"mute\",\"mode\":\"on\"}}"_s);
    QVERIFY(!id.isEmpty());
    QVERIFY(f.service.TestTrigger(id));
    QVERIFY(f.service.petsMuted());
    QCOMPARE(muted.count(), 1);
    QVERIFY(!f.settings.values().value(u"petsMuted"_s).toBool()); // the setting is untouched, and so is settings.json
    f.service.SetPetsMuted(true);
    QCOMPARE(muted.count(), 1);
    f.service.SetPetsMuted(false);
    QCOMPARE(muted.count(), 2);
    // the rescue pauses the triggers, and says how long
    f.service.RescuePets();
    QVERIFY(QJsonDocument::fromJson(f.service.TriggerState().toUtf8()).object().value(u"suspendedMs"_s).toDouble() > 50'000);
    f.service.ResumeAutomation();
    QCOMPARE(QJsonDocument::fromJson(f.service.TriggerState().toUtf8()).object().value(u"suspendedMs"_s).toDouble(), 0.0);
}

void TestMurasakiService::twoPackagesWithOneTitleCoexistAndAreReplacedEachByItself()
{
    AutomationFixture f;
    const QString root = f.tmp.filePath(u"data/imported"_s);
    PetImportManager imports(root);
    f.service.attachImports(&imports);
    QStringList items;
    for (const QString &name : {u"one"_s, u"two"_s}) {
        const QString dir = root + u"/"_s + name;
        testutil::writeFile(dir + u"/hello.gif"_s, animfixtures::gif(32, 24, {5, 5, 5}));
        testutil::writeFile(dir + u"/source.json"_s,
                            R"j({"version":1,"spec":{"name":"Same title"},"mdaa":{"variants":[{"name":"hello","file":"hello.gif"}],"scenes":[{"line":1,"show":true,"variant":"hello"}],
                            "pack":{"name":"Wave","tracks":[{"name":"Hello","variant":"hello","actions":[{"t":0,"kind":"move","x":100,"y":100}]}]}}})j");
        items << f.service.AddLocalPetItem(dir);
        QVERIFY(items.last().startsWith(u"local:"_s));
    }
    for (const QString &item : items) {
        QVERIFY(!f.service.ImportPackAutomation(item).isEmpty());
        QVERIFY(!f.service.TimelineFromScene(item, 0).isEmpty());
    }
    const auto count = [&](const QString &list) { return QJsonDocument::fromJson(list.toUtf8()).array().size(); };
    QCOMPARE(count(f.service.ListTimelines()), 4); // a pack and a scene of each, none replaced by the other
    QCOMPARE(count(f.service.ListTriggers()), 4);
    QSet<QString> fireNames, origins;
    for (const QJsonValue &t : QJsonDocument::fromJson(f.service.ListTriggers().toUtf8()).array()) {
        if (t.toObject().value(u"source"_s).toObject().value(u"kind"_s).toString() == u"fire")
            fireNames.insert(t.toObject().value(u"source"_s).toObject().value(u"name"_s).toString());
        origins.insert(t.toObject().value(u"origin"_s).toString());
    }
    QCOMPARE(fireNames.size(), 2); // FireTrigger of one name can not run both packs
    QCOMPARE(origins.size(), 2);
    QSet<QString> names;
    for (const QJsonValue &t : QJsonDocument::fromJson(f.service.ListTimelines().toUtf8()).array())
        names.insert(t.toObject().value(u"name"_s).toString());
    QCOMPARE(names.size(), 4);
    // each is replaced by its own package's second import
    for (const QString &item : items) {
        f.service.ImportPackAutomation(item);
        f.service.TimelineFromScene(item, 0);
    }
    QCOMPARE(count(f.service.ListTimelines()), 4);
    QCOMPARE(count(f.service.ListTriggers()), 4);
}

QTEST_GUILESS_MAIN(TestMurasakiService)
#include "tst_murasakiservice.moc"
