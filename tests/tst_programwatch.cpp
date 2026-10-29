// ProgramWatch (src/bridge/qml/bridgesupport.*): a program replaced on disk, the way an install does it (a new file renamed over the old one), is reported once, after a quiet
// period and only when the new file is executable and stable. The logic is headless; BridgeItem only starts its host again when it hears this (a crashing new binary then
// goes through RestartBackoff, tested in tst_sceneengine).

#include "bridgesupport.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

using namespace murasaki;

namespace {

constexpr int kQuietMs = 150;
constexpr int kPollMs = 600;

void write(const QString &path, const QByteArray &content, QFileDevice::Permissions extra = QFileDevice::ExeOwner)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | extra);
}

// What an install does: the new file next to the old one, then a rename over it.
void install(const QString &path, const QByteArray &content, QFileDevice::Permissions extra = QFileDevice::ExeOwner)
{
    const QString next = path + QStringLiteral(".new");
    write(next, content, extra);
    QVERIFY(::rename(QFile::encodeName(next).constData(), QFile::encodeName(path).constData()) == 0); // (QFile::rename refuses to replace)
}

} // namespace

class TstProgramWatch : public QObject
{
    Q_OBJECT

private slots:
    void anUntouchedProgramIsNeverReported()
    {
        QTemporaryDir dir;
        const QString program = dir.filePath(QStringLiteral("host"));
        write(program, "#!/bin/sh\n");
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(program);
        QTest::qWait(kPollMs * 2 + kQuietMs * 3);
        QCOMPARE(changed.size(), 0);
    }
    void aReplacementByRenameIsReportedOnceAfterTheQuietPeriod()
    {
        QTemporaryDir dir;
        const QString program = dir.filePath(QStringLiteral("host"));
        write(program, "#!/bin/sh\necho old\n");
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(program);
        QElapsedTimer since;
        since.start();
        install(program, "#!/bin/sh\necho new\n");
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
        QVERIFY2(since.elapsed() >= kQuietMs, "reported before the quiet period");
        QCOMPARE(changed.first().first().toString(), program);
        QTest::qWait(kPollMs * 2 + kQuietMs * 3);
        QCOMPARE(changed.size(), 1); // once
        // and the next install is the next report (the watch was re-armed on the new file)
        install(program, "#!/bin/sh\necho newer\n");
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 2, 5000);
    }
    void aBurstOfWritesIsOneReportAfterTheLastOne()
    {
        QTemporaryDir dir;
        const QString program = dir.filePath(QStringLiteral("host"));
        write(program, "#!/bin/sh\n");
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(program);
        for (int i = 0; i < 6; ++i) { // an install that takes a while: the file is rewritten again and again
            install(program, QByteArray("#!/bin/sh\necho ") + QByteArray::number(i) + "\n");
            QTest::qWait(kQuietMs / 3);
        }
        QCOMPARE(changed.size(), 0); // never in the middle of it
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
        QTest::qWait(kQuietMs * 4);
        QCOMPARE(changed.size(), 1);
    }
    void aFileThatIsNotExecutableYetIsWaitedFor()
    {
        QTemporaryDir dir;
        const QString program = dir.filePath(QStringLiteral("host"));
        write(program, "#!/bin/sh\n");
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(program);
        install(program, "#!/bin/sh\necho new\n", {}); // no execute bit yet
        QTest::qWait(kQuietMs * 5);
        QCOMPARE(changed.size(), 0);
        QVERIFY(QFile::setPermissions(program, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000); // (the poll finds it if no event came)
    }
    void aProgramThatIsRemovedAndComesBackIsReported()
    {
        QTemporaryDir dir;
        const QString program = dir.filePath(QStringLiteral("host"));
        write(program, "#!/bin/sh\n");
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(program);
        QVERIFY(QFile::remove(program));
        QTest::qWait(kQuietMs * 4);
        QCOMPARE(changed.size(), 0); // gone is not a new program
        write(program, "#!/bin/sh\necho back\n");
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
    }
    void aLinkIsFollowedAndWatchingAgainTakesTheNewStateAsTheRunningOne()
    {
        QTemporaryDir dir;
        const QString real = dir.filePath(QStringLiteral("real"));
        const QString link = dir.filePath(QStringLiteral("host"));
        write(real, "#!/bin/sh\n");
        QVERIFY(QFile::link(real, link));
        ProgramWatch watch(kQuietMs, kPollMs);
        QSignalSpy changed(&watch, &ProgramWatch::changed);
        watch.watch(link);
        install(real, "#!/bin/sh\necho new\n");
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 5000);
        // the host was started again from the new file: that is what runs now, so nothing more is reported for it
        watch.watch(link);
        QTest::qWait(kPollMs + kQuietMs * 3);
        QCOMPARE(changed.size(), 1);
        watch.watch(QString()); // stops
        install(real, "#!/bin/sh\necho later\n");
        QTest::qWait(kQuietMs * 4);
        QCOMPARE(changed.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstProgramWatch)
#include "tst_programwatch.moc"
