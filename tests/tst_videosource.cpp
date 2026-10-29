// The video source (videosource.h): clips are made with the ffmpeg binary at run time (the test skips without it), so that what is decoded is known.

#include "keyedsink.h"
#include "videosource.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace murasaki::import;

namespace {

class Capture final : public FrameSink
{
public:
    explicit Capture(long long budget = 0) : m_budget(budget) {}
    bool begin(int w, int h) override
    {
        width = w;
        height = h;
        began++;
        return true;
    }
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override
    {
        bytes += qint64(width) * height * 4;
        if (m_budget > 0 && bytes > m_budget) {
            over = true;
            return false;
        }
        QByteArray f;
        for (int y = 0; y < height; ++y)
            f.append(reinterpret_cast<const char *>(rgba + stride * y), width * 4);
        got.append(f);
        delays.append(delayMs);
        return true;
    }
    bool finish() override
    {
        finished = true;
        return true;
    }
    int frames() const override { return int(got.size()); }
    long long bytesWritten() const override { return bytes; }
    QString error() const override { return over ? QStringLiteral("budget") : QString(); }
    bool overBudget() const override { return over; }

    int width = 0, height = 0, began = 0;
    bool finished = false, over = false;
    qint64 bytes = 0;
    QList<QByteArray> got;
    QList<int> delays;

    long long total() const
    {
        long long t = 0;
        for (int d : delays)
            t += d;
        return t;
    }
    // Channel c (0..3) of the pixel (x, y) of frame i.
    int at(int i, int x, int y, int c) const { return uchar(got.at(i).at((y * width + x) * 4 + c)); }

private:
    long long m_budget;
};

bool startsWith(const QString &text, std::string_view name) { return text.startsWith(QString::fromUtf8(name.data(), qsizetype(name.size()))); }

// Rewrites the first self-contained data reference of an MP4 (the moov is at the end of the file, so no offset moves) into one that names `target`,
// and grows the boxes above it. True when the file was patched.
bool pointDrefAt(const QString &file, const QString &target)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray d = f.readAll();
    f.close();
    auto u32 = [&](qsizetype at) { return quint32(uchar(d[at]) << 24 | uchar(d[at + 1]) << 16 | uchar(d[at + 2]) << 8 | uchar(d[at + 3])); };
    auto put32 = [&](qsizetype at, quint32 v) {
        d[at] = char(v >> 24);
        d[at + 1] = char(v >> 16);
        d[at + 2] = char(v >> 8);
        d[at + 3] = char(v);
    };
    // Walk moov > trak > mdia > minf > dinf > dref.
    QList<qsizetype> chain;
    qsizetype from = 0, to = d.size();
    for (const char *type : {"moov", "trak", "mdia", "minf", "dinf", "dref"}) {
        qsizetype at = from, found = -1;
        while (at + 8 <= to) {
            const quint32 size = u32(at);
            if (size < 8 || at + size > to)
                return false;
            if (d.mid(at + 4, 4) == type) {
                found = at;
                break;
            }
            at += size;
        }
        if (found < 0)
            return false;
        chain.append(found);
        from = found + 8 + (QByteArray(type) == "dref" ? 8 : 0);
        to = found + qsizetype(u32(found));
    }
    const qsizetype dref = chain.last();
    const qsizetype entry = dref + 16; // size, 'dref', version+flags, entry count
    if (d.mid(entry + 4, 4) != "url " || u32(entry) != 12)
        return false;
    QByteArray box;
    const QByteArray location = target.toUtf8() + '\0';
    box.resize(12);
    box.fill(0);
    box.replace(4, 4, "url ");
    // flags 0: the data is in the file the location names
    box.append(location);
    const quint32 newSize = quint32(box.size());
    box[0] = char(newSize >> 24);
    box[1] = char(newSize >> 16);
    box[2] = char(newSize >> 8);
    box[3] = char(newSize);
    const qint32 delta = qint32(newSize) - 12;
    d.replace(entry, 12, box);
    for (qsizetype at : chain)
        put32(at, u32(at) + quint32(delta));
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(d);
    return true;
}

// Runs `work` in a child process whose address space is limited to what the test process uses now plus `extraMiB`; the child's exit code is the answer.
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

class TstVideoSource : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_ffmpeg;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // Runs ffmpeg with lavfi input `source`; empty `extra` skips nothing. Returns the output path, or a null string when the encoder is missing.
    QString make(const QString &name, const QString &source, const QStringList &codec)
    {
        QStringList args{"-v", "error", "-y", "-f", "lavfi", "-i", source};
        args += codec;
        args << path(name);
        QProcess p;
        p.start(m_ffmpeg, args);
        if (!p.waitForFinished(60000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
            return {};
        return path(name);
    }

private slots:
    void initTestCase()
    {
        m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (m_ffmpeg.isEmpty() || !videoSourceAvailable())
            QSKIP("needs the ffmpeg binary and a build with libav");
        QVERIFY(m_dir.isValid());
    }

    void theAllowListIsTheContainersOfThePlan()
    {
        for (const char *ok : {"mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm", "avi", "gif", "apng", "mov", "webm"})
            QVERIFY2(videoContainerAllowed(ok), ok);
        for (const char *no : {"concat", "hls", "hls,applehttp", "dash", "image2", "srt", "ffconcat", "lavfi", "rtsp", "mpegts", "flv", "", "movx", "gif_pipe", "apng2"})
            QVERIFY2(!videoContainerAllowed(no), no);
        QVERIFY(!videoContainerAllowed(nullptr));
    }

    void settingsHaveThePlansDefaultsAndAreChecked()
    {
        const VideoSettings d;
        QCOMPARE(d.maxSide, 1024);
        QCOMPARE(d.maxFps, 30.0);
        QCOMPARE(d.maxFrames, kMaxFrames);
        QVERIFY(validateVideoSettings(d).isEmpty());
        VideoSettings s;
        s.maxSide = kMaxCanvasSide;
        s.maxFps = 60;
        QVERIFY(validateVideoSettings(s).isEmpty());
        for (int side : {0, 15, kMaxCanvasSide + 1, -1}) {
            VideoSettings b;
            b.maxSide = side;
            QVERIFY2(!validateVideoSettings(b).isEmpty(), qPrintable(QString::number(side)));
        }
        for (double fps : {0.0, 0.5, 61.0, -3.0, std::nan(""), HUGE_VAL}) {
            VideoSettings b;
            b.maxFps = fps;
            QVERIFY(!validateVideoSettings(b).isEmpty());
        }
        VideoSettings b;
        b.startMs = -1;
        QVERIFY(!validateVideoSettings(b).isEmpty());
        b = {};
        b.durationMs = -5;
        QVERIFY(!validateVideoSettings(b).isEmpty());
        b = {};
        b.maxFrames = 0;
        QVERIFY(!validateVideoSettings(b).isEmpty());
        b.maxFrames = kMaxFrames + 1;
        QVERIFY(!validateVideoSettings(b).isEmpty());
        // A bad setting is a spec refusal before the file is looked at.
        Capture sink;
        VideoSettings bad;
        bad.maxSide = 3;
        const VideoOutcome out = decodeVideo(path("none.mp4"), bad, sink);
        QVERIFY(!out.ok);
        QVERIFY(startsWith(out.error, kSpecRefused));
        QVERIFY(!probeVideo(path("none.mp4"), bad).ok);
    }

    void anOpaqueH264ClipDecodesAtItsOwnSizeAndSaysItIsOpaque()
    {
        const QString clip = make("opaque.mp4", "color=c=0x00ff00:s=64x48:r=10:d=1", {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
        QVERIFY(!clip.isEmpty());
        const VideoInfo info = probeVideo(clip, {});
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.width, 64);
        QCOMPARE(info.height, 48);
        QCOMPARE(info.outWidth, 64);
        QVERIFY(!info.hasAlpha);
        QVERIFY(info.degraded.contains(QString::fromUtf8(kVideoOpaque.data())));
        QVERIFY(info.container.contains("mp4"));
        QCOMPARE(info.estimatedFrames, 10);

        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(sink.began, 1);
        QVERIFY(sink.finished);
        QCOMPARE(sink.width, 64);
        QCOMPARE(sink.height, 48);
        QCOMPARE(out.frames, 10);
        QCOMPARE(sink.frames(), 10);
        QVERIFY(!out.hasAlpha);
        QVERIFY(out.degraded.contains(QString::fromUtf8(kVideoOpaque.data())));
        QVERIFY(sink.total() >= 990 && sink.total() <= 1010);
        for (int d : sink.delays)
            QVERIFY(d >= 99 && d <= 101);
        // Green, fully opaque (HD-sized limits-range maths within a few levels).
        QVERIFY(sink.at(3, 10, 10, 1) > 245);
        QVERIFY(sink.at(3, 10, 10, 0) < 12);
        QVERIFY(sink.at(3, 10, 10, 2) < 12);
        QCOMPARE(sink.at(3, 10, 10, 3), 255);
    }

    void theColourKeyCutsTheBackgroundOfAnOpaqueVideo()
    {
        const QString clip = path("opaque.mp4");
        QVERIFY(QFile::exists(clip));
        Capture sink;
        ColorKey key;
        key.enabled = true; // green, the defaults
        KeyedSink keyed(sink, key);
        const VideoOutcome out = decodeVideo(clip, {}, keyed);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(sink.at(0, 5, 5, 3), 0);
        QCOMPARE(sink.at(9, 60, 40, 3), 0);
    }

    void aVideoLargerThanTheLongestSideIsScaledDownAndNeverUp()
    {
        const QString clip = make("big.mp4", "color=c=blue:s=320x240:r=10:d=1", {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
        QVERIFY(!clip.isEmpty());
        VideoSettings s;
        s.maxSide = 160;
        Capture small;
        const VideoOutcome a = decodeVideo(clip, s, small);
        QVERIFY2(a.ok, qPrintable(a.error));
        QCOMPARE(small.width, 160);
        QCOMPARE(small.height, 120);
        QVERIFY(a.degraded.contains(QString::fromUtf8(kVideoScaledDown.data())));
        QCOMPARE(small.at(0, 80, 60, 2) > 245, true);
        Capture own;
        const VideoOutcome b = decodeVideo(clip, {}, own); // 1024 by default: 320x240 stays
        QVERIFY(b.ok);
        QCOMPARE(own.width, 320);
        QVERIFY(!b.degraded.contains(QString::fromUtf8(kVideoScaledDown.data())));
        const VideoInfo info = probeVideo(clip, s);
        QCOMPARE(info.outWidth, 160);
        QCOMPARE(info.outHeight, 120);
    }

    void theFrameRateCapDropsFramesByTimestampAndKeepsTheLength()
    {
        const QString clip = make("fps60.mp4", "testsrc=s=64x64:r=60:d=2", {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
        QVERIFY(!clip.isEmpty());
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink); // 30 fps by default
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY2(out.frames >= 59 && out.frames <= 61, qPrintable(QString::number(out.frames)));
        QVERIFY(out.degraded.contains(QString::fromUtf8(kVideoFpsLowered.data())));
        QVERIFY2(sink.total() >= 1980 && sink.total() <= 2030, qPrintable(QString::number(sink.total())));
        VideoSettings s;
        s.maxFps = 10;
        Capture slow;
        QVERIFY(decodeVideo(clip, s, slow).ok);
        QVERIFY2(slow.frames() >= 19 && slow.frames() <= 21, qPrintable(QString::number(slow.frames())));
        QVERIFY(slow.total() >= 1980 && slow.total() <= 2030);
        s.maxFps = 60; // every frame
        Capture all;
        const VideoOutcome whole = decodeVideo(clip, s, all);
        QVERIFY(whole.ok);
        QCOMPARE(all.frames(), 120);
        QVERIFY(!whole.degraded.contains(QString::fromUtf8(kVideoFpsLowered.data())));
        QVERIFY(whole.hasAlpha == false);
    }

    void aTrimKeepsTheWindowOnly()
    {
        const QString clip = path("fps60.mp4");
        VideoSettings s;
        s.maxFps = 10;
        s.startMs = 500;
        s.durationMs = 1000;
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, s, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY2(sink.frames() >= 9 && sink.frames() <= 11, qPrintable(QString::number(sink.frames())));
        QVERIFY2(sink.total() >= 980 && sink.total() <= 1020, qPrintable(QString::number(sink.total())));
        // A start past the end leaves nothing.
        s.startMs = 10000;
        s.durationMs = 0;
        Capture none;
        const VideoOutcome empty = decodeVideo(clip, s, none);
        QVERIFY(!empty.ok);
        QVERIFY(startsWith(empty.error, kVideoNoStream));
        QCOMPARE(none.began, 0);
        // The estimate follows the trim.
        VideoSettings t;
        t.maxFps = 10;
        t.durationMs = 500;
        QCOMPARE(probeVideo(clip, t).estimatedFrames, 5);
    }

    void moreFramesThanTheSettingRefuseTheWholeVideo()
    {
        VideoSettings s;
        s.maxFps = 60;
        s.maxFrames = 10;
        Capture sink;
        const VideoOutcome out = decodeVideo(path("fps60.mp4"), s, sink);
        QVERIFY(!out.ok);
        QVERIFY(startsWith(out.error, kTooManyFrames));
        QVERIFY(!sink.finished);
    }

    void theSinkBudgetEndsTheImport()
    {
        Capture sink(64 * 64 * 4 * 5);
        const VideoOutcome out = decodeVideo(path("fps60.mp4"), {}, sink);
        QVERIFY(!out.ok);
        QVERIFY(out.overBudget);
        QVERIFY(startsWith(out.error, kOutputOverBudget));
    }

    void cancellationStopsAndKeepsNothingFinished()
    {
        int polls = 0;
        VideoCallbacks cb;
        cb.cancelled = [&] { return ++polls > 5; };
        Capture sink;
        const VideoOutcome out = decodeVideo(path("fps60.mp4"), {}, sink, cb);
        QVERIFY(!out.ok);
        QVERIFY(out.cancelled);
        QVERIFY(startsWith(out.error, kCancelled));
        QVERIFY(!sink.finished);
    }

    void progressGrowsWithTheFrames()
    {
        double last = -2;
        int calls = 0;
        VideoCallbacks cb;
        cb.progress = [&](double fraction, int) {
            QVERIFY(fraction >= last || fraction < 0);
            last = fraction;
            ++calls;
        };
        Capture sink;
        QVERIFY(decodeVideo(path("fps60.mp4"), {}, sink, cb).ok);
        QVERIFY(calls > 10);
        QVERIFY(last > 0.9 && last <= 1.0);
    }

    void aVp9WebmWithAlphaKeepsIt()
    {
        const QString clip = make("alpha.webm", "color=c=red@0.5:s=64x64:r=10:d=1,format=yuva420p",
                                  {"-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-auto-alt-ref", "0"});
        if (clip.isEmpty())
            QSKIP("this ffmpeg has no libvpx-vp9 encoder");
        const VideoInfo info = probeVideo(clip, {});
        QVERIFY2(info.ok, qPrintable(info.error));
        QVERIFY(info.hasAlpha);
        QVERIFY(!info.degraded.contains(QString::fromUtf8(kVideoOpaque.data())));
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY(out.hasAlpha);
        QVERIFY(!out.degraded.contains(QString::fromUtf8(kVideoOpaque.data())));
        const int alpha = sink.at(2, 32, 32, 3);
        QVERIFY2(alpha > 120 && alpha < 136, qPrintable(QString::number(alpha)));
        QVERIFY(sink.at(2, 32, 32, 0) > 235); // straight alpha: the red is not darkened by it
    }

    void aProRes4444MovWithAlphaKeepsIt()
    {
        const QString clip = make("alpha.mov", "color=c=red@0.5:s=64x64:r=10:d=1,format=yuva444p10le",
                                  {"-c:v", "prores_ks", "-profile:v", "4444", "-pix_fmt", "yuva444p10le"});
        if (clip.isEmpty())
            QSKIP("this ffmpeg has no prores_ks encoder");
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY(out.hasAlpha);
        const int alpha = sink.at(1, 10, 10, 3);
        QVERIFY2(alpha > 120 && alpha < 136, qPrintable(QString::number(alpha)));
    }

    void aLosslessMatroskaWithAlphaKeepsIt()
    {
        const QString clip = make("alpha.mkv", "color=c=blue@0.25:s=32x32:r=5:d=1,format=yuva420p", {"-c:v", "ffv1", "-pix_fmt", "yuva420p"});
        QVERIFY(!clip.isEmpty());
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY(out.hasAlpha);
        const int alpha = sink.at(0, 5, 5, 3);
        QVERIFY2(alpha > 58 && alpha < 70, qPrintable(QString::number(alpha)));
    }

    void anAviAndAGifAreInTheList()
    {
        const QString avi = make("a.avi", "testsrc=s=32x32:r=10:d=1", {"-c:v", "mpeg4"});
        QVERIFY(!avi.isEmpty());
        Capture a;
        QVERIFY2(decodeVideo(avi, {}, a).ok, "avi");
        QCOMPARE(a.frames(), 10);
        const QString gif = make("a.gif", "testsrc=s=32x32:r=10:d=1", {});
        QVERIFY(!gif.isEmpty());
        Capture g;
        const VideoOutcome out = decodeVideo(gif, {}, g);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(g.frames(), 10);
    }

    void aContainerOutsideTheListIsRefusedByName()
    {
        // Text of the formats that read other files or the network, under the names of allowed ones.
        QFile srt(path("subs.mp4"));
        QVERIFY(srt.open(QIODevice::WriteOnly));
        srt.write("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n2\n00:00:01,000 --> 00:00:02,000\nworld\n");
        srt.close();
        QFile hls(path("list.mp4"));
        QVERIFY(hls.open(QIODevice::WriteOnly));
        hls.write("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nfile:///etc/passwd\n#EXT-X-ENDLIST\n");
        hls.close();
        QFile concat(path("cat.mkv"));
        QVERIFY(concat.open(QIODevice::WriteOnly));
        concat.write("ffconcat version 1.0\nfile '" + path("opaque.mp4").toUtf8() + "'\n");
        concat.close();
        QFile mpegts(path("ts.mp4"));
        QVERIFY(mpegts.open(QIODevice::WriteOnly));
        QByteArray ts;
        for (int i = 0; i < 40; ++i) {
            QByteArray packet(188, char(0xff));
            packet[0] = 0x47;
            packet[1] = 0x1f;
            packet[2] = char(0xff);
            packet[3] = 0x10;
            ts += packet;
        }
        mpegts.write(ts);
        mpegts.close();
        for (const QString name : {"subs.mp4", "list.mp4", "cat.mkv", "ts.mp4"}) {
            Capture sink;
            const VideoOutcome out = decodeVideo(path(name), {}, sink);
            QVERIFY2(!out.ok, qPrintable(name));
            QVERIFY2(startsWith(out.error, kVideoContainerNotAllowed) || startsWith(out.error, kNotAVideo), qPrintable(name + ": " + out.error));
            QCOMPARE(sink.began, 0);
            QVERIFY2(!probeVideo(path(name), {}).ok, qPrintable(name));
        }
        // The ones the probe can recognise are named, an HLS playlist by its extension.
        QVERIFY(QFile::copy(path("list.mp4"), path("list.m3u8")));
        Capture sink;
        QVERIFY2(startsWith(decodeVideo(path("list.m3u8"), {}, sink).error, kVideoContainerNotAllowed), qPrintable(decodeVideo(path("list.m3u8"), {}, sink).error));
        QVERIFY(startsWith(decodeVideo(path("cat.mkv"), {}, sink).error, kVideoContainerNotAllowed));
        QVERIFY(startsWith(decodeVideo(path("subs.mp4"), {}, sink).error, kVideoContainerNotAllowed));
        QCOMPARE(sink.began, 0);
    }

    void aDeviceAndAMissingFileAreRefusedByTheirOwnNames()
    {
        Capture sink;
        QVERIFY(startsWith(decodeVideo(QStringLiteral("/dev/zero"), {}, sink).error, kSourceNotRegular));
        QVERIFY(startsWith(decodeVideo(path("nothing.mp4"), {}, sink).error, kSourceNotFound));
        QVERIFY(startsWith(decodeVideo(m_dir.path(), {}, sink).error, kSourceNotRegular));
        QVERIFY(QFile::link(QStringLiteral("/dev/zero"), path("zero.mp4")));
        QVERIFY(startsWith(decodeVideo(path("zero.mp4"), {}, sink).error, kSourceNotRegular));
        QVERIFY(startsWith(probeVideo(QStringLiteral("/dev/zero"), {}).error, kSourceNotRegular));
        QCOMPARE(sink.began, 0);
    }

    void aFileThatIsNotAVideoIsNamedSo()
    {
        QFile f(path("noise.mp4"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(5000, 'x'));
        f.close();
        Capture sink;
        const VideoOutcome out = decodeVideo(path("noise.mp4"), {}, sink);
        QVERIFY(!out.ok);
        QVERIFY(startsWith(out.error, kNotAVideo));
        QFile empty(path("empty.mp4"));
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        QVERIFY(startsWith(decodeVideo(path("empty.mp4"), {}, sink).error, kNotAVideo));
    }

    void anAudioOnlyFileHasNoVideoStream()
    {
        QProcess p;
        p.start(m_ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "sine=d=1", "-c:a", "aac", path("sound.m4a")});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        Capture sink;
        const VideoOutcome out = decodeVideo(path("sound.m4a"), {}, sink);
        QVERIFY(!out.ok);
        QVERIFY2(startsWith(out.error, kVideoNoStream), qPrintable(out.error));
    }

    void aTruncatedFileKeepsTheFramesBeforeTheDamage()
    {
        const QString clip = make("long.mkv", "testsrc=s=64x64:r=10:d=4", {"-c:v", "ffv1"});
        QVERIFY(!clip.isEmpty());
        QFile in(clip);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray whole = in.readAll();
        in.close();
        QFile out(path("cut.mkv"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(whole.left(whole.size() / 2));
        out.close();
        Capture sink;
        const VideoOutcome o = decodeVideo(path("cut.mkv"), {}, sink);
        QVERIFY2(o.ok, qPrintable(o.error));
        QVERIFY(sink.frames() > 3 && sink.frames() < 40);
        QVERIFY(sink.finished);
        // Cut in the header, there is nothing to keep.
        QFile head(path("head.mkv"));
        QVERIFY(head.open(QIODevice::WriteOnly));
        head.write(whole.left(40));
        head.close();
        Capture none;
        QVERIFY(!decodeVideo(path("head.mkv"), {}, none).ok);
        QCOMPARE(none.began, 0);
    }

    void theDeadlineEndsTheWholeImport()
    {
        VideoSettings s;
        s.deadlineMs = 1;
        Capture sink;
        const VideoOutcome out = decodeVideo(path("fps60.mp4"), s, sink);
        QVERIFY(!out.ok);
        QVERIFY2(startsWith(out.error, kVideoTimeout), qPrintable(out.error));
        QVERIFY(!out.cancelled);
        QVERIFY(!sink.finished);
        s.deadlineMs = 0;
        QVERIFY(!validateVideoSettings(s).isEmpty());
        s.deadlineMs = kMaxDeadlineMs + 1;
        QVERIFY(!validateVideoSettings(s).isEmpty());
        QCOMPARE(VideoSettings().deadlineMs, 600000LL);
    }

    void aCancelBeforeAnythingStopsTheOpen()
    {
        VideoCallbacks cb;
        cb.cancelled = [] { return true; };
        Capture sink;
        const VideoOutcome out = decodeVideo(path("fps60.mp4"), {}, sink, cb);
        QVERIFY(!out.ok);
        QVERIFY(out.cancelled);
        QVERIFY(startsWith(out.error, kCancelled));
        QCOMPARE(sink.began, 0);
    }

    void audioAndAttachmentsAreNeverDecodedOrKept()
    {
        QFile att(path("font.bin"));
        QVERIFY(att.open(QIODevice::WriteOnly));
        att.write(QByteArray(2000, 'z'));
        att.close();
        const QString av = make("av.mp4", "testsrc=s=64x48:r=10:d=1",
                                {"-f", "lavfi", "-i", "sine=d=1", "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac"});
        QVERIFY(!av.isEmpty());
        Capture a;
        const VideoOutcome oa = decodeVideo(av, {}, a);
        QVERIFY2(oa.ok, qPrintable(oa.error));
        QCOMPARE(a.frames(), 10);
        QVERIFY(a.total() >= 990 && a.total() <= 1010);
        const QString mkv = make("att.mkv", "testsrc=s=64x48:r=10:d=1",
                                 {"-c:v", "ffv1", "-attach", path("font.bin"), "-metadata:s:t", "mimetype=application/octet-stream"});
        QVERIFY(!mkv.isEmpty());
        Capture b;
        const VideoOutcome ob = decodeVideo(mkv, {}, b);
        QVERIFY2(ob.ok, qPrintable(ob.error));
        QCOMPARE(b.frames(), 10);
        QVERIFY(probeVideo(mkv, {}).ok);
    }

    void aDataReferenceToAnotherFileIsNotFollowed()
    {
        const QString clip = make("dref.mp4", "testsrc=s=64x48:r=10:d=1", {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
        QVERIFY(!clip.isEmpty());
        QVERIFY2(pointDrefAt(clip, QStringLiteral("file://") + path("not-there.bin")), "patching the dref");
        // With references followed this would fail to open "not-there.bin"; the samples are read from the file itself.
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(sink.frames(), 10);
        QVERIFY(!QFile::exists(path("not-there.bin")));
    }

    void aPictureOverTheCapIsRefusedBeforeAnythingIsDecoded()
    {
        const QString clip = make("huge.mkv", "color=c=black:s=9000x4800:r=1:d=1", {"-c:v", "ffv1", "-threads", "1"});
        if (clip.isEmpty())
            QSKIP("cannot make the 43 megapixel clip");
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY(!out.ok);
        QVERIFY2(startsWith(out.error, kVideoTooLarge), qPrintable(out.error));
        QCOMPARE(sink.began, 0);
        QVERIFY(!probeVideo(clip, {}).ok);
        // And with the address space held at what the process uses plus 400 MiB (a 43 MP frame is 65 MB as YUV, 172 MB as RGBA).
        const int code = underAddressLimit(400, [&] {
            Capture c;
            const VideoOutcome o = decodeVideo(clip, {}, c);
            return !o.ok && startsWith(o.error, kVideoTooLarge) ? 0 : 1;
        });
        QCOMPARE(code, 0);
    }

    void aResolutionChangeInsideTheCapKeepsTheFirstSize()
    {
        QProcess p;
        auto h264 = [&](const QString &name, const QString &size, int frames) {
            p.start(m_ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", QStringLiteral("testsrc=s=%1:r=10:d=%2").arg(size).arg(frames / 10.0), "-c:v", "libx264",
                               "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-f", "h264", path(name)});
            return p.waitForFinished(120000) && p.exitCode() == 0;
        };
        QVERIFY(h264("r1.h264", "64x48", 10));
        QVERIFY(h264("r2.h264", "128x96", 10));
        QFile a(path("r1.h264")), b(path("r2.h264")), joined(path("resize.h264"));
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && joined.open(QIODevice::WriteOnly));
        joined.write(a.readAll());
        joined.write(b.readAll());
        joined.close();
        p.start(m_ffmpeg, {"-v", "error", "-y", "-r", "10", "-f", "h264", "-i", path("resize.h264"), "-c", "copy", path("resize.mkv")});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        Capture sink;
        const VideoOutcome out = decodeVideo(path("resize.mkv"), {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(sink.width, 64);
        QCOMPARE(sink.height, 48);
        QVERIFY2(sink.frames() >= 15, qPrintable(QString::number(sink.frames())));
        for (const QByteArray &f : sink.got)
            QCOMPARE(f.size(), 64 * 48 * 4);
    }

    void aResolutionChangeOverTheCapIsDroppedWithoutAllocatingIt()
    {
        QProcess p;
        p.start(m_ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=64x48:r=10:d=1", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                           "yuv420p", "-f", "h264", path("s1.h264")});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        p.start(m_ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=9000x4800:r=1:d=1", "-c:v", "libx264", "-preset", "ultrafast",
                           "-pix_fmt", "yuv420p", "-threads", "1", "-f", "h264", path("s2.h264")});
        if (!p.waitForFinished(120000) || p.exitCode() != 0)
            QSKIP("cannot make the 43 megapixel frame");
        QFile a(path("s1.h264")), b(path("s2.h264")), joined(path("grow.h264"));
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && joined.open(QIODevice::WriteOnly));
        joined.write(a.readAll());
        joined.write(b.readAll());
        joined.close();
        p.start(m_ffmpeg, {"-v", "error", "-y", "-r", "10", "-f", "h264", "-i", path("grow.h264"), "-c", "copy", path("grow.mkv")});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        const QString file = path("grow.mkv");
        const int code = underAddressLimit(400, [&] {
            Capture c;
            const VideoOutcome o = decodeVideo(file, {}, c);
            // The frames before the oversized one are kept, or the import is refused: either way the process is alive and bounded.
            if (o.ok)
                return c.frames() >= 5 && c.width == 64 ? 0 : 2;
            return startsWith(o.error, kNotAVideo) || startsWith(o.error, kVideoTooLarge) ? 0 : 3;
        });
        QCOMPARE(code, 0);
    }

    void aJumpingClockGivesBoundedDelays()
    {
        const QString clip = make("jump.mkv", "testsrc=s=32x32:r=10:d=1",
                                  {"-vf", "setpts='if(gt(N,4),(N+100000)/(10*TB),N/(10*TB))'", "-fps_mode", "passthrough", "-c:v", "ffv1"});
        QVERIFY(!clip.isEmpty());
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, {}, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QCOMPARE(sink.frames(), 10);
        for (int d : sink.delays)
            QVERIFY(d >= 1 && d <= 60000);
        QVERIFY(sink.delays.at(4) >= 1000); // the gap is long, and capped
    }

    void aScaledPictureWithAlphaKeepsItsColourAtTheEdge()
    {
        // Left half opaque red, right half transparent black, 66 wide: after halving, the edge pixel mixes the two. Straight averaging would
        // darken its red to about half; scaling premultiplied keeps the red and halves only the alpha.
        const QString clip = make("edge.mkv",
                                  "color=c=red:s=33x32:r=5:d=1,format=yuva444p[l];color=c=black@0:s=33x32:r=5:d=1,format=yuva444p[r];[l][r]hstack",
                                  {"-c:v", "ffv1", "-pix_fmt", "yuva444p"});
        QVERIFY(!clip.isEmpty());
        VideoSettings s;
        s.maxSide = 33;
        Capture sink;
        const VideoOutcome out = decodeVideo(clip, s, sink);
        QVERIFY2(out.ok, qPrintable(out.error));
        QVERIFY(out.hasAlpha);
        QCOMPARE(sink.width, 33);
        int mixed = -1;
        for (int x = 0; x < sink.width; ++x) {
            const int a = sink.at(0, x, 8, 3);
            if (a > 20 && a < 235) {
                mixed = x;
                break;
            }
        }
        QVERIFY2(mixed >= 0, "an edge pixel with partial alpha");
        QVERIFY2(sink.at(0, mixed, 8, 0) > 215, qPrintable(QString::number(sink.at(0, mixed, 8, 0))));
        QCOMPARE(sink.at(0, sink.width - 1, 8, 3), 0);
        QCOMPARE(sink.at(0, sink.width - 1, 8, 0), 0);
    }
};

QTEST_GUILESS_MAIN(TstVideoSource)
#include "tst_videosource.moc"
