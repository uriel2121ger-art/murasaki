#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QTest>

using namespace Qt::StringLiterals;

// kwin-cursor.js against a fake KWin (workspace, callDBus, QTimer) in a plain QJSEngine: when it reports, how
// often, and that an idle cursor costs nothing.
class TestKWinCursorScript : public QObject
{
    Q_OBJECT

private slots:
    void reportsWhereTheCursorIsAtLoad();
    void firstMoveGoesOutAtOnce();
    void aBurstCollapsesIntoOneTrailingReport();
    void anIdleCursorSendsNothing();
    void keepsReportingWhileTheCursorKeepsMoving();
    void reportsTheServiceMethodAndTheCoordinates();

private:
    // Loads the script with a fake KWin and returns the API of the fake: move(x, y), tick(), calls, timers.
    QJSValue load(QJSEngine &engine);
    static int count(QJSValue &api) { return api.property(u"calls"_s).property(u"length"_s).toInt(); }
};

QJSValue TestKWinCursorScript::load(QJSEngine &engine)
{
    QFile file(QStringLiteral(KWIN_CURSOR_SCRIPT));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString script = QString::fromUtf8(file.readAll());
    const QString head = QStringLiteral(
        "(function () {"
        "  const calls = [];"
        "  const handlers = [];"
        "  const timers = [];"
        "  const workspace = { cursorPos: { x: 640, y: 360 }, cursorPosChanged: { connect: f => handlers.push(f) } };"
        "  function callDBus(...args) { calls.push(args); }"
        "  function QTimer() {"
        "    this.interval = 0; this.singleShot = false; this.running = false; this.callbacks = [];"
        "    this.timeout = { connect: f => this.callbacks.push(f) };"
        "    this.start = () => { this.running = true; };"
        "    timers.push(this);"
        "  }");
    // tick(): the throttle window elapses, so every running single-shot timer fires once.
    const QString tail = QStringLiteral(
        ";\n  return {"
        "    calls: calls,"
        "    move: (x, y) => { workspace.cursorPos = { x: x, y: y }; handlers.forEach(f => f()); },"
        "    tick: () => timers.filter(t => t.running).forEach(t => { t.running = false; t.callbacks.forEach(f => f()); }),"
        "    running: () => timers.filter(t => t.running).length,"
        "    interval: () => timers[0].interval,"
        "  };"
        "})()");
    const QString source = head + u"\n"_s + script + tail;
    return engine.evaluate(source, u"kwin-cursor-test.js"_s);
}

void TestKWinCursorScript::reportsWhereTheCursorIsAtLoad()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    QCOMPARE(count(api), 1); // a host that registers while the cursor rests still learns where it is
    const QJSValue call = api.property(u"calls"_s).property(0);
    QCOMPARE(call.property(4).toInt(), 640);
    QCOMPARE(call.property(5).toInt(), 360);
}

void TestKWinCursorScript::reportsTheServiceMethodAndTheCoordinates()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    const QJSValue call = api.property(u"calls"_s).property(0);
    QCOMPARE(call.property(0).toString(), u"io.github.uriel2121ger_art.Murasaki.Surface"_s);
    QCOMPARE(call.property(1).toString(), u"/io/github/uriel2121ger_art/Murasaki/Surface"_s);
    QCOMPARE(call.property(2).toString(), u"io.github.uriel2121ger_art.Murasaki.Surface1"_s);
    QCOMPARE(call.property(3).toString(), u"ReportKWinCursor"_s);
    // The throttle window is just under 1/30 s, so the rate stays at or below 30 Hz.
    QVERIFY(api.property(u"interval"_s).callWithInstance(api).toInt() >= 33);
}

void TestKWinCursorScript::firstMoveGoesOutAtOnce()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    const int before = count(api);
    api.property(u"move"_s).callWithInstance(api, {100, 200});
    QCOMPARE(count(api), before + 1);
    const QJSValue call = api.property(u"calls"_s).property(before);
    QCOMPARE(call.property(4).toInt(), 100);
    QCOMPARE(call.property(5).toInt(), 200);
}

void TestKWinCursorScript::aBurstCollapsesIntoOneTrailingReport()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    api.property(u"move"_s).callWithInstance(api, {1, 1}); // opens the window
    const int afterFirst = count(api);
    for (int i = 2; i <= 50; ++i)
        api.property(u"move"_s).callWithInstance(api, {i, i});
    QCOMPARE(count(api), afterFirst); // nothing inside the window

    api.property(u"tick"_s).callWithInstance(api); // the window ends: the latest position goes out
    QCOMPARE(count(api), afterFirst + 1);
    const QJSValue call = api.property(u"calls"_s).property(afterFirst);
    QCOMPARE(call.property(4).toInt(), 50);
    QCOMPARE(call.property(5).toInt(), 50);
}

void TestKWinCursorScript::anIdleCursorSendsNothing()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    api.property(u"move"_s).callWithInstance(api, {1, 1});
    api.property(u"tick"_s).callWithInstance(api); // nothing moved meanwhile: the window closes quietly
    const int sent = count(api);
    QCOMPARE(api.property(u"running"_s).callWithInstance(api).toInt(), 0); // no timer keeps running
    for (int i = 0; i < 5; ++i)
        api.property(u"tick"_s).callWithInstance(api);
    QCOMPARE(count(api), sent);
    // And the next movement is a first move again.
    api.property(u"move"_s).callWithInstance(api, {7, 8});
    QCOMPARE(count(api), sent + 1);
}

void TestKWinCursorScript::keepsReportingWhileTheCursorKeepsMoving()
{
    QJSEngine engine;
    QJSValue api = load(engine);
    const int start = count(api);
    // 10 windows of continuous movement: about one report per window, never one per move.
    for (int window = 0; window < 10; ++window) {
        for (int i = 0; i < 20; ++i)
            api.property(u"move"_s).callWithInstance(api, {window * 100 + i, 0});
        api.property(u"tick"_s).callWithInstance(api);
    }
    // Window 0: the leading report and the trailing one; every later window: its trailing one.
    QCOMPARE(count(api) - start, 11);
}

QTEST_MAIN(TestKWinCursorScript)
#include "tst_kwincursorscript.moc"
