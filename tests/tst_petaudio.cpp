// The protocol of the player process and the process itself (src/audio/protocol.h, petaudio_main.cpp) with the paced backend: no sound device is needed.

#include "petaudioasset.h"
#include "protocol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <time.h>

using namespace murasaki::audio;

namespace {

long long monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::string ID(char c) { return std::string(64, c); }

// Writes an asset of `frames` frames of a constant into `dir`.
void makeAsset(const QString &dir, const std::string &id, std::size_t frames, std::int16_t value = 8000)
{
    WavWriter w((dir + "/" + QString::fromStdString(id) + ".wav").toStdString());
    std::vector<std::int16_t> samples(frames * 2, value);
    QVERIFY(w.append(samples.data(), frames));
    QVERIFY(w.finish());
}

} // namespace

class TstPetAudio : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_log;

    // Starts the player with the paced backend; waits for "ready".
    std::unique_ptr<QProcess> start(double idleSeconds = 30)
    {
        auto p = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("MURASAKI_AUDIO_LOG", m_log);
        p->setProcessEnvironment(env);
        p->start(MURASAKI_PETAUDIO_BIN, {"--assets", m_dir.path(), "--backend", "paced", "--idle-exit", QString::number(idleSeconds)});
        if (!p->waitForStarted(5000) || !p->waitForReadyRead(5000) || !p->readLine().startsWith("ready"))
            return nullptr;
        return p;
    }
    static QStringList lines(QProcess &p, int waitMs)
    {
        QStringList out;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < waitMs) {
            p.waitForReadyRead(20);
            while (p.canReadLine())
                out << QString::fromUtf8(p.readLine()).trimmed();
        }
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); m_log = m_dir.filePath("player.log"); }

    void everyCommandRoundTripsAndNothingElseParses()
    {
        Command play;
        play.kind = Command::Kind::Play;
        play.pet = 7;
        play.generation = 3;
        play.asset = ID('a');
        play.startFrame = 240;
        play.gain = 750;
        play.loop = true;
        play.atNs = 123456789;
        const auto back = parseCommand(formatCommand(play));
        QVERIFY(back);
        QCOMPARE(back->pet, 7);
        QCOMPARE(back->generation, 3u);
        QCOMPARE(back->asset, play.asset);
        QCOMPARE(back->startFrame, 240LL);
        QCOMPARE(back->gain, 750);
        QVERIFY(back->loop);
        QCOMPARE(back->atNs, 123456789LL);
        for (const char *ok : {"quit", "stop 3", "cancel 3", "pause 0", "resume 255", "gain 4 1000", "master 0"})
            QVERIFY2(parseCommand(ok), ok);
        const std::string a = ID('a');
        for (const std::string &bad : std::vector<std::string>{"", " ", "play", "stop", "stop -1", "stop 256", "stop 1 2", "stop  1", "stop x", "gain 1 1001", "gain 1 -1", "master 1001", "quit now",
                                       "play 1 1 " + a + " 0 500 2 0", "play 1 1 " + a + " -5 500 1 0", "play 1 1 " + a + " 0 500 1", "play 1 1 " + a + "0 0 500 1 0",
                                       "play 1 1 ../../etc/passwd 0 500 1 0", "play 1 1 " + std::string(64, 'A') + " 0 500 1 0", "PLAY 1 1 " + a + " 0 500 1 0",
                                       "stop 1\x01", "stop 1\xff", std::string(300, 'x'), "play 1 1 " + a + " 0 500 1 99999999999999999999",
                                       "play 1 1 " + a + " 0 500 1 -1", "stop 1\n", "stop\t1"})
            QVERIFY2(!parseCommand(bad), bad.c_str());
    }
    void randomGarbageNeverParsesAsACommandNorCrashes()
    {
        QRandomGenerator rng(12345);
        int parsed = 0;
        for (int i = 0; i < 20000; ++i) {
            std::string s;
            const int n = int(rng.bounded(40));
            for (int j = 0; j < n; ++j)
                s += char(rng.bounded(256));
            parsed += parseCommand(s).has_value();
        }
        QCOMPARE(parsed, 0);
        // and words of the protocol in the wrong order
        const char *words[] = {"play", "stop", "gain", "master", "1", "0", "99999", "-3", "x"};
        for (int i = 0; i < 20000; ++i) {
            std::string s;
            const int n = int(rng.bounded(1, 9));
            for (int j = 0; j < n; ++j)
                s += (j ? " " : "") + std::string(words[rng.bounded(9)]);
            const auto c = parseCommand(s);
            if (c && c->kind == Command::Kind::Play)
                QFAIL(s.c_str()); // a play needs a 64 hex asset, which these never contain
        }
    }

    void thePlayerPlaysAndSaysWhatHappened()
    {
        makeAsset(m_dir.path(), ID('a'), 4800); // 100 ms
        auto p = start();
        QVERIFY(p);
        Command c;
        c.kind = Command::Kind::Play;
        c.pet = 2;
        c.generation = 1;
        c.asset = ID('a');
        c.gain = 1000;
        p->write((formatCommand(c) + "\n").c_str());
        const QStringList got = lines(*p, 600);
        QVERIFY2(got.contains("started 2 1"), qPrintable(got.join('|')));
        QVERIFY2(got.contains("ended 2 1"), qPrintable(got.join('|')));
        p->write("quit\n");
        QVERIFY(p->waitForFinished(3000));
        QCOMPARE(p->exitCode(), 0);
    }

    void aScheduledStartLandsWhereItWasAskedWithinAFewMilliseconds()
    {
        QFile::remove(m_log);
        makeAsset(m_dir.path(), ID('b'), 5000);
        auto p = start();
        QVERIFY(p);
        QList<long long> errors;
        for (int i = 0; i < 20; ++i) {
            Command c;
            c.kind = Command::Kind::Play;
            c.pet = i;
            c.generation = 1;
            c.asset = ID('b');
            c.gain = 500;
            c.atNs = monotonicNs() + 60'000'000 + i * 3'700'000; // 60 ms ahead, then off the 10 ms grid
            p->write((formatCommand(c) + "\n").c_str());
            QTest::qWait(30);
        }
        lines(*p, 500);
        QFile log(m_log);
        QVERIFY(log.open(QIODevice::ReadOnly));
        int seen = 0;
        for (const QByteArray &line : log.readAll().split('\n')) {
            const QList<QByteArray> w = line.split(' ');
            if (w.size() != 5 || w[0] != "start")
                continue;
            errors << w[4].toLongLong() - w[3].toLongLong();
            ++seen;
        }
        QCOMPARE(seen, 20);
        std::sort(errors.begin(), errors.end());
        // placed on the sample: within one 10 ms step of the render loop (the paced backend renders 480 frames at a time, so up to one step late)
        QVERIFY2(errors.first() > -1'000'000, qPrintable(QString::number(errors.first())));
        QVERIFY2(errors.at(18) < 12'000'000, qPrintable(QString::number(errors.at(18))));
        p->write("quit\n");
        p->waitForFinished(3000);
    }

    void aMissingOrInvalidAssetIsReportedNotFatal()
    {
        QFile bad(m_dir.filePath(QString::fromStdString(ID('c')) + ".wav"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(QByteArray(500, 'x'));
        bad.close();
        auto p = start();
        QVERIFY(p);
        Command c;
        c.kind = Command::Kind::Play;
        c.pet = 1;
        c.generation = 4;
        c.asset = ID('d'); // not there
        p->write((formatCommand(c) + "\n").c_str());
        c.asset = ID('c'); // not a sound
        c.generation = 5;
        p->write((formatCommand(c) + "\n").c_str());
        const QStringList got = lines(*p, 400);
        QVERIFY2(got.contains("failed 1 4 audio asset missing"), qPrintable(got.join('|')));
        QVERIFY2(got.contains("failed 1 5 audio asset invalid"), qPrintable(got.join('|')));
        QCOMPARE(p->state(), QProcess::Running);
        p->write("quit\n");
        p->waitForFinished(3000);
    }

    void aLineThatIsNotACommandClosesThePlayer()
    {
        auto p = start();
        QVERIFY(p);
        p->write("hello there\n");
        QVERIFY(p->waitForFinished(3000));
        QCOMPARE(p->exitCode(), 2);
        auto q = start();
        QVERIFY(q);
        q->write(QByteArray(1000, 'x')); // no newline: a line that never ends
        QVERIFY(q->waitForFinished(3000));
        QCOMPARE(q->exitCode(), 2);
    }

    void aFloodOfCommandsIsSurvived()
    {
        makeAsset(m_dir.path(), ID('e'), 4800);
        auto p = start();
        QVERIFY(p);
        Command c;
        c.kind = Command::Kind::Play;
        c.asset = ID('e');
        c.gain = 100;
        QByteArray flood;
        for (int i = 0; i < 20000; ++i) {
            c.pet = i % 40;
            c.generation = unsigned(i);
            flood += (formatCommand(c) + "\n").c_str();
        }
        p->write(flood);
        lines(*p, 800);
        QCOMPARE(p->state(), QProcess::Running);
        p->write("quit\n");
        QVERIFY(p->waitForFinished(3000));
    }

    void anIdlePlayerExitsOnItsOwnAndAClosedPipeEndsIt()
    {
        auto p = start(0.3);
        QVERIFY(p);
        QVERIFY(p->waitForFinished(3000));
        QCOMPARE(p->exitCode(), 0);
        auto q = start();
        QVERIFY(q);
        q->closeWriteChannel();
        QVERIFY(q->waitForFinished(3000));
        QCOMPARE(q->exitCode(), 0);
    }

    void aStopOnAPausedSoundReachesTheCorkedPlayerSoItCanLeave()
    {
        makeAsset(m_dir.path(), ID('f'), 480000);
        auto p = start(0.6);
        QVERIFY(p);
        Command c;
        c.kind = Command::Kind::Play;
        c.pet = 3;
        c.generation = 1;
        c.asset = ID('f');
        c.gain = 500;
        c.loop = true;
        p->write((formatCommand(c) + "\n").c_str());
        QVERIFY(lines(*p, 400).contains("started 3 1"));
        p->write("pause 3\n"); // the stream is corked now
        lines(*p, 300);
        p->write("resume 3\n");
        p->write("pause 3\n");
        lines(*p, 300);
        p->write("stop 3\n"); // must wake the player to be applied, or the sound stays and the player never leaves
        QVERIFY(p->waitForFinished(5000));
        QCOMPARE(p->exitCode(), 0);
    }

    void theSelfTestPasses()
    {
        QProcess p;
        p.start(MURASAKI_PETAUDIO_BIN, {"--selftest"});
        QVERIFY(p.waitForFinished(5000));
        QCOMPARE(p.exitCode(), 0);
    }

    void importModeWritesOneLineOfJson()
    {
        QProcess make;
        make.start("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "sine=duration=0.3", m_dir.filePath("t.wav")});
        if (!make.waitForFinished(30000) || make.exitCode() != 0)
            QSKIP("needs ffmpeg");
        QProcess p;
        p.start(MURASAKI_PETAUDIO_BIN, {"--import", m_dir.filePath("t.wav"), m_dir.path(), "--name", "my \"sound\""});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        const QByteArray line = p.readAllStandardOutput();
        QVERIFY2(line.contains("\"ok\":true"), line.constData());
        QVERIFY(line.contains("\"durationMs\":3"));
        QProcess q;
        q.start(MURASAKI_PETAUDIO_BIN, {"--import", m_dir.filePath("nothing.wav"), m_dir.path()});
        QVERIFY(q.waitForFinished(30000));
        QCOMPARE(q.exitCode(), 1);
        QVERIFY(q.readAllStandardOutput().contains("\"ok\":false"));
    }
};

QTEST_GUILESS_MAIN(TstPetAudio)
#include "tst_petaudio.moc"
