// Wallpaper.qml's switching logic: the real file, copied next to stand-in delegates, driven offscreen. The old content stays
// visible and animating until the new one reports a first frame (or a timeout), then they crossfade; a preview only stands
// in when nothing was shown before.

#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

void write(const QString &path, const QByteArray &text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

QList<QQuickItem *> slotItems(QQuickItem *root)
{
    QList<QQuickItem *> out;
    for (QQuickItem *c : root->childItems()) {
        if (c->property("revealed").isValid())
            out << c;
    }
    return out;
}

QVariantMap scene(const QString &key)
{
    return {{u"key"_s, key}, {u"kind"_s, u"scene"_s}, {u"itemId"_s, key}, {u"itemDir"_s, u"/x"_s}, {u"assetsDir"_s, u"/a"_s},
            {u"previewUrl"_s, u"file:///nonexistent.png"_s}, {u"fill"_s, u"fill"_s}, {u"properties"_s, QVariantMap{}}, {u"sceneEngine"_s, u"auto"_s}};
}

} // namespace

class TstWallpaperSwitch : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QQmlEngine m_engine;
    std::unique_ptr<QQuickItem> m_root;

    // The scene delegate of a slot (the stand-in with hasFrame).
    static QObject *delegateOf(QQuickItem *slot)
    {
        for (QQuickItem *c : slot->childItems()) {
            if (c->property("hasFrame").isValid())
                return c;
        }
        return nullptr;
    }

    void load(int timeoutMs = 10000)
    {
        QQmlComponent component(&m_engine, QUrl::fromLocalFile(m_dir.filePath(u"Wallpaper.qml"_s)));
        QVERIFY2(component.status() == QQmlComponent::Ready, qPrintable(component.errorString()));
        m_root.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(m_root);
        m_root->setWidth(200);
        m_root->setHeight(100);
        m_root->setProperty("switchTimeout", timeoutMs);
    }

    void setContent(const QVariantMap &c) { m_root->setProperty("content", c.isEmpty() ? QVariant() : QVariant(c)); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QFile source(QString::fromUtf8(MURASAKI_WALLPAPER_QML));
        QVERIFY(source.open(QIODevice::ReadOnly));
        write(m_dir.filePath(u"Wallpaper.qml"_s), source.readAll());
        QFile preview(QString::fromUtf8(MURASAKI_PREVIEWITEM_QML));
        QVERIFY(preview.open(QIODevice::ReadOnly));
        write(m_dir.filePath(u"PreviewItem.qml"_s), preview.readAll());
        write(m_dir.filePath(u"SceneItem.qml"_s),
              "import QtQuick\nItem { property string itemDir; property string assetsDir; property string outputName; property string sceneEngine; property string fill\n"
              "property var properties; property bool sandbox; property bool paused; property bool audible; property real volume; property int fps; property var span\n"
              "property bool hasFrame: false\nproperty string engineLabel: \"Vulkan\"\nsignal engineUpgradeAvailable()\nsignal failed(string reason) }\n");
        write(m_dir.filePath(u"VideoItem.qml"_s),
              "import QtQuick\nItem { property string source; property string fill; property var span; property real frameRateLimit; property bool paused; property bool audible\n"
              "signal started()\nsignal failed(string reason) }\n");
        write(m_dir.filePath(u"WebItem.qml"_s),
              "import QtQuick\nItem { property var profile; property string script; property url contentUrl; property bool networkAllowed; property string proxyToken\n"
              "property bool paused; property bool audible; property real refreshRate; property var audioSpectrum; property bool wantsAudio; property bool ready: false\n"
              "signal failed(string reason) }\n");
    }

    // Nothing drawn within noFrameTimeoutMs: the surface is told, and the preview under the scene is dimmed and marked.
    void aSceneWithoutAFirstFrameIsReportedAndDimmedThenClearedByALateFrame()
    {
        load();
        QVariantMap content = scene(u"a"_s);
        content.insert(u"noFrameTimeoutMs"_s, 100);
        QSignalSpy health(m_root.get(), SIGNAL(health(QString,QString,QString,QString)));
        setContent(content);
        QVERIFY(!m_root->property("failed").toBool());
        QTRY_VERIFY(m_root->property("failed").toBool());
        QCOMPARE(health.count(), 1);
        QCOMPARE(health.at(0).at(0).toString(), u"a"_s);
        QCOMPARE(health.at(0).at(1).toString(), u"failed"_s);
        QVERIFY(health.at(0).at(2).toString().contains(u"first frame"_s));
        QCOMPARE(health.at(0).at(3).toString(), u"Vulkan"_s);

        // It was only slow: the frame arrives, the mark goes, the surface is told.
        delegateOf(slotItems(m_root.get())[0])->setProperty("hasFrame", true);
        QTRY_VERIFY(!m_root->property("failed").toBool());
        QCOMPARE(health.count(), 2);
        QCOMPARE(health.at(1).at(1).toString(), u"frame"_s);
    }

    // Started paused (a game runs): nothing is drawn, and that is not a failure. The wait starts when the pause ends.
    void aPausedSceneIsNotReportedUntilItPlays()
    {
        load();
        QVariantMap content = scene(u"a"_s);
        content.insert(u"noFrameTimeoutMs"_s, 150);
        QSignalSpy health(m_root.get(), SIGNAL(health(QString,QString,QString,QString)));
        m_root->setProperty("paused", true);
        setContent(content);
        QTest::qWait(500);
        QVERIFY(!m_root->property("failed").toBool());
        QCOMPARE(health.count(), 0);
        m_root->setProperty("paused", false);
        QTRY_VERIFY(m_root->property("failed").toBool());
        QCOMPARE(health.count(), 1);
    }

    void aSceneThatDrawsInTimeIsNeverReported()
    {
        load();
        QVariantMap content = scene(u"a"_s);
        content.insert(u"noFrameTimeoutMs"_s, 150);
        QSignalSpy health(m_root.get(), SIGNAL(health(QString,QString,QString,QString)));
        setContent(content);
        delegateOf(slotItems(m_root.get())[0])->setProperty("hasFrame", true);
        QTest::qWait(400);
        QVERIFY(!m_root->property("failed").toBool());
        for (const auto &args : health)
            QCOMPARE(args.at(1).toString(), u"frame"_s);
    }

    // The delegate gave up (refused, or crashed for good): its preview stands in, dimmed and marked, with the reason.
    void aDelegateThatGivesUpLeavesTheDimmedPreviewAndItsReason()
    {
        load();
        setContent(scene(u"a"_s));
        QSignalSpy health(m_root.get(), SIGNAL(health(QString,QString,QString,QString)));
        QObject *delegate = delegateOf(slotItems(m_root.get())[0]);
        QVERIFY(QMetaObject::invokeMethod(delegate, "failed", Q_ARG(QString, u"scene engine exited with code 1"_s)));
        QVERIFY(m_root->property("failed").toBool());
        QCOMPARE(health.count(), 1);
        QCOMPARE(health.at(0).at(0).toString(), u"a"_s);
        QCOMPARE(health.at(0).at(2).toString(), u"scene engine exited with code 1"_s);
        QCOMPARE(health.at(0).at(3).toString(), u"Vulkan"_s);

        // Not a no-frame failure: a frame cannot happen any more, and another item starts clean.
        setContent(scene(u"b"_s));
        QVERIFY(!m_root->property("failed").toBool());
    }

    void theFirstContentIsShownAtOnceWithItsPreviewStandingIn()
    {
        load();
        setContent(scene(u"a"_s));
        const auto s = slotItems(m_root.get());
        QCOMPARE(s.size(), 1);
        QVERIFY(s[0]->property("revealed").toBool());
        QTRY_COMPARE(s[0]->opacity(), 1.0);
        // a preview inside the slot until the scene has a frame
        bool preview = false;
        for (QQuickItem *c : s[0]->childItems())
            preview = preview || (c->property("ready").isValid() && !c->property("hasFrame").isValid());
        QVERIFY(preview);
    }

    void aSwitchKeepsTheOldVisibleUntilTheNewHasAFrameThenCrossfades()
    {
        load();
        setContent(scene(u"a"_s));
        QQuickItem *first = slotItems(m_root.get())[0];
        QObject *firstDelegate = delegateOf(first);
        firstDelegate->setProperty("hasFrame", true);
        QTRY_COMPARE(first->opacity(), 1.0);
        root_audible(true);
        setContent(scene(u"b"_s));
        auto s = slotItems(m_root.get());
        QCOMPARE(s.size(), 2);
        QQuickItem *second = s[0] == first ? s[1] : s[0];
        // the new one is there, running, invisible and mute; the old one is untouched
        QVERIFY(!second->property("revealed").toBool());
        QCOMPARE(second->opacity(), 0.0);
        QCOMPARE(first->opacity(), 1.0);
        QVERIFY(firstDelegate->property("audible").toBool());
        QVERIFY(!delegateOf(second)->property("audible").toBool());
        // no preview of the new item stands in front of the old picture
        for (QQuickItem *c : second->childItems())
            QVERIFY(c->property("hasFrame").isValid());
        QTest::qWait(700); // well past a crossfade: still the same
        QCOMPARE(slotItems(m_root.get()).size(), 2);
        QCOMPARE(first->opacity(), 1.0);
        QCOMPARE(second->opacity(), 0.0);
        // the first frame arrives
        delegateOf(second)->setProperty("hasFrame", true);
        QTRY_VERIFY(second->property("revealed").toBool());
        QTRY_COMPARE(second->opacity(), 1.0);
        QVERIFY(delegateOf(second)->property("audible").toBool()); // its audio follows now
        QVERIFY(!firstDelegate->property("audible").toBool());     // the old one is silenced for the fade
        QTRY_COMPARE(slotItems(m_root.get()).size(), 1);               // and destroyed after it
        QCOMPARE(slotItems(m_root.get())[0], second);
    }

    void aDelegateThatNeverGetsAFrameIsShownAfterTheTimeout()
    {
        load(300);
        setContent(scene(u"a"_s));
        QQuickItem *first = slotItems(m_root.get())[0];
        delegateOf(first)->setProperty("hasFrame", true);
        QTRY_COMPARE(first->opacity(), 1.0);
        setContent(scene(u"b"_s));
        QCOMPARE(slotItems(m_root.get()).size(), 2);
        QTest::qWait(150);
        QCOMPARE(slotItems(m_root.get()).size(), 2); // not yet
        QTRY_COMPARE_WITH_TIMEOUT(slotItems(m_root.get()).size(), 1, 3000);
        QVERIFY(slotItems(m_root.get())[0] != first);
        QTRY_COMPARE(slotItems(m_root.get())[0]->opacity(), 1.0);
    }

    void aSwitchThatIsReplacedBeforeItShowedDisappearsAndTheOldStays()
    {
        load();
        setContent(scene(u"a"_s));
        QQuickItem *first = slotItems(m_root.get())[0];
        delegateOf(first)->setProperty("hasFrame", true);
        QTRY_COMPARE(first->opacity(), 1.0);
        setContent(scene(u"b"_s));
        QCOMPARE(slotItems(m_root.get()).size(), 2);
        setContent(scene(u"c"_s)); // b never had a frame: it goes, c takes its place, a is still what is shown
        QTRY_COMPARE(slotItems(m_root.get()).size(), 2);
        QCOMPARE(first->opacity(), 1.0);
        QQuickItem *third = nullptr;
        for (QQuickItem *s : slotItems(m_root.get()))
            if (s != first)
                third = s;
        QVERIFY(third);
        QCOMPARE(third->opacity(), 0.0);
        delegateOf(third)->setProperty("hasFrame", true);
        QTRY_COMPARE(slotItems(m_root.get()).size(), 1);
        QCOMPARE(slotItems(m_root.get())[0], third);
    }

    void aFailingNewDelegateLeavesTheOldInPlaceAndItsPreviewTakesOver()
    {
        load();
        setContent(scene(u"a"_s));
        QQuickItem *first = slotItems(m_root.get())[0];
        delegateOf(first)->setProperty("hasFrame", true);
        QTRY_COMPARE(first->opacity(), 1.0);
        setContent(scene(u"b"_s));
        QObject *failing = nullptr;
        for (QQuickItem *s : slotItems(m_root.get()))
            if (s != first)
                failing = delegateOf(s);
        QVERIFY(failing);
        QMetaObject::invokeMethod(failing, "failed", Q_ARG(QString, u"engine crashed"_s));
        // the failed scene's slot is gone; a preview of it is on its way, the old picture still shown until it has loaded
        QTRY_VERIFY(slotItems(m_root.get()).size() >= 1);
        QCOMPARE(first->opacity() > 0.0, true);
        QTRY_COMPARE_WITH_TIMEOUT(slotItems(m_root.get()).size(), 1, 4000);
        QVERIFY(slotItems(m_root.get())[0] != first);
    }

    void nothingMeansTheOldFadesAwayAndAnEmptyScreenFollows()
    {
        load();
        setContent(scene(u"a"_s));
        delegateOf(slotItems(m_root.get())[0])->setProperty("hasFrame", true);
        QTRY_COMPARE(slotItems(m_root.get())[0]->opacity(), 1.0);
        setContent(QVariantMap{});
        QTRY_COMPARE(slotItems(m_root.get()).size(), 0);
    }

    void aScenePlayedForTheFirstTimeAfterNothingShowsItsPreviewAgain()
    {
        load();
        setContent(scene(u"a"_s));
        delegateOf(slotItems(m_root.get())[0])->setProperty("hasFrame", true);
        QTRY_COMPARE(slotItems(m_root.get())[0]->opacity(), 1.0);
        setContent(QVariantMap{});
        QTRY_COMPARE(slotItems(m_root.get()).size(), 0);
        setContent(scene(u"b"_s));
        QCOMPARE(slotItems(m_root.get()).size(), 1);
        QVERIFY(slotItems(m_root.get())[0]->property("revealed").toBool()); // nothing was shown: revealed at once
    }

    void anEngineUpgradeCrossfadesToANewDelegateOnce()
    {
        load();
        setContent(scene(u"a"_s));
        QQuickItem *first = slotItems(m_root.get())[0];
        QObject *early = delegateOf(first);
        early->setProperty("hasFrame", true);
        QTRY_COMPARE(first->opacity(), 1.0);
        QMetaObject::invokeMethod(early, "engineUpgradeAvailable");
        QCOMPARE(slotItems(m_root.get()).size(), 2); // the Vulkan delegate is running, invisible
        QCOMPARE(first->opacity(), 1.0);
        QQuickItem *vulkan = nullptr;
        for (QQuickItem *s : slotItems(m_root.get()))
            if (s != first)
                vulkan = s;
        QVERIFY(vulkan);
        QCOMPARE(vulkan->opacity(), 0.0);
        delegateOf(vulkan)->setProperty("hasFrame", true);
        QTRY_COMPARE(slotItems(m_root.get()).size(), 1);
        QCOMPARE(slotItems(m_root.get())[0], vulkan);
        // the first delegate is gone with its slot; the second one is the only one left and offers no upgrade of its own
        QCOMPARE(slotItems(m_root.get()).size(), 1);
    }

private:
    void root_audible(bool on) { m_root->setProperty("audible", on); }
};

QTEST_MAIN(TstWallpaperSwitch)
#include "tst_wallpaperswitch.moc"
