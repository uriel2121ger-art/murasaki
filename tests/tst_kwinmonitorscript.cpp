#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Qt::StringLiterals;

// kwin-monitor.js against a fake KWin: what it reports about the focused window and every window's application, and
// that a change of focus is reported at all.
class TestKWinMonitorScript : public QObject
{
    Q_OBJECT

private slots:
    void reportsTheApplicationOfEveryWindow();
    void reportsTheFocusedWindow();
    void aFocusChangeReportsAgain();
    void ownAndSpecialWindowsAreNotTheFocus();
    void aScriptWithoutTheFocusSignalStillWorks();
    void theOverlayCoversNothingAndIsNotTheFocus();

private:
    // Loads the script; the fake's API is: calls (the ReportKWinState payloads), activate(index), windows.
    QJSValue load(QJSEngine &engine, bool withActivatedSignal = true);
    static QJsonObject lastReport(QJSValue &api)
    {
        const QJSValue calls = api.property(u"calls"_s);
        return QJsonDocument::fromJson(calls.property(calls.property(u"length"_s).toInt() - 1).toString().toUtf8()).object();
    }
};

QJSValue TestKWinMonitorScript::load(QJSEngine &engine, bool withActivatedSignal)
{
    QFile file(QStringLiteral(KWIN_MONITOR_SCRIPT));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString script = QString::fromUtf8(file.readAll());
    const QString head = QStringLiteral(
        "(function () {"
        "  const surfacePid = 111;"
        "  const calls = [];"
        "  const noop = { connect: () => {} };"
        "  const activated = [];"
        "  function mk(klass, pid, extra) {"
        "    return Object.assign({ normalWindow: true, output: { name: 'DP-1' }, pid: pid, resourceClass: klass,"
        "      desktops: [{ id: 'd1' }], onAllDesktops: false, fullScreen: false, minimized: false,"
        "      frameGeometry: { x: 0, y: 0, width: 100, height: 100 },"
        "      fullScreenChanged: noop, minimizedChanged: noop, maximizedChanged: noop, outputChanged: noop,"
        "      desktopsChanged: noop, interactiveMoveResizeFinished: noop, frameGeometryChanged: noop }, extra || {});"
        "  }"
        "  const windows = [mk('obs', 5), mk('steam_app_620', 6, { fullScreen: true }), mk('murasaki', 111), "
        "                   mk('plasmashell', 7, { normalWindow: false }),"
        "                   mk('murasaki-overlay', 222, { frameGeometry: { x: 0, y: 0, width: 100, height: 100 } })];"
        "  const workspace = {"
        "    windowList: () => windows, activeWindow: windows[0],"
        "    currentDesktop: { id: 'd1' }, desktops: [{ id: 'd1', name: 'One' }],"
        "    clientArea: () => ({ x: 0, y: 0, width: 100, height: 100 }),"
        "    windowAdded: noop, windowRemoved: noop, currentDesktopChanged: noop, desktopsChanged: noop, screensChanged: noop,"
        "    windowActivated: %1 };"
        "  const KWin = { MaximizeArea: 0 };"
        "  function callDBus(...args) { calls.push(args[4]); }").arg(withActivatedSignal ? u"{ connect: f => activated.push(f) }"_s : u"undefined"_s);
    const QString tail = QStringLiteral(
        ";\n  return {"
        "    calls: calls, windows: windows,"
        "    activate: i => { workspace.activeWindow = i < 0 ? null : windows[i]; activated.forEach(f => f(workspace.activeWindow)); },"
        "  };"
        "})()");
    return engine.evaluate(head + u"\n"_s + script + tail, u"kwin-monitor-test.js"_s);
}

void TestKWinMonitorScript::reportsTheApplicationOfEveryWindow()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    const QJsonObject report = lastReport(api);
    const QJsonValue windows = report.value(u"windows"_s);
    QVERIFY(windows.isArray());
    QCOMPARE(windows.toArray().size(), 2); // the surface's own window and the panel are left out, as before
    QCOMPARE(windows.toArray().at(0).toObject().value(u"resourceClass"_s).toString(), u"obs"_s);
    QCOMPARE(windows.toArray().at(1).toObject().value(u"resourceClass"_s).toString(), u"steam_app_620"_s);
    QVERIFY(windows.toArray().at(1).toObject().value(u"fullScreen"_s).toBool());
}

void TestKWinMonitorScript::reportsTheFocusedWindow()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    const QJsonObject active = lastReport(api).value(u"activeWindow"_s).toObject();
    QCOMPARE(active.value(u"resourceClass"_s).toString(), u"obs"_s);
    QCOMPARE(active.value(u"fullScreen"_s).toBool(), false);
}

void TestKWinMonitorScript::aFocusChangeReportsAgain()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    const int before = api.property(u"calls"_s).property(u"length"_s).toInt();
    api.property(u"activate"_s).call({1});
    QCOMPARE(api.property(u"calls"_s).property(u"length"_s).toInt(), before + 1);
    const QJsonObject active = lastReport(api).value(u"activeWindow"_s).toObject();
    QCOMPARE(active.value(u"resourceClass"_s).toString(), u"steam_app_620"_s);
    QCOMPARE(active.value(u"fullScreen"_s).toBool(), true);
    // nothing focused: null, not a crash
    api.property(u"activate"_s).call({-1});
    QVERIFY(lastReport(api).value(u"activeWindow"_s).isNull());
}

void TestKWinMonitorScript::ownAndSpecialWindowsAreNotTheFocus()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    api.property(u"activate"_s).call({2}); // the surface's own window: it must not look like a game or an app
    QVERIFY(lastReport(api).value(u"activeWindow"_s).isNull());
    api.property(u"activate"_s).call({3}); // the panel
    QVERIFY(lastReport(api).value(u"activeWindow"_s).isNull());
}

void TestKWinMonitorScript::aScriptWithoutTheFocusSignalStillWorks()
{
    QJSEngine engine;
    QJSValue api = load(engine, false);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    QVERIFY(lastReport(api).contains(u"windows"_s));
}

void TestKWinMonitorScript::theOverlayCoversNothingAndIsNotTheFocus()
{
    // The catcher of the wallpaper interaction mode fills the output; KWin lists it as a normal window. Reported, it
    // paused the wallpaper being clicked on (2026-10-22).
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    const QJsonArray windows = lastReport(api).value(u"windows"_s).toArray();
    for (const QJsonValue &window : windows)
        QVERIFY(window.toObject().value(u"resourceClass"_s).toString() != u"murasaki-overlay"_s);
    api.property(u"activate"_s).call({4});
    QVERIFY(lastReport(api).value(u"activeWindow"_s).isNull());
}

QTEST_GUILESS_MAIN(TestKWinMonitorScript)
#include "tst_kwinmonitorscript.moc"
