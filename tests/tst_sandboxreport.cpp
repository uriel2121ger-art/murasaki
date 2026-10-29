#include "sandboxreport.h"
#include "testutil.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSandboxReport : public QObject
{
    Q_OBJECT

private slots:
    void parsesTheHelpersLine()
    {
        const SandboxReport report = parseSandboxReport(R"({"landlockAbi":8,"tcpDeny":true,"scopes":true,"bwrap":true})" "\n");
        QVERIFY(report.valid && report.confines() && report.bwrap && report.scopes);
        QCOMPARE(report.landlockAbi, 8);
        QCOMPARE(report.summary(), u"Landlock ABI 8, TCP refused, mount namespace yes"_s);
    }

    void aSystemWithoutLandlockDoesNotConfine()
    {
        const SandboxReport report = parseSandboxReport(R"({"landlockAbi":0,"tcpDeny":true,"scopes":false,"bwrap":true})");
        QVERIFY(report.valid);
        QVERIFY(!report.confines());
        QVERIFY(report.summary().startsWith(u"no Landlock"_s));
        QVERIFY(!parseSandboxReport(R"({"landlockAbi":5,"tcpDeny":false,"scopes":false,"bwrap":false})").confines());
    }

    void garbageIsInvalid()
    {
        for (const char *text : {"", "nonsense", "[]", R"({"bwrap":true})", R"({"landlockAbi":"8"})"}) {
            const SandboxReport report = parseSandboxReport(text);
            QVERIFY2(!report.valid && !report.confines(), text);
        }
    }

    void queryRunsTheHelperOnceAndCachesIt()
    {
        QTemporaryDir dir;
        const QString counter = dir.filePath(u"runs"_s);
        const QString helper = dir.filePath(u"helper.sh"_s);
        testutil::writeFile(helper, QByteArray("#!/bin/sh\necho x >> '") + counter.toUtf8()
                                        + "'\necho '{\"landlockAbi\":6,\"tcpDeny\":true,\"scopes\":true,\"bwrap\":false}'\n");
        QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const SandboxReport first = querySandboxReport(helper);
        QVERIFY(first.valid);
        QCOMPARE(first.landlockAbi, 6);
        QVERIFY(querySandboxReport(helper).valid);
        QCOMPARE(QFile(counter).size(), 2); // one line: it ran once
        QVERIFY(!querySandboxReport(QString()).valid);
        QVERIFY(!querySandboxReport(dir.filePath(u"missing"_s)).valid);
    }
};

QTEST_GUILESS_MAIN(TestSandboxReport)
#include "tst_sandboxreport.moc"
