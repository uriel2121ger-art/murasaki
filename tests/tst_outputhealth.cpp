// The failure state of one output: what a refused start, a crash and a missing first frame do to it.
#include "outputhealth.h"

#include <QTest>

using namespace murasaki;
using namespace Qt::StringLiterals;

class TestOutputHealth : public QObject
{
    Q_OBJECT

    static OutputFailure failure(FailureKind kind, const QString &reason = u"why"_s)
    {
        return {kind, reason, u"Vulkan"_s, true};
    }

private slots:
    void startsHealthy()
    {
        OutputHealth health;
        QVERIFY(!health.failure());
        QVERIFY(!health.frame(u"a"_s));
        QVERIFY(!health.retry());
    }

    void aReportForTheContentOnScreenIsKept()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        QVERIFY(health.report(u"a"_s, failure(FailureKind::Crashed)));
        QVERIFY(health.failure());
        QCOMPARE(health.failure()->kind, FailureKind::Crashed);
    }

    void theSameFailureIsAnnouncedOnce()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        QVERIFY(health.report(u"a"_s, failure(FailureKind::Refused)));
        QVERIFY(!health.report(u"a"_s, failure(FailureKind::Refused))); // not news: no second log line
        QVERIFY(health.report(u"a"_s, failure(FailureKind::Refused, u"another reason"_s))); // a different one is
    }

    void aStaleReportIsIgnored()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        health.begin(u"b"_s); // the user chose another item meanwhile
        QVERIFY(!health.report(u"a"_s, failure(FailureKind::Crashed)));
        QVERIFY(!health.failure());
    }

    void newContentClearsTheFailure()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        health.report(u"a"_s, failure(FailureKind::Crashed));
        QVERIFY(health.begin(u"b"_s));
        QVERIFY(!health.failure());
        QVERIFY(!health.begin(u"c"_s)); // nothing to clear
    }

    void theSameKeyAgainKeepsTheFailure()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        health.report(u"a"_s, failure(FailureKind::Crashed));
        QVERIFY(!health.begin(u"a"_s));
        QVERIFY(health.failure());
    }

    void aLateFrameClearsOnlyANoFrameFailure()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        health.report(u"a"_s, failure(FailureKind::NoFrame));
        QVERIFY(!health.frame(u"other"_s));
        QVERIFY(health.frame(u"a"_s));
        QVERIFY(!health.failure());

        health.report(u"a"_s, failure(FailureKind::Crashed));
        QVERIFY(!health.frame(u"a"_s)); // a crash that gave up stays, a frame from before it does not undo it
        QVERIFY(health.failure());
    }

    void retryClearsAndTheNextFailureIsNewsAgain()
    {
        OutputHealth health;
        health.begin(u"a"_s);
        health.report(u"a"_s, failure(FailureKind::Refused));
        QVERIFY(health.retry());
        QVERIFY(!health.failure());
        QCOMPARE(health.key(), u"a"_s);
        QVERIFY(health.report(u"a"_s, failure(FailureKind::Refused))); // logged again: the user asked
    }

    void classifiesTheReasonsDelegatesGive_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::addColumn<FailureKind>("kind");
        QTest::newRow("silent") << u"no frame within 15 s (first frame never arrived)"_s << FailureKind::NoFrame;
        QTest::newRow("exit") << u"scene engine exited with code 1"_s << FailureKind::Crashed;
        QTest::newRow("page") << u"the page crashed 3 times in 60 s"_s << FailureKind::Crashed;
        QTest::newRow("context") << u"scene rendering context unavailable"_s << FailureKind::Crashed;
        QTest::newRow("not installed") << u"scene engine not installed"_s << FailureKind::Refused;
        QTest::newRow("spanning") << u"Spanning needs the Vulkan engine, which cannot draw this scene"_s << FailureKind::Refused;
        QTest::newRow("load") << u"load failed: net::ERR_FILE_NOT_FOUND"_s << FailureKind::Refused;
    }
    void classifiesTheReasonsDelegatesGive()
    {
        QFETCH(QString, reason);
        QFETCH(FailureKind, kind);
        QCOMPARE(classifyFailure(reason), kind);
    }

    void kindsHaveStableNames()
    {
        QCOMPARE(failureKindName(FailureKind::Refused), u"refused"_s);
        QCOMPARE(failureKindName(FailureKind::Crashed), u"crashed"_s);
        QCOMPARE(failureKindName(FailureKind::NoFrame), u"noframe"_s);
    }
};

QTEST_GUILESS_MAIN(TestOutputHealth)
#include "tst_outputhealth.moc"
