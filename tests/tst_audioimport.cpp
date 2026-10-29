// The import of a sound file (src/audio/audioimport.h) and the asset format (petaudioasset.h). Fixtures are made with the ffmpeg binary at run time.

#include "audioimport.h"
#include "petaudioasset.h"
#include "sha256.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace murasaki::audio;
using murasaki::kMaxPetAudioMs;

namespace {

bool startsWith(const std::string &text, std::string_view name) { return text.compare(0, name.size(), name) == 0; }

double rms(const MappedClip &clip)
{
    double sum = 0;
    for (std::size_t i = 0; i < clip.frames() * 2; ++i) {
        const double v = clip.data()[i] / 32768.0;
        sum += v * v;
    }
    return std::sqrt(sum / double(clip.frames() * 2));
}

int underAddressLimit(int extraMiB, const std::function<int()> &work)
{
    long pages = 0, resident = 0;
    FILE *statm = fopen("/proc/self/statm", "r");
    if (!statm || fscanf(statm, "%ld %ld", &pages, &resident) != 2)
        return -2;
    fclose(statm);
    const rlim_t limit = rlim_t(pages) * rlim_t(sysconf(_SC_PAGESIZE)) + rlim_t(extraMiB) * 1024 * 1024;
    const pid_t child = fork();
    if (child < 0)
        return -2;
    if (child == 0) {
        const rlimit lim{limit, limit};
        if (setrlimit(RLIMIT_AS, &lim) != 0)
            _exit(90);
        _exit(work());
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

} // namespace

class TstAudioImport : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_ffmpeg;
    int m_n = 0;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    std::string out() const { return m_dir.filePath(QStringLiteral("assets")).toStdString(); }

    // ffmpeg with the lavfi `source` and the output options; the file name decides the container. Null when the encoder is missing.
    QString make(const QString &name, const QString &source, const QStringList &options)
    {
        QStringList args{"-v", "error", "-y", "-f", "lavfi", "-i", source};
        args += options;
        args << path(name);
        QProcess p;
        p.start(m_ffmpeg, args);
        if (!p.waitForFinished(60000) || p.exitCode() != 0)
            return {};
        return path(name);
    }
    ImportResult run(const QString &file, const ImportSettings &settings = {}, const ImportCallbacks &callbacks = {})
    {
        return importAudio(file.toStdString(), out(), QFileInfo(file).fileName().toStdString(), settings, callbacks);
    }

private slots:
    void initTestCase()
    {
        m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (m_ffmpeg.isEmpty() || !importAvailable())
            QSKIP("needs the ffmpeg binary and a build with libav");
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("assets")));
    }

    void sha256MatchesTheReference()
    {
        for (const QByteArray &data : {QByteArray(), QByteArray("abc"), QByteArray(1000, 'x'), QByteArray(55, 'a'), QByteArray(56, 'a'), QByteArray(64, 'a')}) {
            Sha256 sha;
            sha.update(data.constData(), std::size_t(data.size()));
            QCOMPARE(QString::fromStdString(Sha256::hex(sha.finish())), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
        }
        // in pieces
        Sha256 sha;
        const QByteArray data(1000, 'q');
        for (int i = 0; i < 1000; i += 7)
            sha.update(data.constData() + i, std::size_t(std::min(7, 1000 - i)));
        QCOMPARE(QString::fromStdString(Sha256::hex(sha.finish())), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    }

    void theAssetFormatIsVerifiedStrictly()
    {
        std::vector<std::uint8_t> file = wavHeader(4800);
        file.resize(kWavHeaderBytes + 4800 * 4);
        QCOMPARE(verifyWavHeader(file.data(), file.size()), 4800LL);
        QCOMPARE(verifyWavHeader(file.data(), file.size() - 1), -1LL);   // size disagrees with the file
        QCOMPARE(verifyWavHeader(file.data(), file.size() + 4), -1LL);
        auto bad = [&](std::size_t at, std::uint8_t value) {
            std::vector<std::uint8_t> copy = file;
            copy[at] = value;
            return verifyWavHeader(copy.data(), copy.size());
        };
        QCOMPARE(bad(0, 'X'), -1LL);        // RIFF
        QCOMPARE(bad(8, 'X'), -1LL);        // WAVE
        QCOMPARE(bad(20, 3), -1LL);         // not PCM
        QCOMPARE(bad(22, 1), -1LL);         // mono
        QCOMPARE(bad(24, 0x44), -1LL);      // rate
        QCOMPARE(bad(34, 8), -1LL);         // bits
        QCOMPARE(bad(36, 'x'), -1LL);       // data
        QCOMPARE(bad(41, 0x7f), -1LL);      // a data size that disagrees
        std::vector<std::uint8_t> tiny = wavHeader(10);
        tiny.resize(kWavHeaderBytes + 40);
        QCOMPARE(verifyWavHeader(tiny.data(), tiny.size()), -1LL); // shorter than 20 ms
        QVERIFY(!isAssetId("ABC"));
        QVERIFY(!isAssetId(std::string(64, 'g')));
        QVERIFY(isAssetId(std::string(64, 'a')));
        QVERIFY(assetPath("/x", "../../../etc/passwd").empty());
    }

    void everyAcceptedFormatBecomesTheSameKindOfAsset_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QStringList>("options");
        const QString tone = "sine=frequency=440:duration=0.5:sample_rate=44100";
        QTest::newRow("wav") << "a.wav" << tone << QStringList{"-c:a", "pcm_s16le"};
        QTest::newRow("wav u8 mono 22k") << "u8.wav" << "sine=frequency=440:duration=0.5:sample_rate=22050" << QStringList{"-c:a", "pcm_u8", "-ac", "1"};
        QTest::newRow("wav 24 bit") << "s24.wav" << tone << QStringList{"-c:a", "pcm_s24le"};
        QTest::newRow("wav float") << "f32.wav" << tone << QStringList{"-c:a", "pcm_f32le"};
        QTest::newRow("wav 192k") << "hi.wav" << "sine=frequency=440:duration=0.5:sample_rate=192000" << QStringList{"-c:a", "pcm_s16le"};
        QTest::newRow("wav 8k") << "lo.wav" << "sine=frequency=440:duration=0.5:sample_rate=8000" << QStringList{"-c:a", "pcm_s16le"};
        QTest::newRow("wav 8 channels") << "eight.wav" << tone << QStringList{"-c:a", "pcm_s16le", "-ac", "8"};
        QTest::newRow("mp3") << "a.mp3" << tone << QStringList{"-c:a", "libmp3lame"};
        QTest::newRow("ogg vorbis") << "v.ogg" << tone << QStringList{"-c:a", "libvorbis"};
        QTest::newRow("oga") << "v.oga" << tone << QStringList{"-c:a", "libvorbis"};
        QTest::newRow("ogg opus") << "o.ogg" << tone << QStringList{"-c:a", "libopus"};
        QTest::newRow("flac") << "a.flac" << tone << QStringList{"-c:a", "flac"};
        QTest::newRow("m4a aac") << "a.m4a" << tone << QStringList{"-c:a", "aac"};
        QTest::newRow("adts aac") << "a.aac" << tone << QStringList{"-c:a", "aac", "-f", "adts"};
        QTest::newRow("wma") << "a.wma" << tone << QStringList{"-c:a", "wmav2"};
        QTest::newRow("mkv audio") << "a.mka" << tone << QStringList{"-c:a", "flac"};
        QTest::newRow("webm opus") << "a.webm" << tone << QStringList{"-c:a", "libopus"};
        QTest::newRow("a name that lies") << "really-flac.mp3" << tone << QStringList{"-c:a", "flac", "-f", "flac"};
    }
    void everyAcceptedFormatBecomesTheSameKindOfAsset()
    {
        QFETCH(QString, name);
        QFETCH(QString, source);
        QFETCH(QStringList, options);
        const QString file = make(name, source, options);
        if (file.isEmpty())
            QSKIP("this ffmpeg cannot make the fixture");
        const ImportResult r = run(file);
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(isAssetId(r.assetId));
        QVERIFY2(r.durationMs >= 450 && r.durationMs <= 560, std::to_string(r.durationMs).c_str());
        std::string error;
        const auto clip = MappedClip::open(assetPath(out(), r.assetId), &error);
        QVERIFY2(clip, error.c_str());
        QVERIFY(std::llabs(static_cast<long long>(clip->frames()) - 24000) < 2600);
        const double level = rms(*clip);
        QVERIFY2(level > 0.01 && level < 0.3, std::to_string(level).c_str()); // the sine of lavfi is about 0.35 peak: a live signal, not silence or noise
        QFile sourceFile(file);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromStdString(r.assetId), QString::fromLatin1(QCryptographicHash::hash(sourceFile.readAll(), QCryptographicHash::Sha256).toHex()));
        QVERIFY(QFile::exists(QString::fromStdString(out() + "/" + r.assetId + ".json")));
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), r.assetId) + ".part")));
        if (name == "eight.wav")
            QVERIFY(std::find(r.degraded.begin(), r.degraded.end(), std::string(murasaki::kAudioDownmixed)) != r.degraded.end());
        if (name == "a.wav")
            QVERIFY(std::find(r.degraded.begin(), r.degraded.end(), std::string(murasaki::kAudioResampled)) != r.degraded.end());
    }

    void aPcmFileAt48kIsCopiedSampleForSample()
    {
        const QString file = make("exact.wav", "sine=frequency=1000:duration=0.25:sample_rate=48000", {"-c:a", "pcm_s16le", "-ac", "2"});
        QVERIFY(!file.isEmpty());
        const ImportResult r = run(file);
        QVERIFY2(r.ok, r.error.c_str());
        const auto clip = MappedClip::open(assetPath(out(), r.assetId), nullptr);
        QVERIFY(clip);
        QProcess p;
        p.start(m_ffmpeg, {"-v", "error", "-i", file, "-f", "s16le", "-ar", "48000", "-ac", "2", "-"});
        QVERIFY(p.waitForFinished(30000));
        const QByteArray reference = p.readAllStandardOutput();
        QCOMPARE(qsizetype(clip->frames() * 4), reference.size());
        int worst = 0;
        const auto *ref = reinterpret_cast<const std::int16_t *>(reference.constData());
        for (std::size_t i = 0; i < clip->frames() * 2; ++i)
            worst = std::max(worst, std::abs(int(clip->data()[i]) - int(ref[i])));
        QVERIFY2(worst <= 1, std::to_string(worst).c_str());
    }

    void theSameSourceIsTheSameAsset()
    {
        const QString file = path("a.wav");
        const ImportResult first = run(file);
        QVERIFY(first.ok);
        const ImportResult again = run(file);
        QVERIFY(again.ok);
        QVERIFY(again.reused);
        QCOMPARE(again.assetId, first.assetId);
        QCOMPARE(again.durationMs, first.durationMs);
    }

    void aVideoGivesItsSoundAndAVideoWithoutOneSaysSo()
    {
        const QString withSound = make("clip.mp4", "testsrc=s=64x48:r=10:d=1", {"-f", "lavfi", "-i", "sine=duration=1", "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac"});
        QVERIFY(!withSound.isEmpty());
        const ImportResult r = run(withSound);
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(r.durationMs >= 950 && r.durationMs <= 1100);
        const QString silent = make("silent.mp4", "testsrc=s=64x48:r=10:d=1", {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
        QVERIFY(!silent.isEmpty());
        const ImportResult none = run(silent);
        QVERIFY(!none.ok);
        QVERIFY2(startsWith(none.error, murasaki::kAudioNoStream), none.error.c_str());
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), none.assetId))));
    }

    void whatIsNotAudioIsRefusedByName()
    {
        QFile srt(path("subs.mp3"));
        QVERIFY(srt.open(QIODevice::WriteOnly));
        srt.write("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n");
        srt.close();
        QFile hls(path("list.m4a"));
        QVERIFY(hls.open(QIODevice::WriteOnly));
        hls.write("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nfile:///etc/passwd\n#EXT-X-ENDLIST\n");
        hls.close();
        QFile concat(path("cat.mka"));
        QVERIFY(concat.open(QIODevice::WriteOnly));
        concat.write("ffconcat version 1.0\nfile '" + path("a.wav").toUtf8() + "'\n");
        concat.close();
        QFile noise(path("noise.wav"));
        QVERIFY(noise.open(QIODevice::WriteOnly));
        noise.write(QByteArray(8000, 'x'));
        noise.close();
        QFile empty(path("empty.flac"));
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        QVERIFY(QFile::link("/dev/zero", path("zero.mp3")));
        for (const char *name : {"subs.mp3", "list.m4a", "cat.mka", "noise.wav", "empty.flac"}) {
            const ImportResult r = run(path(name));
            QVERIFY2(!r.ok, name);
            QVERIFY2(startsWith(r.error, murasaki::kAudioNotAudio) || startsWith(r.error, murasaki::kAudioContainerNotAllowed), (std::string(name) + ": " + r.error).c_str());
        }
        QVERIFY(startsWith(run(path("zero.mp3")).error, "source not a regular file"));
        QVERIFY(startsWith(run(path("nothing.mp3")).error, "source not found"));
        QVERIFY(startsWith(run(m_dir.path()).error, "source not a regular file"));
        // nothing was left behind by any of them
        for (const QString &f : QDir(path("assets")).entryList(QDir::Files))
            QVERIFY2(!f.endsWith(".part"), qPrintable(f));
    }

    void tooShortAndTooLongAreRefused()
    {
        const QString shortFile = make("blip.wav", "sine=frequency=440:duration=0.005:sample_rate=48000", {"-c:a", "pcm_s16le"});
        QVERIFY(!shortFile.isEmpty());
        QVERIFY2(startsWith(run(shortFile).error, murasaki::kAudioTooShort), run(shortFile).error.c_str());
        // 11 minutes, mono 8 kHz u8: 5 MB on disk, refused from the claim before anything is decoded
        const QString longFile = make("long.wav", "anullsrc=r=8000:cl=mono", {"-t", "660", "-c:a", "pcm_u8"});
        QVERIFY(!longFile.isEmpty());
        const ImportResult r = run(longFile);
        QVERIFY(!r.ok);
        QVERIFY2(startsWith(r.error, murasaki::kAudioTooLong), r.error.c_str());
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), r.assetId))));
        // exactly ten minutes is fine
        const QString ten = make("ten.wav", "anullsrc=r=8000:cl=mono", {"-t", "600", "-c:a", "pcm_u8"});
        QVERIFY(!ten.isEmpty());
        QVERIFY2(run(ten).ok, run(ten).error.c_str());
        QVERIFY(QFile::remove(QString::fromStdString(assetPath(out(), run(ten).assetId)))); // keep the assets folder small
    }

    void aTruncatedFileKeepsWhatWasDecoded()
    {
        const QString file = make("whole.flac", "sine=frequency=300:duration=4:sample_rate=48000", {"-c:a", "flac"});
        QVERIFY(!file.isEmpty());
        QFile in(file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray whole = in.readAll();
        QFile cut(path("cut.flac"));
        QVERIFY(cut.open(QIODevice::WriteOnly));
        cut.write(whole.left(whole.size() / 2));
        cut.close();
        const ImportResult r = run(path("cut.flac"));
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(r.durationMs > 500 && r.durationMs < 3500);
        QFile head(path("head.flac"));
        QVERIFY(head.open(QIODevice::WriteOnly));
        head.write(whole.left(30));
        head.close();
        QVERIFY(!run(path("head.flac")).ok);
    }

    void theDeadlineAndACancelStopTheImportAndLeaveNothing()
    {
        const QString file = make("slow.flac", "sine=frequency=300:duration=20:sample_rate=48000", {"-c:a", "flac"});
        QVERIFY(!file.isEmpty());
        ImportSettings s;
        s.deadlineMs = 1;
        const ImportResult late = run(file, s);
        QVERIFY(!late.ok);
        QVERIFY2(startsWith(late.error, murasaki::kAudioTimeout), late.error.c_str());
        QVERIFY(!late.cancelled);
        ImportCallbacks cb;
        cb.cancelled = [] { return true; };
        const ImportResult stopped = run(file, {}, cb);
        QVERIFY(!stopped.ok);
        QVERIFY(stopped.cancelled);
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), stopped.assetId))));
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), stopped.assetId) + ".part")));
        int polls = 0;
        cb.cancelled = [&] { return ++polls > 20; };
        const ImportResult midway = run(file, {}, cb);
        QVERIFY(midway.cancelled);
        QVERIFY(!QFile::exists(QString::fromStdString(assetPath(out(), midway.assetId) + ".part")));
    }

    void aLongSoundIsDecodedWithBoundedMemory()
    {
        const QString file = make("minute.flac", "anullsrc=r=8000:cl=mono", {"-t", "60", "-c:a", "flac"});
        QVERIFY(!file.isEmpty());
        const std::string source = file.toStdString();
        const std::string outDir = path("rlimit").toStdString();
        QVERIFY(QDir().mkpath(path("rlimit")));
        const int code = underAddressLimit(300, [&] {
            const ImportResult r = importAudio(source, outDir, "minute");
            return r.ok && r.durationMs >= 59000 ? 0 : 1;
        });
        QCOMPARE(code, 0);
    }

    void collectingKeepsWhatIsReferencedAndWhatIsYoung()
    {
        QVERIFY(QDir().mkpath(path("gc")));
        const std::string dir = path("gc").toStdString();
        const std::string a(64, 'a'), b(64, 'b'), c(64, 'c');
        for (const std::string &id : {a, b, c}) {
            QFile w(QString::fromStdString(dir + "/" + id + ".wav"));
            QVERIFY(w.open(QIODevice::WriteOnly));
            w.write("x");
            QFile j(QString::fromStdString(dir + "/" + id + ".json"));
            QVERIFY(j.open(QIODevice::WriteOnly));
            j.write("{}");
        }
        QFile other(path("gc/notes.txt"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("keep");
        other.close();
        // nothing is old enough
        QCOMPARE(collectAssets(dir, {a}, 3600), 0);
        QCOMPARE(collectAssets(dir, {a}, 0), 2);
        QVERIFY(QFile::exists(QString::fromStdString(dir + "/" + a + ".wav")));
        QVERIFY(QFile::exists(QString::fromStdString(dir + "/" + a + ".json")));
        QVERIFY(!QFile::exists(QString::fromStdString(dir + "/" + b + ".wav")));
        QVERIFY(!QFile::exists(QString::fromStdString(dir + "/" + b + ".json")));
        QVERIFY(QFile::exists(path("gc/notes.txt"))); // not an asset's name: untouched
    }
};

QTEST_GUILESS_MAIN(TstAudioImport)
#include "tst_audioimport.moc"
