#include "filelog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

class TestFileLog : public QObject
{
    Q_OBJECT

private slots:
    void theDirectoryFollowsXdgStateHome();
    void namesCannotLeaveTheDirectory();
    void linesAreAppendedAndFilesArePrivate();
    void rotationKeepsTheConfiguredNumberOfFiles();
    void rotationNeverSplitsALine();
    void anUnwritableLogIsDroppedSilently();
    void childLinesGoToTheirOwnFile();
    void theMessageHandlerWritesTheLogAndStillForwards();
};

void TestFileLog::theDirectoryFollowsXdgStateHome()
{
    qputenv("XDG_STATE_HOME", "/somewhere/state");
    QCOMPARE(logDirectory(), u"/somewhere/state/murasaki/logs"_s);
    qputenv("XDG_STATE_HOME", "relative/is/invalid");
    QCOMPARE(logDirectory(), QDir::homePath() + u"/.local/state/murasaki/logs"_s);
    qunsetenv("XDG_STATE_HOME");
    QCOMPARE(logDirectory(), QDir::homePath() + u"/.local/state/murasaki/logs"_s);
}

void TestFileLog::namesCannotLeaveTheDirectory()
{
    QCOMPARE(logFileName(u"scene"_s, u"DP-1"_s), u"scene-DP-1.log"_s);
    QCOMPARE(logFileName(u"murasakid"_s), u"murasakid.log"_s);
    QCOMPARE(logFileName(u"scene"_s, u"../../etc/passwd"_s), u"scene-.._.._etc_passwd.log"_s);
    QCOMPARE(logFileName(u"..hidden"_s), u"_..hidden.log"_s);
    QCOMPARE(logFileName(QString()), u"_.log"_s);
    QVERIFY(!logFileName(u"a/b"_s, u"c\\d"_s).contains(u'/'));
}

void TestFileLog::linesAreAppendedAndFilesArePrivate()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"logs/x.log"_s); // the directory does not exist yet
    {
        RotatingLog log(path);
        log.append(u"one"_s);
        log.append(u"two"_s);
    }
    {
        RotatingLog again(path); // a new process appends to what is there
        again.append(u"three"_s);
    }
    QCOMPARE(readAll(path), u"one\ntwo\nthree\n"_s);
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                            | QFileDevice::WriteOther | QFileDevice::ExeOther | QFileDevice::ExeOwner;
    QVERIFY(QFileInfo(path).permissions() & QFileDevice::WriteOwner);
    QVERIFY(!(QFileInfo(path).permissions() & others)); // 0600
    QVERIFY(!(QFileInfo(dir.filePath(u"logs"_s)).permissions() & (others & ~QFileDevice::ExeOwner))); // 0700
}

void TestFileLog::rotationKeepsTheConfiguredNumberOfFiles()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"r.log"_s);
    RotatingLog log(path, 100, 3);
    const QString line = QString(39, u'x'); // 40 bytes with the newline: two fit in a file, the third rotates
    for (int i = 0; i < 12; ++i)
        log.append(QString::number(i % 10) + line.mid(1));
    QVERIFY(QFile::exists(path));
    QVERIFY(QFile::exists(path + u".1"_s));
    QVERIFY(QFile::exists(path + u".2"_s));
    QVERIFY(!QFile::exists(path + u".3"_s)); // three files in all
    for (const QString &name : {path, path + u".1"_s, path + u".2"_s})
        QVERIFY2(QFileInfo(name).size() <= 100, qPrintable(name));
    // The newest line is in the current file, the oldest kept ones in the numbered files.
    QVERIFY(readAll(path).contains(u"1xxx"_s)); // line 11
    QVERIFY(!readAll(path + u".2"_s).isEmpty());
}

void TestFileLog::rotationNeverSplitsALine()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(u"s.log"_s);
    RotatingLog log(path, 64, 2);
    for (int i = 0; i < 20; ++i)
        log.append(u"line %1 with some padding"_s.arg(i, 2, 10, u'0'));
    for (const QString &name : {path, path + u".1"_s})
        for (const QString &line : readAll(name).split(u'\n', Qt::SkipEmptyParts))
            QVERIFY2(line.startsWith(u"line "_s) && line.endsWith(u"padding"_s), qPrintable(line));
    QVERIFY(!QFile::exists(path + u".2"_s));
}

void TestFileLog::anUnwritableLogIsDroppedSilently()
{
    // A path below a regular file can never be created: appending must neither crash nor throw.
    QTemporaryDir dir;
    QFile blocker(dir.filePath(u"file"_s));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    RotatingLog log(dir.filePath(u"file/sub/x.log"_s));
    log.append(u"lost"_s);
    QVERIFY(!QFile::exists(dir.filePath(u"file/sub/x.log"_s)));
}

void TestFileLog::childLinesGoToTheirOwnFile()
{
    QTemporaryDir dir;
    qputenv("XDG_STATE_HOME", dir.path().toLocal8Bit());
    appendChildLog(u"scene-DP-1"_s, u"engine says hello"_s);
    appendChildLog(u"scene-DP-1"_s, u"and again"_s);
    appendChildLog(u"scene-HDMI-A-1"_s, u"other output"_s);
    const QString first = readAll(dir.filePath(u"murasaki/logs/scene-DP-1.log"_s));
    QVERIFY2(first.contains(u"engine says hello\n"_s) && first.contains(u"and again\n"_s), qPrintable(first));
    QVERIFY(!first.contains(u"other output"_s));
    QVERIFY(readAll(dir.filePath(u"murasaki/logs/scene-HDMI-A-1.log"_s)).contains(u"other output"_s));
    // Every line starts with an ISO timestamp.
    QVERIFY(first.at(0).isDigit() && first.contains(u'T'));
    qunsetenv("XDG_STATE_HOME");
}

static int g_forwarded = 0;

void TestFileLog::theMessageHandlerWritesTheLogAndStillForwards()
{
    QTemporaryDir dir;
    qputenv("XDG_STATE_HOME", dir.path().toLocal8Bit());
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) { ++g_forwarded; });
    installFileLogging(u"unit"_s);
    qInfo("hello %d", 42);
    qWarning("careful");
    qDebug("quiet");
    const QString text = readAll(dir.filePath(u"murasaki/logs/unit.log"_s));
    QVERIFY2(text.contains(u"[I] hello 42\n"_s), qPrintable(text));
    QVERIFY2(text.contains(u"[W] careful\n"_s), qPrintable(text));
    QVERIFY2(text.contains(u"[D] quiet\n"_s), qPrintable(text));
    QCOMPARE(g_forwarded, 3);
    qInstallMessageHandler(nullptr);
    qunsetenv("XDG_STATE_HOME");
}

QTEST_GUILESS_MAIN(TestFileLog)
#include "tst_filelog.moc"
