// importSandboxArgs against the real murasaki-sandbox: the job's sources can be read, the output root written, and nothing else.

#include "sandboxargs.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki::import;

class TstSandboxArgs : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path(const QString &n) const { return m_dir.filePath(n); }
    int run(const QStringList &sandboxArgs, const QStringList &command)
    {
        QProcess p;
        p.start(QStringLiteral(MURASAKI_SANDBOX_BIN), sandboxArgs + QStringList{"--"} + command);
        if (!p.waitForFinished(10000))
            return -99;
        return p.exitCode();
    }
    static void write(const QString &file, const QByteArray &bytes)
    {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        write(path("source.txt"), "source");
        write(path("secret.txt"), "secret");
        QVERIFY(QDir().mkpath(path("out")));
        QVERIFY(QDir().mkpath(path("elsewhere")));
    }
    void theDaemonPassesOnlyTheSourcesAndTheOutputRoot()
    {
        const QStringList args = importSandboxArgs({path("source.txt"), path("missing")}, path("out"), true);
        QCOMPARE(args, (QStringList{"--profile", "import", "--src", path("source.txt"), "--out", path("out"), "--require"}));
        QCOMPARE(importSandboxArgs({}, path("out"), false), (QStringList{"--profile", "import", "--out", path("out")}));
    }
    void theProfileLetsAJobReadItsSourcesAndWriteItsOutputOnly()
    {
        QProcess probe;
        probe.start(QStringLiteral(MURASAKI_SANDBOX_BIN), {"--report"});
        QVERIFY(probe.waitForFinished(5000));
        const QJsonObject report = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
        if (report.value("landlockAbi").toInt() <= 0 || !report.value("unixSocketsHidden").toBool())
            QSKIP("Landlock or a mount namespace (bwrap) is not available here"); // the import profile needs both under --require
        const QStringList args = importSandboxArgs({path("source.txt")}, path("out"), true);
        QCOMPARE(run(args, {"/usr/bin/cat", path("source.txt")}), 0);
        QVERIFY(run(args, {"/usr/bin/cat", path("secret.txt")}) != 0);
        QCOMPARE(run(args, {"/usr/bin/sh", "-c", "echo x > '" + path("out/result") + "'"}), 0);
        QVERIFY(QFile::exists(path("out/result")));
        QVERIFY(run(args, {"/usr/bin/sh", "-c", "echo x > '" + path("elsewhere/leak") + "'"}) != 0);
        QVERIFY(!QFile::exists(path("elsewhere/leak")));
        QVERIFY(run(args, {"/usr/bin/sh", "-c", "echo x > '" + path("source.txt") + "'"}) != 0);
    }
};

QTEST_GUILESS_MAIN(TstSandboxArgs)
#include "tst_sandboxargs.moc"
