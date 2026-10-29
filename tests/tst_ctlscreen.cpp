// `murasakictl triggers screen ...` (API revision 21): the pure helpers (what is printed, how a capture is checked) and the whole command against a FAKE daemon on a private bus that
// speaks the contract of the screen calls (StartScreenWatch, StopScreenWatch, ScreenWatchState, CaptureScreenReference, CommitScreenReference, ScreenCaptured). A picture is kept only
// with --yes or a yes at a terminal; the tests pin that.

#include "constants.h"
#include "screenctl.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDir>
#include <QDBusMessage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A PNG that is only a header the checks read: signature, IHDR of w x h, and an end chunk. (The ctl never decodes it.)
QByteArray fakePng(int w, int h)
{
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    QByteArray len(4, 0), width(4, 0), height(4, 0);
    qToBigEndian<quint32>(13, len.data());
    qToBigEndian<quint32>(quint32(w), width.data());
    qToBigEndian<quint32>(quint32(h), height.data());
    return out + len + "IHDR" + width + height + QByteArray("\x08\x02\x00\x00\x00", 5) + QByteArray(4, 0) + QByteArray(12, 'x');
}

QJsonObject step(int index, int w, int h, const QByteArray &png)
{
    return {{u"step"_s, index}, {u"width"_s, w}, {u"height"_s, h}, {u"png"_s, QString::fromLatin1(png.toBase64())}};
}

class FakeDaemon : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki1")
    Q_PROPERTY(uint ApiVersion READ apiVersion)
    Q_PROPERTY(QString ScreenWatchState READ screenWatchState)

public:
    uint api = 22;
    QString state = u"{\"state\":\"off\",\"reason\":\"\",\"regions\":0,\"ticksPerSecond\":0,\"since\":0,\"paused\":false,\"notes\":[]}"_s;
    QString triggers;
    QString triggerState = u"{\"states\":[],\"log\":[]}"_s;
    // what it saw
    int starts = 0, stops = 0;
    QList<QVariantList> captures;
    QStringList commits, discards;
    // what it does for a capture
    enum class Answer { Good, Error, BadPng, Silent, WrongSize, OtherFirst } answer = Answer::Good;
    int pictures = 1;

    uint apiVersion() const { return api; }
    QString screenWatchState() const { return state; }

public slots:
    Q_SCRIPTABLE QString ListTriggers() { return triggers; }
    Q_SCRIPTABLE QString TriggerState() { return triggerState; }
    Q_SCRIPTABLE void StartScreenWatch()
    {
        ++starts;
        state = u"{\"state\":\"asking\",\"reason\":\"\",\"regions\":2,\"ticksPerSecond\":5,\"since\":1,\"paused\":false,\"notes\":[]}"_s;
    }
    Q_SCRIPTABLE void StopScreenWatch() { ++stops; }
    Q_SCRIPTABLE QString CaptureScreenReference(const QString &triggerId, int region, int steps)
    {
        captures << QVariantList{triggerId, region, steps};
        const QString request = u"req-%1"_s.arg(captures.size());
        if (answer != Answer::Silent)
            QTimer::singleShot(20, this, [this, triggerId, region, request] {
                if (answer == Answer::OtherFirst) // a failure of somebody else's capture, and an old result of this trigger and region
                    emit ScreenCaptured(u"someone-else"_s, u"{\"request\":\"someone-else\",\"error\":\"screen: not watching\"}"_s);
                emit ScreenCaptured(request, resultFor(request, triggerId, region));
            });
        return request;
    }
    Q_SCRIPTABLE void CommitScreenReference(const QString &request) { commits << request; }
    Q_SCRIPTABLE void DiscardScreenReference(const QString &request) { discards << request; }

signals:
    Q_SCRIPTABLE void ScreenCaptured(const QString &request, const QString &json);

private:
    QString resultFor(const QString &request, const QString &triggerId, int region) const
    {
        QJsonObject o{{u"request"_s, request}, {u"trigger"_s, triggerId}, {u"region"_s, region}};
        if (answer == Answer::Error) {
            o = {{u"request"_s, request}, {u"error"_s, u"screen: not watching"_s}};
        } else {
            QJsonArray steps;
            for (int i = 0; i < pictures; ++i)
                steps.append(answer == Answer::BadPng ? step(i, 10, 10, QByteArray("not a png at all, long enough to pass a length check.....")) : answer == Answer::WrongSize ? step(i, 99, 99, fakePng(10, 8)) : step(i, 10, 8, fakePng(10, 8)));
            o.insert(u"steps"_s, steps);
        }
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
};

const QString kTriggers = QStringLiteral(R"([
  {"id":"t-screen","name":"Victory","enabled":true,"cooldownMs":500,"mode":"hold","source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":10,"y":20,"w":300,"h":100},{"kind":"circle","x":5.4,"y":6.6,"w":50,"h":50}],"match":{"mode":"hash"}}},"action":{"type":"hide-all"}},
  {"id":"t-fire","name":"Wave","enabled":true,"cooldownMs":500,"mode":"press","source":{"kind":"fire","name":"wave"},"action":{"type":"hide-all"}}])");

} // namespace

class TestCtlScreen : public QObject
{
    Q_OBJECT

private slots:
    void theStateIsPrintedFromTheDaemonsJson();
    void theRegionsOfATriggerArePrintedWithTheirScores();
    void stepsAreWholeNumbersFromOneToSixtyFour();
    void aCaptureIsCheckedBeforeAnythingIsKept();
    void theWaiterTimesOutWithoutAnAnswer();
    void startStopAndStatusTalkToTheDaemon();
    void aPictureIsKeptOnlyWhenYouSayYes();
    void flagsAndRegionsAreCheckedBeforeTalkingToTheDaemon();
    void aBadCaptureIsRefusedAndNothingIsKept();
    void onlyTheResultOfMyRequestCounts();
    void anOldDaemonIsToldSo();
    void badArgumentsAreUsageErrors();

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
        process.closeWriteChannel(); // (stdin is not a terminal: no question is asked)
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 60000)) {
            process.kill();
            process.waitForFinished(2000);
            return {-2, {}, u"timed out"_s};
        }
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()), QString::fromUtf8(process.readAllStandardError())};
    }
    struct Bus {
        QDBusConnection bus = QDBusConnection::sessionBus();
        FakeDaemon daemon;
        bool up = false;
        Bus()
        {
            daemon.triggers = kTriggers;
            up = bus.isConnected() && bus.registerObject(QString(kDBusPath), &daemon, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals | QDBusConnection::ExportAllProperties)
                && bus.registerService(QString(kDBusService));
        }
        ~Bus()
        {
            bus.unregisterService(QString(kDBusService));
            bus.unregisterObject(QString(kDBusPath));
        }
    };
};

void TestCtlScreen::theStateIsPrintedFromTheDaemonsJson()
{
    QCOMPARE(formatScreenWatchState({}), (QStringList{u"state: off"_s}));
    const QJsonObject watching{{u"state"_s, u"watching"_s}, {u"regions"_s, 3}, {u"ticksPerSecond"_s, 5}, {u"paused"_s, true}, {u"notes"_s, QJsonArray{u"screen: region outside the shared screens"_s}}};
    QCOMPARE(formatScreenWatchState(watching), (QStringList{u"state: watching"_s, u"regions: 3"_s, u"ticks a second: 5"_s, u"paused: yes"_s, u"note: screen: region outside the shared screens"_s}));
    const QJsonObject refused{{u"state"_s, u"refused"_s}, {u"reason"_s, u"screen: permission refused"_s}};
    QCOMPARE(formatScreenWatchState(refused), (QStringList{u"state: refused"_s, u"reason: screen: permission refused"_s}));
    // text from the daemon is never trusted with a terminal
    const QStringList evil = formatScreenWatchState({{u"state"_s, u"stopped"_s}, {u"reason"_s, u"bad\x1b[31m\nreason"_s}});
    for (const QString &line : evil)
        QVERIFY2(!line.contains(QChar(0x1b)) && !line.contains(u'\r'), qPrintable(line));
    QCOMPARE(evil.size(), 2);
}

void TestCtlScreen::theRegionsOfATriggerArePrintedWithTheirScores()
{
    const QJsonObject trigger = QJsonDocument::fromJson(kTriggers.toUtf8()).array().at(0).toObject();
    QCOMPARE(formatScreenRegions(trigger, {}), (QStringList{u"0\trect\t10\t20\t300\t100\t-\t-"_s, u"1\tcircle\t5\t7\t50\t50\t-\t-"_s}));
    const QJsonObject live{{u"score"_s, QJsonArray{875, -1}}, {u"refs"_s, QJsonArray{2, 0}}};
    QCOMPARE(formatScreenRegions(trigger, live), (QStringList{u"0\trect\t10\t20\t300\t100\t2\t87.5%"_s, u"1\tcircle\t5\t7\t50\t50\t0\t-"_s}));
    QVERIFY(formatScreenRegions({}, {}).isEmpty());
    QJsonArray many;
    for (int i = 0; i < 500; ++i)
        many.append(QJsonObject{{u"x"_s, i}});
    QCOMPARE(formatScreenRegions(QJsonObject{{u"source"_s, QJsonObject{{u"data"_s, QJsonObject{{u"regions"_s, many}}}}}}, {}).size(), 64);
}

void TestCtlScreen::stepsAreWholeNumbersFromOneToSixtyFour()
{
    int n = 0;
    QString why;
    for (const char *ok : {"1", "8", "64"}) {
        QVERIFY(parseScreenSteps(QString::fromLatin1(ok), &n, &why));
        QCOMPARE(n, QString::fromLatin1(ok).toInt());
    }
    for (const char *bad : {"", "0", "65", "-1", "3.5", "x", "08", " 3", "1e1", "+4"}) {
        QVERIFY2(!parseScreenSteps(QString::fromLatin1(bad), &n, &why), bad);
        QVERIFY(!why.isEmpty());
    }
}

void TestCtlScreen::aCaptureIsCheckedBeforeAnythingIsKept()
{
    QString why;
    const QJsonObject good{{u"request"_s, u"r1"_s}, {u"steps"_s, QJsonArray{step(0, 10, 8, fakePng(10, 8)), step(1, 360, 203, fakePng(360, 203))}}};
    const auto capture = parseScreenCapture(good, &why);
    QVERIFY2(capture.has_value(), qPrintable(why));
    QCOMPARE(capture->request, u"r1"_s);
    QCOMPARE(capture->sizes, (QList<QSize>{QSize(10, 8), QSize(360, 203)}));
    const auto refused = [&](const QJsonObject &o) {
        QString problem;
        const bool r = !parseScreenCapture(o, &problem).has_value();
        return r && !problem.isEmpty();
    };
    QVERIFY(refused({{u"error"_s, u"screen: not watching"_s}}));
    QVERIFY(refused({}));
    QVERIFY(refused({{u"steps"_s, QJsonArray()}}));
    QVERIFY(refused({{u"steps"_s, QJsonArray{step(0, 10, 8, QByteArray("GIF89a not a png.............................................."))}}}));
    QVERIFY(refused({{u"steps"_s, QJsonArray{step(0, 361, 8, fakePng(361, 8))}}}));        // over 360
    QVERIFY(refused({{u"steps"_s, QJsonArray{step(0, 10, 8, fakePng(10, 9))}}}));          // the size it says is not the header's
    QVERIFY(refused({{u"steps"_s, QJsonArray{step(0, 0, 0, fakePng(0, 0))}}}));
    QVERIFY(refused({{u"steps"_s, QJsonArray{QJsonObject{{u"png"_s, u"!!!!"_s}}}}}));
    QByteArray big = fakePng(10, 8) + QByteArray(5 << 20, 'x');
    QVERIFY(refused({{u"steps"_s, QJsonArray{step(0, 10, 8, big)}}}));                     // over 4 MiB
    QJsonArray many;
    for (int i = 0; i < 65; ++i)
        many.append(step(i, 10, 8, fakePng(10, 8)));
    QVERIFY(refused({{u"steps"_s, many}}));
    // each one under the limit, together over 16 MiB
    QJsonArray heavy;
    const QByteArray three = fakePng(10, 8) + QByteArray(3 << 20, 'x');
    for (int i = 0; i < 6; ++i)
        heavy.append(step(i, 10, 8, three));
    QVERIFY(refused({{u"steps"_s, heavy}}));
    // text of an error is cleaned for a terminal
    QString problem;
    parseScreenCapture({{u"error"_s, u"bad\x1b[2Jtext"_s}}, &problem);
    QVERIFY(!problem.contains(QChar(0x1b)));
}

void TestCtlScreen::theWaiterTimesOutWithoutAnAnswer()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    ScreenCaptureWaiter waiter(b.bus, QString(kDBusService), QString(kDBusPath), QString(kDBusInterface));
    QElapsedTimer t;
    t.start();
    QVERIFY(!waiter.wait(u"r2"_s, 300).has_value());
    QVERIFY(t.elapsed() >= 250 && t.elapsed() < 3000);
    // another client's result, an error of another request, and a late result of an earlier call are not this call's
    QTimer::singleShot(30, &b.daemon, [&] {
        emit b.daemon.ScreenCaptured(u"r"_s, u"{\"trigger\":\"t-screen\",\"region\":0,\"steps\":[]}"_s);
        emit b.daemon.ScreenCaptured(u"other"_s, u"{\"request\":\"other\",\"error\":\"screen: not watching\"}"_s);
    });
    QVERIFY(!waiter.wait(u"r2"_s, 400).has_value());
    // the argument names the request even when the body says another
    QTimer::singleShot(30, &b.daemon, [&] { emit b.daemon.ScreenCaptured(u"r2"_s, u"{\"request\":\"liar\",\"trigger\":\"t-screen\",\"region\":0,\"steps\":[]}"_s); });
    const auto got = waiter.wait(u"r2"_s, 2000);
    QVERIFY(got.has_value());
    QCOMPARE(got->value(u"request"_s).toString(), u"r2"_s);
}

void TestCtlScreen::startStopAndStatusTalkToTheDaemon()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    Result r = ctl({u"triggers"_s, u"screen"_s, u"status"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.out.trimmed(), u"state: off"_s);
    r = ctl({u"triggers"_s, u"screen"_s, u"start"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(b.daemon.starts, 1);
    QVERIFY2(r.out.contains(u"state: asking"_s) && r.out.contains(u"answer its dialog"_s), qPrintable(r.out));
    r = ctl({u"triggers"_s, u"screen"_s, u"stop"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(b.daemon.stops, 1);
    // regions: names resolve like for every trigger command; a trigger that is not a screen one is refused by name
    b.daemon.triggerState = QStringLiteral(R"({"states":[{"id":"t-screen","enabled":true,"available":true,"refs":[1,0],"screen":{"score":[640,-1],"detected":false,"step":0,"active":false}}],"log":[]})");
    r = ctl({u"triggers"_s, u"screen"_s, u"regions"_s, u"Victory"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.out.trimmed().split(u'\n'), (QStringList{u"0\trect\t10\t20\t300\t100\t1\t64.0%"_s, u"1\tcircle\t5\t7\t50\t50\t0\t-"_s}));
    r = ctl({u"triggers"_s, u"screen"_s, u"regions"_s, u"Wave"_s});
    QCOMPARE(r.code, 1);
    QVERIFY2(r.err.contains(u"is not a screen trigger"_s), qPrintable(r.err));
    r = ctl({u"triggers"_s, u"screen"_s, u"regions"_s, u"nobody"_s});
    QCOMPARE(r.code, 1);
}

void TestCtlScreen::aPictureIsKeptOnlyWhenYouSayYes()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    b.daemon.pictures = 3;
    // without --yes and without a terminal: the pictures are described and NOT kept
    Result r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"Victory"_s, u"0"_s, u"--steps"_s, u"3"_s});
    QCOMPARE(r.code, 2); // nobody to ask: a usage error, run it again with --yes
    QCOMPARE(b.daemon.captures.size(), 1);
    QCOMPARE(b.daemon.captures.at(0), (QVariantList{u"t-screen"_s, 0, 3}));
    QVERIFY2(r.out.contains(u"picture 2: 10x8"_s), qPrintable(r.out));
    QVERIFY2(r.err.contains(u"nothing was kept"_s) && r.err.contains(u"--save-to"_s), qPrintable(r.err));
    QVERIFY(b.daemon.commits.isEmpty());
    QCOMPARE(b.daemon.discards, (QStringList{u"req-1"_s})); // what the daemon held is dropped, not left to expire

    // with --save-to the pictures can be looked at first; still not kept
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(u"look"_s)));
    QVERIFY(QFile(dir.filePath(u"look/step7.png"_s)).open(QIODevice::WriteOnly)); // left by an earlier run: removed
    QVERIFY(QFile(dir.filePath(u"look/mine.txt"_s)).open(QIODevice::WriteOnly));  // not ours: stays
    r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"t-screen"_s, u"1"_s, u"--save-to"_s, dir.filePath(u"look"_s)});
    QCOMPARE(r.code, 2);
    QVERIFY(!QFile::exists(dir.filePath(u"look/step7.png"_s)));
    QVERIFY(QFile::exists(dir.filePath(u"look/mine.txt"_s)));
    QVERIFY(QFile::exists(dir.filePath(u"look/step0.png"_s)));
    QVERIFY(QFile::exists(dir.filePath(u"look/step2.png"_s)));
    QFile f(dir.filePath(u"look/step1.png"_s));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), fakePng(10, 8));
    QVERIFY(r.err.contains(u"they are in the folder"_s));
    QVERIFY(b.daemon.commits.isEmpty());

    // --yes keeps them: CommitScreenReference with the request the daemon gave
    r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"Victory"_s, u"0"_s, u"--yes"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(b.daemon.commits, (QStringList{u"req-3"_s}));
    QVERIFY2(r.out.contains(u"kept 3 picture(s) for region 0"_s), qPrintable(r.out));
}

void TestCtlScreen::flagsAndRegionsAreCheckedBeforeTalkingToTheDaemon()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    // --steps, --save-to and --yes belong to capture only
    for (const QStringList &extra : {QStringList{u"--yes"_s}, QStringList{u"--steps"_s, u"2"_s}, QStringList{u"--save-to"_s, u"/nonexistent/x"_s}}) {
        for (const QString &verb : {u"start"_s, u"stop"_s, u"status"_s}) {
            const Result r = ctl(QStringList{u"triggers"_s, u"screen"_s, verb} + extra);
            QCOMPARE(r.code, 2);
            QVERIFY2(r.err.contains(u"capture"_s), qPrintable(r.err));
        }
    }
    QCOMPARE(b.daemon.starts, 0);
    QCOMPARE(b.daemon.stops, 0);
    // a region the trigger does not have (it has two)
    const Result r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"Victory"_s, u"2"_s, u"--yes"_s});
    QCOMPARE(r.code, 2);
    QVERIFY2(r.err.contains(u"2 region(s)"_s), qPrintable(r.err));
    QVERIFY(b.daemon.captures.isEmpty());
}

void TestCtlScreen::aBadCaptureIsRefusedAndNothingIsKept()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    using Answer = FakeDaemon::Answer;
    for (const Answer answer : {Answer::Error, Answer::BadPng, Answer::WrongSize}) {
        b.daemon.answer = answer;
        const Result r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"Victory"_s, u"0"_s, u"--yes"_s});
        QCOMPARE(r.code, 1);
        QVERIFY2(!r.err.isEmpty(), qPrintable(QString::number(int(answer))));
        QVERIFY2(!r.out.contains(u"kept"_s), qPrintable(r.out));
    }
    QVERIFY(b.daemon.commits.isEmpty());
    QVERIFY(!b.daemon.discards.isEmpty()); // (whatever the failure, the held capture is dropped)
}

void TestCtlScreen::onlyTheResultOfMyRequestCounts()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    b.daemon.answer = FakeDaemon::Answer::OtherFirst;
    const Result r = ctl({u"triggers"_s, u"screen"_s, u"capture"_s, u"Victory"_s, u"0"_s, u"--yes"_s});
    QCOMPARE(r.code, 0);
    QCOMPARE(b.daemon.commits, (QStringList{u"req-1"_s}));
    QVERIFY2(r.out.contains(u"kept 1 picture(s)"_s), qPrintable(r.out + r.err));
}

void TestCtlScreen::anOldDaemonIsToldSo()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    b.daemon.api = 20;
    for (const QStringList &args : {QStringList{u"start"_s}, QStringList{u"status"_s}, QStringList{u"regions"_s, u"Victory"_s}}) {
        const Result r = ctl(QStringList{u"triggers"_s, u"screen"_s} + args);
        QCOMPARE(r.code, 1);
        QVERIFY2(r.err.contains(u"needs 21"_s), qPrintable(r.err));
    }
    QCOMPARE(b.daemon.starts, 0);
}

void TestCtlScreen::badArgumentsAreUsageErrors()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    Bus b;
    QVERIFY(b.up);
    for (const QStringList &args : {QStringList{u"nonsense"_s}, QStringList{u"start"_s, u"now"_s}, QStringList{u"capture"_s, u"Victory"_s}, QStringList{u"capture"_s, u"Victory"_s, u"x"_s},
                                    QStringList{u"capture"_s, u"Victory"_s, u"-1"_s}, QStringList{u"capture"_s, u"Victory"_s, u"0"_s, u"--steps"_s, u"0"_s},
                                    QStringList{u"capture"_s, u"Victory"_s, u"0"_s, u"--steps"_s, u"65"_s}}) {
        const Result r = ctl(QStringList{u"triggers"_s, u"screen"_s} + args);
        QCOMPARE(r.code, 2);
    }
    QVERIFY(b.daemon.captures.isEmpty());
}

QTEST_MAIN(TestCtlScreen)
#include "tst_ctlscreen.moc"
