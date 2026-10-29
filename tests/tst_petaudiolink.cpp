// How the overlay starts the sound player (ProcessPetAudioLink): inside murasaki-sandbox, whether the sandbox is required, and what a failure of the player or of the
// sandbox does to the next start. The player and the sandbox are shell scripts that write down what they were run with.

#include "petaudiocontroller.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki;
using namespace Qt::StringLiterals;

class TstPetAudioLink : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_log, m_player, m_sandbox, m_code;

    static void script(const QString &path, const QByteArray &body)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\n" + body);
        f.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    QStringList lines() const
    {
        QFile f(m_log);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split(u'\n', Qt::SkipEmptyParts) : QStringList();
    }
    // Starts the link, waits for what the scripts say, stops it. Returns the lines the scripts wrote.
    QStringList run(ProcessPetAudioLink &link, bool expectReady)
    {
        QFile::remove(m_log);
        QSignalSpy lines_(&link, &PetAudioLink::line);
        QSignalSpy exited(&link, &PetAudioLink::exited);
        if (!link.start())
            return {u"start refused"_s};
        if (expectReady)
            link.send(u"hello"_s);
        QElapsedTimer t;
        t.start();
        const auto done = [&] { return expectReady ? (exited.count() > 0 || lines().size() >= 2) : exited.count() > 0; };
        while (!done() && t.elapsed() < 5000)
            QTest::qWait(10);
        if (expectReady)
            QTest::qWait(100); // the echo of "hello" is written after the second line
        link.stop();
        QTest::qWait(50);
        return lines();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_log = m_dir.filePath(u"log"_s);
        m_player = m_dir.filePath(u"player.sh"_s);
        m_sandbox = m_dir.filePath(u"sandbox.sh"_s);
        m_code = m_dir.filePath(u"sandbox-code"_s);
        // the player writes its arguments, says ready, echoes what it is sent until the pipe closes; with player-code it exits with it at once
        script(m_player, "echo \"player $*\" >>'" + m_log.toUtf8() + "'\nif [ -f '" + m_dir.filePath(u"player-code"_s).toUtf8() + "' ]; then exit $(cat '"
                             + m_dir.filePath(u"player-code"_s).toUtf8() + "'); fi\necho ready\nwhile read l; do echo \"got $l\" >>'" + m_log.toUtf8() + "'; done\n");
        // the sandbox writes its arguments and runs the program after `--`, unless sandbox-code says it fails with that status
        script(m_sandbox, "echo \"sandbox $*\" >>'" + m_log.toUtf8() + "'\nif [ -f '" + m_code.toUtf8() + "' ]; then exit $(cat '" + m_code.toUtf8()
                              + "'); fi\nwhile [ \"$1\" != -- ]; do shift; done\nshift\nexec \"$@\"\n");
        qputenv("MURASAKI_SANDBOX", m_sandbox.toUtf8());
        qunsetenv("MURASAKI_PETAUDIO_SANDBOX");
        qunsetenv("MURASAKI_AUDIO_LOG");
    }
    void cleanup()
    {
        QTest::qWait(200); // a player stopped by the test may still be writing its last line
        QFile::remove(m_code);
        QFile::remove(m_dir.filePath(u"player-code"_s));
        qputenv("MURASAKI_SANDBOX", m_sandbox.toUtf8());
        qunsetenv("MURASAKI_PETAUDIO_SANDBOX");
    }

    void theDefaultIsTheSandboxWithItsRequirement()
    {
        ProcessPetAudioLink link(m_dir.path(), m_player);
        const QStringList l = run(link, true);
        QVERIFY2(l.size() >= 2, qPrintable(l.join('|')));
        QVERIFY(l[0].startsWith(u"sandbox --require "_s));
        QVERIFY(l[0].contains(u" --no-net -- "_s));
        QVERIFY(l[0].contains(u"--ro /usr"_s));
        QVERIFY(l[0].contains(u"--ro "_s + m_dir.path()));
        QVERIFY(l[1].startsWith(u"player --assets "_s));
        QVERIFY(l.contains(u"got hello"_s));
    }
    void withoutTheRequirementTheSandboxIsStillUsedButNotRequired()
    {
        ProcessPetAudioLink link(m_dir.path(), m_player);
        link.setRequireSandbox(false);
        const QStringList l = run(link, true);
        QVERIFY(l[0].startsWith(u"sandbox --ro "_s));
        QVERIFY(!l[0].contains(u"--require"_s));
        // and the switch of the environment turns it off only where it is not required
        qputenv("MURASAKI_PETAUDIO_SANDBOX", "0");
        ProcessPetAudioLink bare(m_dir.path(), m_player);
        bare.setRequireSandbox(false);
        QVERIFY(run(bare, true)[0].startsWith(u"player "_s));
        ProcessPetAudioLink strict(m_dir.path(), m_player);
        QVERIFY(run(strict, true)[0].startsWith(u"sandbox --require"_s)); // required: the switch is ignored
    }
    void whenItIsRequiredAMissingSandboxMeansNoPlayer()
    {
        qputenv("MURASAKI_SANDBOX", "/nonexistent/murasaki-sandbox");
        ProcessPetAudioLink link(m_dir.path(), m_player);
        QVERIFY(!link.start());
        link.setRequireSandbox(false);
        QVERIFY(link.start()); // not required: the player runs without
        link.stop();
    }
    void aSandboxThatCannotRunThePlayerFallsBackOnceOnlyWhenNotRequired()
    {
        QFile code(m_code);
        QVERIFY(code.open(QIODevice::WriteOnly));
        code.write("127");
        code.close();
        ProcessPetAudioLink link(m_dir.path(), m_player);
        link.setRequireSandbox(false);
        QSignalSpy exited(&link, &PetAudioLink::exited);
        QStringList l = run(link, false);
        QVERIFY2(l.value(0).startsWith(u"sandbox "_s), qPrintable(l.join(u"|"_s)));
        QCOMPARE(exited.count(), 1);
        QCOMPARE(exited.at(0).at(0).toInt(), 127);
        l = run(link, true); // the next start: without the sandbox, once
        QVERIFY2(l[0].startsWith(u"player "_s), qPrintable(l.join('|')));
        QFile::remove(m_code);
        l = run(link, true); // and the one after: confined again
        QVERIFY2(l[0].startsWith(u"sandbox "_s), qPrintable(l.join('|')));
    }
    void whenRequiredASandboxFailureNeverOpensTheDoor()
    {
        QFile code(m_code);
        QVERIFY(code.open(QIODevice::WriteOnly));
        code.write("125");
        code.close();
        ProcessPetAudioLink link(m_dir.path(), m_player);
        for (int i = 0; i < 3; ++i) {
            const QStringList l = run(link, false);
            QVERIFY2(l[0].startsWith(u"sandbox --require"_s), qPrintable(l.join('|')));
            QVERIFY(!l.join('|').contains(u"player "_s));
        }
    }
    void thePlayersOwnUnavailableStatusSaysNothingAboutTheSandbox()
    {
        QFile code(m_dir.filePath(u"player-code"_s));
        QVERIFY(code.open(QIODevice::WriteOnly));
        code.write("3");
        code.close();
        ProcessPetAudioLink link(m_dir.path(), m_player);
        link.setRequireSandbox(false);
        QSignalSpy exited(&link, &PetAudioLink::exited);
        for (int i = 0; i < 3; ++i) {
            const QStringList l = run(link, false);
            QVERIFY2(l[0].startsWith(u"sandbox "_s), qPrintable(l.join('|'))); // always confined
        }
        QCOMPARE(exited.count(), 3);
        QCOMPARE(exited.at(2).at(0).toInt(), 3);
    }
    void theLogFileIsTheOnlyThingATestMayAddToTheSandbox()
    {
        const QString file = m_dir.filePath(u"audio.log"_s);
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        qputenv("MURASAKI_AUDIO_LOG", file.toUtf8());
        ProcessPetAudioLink link(m_dir.path(), m_player);
        const QStringList l = run(link, true);
        QVERIFY(l[0].contains(u"--rw "_s + file));
        qunsetenv("MURASAKI_AUDIO_LOG");
        QStringList l2 = run(link, true);
        QVERIFY(!l2[0].contains(u"--rw"_s));
    }
};

QTEST_GUILESS_MAIN(TstPetAudioLink)
#include "tst_petaudiolink.moc"
