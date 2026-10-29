// The pages of the automation (Profiles, Triggers, Timelines) loaded from their QML files against a daemon client with data: no QML warning, the texts that come
// from files and imports are plain text, and the dialogs build what the daemon expects.

#include "daemonclient.h"
#include "overlayclient.h"
#include "fakebackend.h"
#include "shortcut/shortcutcatalog.h"
#include "surfaceclient.h"
#include "systemcontrol.h"
#include "testutil.h"

#include <algorithm>
#include <optional>

#include <QGuiApplication>
#include <QJSValue>
#include <QMap>
#include <QPair>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::DaemonClient;

namespace {

QStringList g_warnings;
QtMessageHandler g_previous = nullptr;

void capture(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        g_warnings << message;
    if (g_previous)
        g_previous(type, context, message);
}

// All the visual items below `item` (a Repeater's delegates are children of the layout visually, not always as QObjects).
void collectItems(QQuickItem *item, QList<QQuickItem *> *all)
{
    all->append(item);
    for (QQuickItem *child : item->childItems())
        collectItems(child, all);
}

// Every item of a QML object that may be a Dialog (not an item itself): its item children and everything below them, delegates of Repeaters included.
QList<QQuickItem *> allItems(QObject *root)
{
    QList<QQuickItem *> all;
    if (auto *item = qobject_cast<QQuickItem *>(root))
        collectItems(item, &all);
    for (QQuickItem *child : root->findChildren<QQuickItem *>(Qt::FindDirectChildrenOnly))
        collectItems(child, &all);
    for (QQuickItem *child : root->findChildren<QQuickItem *>())
        if (!all.contains(child))
            collectItems(child, &all);
    return all;
}

// Every Text-like item below `item` that shows a string with markup in it.
void collectRisky(QQuickItem *item, QList<QQuickItem *> *risky)
{
    const QVariant text = item->property("text");
    if (text.metaType().id() == QMetaType::QString && (text.toString().contains(u'<') || text.toString().contains(u'&')))
        risky->append(item); // (an item that reads markup and has no textFormat property is judged by its text alone: see the test)
    for (QQuickItem *child : item->childItems())
        collectRisky(child, risky);
}

}

class TstAutomationPages : public QObject
{
    Q_OBJECT

private:
    FakeBackend *m_backend = nullptr;
    DaemonClient *m_daemon = nullptr;
    QTemporaryDir m_journalDir;
    QQmlEngine *m_engine = nullptr;

    QObject *load(const QString &name)
    {
        QQmlComponent component(m_engine, QUrl::fromLocalFile(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../src/app/qml/") + name));
        if (component.status() != QQmlComponent::Ready) {
            ADD_FAILURE_TEXT(component.errorString());
            return nullptr;
        }
        QObject *object = component.create();
        return object;
    }
    QObject *loadWith(const QString &name, const QVariantMap &properties)
    {
        QQmlComponent component(m_engine, QUrl::fromLocalFile(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../src/app/qml/") + name));
        if (component.status() != QQmlComponent::Ready) {
            ADD_FAILURE_TEXT(component.errorString());
            return nullptr;
        }
        QObject *object = component.createWithInitialProperties(properties);
        if (!object)
            ADD_FAILURE_TEXT(component.errorString());
        return object;
    }
    // `skip` calls of that method back from the last one.
    QJsonObject lastCallJson(const QString &method, int argument, int skip = 0) const
    {
        for (int i = m_backend->calls.size() - 1; i >= 0; --i)
            if (m_backend->calls.at(i).first == method && skip-- == 0)
                return QJsonDocument::fromJson(m_backend->calls.at(i).second.value(argument).toString().toUtf8()).object();
        return {};
    }
    // The item of a page that has this property (the first-visit card has `closedNow`, a button has `text`).
    static QQuickItem *itemWith(QObject *root, const char *property, const QVariant &value = QVariant())
    {
        for (QQuickItem *item : root->findChildren<QQuickItem *>())
            if (item->property(property).isValid() && (!value.isValid() || item->property(property) == value))
                return item;
        return nullptr;
    }
    static void ADD_FAILURE_TEXT(const QString &text) { g_warnings << text; }

private slots:
    void initTestCase()
    {
        g_previous = qInstallMessageHandler(capture);
        // The pages remember view choices with QtCore's Settings: in a test that goes to the test location, never the user's.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(u"murasaki-tests"_s);
        QCoreApplication::setApplicationName(u"tst_automationpages"_s);
        m_backend = new FakeBackend;
        m_backend->setProperty(u"ApiVersion"_s, 23u);
        m_backend->setProperty(u"Status"_s, u"ok"_s);
        m_backend->setProperty(u"WallpaperEngineAssetsDir"_s, QString());
        m_backend->setProperty(u"ActiveProfile"_s, u"p1"_s);
        m_backend->setProperty(u"ProfileModified"_s, true);
        m_backend->setProperty(u"PetsArrange"_s, false);
        m_backend->setProperty(u"PetsHidden"_s, false);
        m_backend->answerJson(u"ListItems"_s, "[]");
        m_backend->answerJson(u"Assignments"_s, "[]");
        m_backend->answerJson(u"Settings"_s, R"({"automationEnabled":true,"fireByTimelineName":false,"triggerCooldownMs":500,"demoCardsDismissed":[],"screenTriggersEnabled":true,"screenTicksPerSecond":5,"screenRemember":false,"screenWatchAtLogin":false,"screenIgnorePets":true})");
        m_backend->setProperty(u"ScreenWatchState"_s, u"{\"state\":\"off\",\"reason\":\"\",\"regions\":0,\"ticksPerSecond\":0,\"since\":0,\"paused\":false,\"notes\":[]}"_s);
        for (const char *method : {"StartScreenWatch", "StopScreenWatch", "CommitScreenReference", "DiscardScreenReference"})
            m_backend->answer(QString::fromLatin1(method), QVariant());
        m_backend->answer(u"CaptureScreenReference"_s, u"req-1"_s);
        m_backend->answerJson(u"ListPets"_s, R"([{"petId":"pet1","itemId":"local:abc","variant":"idle","output":"*"}])");
        m_backend->answerJson(u"ListProfiles"_s, R"([{"id":"p1","name":"<b>Work</b> & play","pets":[{},{}],"origin":"<i>package</i>"},{"id":"p2","name":"Home","pets":[]}])");
        m_backend->answerJson(u"ListTriggers"_s, R"([
            {"id":"t1","name":"<script>alert(1)</script>","enabled":false,"origin":"package <b>Wave</b>","cooldownMs":500,"mode":"press","source":{"kind":"fire","name":"wave"},"action":{"type":"timeline","id":"l1"}},
            {"id":"t2","name":"Focus","enabled":true,"cooldownMs":500,"mode":"press","source":{"kind":"app-focus","app":"fire<fox>*","on":"enter"},"action":{"type":"hide-all"}},
            {"id":"t3","name":"Screen","enabled":false,"cooldownMs":500,"mode":"press","source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":10,"y":20,"w":300,"h":100},{"kind":"circle","x":5,"y":6,"w":7,"h":8}],"match":{"mode":"ORB_HASH","threshold":0.75}}},"action":{"type":"timeline","id":"l1"}},
            {"id":"t4","name":"Key","enabled":true,"cooldownMs":500,"mode":"hold","source":{"kind":"shortcut","id":"peek"},"action":{"type":"hide-all"}}])");
        m_backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":false,"available":true,"outcome":"disabled"},{"id":"t2","enabled":true,"available":false,"outcome":""},
            {"id":"t3","enabled":false,"available":true,"refs":[1,0],"screen":{"score":[640,-1],"detected":false,"step":0,"active":false}},{"id":"t4","enabled":true,"available":true,"outcome":"ran","agoMs":4000}],"log":[]})");
        m_backend->answerJson(u"ListTimelines"_s, R"([{"id":"l1","name":"<u>wave</u>","speed":2,"restore":true,"origin":"package <b>Wave</b>","tracks":[{"pet":{"petId":"pet1"},"startVisible":true,
            "actions":[{"kind":"move","x":1,"y":2,"mode":"smooth","durationMs":300},{"kind":"wait","ms":10},{"kind":"show","variant":"v"}]},{"pet":{"item":"local:abc","variant":"idle"},"actions":[]},{"pet":{"slot":2},"actions":[]}]}])");
        m_backend->answer(u"RunningTimelines"_s, QStringList{u"l1"_s});
        m_backend->answerJson(u"SwitchProfile"_s, R"({"profile":"<b>Work</b> & <i>play</i>","kept":0,"updated":0,"added":0,"removed":0,
            "skipped":[{"slot":1,"itemId":"<img src=x onerror=1>local:&abc","reason":"profile: item missing"},{"slot":2,"itemId":"b","reason":"<u>weird</u>"}]})");
        m_backend->answer(u"SetTrigger"_s, u"t9"_s);
        m_backend->answer(u"SetTimeline"_s, u"l9"_s);
        m_backend->answer(u"SaveProfile"_s, u"p9"_s);
        m_backend->answer(u"SaveExampleProfile"_s, u"p9"_s);
        for (const char *method : {"SetProfileOrigin", "RunTimeline", "StopTimeline", "DeleteTimeline", "DeleteTrigger", "DeleteProfile", "SetSettings"})
            m_backend->answer(QString::fromLatin1(method), QVariant());
        m_daemon = new DaemonClient(m_backend);
        m_daemon->setScreenDemoJournalPath(m_journalDir.filePath(u"screen-demo.ini"_s)); // (never the real config)
        m_engine = new QQmlEngine;
        qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Overlay", new murasaki::OverlayClient(new FakeBackend, m_engine)); // (the state rows of the pet page read it)
        qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Daemon", m_daemon);
        // The applications of the Automations area: one installed application, with the class its windows carry.
        testutil::writeFile(m_journalDir.filePath(u"applications/discord.desktop"_s),
                            "[Desktop Entry]\nType=Application\nName=Discord\nExec=discord\nIcon=discord\nStartupWMClass=discord\n");
        murasaki::shortcut::DesktopEnvironment env;
        env.locales = {u"en"_s};
        auto *catalog = new murasaki::shortcut::ShortcutCatalog({m_journalDir.filePath(u"applications"_s)}, env, m_engine);
        catalog->refreshNow();
        qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "ShortcutCatalog", catalog);
        qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "SystemControl", new murasaki::SystemControl(m_engine));
        // What each screen does now: DP-1 paused by a game, HDMI-A-1 covered (the rows of "My automations" say so).
        auto *surfaceBackend = new FakeBackend;
        surfaceBackend->answerJson(u"Outputs"_s, R"([{"name":"DP-1","occluded":false,"paused":true,"playing":false,"pauseReason":"game"},
            {"name":"HDMI-A-1","occluded":true,"paused":true,"playing":false,"pauseReason":""},
            {"name":"eDP-1","occluded":false,"paused":false,"playing":true,"pauseReason":""}])");
        surfaceBackend->answerJson(u"Desktops"_s, "[]");
        auto *surface = new murasaki::SurfaceClient(surfaceBackend, m_engine);
        qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Surface", surface);
        surfaceBackend->setAvailable(true);
        QTRY_COMPARE(surface->outputs().size(), 3);
        m_backend->setAvailable(true);
        QTRY_VERIFY(m_daemon->automationSupported());
        QTRY_COMPARE(m_daemon->timelines().size(), 1);
        QTRY_COMPARE(m_daemon->triggers().size(), 4);
        QTRY_COMPARE(m_daemon->profiles().size(), 2);
        QTRY_VERIFY(!m_daemon->triggerStates().isEmpty());
        QTRY_COMPARE(m_daemon->pets().size(), 1);
        QSignalSpy switched(m_daemon, &DaemonClient::profileSwitched);
        m_daemon->switchProfile(u"p1"_s); // the page shows what the last switch skipped: names from a file, in a message that reads markup
        QTRY_COMPARE(switched.size(), 1);
    }
    void cleanupTestCase()
    {
        qInstallMessageHandler(g_previous);
    }

    void everyPageLoadsWithDataAndShowsFileTextsAsPlainText()
    {
        for (const QString &name : {u"ProfilesPage.qml"_s, u"TriggersPage.qml"_s, u"TimelinesPage.qml"_s, u"AutomationsPage.qml"_s}) {
            g_warnings.clear();
            QObject *page = load(name);
            QVERIFY2(page, qPrintable(name + u": "_s + g_warnings.join(u"; "_s)));
            QVERIFY(QMetaObject::invokeMethod(page, "smoke"));
            QTest::qWait(50);
            QVERIFY2(g_warnings.isEmpty(), qPrintable(name + u": "_s + g_warnings.join(u"; "_s)));
            // every text with markup in it, from a file or an import, is plain text
            QList<QQuickItem *> risky;
            collectRisky(qobject_cast<QQuickItem *>(page), &risky);
            if (name == u"AutomationsPage.qml"_s) {
                // the sentences take the brackets out of a file's text before any control sees it: the hostile name shows, defused
                QStringList shown;
                for (QQuickItem *item : allItems(page))
                    if (item->property("text").toString().contains(u"script"_s))
                        shown << item->property("text").toString();
                QVERIFY2(shown.contains(u"‹script›alert(1)‹/script›"_s), qPrintable(shown.join(u" | "_s)));
            } else {
                QVERIFY2(!risky.isEmpty(), qPrintable(name + u" shows no hostile text at all: the fixture is not reaching the page"_s));
            }
            for (QQuickItem *item : std::as_const(risky)) {
                const QVariant format = item->property("textFormat");
                // a label is plain text; a Kirigami.InlineMessage (which reads markup) must hold none of the file's brackets or ampersands at all
                const QString shown = item->property("text").toString();
                if (format.isValid())
                    QVERIFY2(format.toInt() == int(Qt::PlainText), qPrintable(name + u": "_s + shown));
                else
                    QVERIFY2(!shown.contains(u'<') && !shown.contains(u'>') && !shown.contains(u'&'), qPrintable(name + u": a message with markup: "_s + shown));
            }
            delete page;
        }
    }
    void tryItOnEachPageMakesItsMarkedExample()
    {
        g_warnings.clear();
        // Timelines: the example timeline, marked, with the steps of the user's two pets
        QObject *page = load(u"TimelinesPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        const int before = m_backend->callCount(u"SetTimeline"_s);
        QVERIFY(QMetaObject::invokeMethod(page, "smoke"));
        QTRY_VERIFY(m_backend->callCount(u"SetTimeline"_s) > before);
        QCOMPARE(lastCallJson(u"SetTimeline"_s, 1).value(u"origin"_s).toString(), u"demo"_s);
        QVERIFY(lastCallJson(u"SetTimeline"_s, 1).value(u"name"_s).toString().startsWith(u"Example: "_s));
        QCOMPARE(lastCallJson(u"SetTimeline"_s, 1).value(u"tracks"_s).toArray().size(), 1); // (the fixture has one pet: it does both parts)
        delete page;
        // Triggers: a shortcut trigger of the demo that plays the timeline
        page = load(u"TriggersPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        QVERIFY(QMetaObject::invokeMethod(page, "smoke"));
        QTRY_VERIFY(m_backend->callCount(u"SetTrigger"_s) > 0);
        const QJsonObject trigger = lastCallJson(u"SetTrigger"_s, 1);
        QCOMPARE(trigger.value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(trigger.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"shortcut"_s);
        QCOMPARE(trigger.value(u"action"_s).toObject().value(u"type"_s).toString(), u"timeline"_s);
        delete page;
        // Profiles: the desktop saved, marked, and switched to
        page = load(u"ProfilesPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        const int switches = m_backend->callCount(u"SwitchProfile"_s);
        QVERIFY(QMetaObject::invokeMethod(page, "smoke"));
        QTRY_VERIFY(m_backend->callCount(u"SaveExampleProfile"_s) > 0);
        QCOMPARE(m_backend->callCount(u"SwitchProfile"_s), switches); // saving the example does not switch to it
        delete page;
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
    }
    void tryItOnThePetPageFollowsTheCursorThroughATimeline()
    {
        g_warnings.clear();
        const QVariantMap pet = m_daemon->pets().first().toMap();
        QObject *states = loadWith(u"PetStates.qml"_s, {{u"petId"_s, u"pet1"_s}, {u"pet"_s, pet}, {u"settled"_s, true}, {u"variantChoices"_s, QVariantList()}});
        QVERIFY2(states, qPrintable(g_warnings.join(u"; "_s)));
        QQuickItem *button = itemWith(states, "text", u"Try it"_s);
        QVERIFY(button);
        const int runs = m_backend->callCount(u"RunTimeline"_s);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_COMPARE(m_backend->callCount(u"RunTimeline"_s), runs + 1);
        const QJsonObject timeline = lastCallJson(u"SetTimeline"_s, 1);
        QCOMPARE(timeline.value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(timeline.value(u"tracks"_s).toArray().at(0).toObject().value(u"actions"_s).toArray().at(0).toObject().value(u"kind"_s).toString(), u"track"_s);
        // "Remove examples" shows while an example exists (the fixture has marked entries only after the lists carry them)
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete states;
    }
    void theFirstVisitCardIsShownUntilItIsClosedAndThenNeverAgain()
    {
        g_warnings.clear();
        QObject *page = load(u"TimelinesPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        QQuickItem *card = itemWith(page, "closedNow");
        QVERIFY(card);
        QVERIFY(card->isVisible());
        QVERIFY(QMetaObject::invokeMethod(card, "dismiss"));
        QCOMPARE(card->isVisible(), false);
        QTRY_VERIFY(m_backend->callCount(u"SetSettings"_s) > 0);
        QTRY_VERIFY([this] { // (the smoke calls of the other pages closed theirs too: look for this one)
            for (const auto &call : std::as_const(m_backend->calls))
                if (call.first == u"SetSettings"_s && call.second.value(0).toString().contains(u"timelines"_s))
                    return true;
            return false;
        }());
        delete page;
        // the daemon remembers it: a page opened later, in a new session, does not show the card
        m_backend->answerJson(u"Settings"_s, R"({"automationEnabled":true,"fireByTimelineName":false,"triggerCooldownMs":500,"demoCardsDismissed":["timelines"]})");
        m_backend->emitSignal(u"SettingsChanged"_s);
        QTRY_VERIFY(m_daemon->settings().value(u"demoCardsDismissed"_s).toList().contains(u"timelines"_s));
        page = load(u"TimelinesPage.qml"_s);
        QVERIFY(page);
        QQuickItem *again = itemWith(page, "closedNow");
        QVERIFY(again);
        QCOMPARE(again->isVisible(), false);
        // the other pages' cards are still there
        QObject *triggers = load(u"TriggersPage.qml"_s);
        QVERIFY(triggers);
        QVERIFY(itemWith(triggers, "closedNow")->isVisible());
        delete triggers;
        delete page;
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
    }
    void theTimelineDialogShowsAnExampleReadOnlyAndPlaysIt()
    {
        g_warnings.clear();
        QObject *dialog = load(u"TimelineDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        const QVariantMap example = QJsonDocument::fromJson(R"({"id":"l9","name":"Example: x","speed":1,"restore":true,"origin":"demo","tracks":[{"pet":{"petId":"pet1"},"startVisible":true,
            "actions":[{"kind":"track","mode":"follow"},{"kind":"wait","ms":20000}]}]})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openExample", Q_ARG(QVariant, QVariant(example))));
        QCOMPARE(dialog->property("readOnly").toBool(), true);
        QCOMPARE(dialog->property("timeline").toMap().value(u"id"_s).toString(), u"l9"_s);
        const int runs = m_backend->callCount(u"RunTimeline"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "playExample")); // (what its Play button does)
        QTRY_COMPARE(m_backend->callCount(u"RunTimeline"_s), runs + 1);
        QCOMPARE(m_backend->calls.last().second, QVariantList{u"l9"_s});
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }
    void savingAnEditedExampleClearsItsMarker()
    {
        g_warnings.clear();
        QObject *dialog = load(u"TimelineDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        const QVariantMap example = QJsonDocument::fromJson(R"({"id":"l9","name":"Example: x","speed":1,"restore":true,"origin":"demo","tracks":[]})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(example))));
        const int saves = m_backend->callCount(u"SetTimeline"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "save", Q_ARG(QVariant, QVariant(false))));
        QTRY_COMPARE(m_backend->callCount(u"SetTimeline"_s), saves + 1);
        const QJsonObject sent = lastCallJson(u"SetTimeline"_s, 1);
        QVERIFY(sent.contains(u"origin"_s));
        QCOMPARE(sent.value(u"origin"_s).toString(), QString()); // (the daemon takes an explicit empty origin as "clear it")
        // a timeline of an import keeps its origin
        const QVariantMap packed = QJsonDocument::fromJson(R"({"id":"l8","name":"p","speed":1,"restore":true,"origin":"package X","tracks":[]})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(packed))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "save", Q_ARG(QVariant, QVariant(false))));
        QTRY_COMPARE(m_backend->callCount(u"SetTimeline"_s), saves + 2);
        QVERIFY(!lastCallJson(u"SetTimeline"_s, 1).contains(u"origin"_s));
        delete dialog;
    }
    void theTimelineEditorShowsAnErrorOfTheDaemonWithoutMarkup()
    {
        g_warnings.clear();
        QObject *dialog = load(u"TimelineDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant())));
        dialog->setProperty("saveError", u"<b>track 1</b>: a name <script>x</script> & more"_s);
        QTest::qWait(30);
        QList<QQuickItem *> risky;
        for (QQuickItem *root : dialog->findChildren<QQuickItem *>())
            if (root->property("text").toString().contains(u"track 1"_s))
                risky.append(root);
        QVERIFY(!risky.isEmpty());
        for (QQuickItem *item : std::as_const(risky))
            QVERIFY2(!item->property("text").toString().contains(u'<') && !item->property("text").toString().contains(u'&'), qPrintable(item->property("text").toString()));
        delete dialog;
    }
    void theScreenTriggerDialogKeepsWhatTheDaemonKeptAndBuildsWhatItChecks()
    {
        g_warnings.clear();
        QObject *dialog = load(u"ScreenTriggerDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        QVariantMap existing = QJsonDocument::fromJson(R"({"id":"t3","name":"Victory","enabled":true,"cooldownMs":500,"mode":"hold","origin":"package X",
            "source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":10,"y":20,"w":300,"h":100,"id":"r0"},{"kind":"circle","x":5,"y":6,"w":7,"h":8,"points":[[1,2],[3,4]]}],
            "match":{"mode":"SIFT_LIB","agg":"min","threshold":0.8,"cooldownMs":700,"cooldownFrom":"deactivate","startFramesToCheck":4,"stopGraceMs":300,"stopOnMismatch":false,"refMode":"sequencer","seqLen":5,"seqStep":2,"loopDelayMs":900,"extra":"kept"},
            "event":{"name":"e","startIndex":2},"pack":{"id":"abc"}}},"action":{"type":"timeline","id":"l1"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(existing))));
        QVariant changes;
        QVERIFY(QMetaObject::invokeMethod(dialog, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        const QJsonObject c = QJsonObject::fromVariantMap(changes.toMap());
        QCOMPARE(c.value(u"name"_s).toString(), u"Victory"_s);
        QCOMPARE(c.value(u"enabled"_s).toBool(), true);
        QCOMPARE(c.value(u"mode"_s).toString(), u"press"_s); // (a screen trigger holds by its kind: the engine undoes the action when the picture goes; "hold" is for shortcuts)
        QCOMPARE(c.value(u"action"_s).toObject(), (QJsonObject{{u"type"_s, u"timeline"_s}, {u"id"_s, u"l1"_s}}));
        const QJsonObject source = c.value(u"source"_s).toObject();
        QCOMPARE(source.value(u"kind"_s).toString(), u"screen"_s);
        const QJsonObject data = source.value(u"data"_s).toObject();
        // what the dialog does not show comes back as it was: the pack, the event, the other keys of match, a circle with its points, a region's id
        QCOMPARE(data.value(u"pack"_s).toObject().value(u"id"_s).toString(), u"abc"_s);
        QCOMPARE(data.value(u"event"_s).toObject().value(u"startIndex"_s).toInt(), 2);
        QCOMPARE(data.value(u"regions"_s).toArray().size(), 2);
        QCOMPARE(data.value(u"regions"_s).toArray().at(0).toObject().value(u"id"_s).toString(), u"r0"_s);
        QCOMPARE(data.value(u"regions"_s).toArray().at(1).toObject().value(u"kind"_s).toString(), u"circle"_s);
        QCOMPARE(data.value(u"regions"_s).toArray().at(1).toObject().value(u"points"_s).toArray().size(), 2);
        const QJsonObject match = data.value(u"match"_s).toObject();
        QCOMPARE(match.value(u"extra"_s).toString(), u"kept"_s);
        QCOMPARE(match.value(u"loopDelayMs"_s).toInt(), 900);
        QCOMPARE(match.value(u"mode"_s).toString(), u"structure"_s); // SIFT_LIB is matched by structure
        QCOMPARE(match.value(u"agg"_s).toString(), u"all"_s);         // "min" is Anima's name for it
        QCOMPARE(match.value(u"threshold"_s).toDouble(), 0.8);
        QCOMPARE(match.value(u"cooldownMs"_s).toInt(), 700);
        QCOMPARE(match.value(u"cooldownFrom"_s).toString(), u"deactivate"_s);
        QCOMPARE(match.value(u"startFramesToCheck"_s).toInt(), 4);
        QCOMPARE(match.value(u"stopGraceMs"_s).toInt(), 300);
        QCOMPARE(match.value(u"stopOnMismatch"_s).toBool(), false);
        QCOMPARE(match.value(u"refMode"_s).toString(), u"sequencer"_s);
        QCOMPARE(match.value(u"seqLen"_s).toInt(), 5);
        QCOMPARE(match.value(u"seqStep"_s).toInt(), 2);

        // a new trigger: a region to type, the default numbers, nothing of an old one
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant())));
        QVERIFY(QMetaObject::invokeMethod(dialog, "addRegion", Q_ARG(QVariant, 5), Q_ARG(QVariant, 6), Q_ARG(QVariant, 70), Q_ARG(QVariant, 80)));
        QVERIFY(QMetaObject::invokeMethod(dialog, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        const QJsonObject fresh = QJsonObject::fromVariantMap(changes.toMap());
        QCOMPARE(fresh.value(u"enabled"_s).toBool(), false); // off until the user switches it on
        const QJsonObject freshData = fresh.value(u"source"_s).toObject().value(u"data"_s).toObject();
        QCOMPARE(freshData.value(u"regions"_s).toArray().size(), 1);
        QCOMPARE(freshData.value(u"regions"_s).toArray().at(0).toObject().value(u"w"_s).toInt(), 70);
        QCOMPARE(freshData.value(u"match"_s).toObject().value(u"mode"_s).toString(), u"hash"_s);
        QCOMPARE(freshData.value(u"match"_s).toObject().value(u"refMode"_s).toString(), u"anchor"_s);
        QVERIFY(!freshData.contains(u"pack"_s));
        // at most 8 regions
        for (int i = 0; i < 12; ++i)
            QMetaObject::invokeMethod(dialog, "addRegion", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 10), Q_ARG(QVariant, 10));
        QVERIFY(QMetaObject::invokeMethod(dialog, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        QCOMPARE(QJsonObject::fromVariantMap(changes.toMap()).value(u"source"_s).toObject().value(u"data"_s).toObject().value(u"regions"_s).toArray().size(), 8);
        // saving sends it to the daemon
        const int saves = m_backend->callCount(u"SetTrigger"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accepted"));
        QTRY_COMPARE(m_backend->callCount(u"SetTrigger"_s), saves + 1);
        QCOMPARE(lastCallJson(u"SetTrigger"_s, 1).value(u"source"_s).toObject().value(u"kind"_s).toString(), u"screen"_s);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }

    void theWatchBannerFollowsTheDaemonsState()
    {
        g_warnings.clear();
        // (the page tests before this one answered Settings without the screen keys: say that the setting is on)
        m_backend->answerJson(u"Settings"_s, R"({"automationEnabled":true,"demoCardsDismissed":[],"screenTriggersEnabled":true,"screenTicksPerSecond":5})");
        m_backend->emitSignal(u"SettingsChanged"_s);
        QTRY_COMPARE(m_daemon->settings().value(u"screenTriggersEnabled"_s).toBool(), true);
        QObject *banner = load(u"ScreenWatchBanner.qml"_s);
        QVERIFY2(banner, qPrintable(g_warnings.join(u"; "_s)));
        const auto setState = [this](const QByteArray &json) {
            m_backend->setProperty(u"ScreenWatchState"_s, QString::fromUtf8(json));
            m_backend->emitSignal(u"ScreenWatchChanged"_s);
        };
        const auto messageText = [&] {
            for (QQuickItem *item : banner->findChildren<QQuickItem *>())
                if (item->property("showCloseButton").isValid())
                    return item->property("text").toString();
            return QString();
        };
        QTRY_VERIFY(messageText().contains(u"Not watching"_s));
        setState(R"({"state":"asking","reason":"","regions":0,"ticksPerSecond":0,"since":1,"paused":false,"notes":[]})");
        QTRY_VERIFY(messageText().contains(u"asking which screens"_s));
        setState(R"({"state":"watching","reason":"","regions":2,"ticksPerSecond":5,"since":2,"paused":true,"notes":["<b>screen: region outside the shared screens</b>"]})");
        QTRY_VERIFY(messageText().contains(u"Watching 2 region"_s));
        QVERIFY(messageText().contains(u"5 times a second"_s));
        QVERIFY(messageText().contains(u"Paused"_s));
        // the daemon's notes are shown as plain text
        QTRY_VERIFY([&] {
            QList<QQuickItem *> all;
            collectItems(qobject_cast<QQuickItem *>(banner), &all);
            for (QQuickItem *item : std::as_const(all))
                if (item->property("text").toString().contains(u"region outside"_s))
                    return item->property("textFormat").toInt() == int(Qt::PlainText);
            return false;
        }());
        setState(R"({"state":"refused","reason":"screen: permission refused","regions":0,"ticksPerSecond":0,"since":3,"paused":false,"notes":[]})");
        QTRY_VERIFY(messageText().contains(u"did not allow it"_s));
        setState(R"({"state":"stopped","reason":"some <i>new</i> reason","regions":0,"ticksPerSecond":0,"since":4,"paused":false,"notes":[]})");
        // an unknown reason never reaches the InlineMessage (it reads markup); a plain text label shows it
        QTRY_COMPARE(messageText(), u"Not watching."_s);
        QTRY_VERIFY([&] {
            QList<QQuickItem *> all;
            collectItems(qobject_cast<QQuickItem *>(banner), &all);
            for (QQuickItem *item : std::as_const(all))
                if (item->property("text").toString().contains(u"some <i>new</i> reason"_s))
                    return item->property("textFormat").toInt() == int(Qt::PlainText);
            return false;
        }());
        setState(R"({"state":"off","reason":"","regions":0,"ticksPerSecond":0,"since":0,"paused":false,"notes":[]})");
        QTRY_VERIFY(messageText().contains(u"Not watching"_s));
        // the buttons call the daemon
        const int starts = m_backend->callCount(u"StartScreenWatch"_s);
        m_daemon->startScreenWatch();
        QTRY_COMPARE(m_backend->callCount(u"StartScreenWatch"_s), starts + 1);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete banner;
    }

    void theCaptureDialogShowsThePicturesAndKeepsThemOnlyOnRequest()
    {
        g_warnings.clear();
        QObject *dialog = load(u"ReferenceCaptureDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        m_backend->setProperty(u"ScreenWatchState"_s, QStringLiteral(R"({"state":"watching","reason":"","regions":1,"ticksPerSecond":5,"since":1,"paused":false,"notes":[]})"));
        m_backend->emitSignal(u"ScreenWatchChanged"_s);
        QTRY_VERIFY(m_daemon->screenWatch().value(u"state"_s).toString() == u"watching"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, u"t3"_s), Q_ARG(QVariant, 0)));
        QCOMPARE(dialog->property("phase").toString(), u"idle"_s);
        // "Capture now" asks the daemon
        const QString onePixel = QStringLiteral("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC");
        const auto result = [&](const QString &request, const QString &trigger, const QString &data, int w = 1, int h = 1) {
            return QStringLiteral(R"({"request":"%1","trigger":"%2","region":0,"steps":[{"step":0,"width":%3,"height":%4,"png":"%5"}]})").arg(request, trigger).arg(w).arg(h).arg(data);
        };
        // each click gets its own request id from the daemon
        const auto capture = [&](const QString &request) {
            m_backend->answer(u"CaptureScreenReference"_s, request);
            const int before = m_backend->callCount(u"CaptureScreenReference"_s);
            QQuickItem *button = itemWith(dialog, "text", u"Capture now"_s);
            QMetaObject::invokeMethod(button, "clicked");
            QTRY_COMPARE(m_backend->callCount(u"CaptureScreenReference"_s), before + 1);
            QTest::qWait(30); // (the reply names the request)
        };
        QQuickItem *button = itemWith(dialog, "text", u"Capture now"_s);
        QVERIFY(button);
        capture(u"req-1"_s);
        QCOMPARE(m_backend->calls.last().second, (QVariantList{u"t3"_s, 0, 1}));
        QCOMPARE(dialog->property("phase").toString(), u"taking"_s);
        // the result of another request (a result, then an error) is not this one's
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"x"_s, result(u"x"_s, u"t3"_s, onePixel)});
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"y"_s, u"{\"request\":\"y\",\"error\":\"screen: not watching\"}"_s});
        QCOMPARE(dialog->property("phase").toString(), u"taking"_s);
        // the right one is shown, and nothing is kept yet
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-1"_s, result(u"req-1"_s, u"t3"_s, onePixel)});
        QCOMPARE(dialog->property("phase").toString(), u"shown"_s);
        QCOMPARE(dialog->property("pictures").toList().size(), 1);
        QCOMPARE(dialog->property("request").toString(), u"req-1"_s);
        QCOMPARE(m_backend->callCount(u"CommitScreenReference"_s), 0);
        // the picture is decoded no bigger than the preview, and says what it is
        QTRY_VERIFY([&] {
            const QList<QQuickItem *> all = allItems(dialog);
            for (QQuickItem *item : std::as_const(all))
                if (item->property("sourceSize").isValid() && item->property("source").toString().startsWith(u"data:image/png"_s))
                    return item->property("sourceSize").toSize() == QSize(360, 360) && item->property("asynchronous").toBool();
            return false;
        }());
        // discarding keeps nothing
        QVERIFY(QMetaObject::invokeMethod(dialog, "discard"));
        QCOMPARE(dialog->property("phase").toString(), u"idle"_s);
        QCOMPARE(m_backend->callCount(u"CommitScreenReference"_s), 0);
        QTRY_COMPARE(m_backend->callCount(u"DiscardScreenReference"_s), 1);
        QCOMPARE(m_backend->calls.last().second, (QVariantList{u"req-1"_s}));
        // a picture that is not what it says is refused whole (the header says 1x1)
        capture(u"req-4"_s);
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-4"_s, result(u"req-4"_s, u"t3"_s, onePixel, 999, 8)});
        QCOMPARE(dialog->property("phase").toString(), u"failed"_s);
        QCOMPARE(dialog->property("pictures").toList().size(), 0);
        // an error of the daemon is words, not markup: the InlineMessage says a fixed sentence and the daemon's text goes to a plain label
        capture(u"req-2"_s);
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-2"_s, QStringLiteral(R"({"request":"req-2","error":"<a href='x'>screen:</a> <img src=y> not watching"})")});
        QCOMPARE(dialog->property("phase").toString(), u"failed"_s);
        {
            const QList<QQuickItem *> all = allItems(dialog);
            bool plainLabel = false;
            for (QQuickItem *item : std::as_const(all)) {
                const QString text = item->property("text").toString();
                if (text.contains(u"<a href"_s)) {
                    QCOMPARE(item->property("textFormat").toInt(), int(Qt::PlainText));
                    plainLabel = true;
                } else if (item->property("showCloseButton").isValid()) {
                    QVERIFY2(!text.contains(u'<'), qPrintable(text));
                }
            }
            QVERIFY(plainLabel);
        }
        // and Keep commits exactly the request that was shown
        capture(u"req-3"_s);
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-3"_s, result(u"req-3"_s, u"t3"_s, onePixel)});
        QCOMPARE(dialog->property("phase").toString(), u"shown"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "keep"));
        QTRY_COMPARE(m_backend->callCount(u"CommitScreenReference"_s), 1);
        QCOMPARE(m_backend->calls.last().second, (QVariantList{u"req-3"_s}));
        // closing the preview with neither Keep nor Discard drops it on the daemon too
        capture(u"req-6"_s);
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-6"_s, result(u"req-6"_s, u"t3"_s, onePixel)});
        QCOMPARE(dialog->property("phase").toString(), u"shown"_s);
        const int dropped = m_backend->callCount(u"DiscardScreenReference"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "closed"));
        QTRY_COMPARE(m_backend->callCount(u"DiscardScreenReference"_s), dropped + 1);
        QCOMPARE(m_backend->calls.last().second, (QVariantList{u"req-6"_s}));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, u"t3"_s), Q_ARG(QVariant, 0)));
        // closing while a capture is on its way drops what comes late
        capture(u"req-5"_s);
        QCOMPARE(dialog->property("phase").toString(), u"taking"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, u"t3"_s), Q_ARG(QVariant, 0)));
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-5"_s, result(u"req-5"_s, u"t3"_s, onePixel)});
        QCOMPARE(dialog->property("phase").toString(), u"idle"_s);
        // without watching there is nothing to capture
        m_backend->setProperty(u"ScreenWatchState"_s, QStringLiteral(R"({"state":"off","reason":"","regions":0,"ticksPerSecond":0,"since":0,"paused":false,"notes":[]})"));
        m_backend->emitSignal(u"ScreenWatchChanged"_s);
        QTRY_VERIFY(m_daemon->screenWatch().value(u"state"_s).toString() == u"off"_s);
        QCOMPARE(button->isEnabled(), false);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }

    void theRegionPickerOpensNoWindowUntilAsked()
    {
        g_warnings.clear();
        QObject *picker = load(u"RegionPicker.qml"_s);
        QVERIFY2(picker, qPrintable(g_warnings.join(u"; "_s)));
        QCOMPARE(picker->property("active").toBool(), false);
        QVERIFY(QGuiApplication::allWindows().size() < 2); // (no shade window exists)
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete picker;
    }

    void thePickerOpensAShadeOnEachScreenAndRemovesThemWhenDone()
    {
        g_warnings.clear();
        QObject *picker = load(u"RegionPicker.qml"_s);
        QVERIFY2(picker, qPrintable(g_warnings.join(u"; "_s)));
        const int before = QGuiApplication::allWindows().size();
        QVERIFY(QMetaObject::invokeMethod(picker, "pick"));
        QTRY_COMPARE(QGuiApplication::allWindows().size(), before + QGuiApplication::screens().size());
        QVERIFY(QMetaObject::invokeMethod(picker, "finish"));
        QTRY_COMPARE(QGuiApplication::allWindows().size(), before);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete picker;
    }

    void thePickerPlacesTheRectangleOnTheDesktop()
    {
        g_warnings.clear();
        QObject *picker = load(u"RegionPicker.qml"_s);
        QVERIFY2(picker, qPrintable(g_warnings.join(u"; "_s)));
        const auto rect = [&](int sx, int sy, int wx, int wy, double x0, double y0, double x1, double y1) {
            QVariant r;
            const bool ok = QMetaObject::invokeMethod(picker, "desktopRect", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, sx), Q_ARG(QVariant, sy), Q_ARG(QVariant, wx), Q_ARG(QVariant, wy),
                                                      Q_ARG(QVariant, x0), Q_ARG(QVariant, y0), Q_ARG(QVariant, x1), Q_ARG(QVariant, y1));
            Q_ASSERT(ok);
            Q_UNUSED(ok);
            return (r.metaType() == QMetaType::fromType<QJSValue>() ? r.value<QJSValue>().toVariant() : r).toMap();
        };
        // a screen at the origin, dragged any way
        QCOMPARE(rect(0, 0, 0, 0, 10, 20, 110, 70), (QVariantMap{{u"x"_s, 10}, {u"y"_s, 20}, {u"w"_s, 100}, {u"h"_s, 50}}));
        QCOMPARE(rect(0, 0, 0, 0, 110, 70, 10.4, 20.4), (QVariantMap{{u"x"_s, 10}, {u"y"_s, 20}, {u"w"_s, 100}, {u"h"_s, 50}}));
        // a screen left of the origin, and one above it
        QCOMPARE(rect(-1920, 0, -1920, 0, 100, 100, 300, 200), (QVariantMap{{u"x"_s, -1820}, {u"y"_s, 100}, {u"w"_s, 200}, {u"h"_s, 100}}));
        QCOMPARE(rect(0, -1080, 0, -1080, 5, 5, 15, 25).value(u"y"_s).toInt(), -1075);
        // the compositor put the window 40 pixels below its screen's top (a panel kept out): the offset counts
        QCOMPARE(rect(1920, 0, 1920, 40, 10, 10, 20, 30).value(u"y"_s).toInt(), 50);
        // a window position that means nothing (Wayland says 0,0 for a screen that is elsewhere): the screen's own origin
        QCOMPARE(rect(1920, 0, 0, 0, 10, 10, 20, 30).value(u"x"_s).toInt(), 1930);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete picker;
    }

    void aScreenTriggerNeedsANameAndARegionToBeSaved()
    {
        g_warnings.clear();
        QObject *dialog = load(u"ScreenTriggerDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant())));
        QCOMPARE(dialog->property("saveable").toBool(), false); // no name, no region
        // (the footer's buttons exist once the dialog is open, which needs a window)
        QQuickWindow window;
        window.resize(800, 600);
        window.show();
        dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        const auto saveEnabled = [&] {
            QObject *object = nullptr;
            if (auto *footer = dialog->property("footer").value<QQuickItem *>())
                for (QQuickItem *item : allItems(footer))
                    if (item->property("text").toString() == u"Save"_s && item->property("down").isValid())
                        object = item;
            return object ? std::optional<bool>(object->property("enabled").toBool()) : std::nullopt;
        };
        QCOMPARE(saveEnabled(), std::optional<bool>(false)); // (the Save button itself is off)
        QVERIFY(QMetaObject::invokeMethod(dialog, "addRegion", Q_ARG(QVariant, 1), Q_ARG(QVariant, 2), Q_ARG(QVariant, 30), Q_ARG(QVariant, 40)));
        QCOMPARE(dialog->property("saveable").toBool(), false); // a region, still no name
        const QList<QQuickItem *> fields = allItems(dialog);
        QQuickItem *name = nullptr;
        for (QQuickItem *item : fields)
            if (item->property("placeholderText").toString().startsWith(u"For example"_s))
                name = item;
        QVERIFY(name);
        name->setProperty("text", u"   "_s);
        QCOMPARE(dialog->property("saveable").toBool(), false); // blanks are no name
        name->setProperty("text", u"Victory"_s);
        QCOMPARE(dialog->property("saveable").toBool(), true);
        QCOMPARE(saveEnabled(), std::optional<bool>(true));
        QVERIFY(QMetaObject::invokeMethod(dialog, "removeRegion", Q_ARG(QVariant, 0)));
        QCOMPARE(dialog->property("saveable").toBool(), false);
        // (a Kirigami dialog opened on a bare window, as here, reports its own x/y binding loops; nothing else may be said)
        g_warnings.erase(std::remove_if(g_warnings.begin(), g_warnings.end(), [](const QString &w) { return w.contains(u"Binding loop detected for property \"x\""_s) || w.contains(u"Binding loop detected for property \"y\""_s); }), g_warnings.end());
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }

    // The guided walk of "Try a screen trigger": every step, and leaving at any of them puts back what the walk changed.
    void theScreenDemoWalksToTheLiveScoreAndKeepsOnlyOnRequest()
    {
        g_warnings.clear();
        m_backend->answerJson(u"Settings"_s, R"({"automationEnabled":true,"demoCardsDismissed":[],"screenTriggersEnabled":false,"screenTicksPerSecond":5})");
        m_backend->emitSignal(u"SettingsChanged"_s);
        QTRY_COMPARE(m_daemon->settings().value(u"screenTriggersEnabled"_s).toBool(), false);
        m_backend->setProperty(u"ScreenWatchState"_s, QStringLiteral(R"({"state":"off","reason":"","regions":0,"ticksPerSecond":0,"since":0,"paused":false,"notes":[]})"));
        m_backend->emitSignal(u"ScreenWatchChanged"_s);
        m_backend->answer(u"SetTriggerEnabled"_s, QVariant());
        const QByteArray fixtureTriggers = m_backend->replies.value(u"ListTriggers"_s).value.toString().toUtf8();
        // (what the daemon lists once the walk made its example: SetTrigger answers t9)
        const auto listWithExample = [&](bool demo) {
            QJsonArray list = QJsonDocument::fromJson(fixtureTriggers).array();
            QJsonObject example = QJsonDocument::fromJson(R"({"id":"t9","name":"Example: hide","enabled":true,"cooldownMs":500,"mode":"press","source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":100,"y":100,"w":320,"h":180}],"match":{"mode":"hash","threshold":0.65}}},"action":{"type":"hide-all"}})").object();
            if (demo)
                example.insert(u"origin"_s, u"demo"_s);
            list.append(example);
            m_backend->answerJson(u"ListTriggers"_s, QJsonDocument(list).toJson(QJsonDocument::Compact));
            m_backend->emitSignal(u"TriggersChanged"_s);
        };
        const auto journalHas = [&](const char *key) {
            QSettings journal(m_journalDir.filePath(u"screen-demo.ini"_s), QSettings::IniFormat);
            return journal.value(QLatin1String(key));
        };
        QObject *picker = load(u"RegionPicker.qml"_s);
        QVERIFY(picker);
        QObject *dialog = loadWith(u"ScreenDemoDialog.qml"_s, {{u"regionPicker"_s, QVariant::fromValue(picker)}});
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        const auto step = [&] { return dialog->property("step").toString(); };
        const auto count = [&](const char *method) { return m_backend->callCount(QString::fromLatin1(method)); };
        const auto click = [&](const QString &text) {
            QQuickItem *button = itemWith(dialog, "text", text);
            QVERIFY2(button, qPrintable(text));
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        };
        const auto setWatch = [&](const char *state, const char *reason = "") {
            m_backend->setProperty(u"ScreenWatchState"_s, QStringLiteral(R"({"state":"%1","reason":"%2","regions":1,"ticksPerSecond":5,"since":1,"paused":false,"notes":[]})").arg(QLatin1String(state), QLatin1String(reason)));
            m_backend->emitSignal(u"ScreenWatchChanged"_s);
            QTRY_COMPARE(m_daemon->screenWatch().value(u"state"_s).toString(), QString::fromLatin1(state));
        };

        // a walk that is given up in the middle (cancel while the desktop asks): the example is removed, the setting goes back, the watch stops
        listWithExample(true);
        QTRY_COMPARE(m_daemon->triggers().size(), 5);
        QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
        QCOMPARE(step(), u"intro"_s);
        const int sets = count("SetTrigger"), deletes = count("DeleteTrigger"), stops = count("StopScreenWatch");
        click(u"Begin"_s);
        QTRY_COMPARE(step(), u"start"_s);
        QCOMPARE(count("SetTrigger"), sets + 1);
        QCOMPARE(lastCallJson(u"SetTrigger"_s, 1).value(u"enabled"_s).toBool(), false); // (it is made OFF)
        QCOMPARE(lastCallJson(u"SetTrigger"_s, 1).value(u"origin"_s).toString(), u"demo"_s);
        QCOMPARE(dialog->property("triggerId").toString(), u"t9"_s);
        QTRY_VERIFY(std::any_of(m_backend->calls.cbegin(), m_backend->calls.cend(), [](const auto &c) { return c.first == u"SetTriggerEnabled"_s && c.second == QVariantList{u"t9"_s, true}; }));
        QCOMPARE(lastCallJson(u"SetSettings"_s, 0).value(u"screenTriggersEnabled"_s).toBool(), true);
        // (the journal of what the walk changed was written first: a GUI that dies now is put right by the next one, see tst_daemonclient)
        QCOMPARE(journalHas("trigger").toString(), u"t9"_s);
        QCOMPARE(journalHas("settingTurnedOn").toBool(), true);
        QCOMPARE(journalHas("watchStarted").toBool(), false);
        click(u"Start watching"_s);
        QCOMPARE(step(), u"asking"_s);
        QTRY_COMPARE(count("StartScreenWatch") >= 1, true);
        QCOMPARE(journalHas("watchStarted").toBool(), true);
        setWatch("asking");
        QVERIFY(QMetaObject::invokeMethod(dialog, "settle", Q_ARG(QVariant, false)));
        QTRY_COMPARE(count("DeleteTrigger"), deletes + 1);
        QTRY_COMPARE(count("StopScreenWatch"), stops + 1);
        QTRY_COMPARE(lastCallJson(u"SetSettings"_s, 0).value(u"screenTriggersEnabled"_s).toBool(), false); // (it had been off)
        QTRY_VERIFY(!journalHas("trigger").isValid()); // (nothing left to put back)
        setWatch("off");

        // the daemon turns the start down (no helper, a locked session): the walk ends with the reason instead of waiting for a desktop that was never asked
        QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
        click(u"Begin"_s);
        QTRY_COMPARE(step(), u"start"_s);
        m_backend->fail(u"StartScreenWatch"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"murasaki-screenmatch was not found"_s);
        click(u"Start watching"_s);
        QTRY_COMPARE(step(), u"failed"_s);
        QCOMPARE(dialog->property("problem").toString(), u"murasaki-screenmatch was not found"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "settle", Q_ARG(QVariant, false)));
        m_backend->answer(u"StartScreenWatch"_s, QVariant());
        QTRY_VERIFY(!journalHas("trigger").isValid());

        // the watch goes back to off while the desktop is asked (the daemon restarted, the rescue): the same, no spinner for good
        QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
        click(u"Begin"_s);
        QTRY_COMPARE(step(), u"start"_s);
        click(u"Start watching"_s);
        setWatch("asking");
        QCOMPARE(step(), u"asking"_s);
        setWatch("off");
        QTRY_COMPARE(step(), u"failed"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "settle", Q_ARG(QVariant, false)));
        QTRY_VERIFY(!journalHas("trigger").isValid());

        // the desktop says no
        QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
        click(u"Begin"_s);
        QTRY_COMPARE(step(), u"start"_s);
        click(u"Start watching"_s);
        setWatch("refused", "screen: permission refused");
        QTRY_COMPARE(step(), u"failed"_s);
        QVERIFY(QMetaObject::invokeMethod(dialog, "settle", Q_ARG(QVariant, false)));
        QTRY_VERIFY(!journalHas("trigger").isValid());
        setWatch("off");

        // the window of the walk goes away while the picture dialog is still open: that ends the walk too (watch stopped, example deleted, setting back)
        {
            QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
            click(u"Begin"_s);
            QTRY_COMPARE(step(), u"start"_s);
            click(u"Start watching"_s);
            setWatch("watching");
            QTRY_COMPARE(step(), u"region"_s);
            QVERIFY(QMetaObject::invokeMethod(dialog, "chooseRegion"));
            QMetaObject::invokeMethod(picker, "finish");
            QVERIFY(QMetaObject::invokeMethod(picker, "picked", Q_ARG(int, 11), Q_ARG(int, 22), Q_ARG(int, 330), Q_ARG(int, 140)));
            QCOMPARE(step(), u"capture"_s);
            click(u"Take the picture…"_s);
            QQuickItem *capturing = itemWith(dialog, "text", u"Capture now"_s);
            QVERIFY(capturing);
            QVERIFY(QMetaObject::invokeMethod(capturing, "clicked"));
            QTest::qWait(30);
            const int deletesNow = count("DeleteTrigger"), stopsNow = count("StopScreenWatch");
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QTRY_COMPARE(count("DeleteTrigger"), deletesNow + 1);
            QTRY_COMPARE(count("StopScreenWatch"), stopsNow + 1);
            QVERIFY(dialog->property("finished").toBool());
            QTRY_VERIFY(!journalHas("trigger").isValid());
            setWatch("off");
        }

        // the whole walk: watching, region, picture (seen and kept), live score, and "Keep this trigger"
        QVERIFY(QMetaObject::invokeMethod(dialog, "begin"));
        click(u"Begin"_s);
        QTRY_COMPARE(step(), u"start"_s);
        click(u"Start watching"_s);
        setWatch("watching");
        QTRY_COMPARE(step(), u"region"_s);
        // the user draws the region
        QVERIFY(QMetaObject::invokeMethod(dialog, "chooseRegion"));
        QVERIFY(dialog->property("picking").toBool());
        QMetaObject::invokeMethod(picker, "finish");
        QVERIFY(QMetaObject::invokeMethod(picker, "picked", Q_ARG(int, 11), Q_ARG(int, 22), Q_ARG(int, 330), Q_ARG(int, 140)));
        QCOMPARE(step(), u"capture"_s);
        const QJsonObject region = lastCallJson(u"SetTrigger"_s, 1).value(u"source"_s).toObject().value(u"data"_s).toObject().value(u"regions"_s).toArray().at(0).toObject();
        QCOMPARE(region.value(u"x"_s).toInt(), 11);
        QCOMPARE(region.value(u"w"_s).toInt(), 330);
        QCOMPARE(lastCallJson(u"SetTrigger"_s, 1).value(u"source"_s).toObject().value(u"data"_s).toObject().value(u"match"_s).toObject().value(u"threshold"_s).toDouble(), 0.65); // (what it had stays)
        // the picture: shown before it is kept; kept, the walk goes to the live score
        m_backend->answer(u"CaptureScreenReference"_s, u"req-d"_s);
        click(u"Take the picture…"_s);
        QQuickItem *capture = itemWith(dialog, "text", u"Capture now"_s);
        QVERIFY(capture);
        QVERIFY(QMetaObject::invokeMethod(capture, "clicked"));
        QTest::qWait(30);
        m_backend->emitSignal(u"ScreenCaptured"_s, {u"req-d"_s, QStringLiteral(R"({"request":"req-d","trigger":"t9","region":0,"steps":[{"step":0,"width":1,"height":1,"png":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}]})")});
        QQuickItem *keep = itemWith(dialog, "text", u"Keep"_s);
        QVERIFY(keep);
        const int commits = count("CommitScreenReference");
        QCOMPARE(commits, count("CommitScreenReference"));
        QVERIFY(QMetaObject::invokeMethod(keep, "clicked"));
        QTRY_COMPARE(count("CommitScreenReference"), commits + 1);
        QTRY_COMPARE(step(), u"live"_s);
        m_backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t9","enabled":true,"available":true,"refs":[1],"screen":{"score":[870],"detected":true,"step":0,"active":true}}],"log":[]})");
        m_daemon->refreshTriggerStates();
        QTRY_COMPARE(dialog->property("score").toInt(), 870);
        QVERIFY(dialog->property("matching").toBool());
        // keeping it: the watch stops (Start is pressed each session), the example and the setting stay
        const int deletesBefore = count("DeleteTrigger"), stopsBefore = count("StopScreenWatch"), settingsBefore = count("SetSettings");
        click(u"Keep this trigger"_s);
        QTRY_COMPARE(count("StopScreenWatch"), stopsBefore + 1);
        QCOMPARE(count("DeleteTrigger"), deletesBefore);
        QCOMPARE(count("SetSettings"), settingsBefore);
        // it is the user's now: the demo marker is cleared (an explicit empty origin), so "Remove examples" never takes it, and there is nothing left to put back
        const QJsonObject kept = lastCallJson(u"SetTrigger"_s, 1);
        QVERIFY(kept.contains(u"origin"_s));
        QCOMPARE(kept.value(u"origin"_s).toString(), QString());
        const auto lastSet = std::find_if(m_backend->calls.crbegin(), m_backend->calls.crend(), [](const auto &c) { return c.first == u"SetTrigger"_s; });
        QCOMPARE(lastSet->second.at(0).toString(), u"t9"_s);
        QVERIFY(!journalHas("trigger").isValid());
        // the ids of the first walk came back as the same t9: restore the fixture
        m_backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":false,"available":true,"outcome":"disabled"}],"log":[]})");
        m_backend->answerJson(u"ListTriggers"_s, fixtureTriggers);
        m_backend->emitSignal(u"TriggersChanged"_s);
        QTRY_COMPARE(m_daemon->triggers().size(), 4);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
        delete picker;
    }

    void withAnOldDaemonNothingOfTheScreenIsOffered()
    {
        g_warnings.clear();
        // revision 20: no screen triggers, no examples. (The singleton is shared: restore it for the tests after this one.)
        const QScopeGuard restore([this] { // (also when an assertion below fails: the other tests need the daemon they were written for)
            m_backend->setProperty(u"ApiVersion"_s, 23u);
            m_backend->setAvailable(false);
            m_backend->setAvailable(true);
            QTest::qWait(200); // (the daemon is read again before the next test)
        });
        m_backend->setProperty(u"ApiVersion"_s, 20u);
        m_backend->setAvailable(false);
        m_backend->setAvailable(true);
        QTRY_VERIFY(!m_daemon->screenSupported());
        QVERIFY(!m_daemon->demosSupported());
        QVERIFY(!m_daemon->idleBackSupported());
        QObject *page = load(u"TriggersPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        // (visibility only means something inside a shown window)
        QQuickWindow window;
        window.resize(900, 700);
        qobject_cast<QQuickItem *>(page)->setParentItem(window.contentItem());
        window.show();
        const int calls = m_backend->calls.size();
        // the banner, the card of the screen section, "New screen trigger" and "Try a screen trigger" are all hidden by the revision
        // (each part by name: true when any item of that name is visible)
        const auto screenParts = [&] {
            QMap<QString, bool> parts;
            const auto note = [&parts](const QString &name, bool visible) { parts[name] = parts.value(name, false) || visible; };
            for (QQuickItem *item : page->findChildren<QQuickItem *>()) {
                if (item->property("screenTriggerCount").isValid())
                    note(u"banner"_s, item->isVisible());
                if (item->property("kind").toString() == u"screen"_s && item->property("closedNow").isValid())
                    note(u"card"_s, item->isVisible());
            }
            for (QObject *object : page->findChildren<QObject *>()) {
                const QString text = object->property("text").toString();
                if (text == u"New screen trigger…"_s || text == u"Try a screen trigger"_s)
                    note(text, object->property("visible").toBool());
            }
            return parts;
        };
        const auto oldParts = screenParts();
        QCOMPARE(oldParts.keys(), (QStringList{u"New screen trigger…"_s, u"Try a screen trigger"_s, u"banner"_s, u"card"_s}));
        for (auto it = oldParts.cbegin(); it != oldParts.cend(); ++it)
            QVERIFY2(!it.value(), qPrintable(it.key()));
        // the dialog's walk cannot start it either: asking for the example says so and sends nothing
        QSignalSpy errors(m_daemon, &DaemonClient::error);
        m_daemon->runDemo(u"screen"_s, {});
        m_daemon->startScreenWatch();
        QCOMPARE(errors.size(), 2);
        for (const char *method : {"SetTrigger", "StartScreenWatch", "CaptureScreenReference"})
            for (int i = calls; i < m_backend->calls.size(); ++i)
                QVERIFY2(m_backend->calls.at(i).first != QString::fromLatin1(method), method);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        // back to the daemon the other tests talk to, and the same parts are there (the pieces above were not just always hidden)
        m_backend->setProperty(u"ApiVersion"_s, 23u);
        m_backend->setAvailable(false);
        m_backend->setAvailable(true);
        QTRY_VERIFY(m_daemon->screenSupported());
        QTRY_VERIFY(m_daemon->demosSupported());
        QTRY_VERIFY([&] {
            const auto parts = screenParts();
            return parts.size() == 4 && parts.value(u"card"_s) && parts.value(u"New screen trigger…"_s) && parts.value(u"Try a screen trigger"_s);
        }());
        delete page;
    }

    // The editor of the Automations area: every condition and action reads as a sentence, the technical part starts closed, and what
    // it saves is what the trigger dialog saved.
    void theAutomationEditorReadsAsASentenceAndBuildsATrigger()
    {
        g_warnings.clear();
        QObject *editor = load(u"AutomationEditor.qml"_s);
        QVERIFY2(editor, qPrintable(g_warnings.join(u"; "_s)));
        QVERIFY(QMetaObject::invokeMethod(editor, "openFor", Q_ARG(QVariant, QVariant())));
        // "More options" is closed by default, and the window class typed by hand is in it
        QQuickItem *more = itemWith(editor, "text", u"More options"_s);
        QVERIFY(more);
        QCOMPARE(more->property("checked").toBool(), false);
        QQuickItem *classField = itemWith(editor, "placeholderText", u"A window class such as firefox (* and ? match)"_s);
        QVERIFY(classField);
        QVERIFY(!classField->isVisible());

        // every condition and every action: a sentence without holes
        const QStringList kinds{u"app-open"_s, u"app-focus"_s, u"game"_s, u"fullscreen"_s, u"power"_s, u"time"_s, u"interval"_s, u"idle"_s,
                          u"session"_s, u"output"_s, u"shortcut"_s, u"signal-file"_s, u"fire"_s};
        const QStringList actions{u"hide-all"_s, u"show-all"_s, u"toggle-hidden"_s, u"mute-on"_s, u"mute-off"_s, u"mute-toggle"_s, u"stop-sounds"_s,
                            u"play-sound"_s, u"timeline"_s, u"profile"_s, u"scene"_s};
        QSet<QString> sentences;
        for (const QString &kind : std::as_const(kinds)) {
            QVERIFY(QMetaObject::invokeMethod(editor, "chooseKind", Q_ARG(QVariant, kind)));
            for (const QString &action : std::as_const(actions)) {
                editor->setProperty("actionKey", action);
                const QString sentence = editor->property("sentence").toString();
                QVERIFY2(sentence.startsWith(u"When "_s), qPrintable(kind + u"/"_s + action + u": "_s + sentence));
                QVERIFY2(!sentence.contains(u"undefined"_s) && !sentence.contains(u"null"_s) && !sentence.contains(u"%"_s),
                         qPrintable(kind + u"/"_s + action + u": "_s + sentence));
                sentences.insert(sentence);
            }
        }
        QVERIFY(sentences.size() >= kinds.size() * actions.size() - 4); // (the shortcut's key id differs per build)

        // an application from the installed list: its name in the sentence, its window class in the trigger
        QVERIFY(QMetaObject::invokeMethod(editor, "chooseKind", Q_ARG(QVariant, u"app-open"_s)));
        editor->setProperty("actionKey", u"mute-on"_s);
        editor->setProperty("appClass", QString());
        QVERIFY(!editor->property("missing").toString().isEmpty()); // no application yet: Save waits
        editor->setProperty("appClass", u"discord"_s);
        QCOMPARE(editor->property("missing").toString(), QString());
        QCOMPARE(editor->property("sentence").toString(), u"When I open Discord, mute the pets"_s);
        QVariant changes;
        QVERIFY(QMetaObject::invokeMethod(editor, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        const QJsonObject built = QJsonObject::fromVariantMap(changes.value<QJSValue>().toVariant().toMap());
        QCOMPARE(built.value(u"name"_s).toString(), u"When I open Discord, mute the pets"_s); // no name given: the sentence
        QCOMPARE(built.value(u"source"_s).toObject(), (QJsonObject{{u"kind"_s, u"app-open"_s}, {u"app"_s, u"discord"_s}, {u"on"_s, u"open"_s}}));
        QCOMPARE(built.value(u"action"_s).toObject(), (QJsonObject{{u"type"_s, u"mute"_s}, {u"mode"_s, u"on"_s}}));
        QCOMPARE(built.value(u"mode"_s).toString(), u"press"_s);

        // a profile's id is not a timeline's: switching the action asks for a timeline instead of saving a dangling id
        QVariantMap profileTrigger = QJsonDocument::fromJson(R"({"id":"t7","name":"P","enabled":true,"cooldownMs":500,"mode":"press",
            "source":{"kind":"game","state":"started"},"action":{"type":"profile","id":"p1"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(editor, "openFor", Q_ARG(QVariant, QVariant(profileTrigger))));
        QCOMPARE(editor->property("missing").toString(), QString());
        editor->setProperty("actionKey", u"timeline"_s);
        QVERIFY(!editor->property("missing").toString().isEmpty());
        // a sound action that names its pets by item keeps the item when no pet is chosen
        QVariantMap itemTrigger = QJsonDocument::fromJson(R"({"id":"t8","name":"S","enabled":true,"cooldownMs":500,"mode":"press",
            "source":{"kind":"game","state":"started"},"action":{"type":"play-sound","item":"local:abc"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(editor, "openFor", Q_ARG(QVariant, QVariant(itemTrigger))));
        QCOMPARE(editor->property("missing").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(editor, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        QCOMPARE(QJsonObject::fromVariantMap(changes.value<QJSValue>().toVariant().toMap()).value(u"action"_s).toObject(),
                 (QJsonObject{{u"type"_s, u"play-sound"_s}, {u"item"_s, u"local:abc"_s}}));
        // "when I come back" reads and builds as it was (revision 23)
        const QVariantMap backTrigger = QJsonDocument::fromJson(R"({"id":"t8","name":"Back","enabled":true,"cooldownMs":500,"mode":"press",
            "source":{"kind":"idle","minutes":5,"state":"back"},"action":{"type":"show-all"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(editor, "openFor", Q_ARG(QVariant, QVariant(backTrigger))));
        QVERIFY2(editor->property("sentence").toString().contains(u"I come back after 5 minute"_s), qPrintable(editor->property("sentence").toString()));
        QVERIFY(QMetaObject::invokeMethod(editor, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        QCOMPARE(QJsonObject::fromVariantMap(changes.value<QJSValue>().toVariant().toMap()).value(u"source"_s).toObject(),
                 (QJsonObject{{u"kind"_s, u"idle"_s}, {u"minutes"_s, 5}, {u"state"_s, u"back"_s}}));
        // back to the Discord automation for the save below
        QVERIFY(QMetaObject::invokeMethod(editor, "openFor", Q_ARG(QVariant, QVariant())));
        editor->setProperty("actionKey", u"mute-on"_s);
        editor->setProperty("appClass", u"discord"_s);

        // saving sends it to the daemon as a new trigger (and the answer is waited for: it is not the next test's)
        const int before = m_backend->callCount(u"SetTrigger"_s);
        QSignalSpy saved(m_daemon, &DaemonClient::triggerSaved);
        QVERIFY(QMetaObject::invokeMethod(editor, "accepted"));
        QTRY_COMPARE(m_backend->callCount(u"SetTrigger"_s), before + 1);
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(lastCallJson(u"SetTrigger"_s, 1).value(u"source"_s).toObject().value(u"app"_s).toString(), u"discord"_s);
        delete editor;
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
    }

    // The wallpaper rules of the Automations page are today's settings, and an idea for the pets is a trigger.
    // The rows say when a rule acts now and when an automation last ran; ideas say they are not on yet.
    void theAutomationsPageSaysWhatActsNow()
    {
        g_warnings.clear();
        QObject *page = load(u"AutomationsPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        QVariant text;
        QVERIFY(QMetaObject::invokeMethod(page, "actingNow", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, u"game"_s)));
        QVERIFY2(text.toString().contains(u"DP-1"_s) && !text.toString().contains(u"HDMI"_s), qPrintable(text.toString()));
        QVERIFY(QMetaObject::invokeMethod(page, "actingNow", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, u"covered"_s)));
        QVERIFY2(text.toString().contains(u"HDMI-A-1"_s) && !text.toString().contains(u"DP-1"_s), qPrintable(text.toString()));
        QVERIFY(QMetaObject::invokeMethod(page, "actingNow", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, u"apps"_s)));
        QCOMPARE(text.toString(), QString()); // nothing paused for an application
        // t4 ran 4 s ago; t1 never ran
        m_backend->answerJson(u"TriggerState"_s, R"({"states":[{"id":"t1","enabled":true,"available":true,"outcome":""},
            {"id":"t4","enabled":true,"available":true,"outcome":"ran","agoMs":4000}],"log":[]})");
        m_daemon->refreshTriggerStates();
        QTRY_COMPARE(m_daemon->triggerStates().value(u"states"_s).toList().size(), 2);
        QVERIFY(QMetaObject::invokeMethod(page, "lastRunFor", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, u"t4"_s)));
        QVERIFY2(text.toString().startsWith(u"Last ran"_s), qPrintable(text.toString()));
        QVERIFY(QMetaObject::invokeMethod(page, "lastRunFor", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, u"t1"_s)));
        QCOMPARE(text.toString(), QString());
        delete page;
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
    }

    void theAutomationsPageWritesTheSettingsAndAddsIdeas()
    {
        g_warnings.clear();
        QObject *page = load(u"AutomationsPage.qml"_s);
        QVERIFY2(page, qPrintable(g_warnings.join(u"; "_s)));
        // the fixture's daemon has no pauseOnGame: the default (on) is one of "my automations"
        QVERIFY(page->property("gameOn").toBool());
        const int settings = m_backend->callCount(u"SetSettings"_s);
        QVERIFY(QMetaObject::invokeMethod(page, "setRule", Q_ARG(QVariant, u"game"_s), Q_ARG(QVariant, false)));
        QTRY_COMPARE(m_backend->callCount(u"SetSettings"_s), settings + 1);
        QCOMPARE(lastCallJson(u"SetSettings"_s, 0), (QJsonObject{{u"pauseOnGame"_s, false}}));
        QVERIFY(QMetaObject::invokeMethod(page, "setRule", Q_ARG(QVariant, u"battery"_s), Q_ARG(QVariant, true)));
        QTRY_COMPARE(lastCallJson(u"SetSettings"_s, 0), (QJsonObject{{u"batteryMode"_s, u"lowfps"_s}}));
        QVERIFY(QMetaObject::invokeMethod(page, "setRule", Q_ARG(QVariant, u"covered"_s), Q_ARG(QVariant, false)));
        QTRY_COMPARE(lastCallJson(u"SetSettings"_s, 0), (QJsonObject{{u"pauseWhenCovered"_s, false}}));

        const QVariantList ideas = page->property("petIdeas").toList();
        const int triggers = m_backend->callCount(u"SetTrigger"_s);
        QSignalSpy saved(m_daemon, &DaemonClient::triggerSaved);
        // the game idea hides the pets while a game runs: one trigger hides them, a second shows them when it ends
        QVariant play;
        for (const QVariant &idea : ideas)
            if (idea.toMap().value(u"key"_s) == u"play"_s)
                play = idea;
        QVERIFY(play.isValid());
        QVERIFY(QMetaObject::invokeMethod(page, "addIdea", Q_ARG(QVariant, play)));
        QTRY_COMPARE(m_backend->callCount(u"SetTrigger"_s), triggers + 2);
        QTRY_COMPARE(saved.size(), 2);
        const QJsonObject hide = lastCallJson(u"SetTrigger"_s, 1, 1);
        const QJsonObject show = lastCallJson(u"SetTrigger"_s, 1);
        QCOMPARE(hide.value(u"source"_s).toObject(), (QJsonObject{{u"kind"_s, u"game"_s}, {u"state"_s, u"started"_s}}));
        QCOMPARE(hide.value(u"action"_s).toObject().value(u"type"_s).toString(), u"hide-all"_s);
        QCOMPARE(show.value(u"source"_s).toObject(), (QJsonObject{{u"kind"_s, u"game"_s}, {u"state"_s, u"ended"_s}}));
        QCOMPARE(show.value(u"action"_s).toObject().value(u"type"_s).toString(), u"show-all"_s);
        QVERIFY(hide.value(u"name"_s).toString() != show.value(u"name"_s).toString());
        delete page;
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
    }

    void theTriggerDialogBuildsWhatTheDaemonChecks()
    {
        QObject *dialog = load(u"TriggerDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        QVariantMap existing = QJsonDocument::fromJson(R"({"id":"t5","name":"Night","enabled":true,"cooldownMs":900,"mode":"press",
            "source":{"kind":"time","at":"23:30","days":[1,5]},"action":{"type":"mute","mode":"on"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(existing))));
        QVariant changes;
        QVERIFY(QMetaObject::invokeMethod(dialog, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        const QVariantMap map = changes.toMap();
        QCOMPARE(map.value(u"name"_s).toString(), u"Night"_s);
        QCOMPARE(map.value(u"cooldownMs"_s).toInt(), 900);
        QCOMPARE(map.value(u"source"_s).toMap().value(u"kind"_s).toString(), u"time"_s);
        QCOMPARE(map.value(u"source"_s).toMap().value(u"at"_s).toString(), u"23:30"_s);
        QCOMPARE(map.value(u"source"_s).toMap().value(u"days"_s).toList(), (QVariantList{1, 5}));
        QCOMPARE(map.value(u"action"_s).toMap().value(u"type"_s).toString(), u"mute"_s);
        QCOMPARE(map.value(u"action"_s).toMap().value(u"mode"_s).toString(), u"on"_s);
        // a hold shortcut keeps its mode
        existing = QJsonDocument::fromJson(R"({"id":"t6","name":"Peek","enabled":true,"cooldownMs":500,"mode":"hold","source":{"kind":"shortcut","id":"peek"},"action":{"type":"hide-all"}})").object().toVariantMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(existing))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "buildChanges", Q_RETURN_ARG(QVariant, changes)));
        QCOMPARE(changes.toMap().value(u"mode"_s).toString(), u"hold"_s);
        QCOMPARE(changes.toMap().value(u"source"_s).toMap().value(u"id"_s).toString(), u"peek"_s);
        // a new one is sent with an empty id (the calls of the tests before this one are not its)
        m_backend->calls.clear();
        QSignalSpy saved(m_daemon, &DaemonClient::triggerSaved);
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant())));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accepted"));
        QTRY_COMPARE(saved.size(), 1);
        for (const auto &call : std::as_const(m_backend->calls))
            if (call.first == u"SetTrigger"_s)
                QCOMPARE(call.second.value(0).toString(), QString());
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }
    void theTimelineEditorKeepsTheDataItIsGiven()
    {
        g_warnings.clear();
        QObject *dialog = load(u"TimelineDialog.qml"_s);
        QVERIFY2(dialog, qPrintable(g_warnings.join(u"; "_s)));
        const QVariantMap existing = m_daemon->timelines().first().toMap();
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QVariant(existing))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "addTrack"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "addAction", Q_ARG(QVariant, 0), Q_ARG(QVariant, QVariant(u"hide"_s))));
        const QVariantMap draft = dialog->property("draft").toMap();
        QCOMPARE(draft.value(u"tracks"_s).toList().size(), 4);
        QCOMPARE(draft.value(u"tracks"_s).toList().at(0).toMap().value(u"actions"_s).toList().size(), 4);
        QSignalSpy saved(m_daemon, &DaemonClient::timelineSaved);
        QVERIFY(QMetaObject::invokeMethod(dialog, "save", Q_ARG(QVariant, QVariant(false))));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u"; "_s)));
        delete dialog;
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_ACCESSIBILITY", "0");
    QGuiApplication app(argc, argv);
    TstAutomationPages test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_automationpages.moc"
