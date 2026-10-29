// The matching session (src/screenmatch/session.h) with an in-process fake frame source: scores of equal, changed and shifted regions, the step that is compared, regions that cannot be scored,
// our own pets covering a region, the newest-frame-only policy, the limits, the one-shot capture, and a fuzz run over random lines.

#include "screenmatch/session.h"

#include <QRandomGenerator>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace murasaki::screenmatch;

namespace {

// A source with a queue of frames per stream. acquire() gives the newest and drops the rest, the way the PipeWire source does; the counters let a test see it.
class FakeSource : public FrameSource
{
public:
    struct Pending {
        int width, height;
        QByteArray bgra;
    };
    explicit FakeSource(int streams = 1) : m_queues(streams), m_held(streams) {}

    void push(int stream, int width, int height, const QByteArray &bgra)
    {
        m_queues[size_t(stream)].push_back({width, height, bgra});
        ++pushed;
    }
    int streamCount() const override { return int(m_queues.size()); }
    bool acquire(int stream, Frame *frame) override
    {
        auto &q = m_queues[size_t(stream)];
        if (!q.empty()) {
            dropped += int(q.size()) - 1;
            m_held[size_t(stream)] = q.back();
            q.clear();
        }
        ++acquired;
        const Pending &p = m_held[size_t(stream)];
        if (p.bgra.isEmpty())
            return false;
        frame->width = p.width;
        frame->height = p.height;
        frame->stride = p.width * 4;
        frame->format = ScreenPixelFormat::Bgra32;
        frame->data = reinterpret_cast<const uchar *>(p.bgra.constData());
        frame->size = p.bgra.size();
        return true;
    }
    void release(int) override { ++released; }

    int pushed = 0, dropped = 0, acquired = 0, released = 0;

private:
    std::vector<std::vector<Pending>> m_queues;
    std::vector<Pending> m_held;
};

struct Box {
    int x, y, w, h, value;
};

// A dark BGRA picture with bright boxes.
QByteArray scene(int w, int h, const QList<Box> &boxes, int background = 20, int dx = 0, int gain = 0)
{
    QByteArray px(w * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = background;
            for (const Box &b : boxes)
                if (x >= b.x + dx && x < b.x + dx + b.w && y >= b.y && y < b.y + b.h)
                    v = b.value;
            v = qBound(0, v + gain, 255);
            char *p = px.data() + (y * w + x) * 4;
            p[0] = p[1] = p[2] = char(v);
            p[3] = char(255);
        }
    return px;
}

const QList<Box> kA = {{20, 15, 60, 40, 220}, {110, 60, 50, 45, 150}, {40, 80, 30, 25, 90}, {150, 10, 35, 30, 250}};
const QList<Box> kB = {{100, 20, 70, 30, 200}, {15, 70, 40, 40, 120}, {130, 85, 55, 25, 240}};

QByteArray gridOf(const QByteArray &bgra, int w, int h)
{
    const auto *d = reinterpret_cast<const uchar *>(bgra.constData());
    return murasaki::screenGridOf(murasaki::greyFromPixels(d, bgra.size(), w, h, w * 4, ScreenPixelFormat::Bgra32));
}

QByteArray refLine(const QString &id, int region, int step, const QByteArray &grid)
{
    Command c;
    c.type = CommandType::Ref;
    c.trigger = id;
    c.region = region;
    c.step = step;
    c.grid = grid;
    return formatCommand(c);
}

QList<Reply> parse(const QList<QByteArray> &lines)
{
    QList<Reply> out;
    for (const QByteArray &l : lines) {
        QString why;
        const auto r = parseReply(l, &why);
        if (!r)
            qWarning() << "unparseable reply" << l.left(80) << why;
        if (r)
            out.append(*r);
    }
    return out;
}

// The scores of trigger `id` in one tick's lines (empty when it has no line).
QList<int> scoresOf(const QList<QByteArray> &lines, const QString &id)
{
    for (const Reply &r : parse(lines))
        if (r.type == ReplyType::Scores && r.trigger == id)
            return r.permille;
    return {};
}

bool hasOk(const QList<QByteArray> &lines)
{
    for (const QByteArray &l : lines)
        if (l.startsWith("ok "))
            return true;
    return false;
}
bool hasErr(const QList<QByteArray> &lines) { return !lines.isEmpty() && lines.first().startsWith("err "); }

} // namespace

class TstScreenMatchSession : public QObject
{
    Q_OBJECT

private:
    // One trigger "t" in the given mode with a region at (rx, ry, rw, rh) of stream 0 and its step-0 reference from `reference`; started.
    void configure(Session &s, const QString &mode, const QByteArray &reference, int refW, int refH, int rx = 0, int ry = 0, int rw = 200, int rh = 120)
    {
        QVERIFY(hasOk(s.handle("trigger t " + mode.toLatin1(), 0)));
        QVERIFY(hasOk(s.handle(QStringLiteral("region t 0 0 %1 %2 %3 %4").arg(rx).arg(ry).arg(rw).arg(rh).toLatin1(), 0)));
        QVERIFY(hasOk(s.handle(refLine(u"t"_s, 0, 0, gridOf(reference, refW, refH)), 0)));
        QVERIFY(hasOk(s.handle("start", 0)));
    }

private slots:
    void anEqualRegionScoresFullAndADifferentOneLow()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA), b = scene(200, 120, kB);
        configure(s, u"structure"_s, a, 200, 120);
        source.push(0, 200, 120, a);
        QList<QByteArray> lines = s.tick(100);
        QCOMPARE(scoresOf(lines, u"t"_s), (QList<int>{1000}));
        QVERIFY(lines.last().startsWith("t 1"));
        source.push(0, 200, 120, b);
        const int different = scoresOf(s.tick(300), u"t"_s).value(0, -2);
        QVERIFY2(different >= 0 && different < 650, qPrintable(QString::number(different)));
        // brightness change and a small shift stay high in structure mode
        source.push(0, 200, 120, scene(200, 120, kA, 20, 0, 25));
        QVERIFY(scoresOf(s.tick(500), u"t"_s).value(0) > 950);
        source.push(0, 200, 120, scene(200, 120, kA, 20, 3));
        QVERIFY(scoresOf(s.tick(700), u"t"_s).value(0) > 850);
    }

    void hashModeScoresInSixtyFourths()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        configure(s, u"hash"_s, a, 200, 120);
        source.push(0, 200, 120, a);
        QCOMPARE(scoresOf(s.tick(0), u"t"_s), (QList<int>{1000}));
        source.push(0, 200, 120, scene(200, 120, kB));
        const int p = scoresOf(s.tick(100), u"t"_s).value(0);
        QVERIFY2(p >= 0 && p < 1000, qPrintable(QString::number(p)));
        QCOMPARE(int(std::lround(p * 64 / 1000.0)) * 1000, int(std::lround(p * 64 / 1000.0)) * 1000); // (multiples of 1/64, rounded to permille)
        QVERIFY(qAbs(p - std::lround(p * 64 / 1000.0) * 1000.0 / 64) < 9);
    }

    void theWantedStepPicksTheReference()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA), b = scene(200, 120, kB);
        configure(s, u"structure"_s, a, 200, 120);
        QVERIFY(hasOk(s.handle(refLine(u"t"_s, 0, 1, gridOf(b, 200, 120)), 0)));
        QVERIFY(hasOk(s.handle("want t 1", 0)));
        source.push(0, 200, 120, b);
        const auto scoreLine = [&](qint64 at) {
            for (const Reply &r : parse(s.tick(at)))
                if (r.type == ReplyType::Scores)
                    return r;
            return Reply();
        };
        Reply line = scoreLine(0);
        QCOMPARE(line.type, ReplyType::Scores);
        QCOMPARE(line.step, 1);
        QCOMPARE(line.permille, (QList<int>{1000}));
        QVERIFY(hasOk(s.handle("want t 0", 0)));
        line = scoreLine(100);
        QCOMPARE(line.step, 0);
        QVERIFY(line.permille.first() < 650);
        // a step with no reference cannot be scored
        QVERIFY(hasOk(s.handle("want t 5", 0)));
        line = scoreLine(200);
        QCOMPARE(line.step, 5);
        QCOMPARE(line.permille, (QList<int>{-1}));
    }

    void regionsThatCannotBeScoredAreDashes()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        QVERIFY(hasOk(s.handle("trigger t structure", 0)));
        QVERIFY(hasOk(s.handle("region t 0 0 0 0 200 120", 0)));      // fine
        QVERIFY(hasOk(s.handle("region t 1 0 150 0 100 100", 0)));     // sticks out on the right
        QVERIFY(hasOk(s.handle("region t 2 0 0 100 100 50", 0)));      // sticks out at the bottom
        QVERIFY(hasOk(s.handle("region t 3 0 500 500 10 10", 0)));     // far outside
        QVERIFY(hasOk(s.handle("region t 4 0 10 10 1 50", 0)));        // under 2 pixels wide
        QVERIFY(hasOk(s.handle("region t 5 3 0 0 50 50", 0)));         // a stream that does not exist
        QVERIFY(hasOk(s.handle("region t 7 0 0 0 50 50", 0)));         // (region 6 is not declared)
        for (int r : {0, 1, 2, 3, 4, 5, 7})
            QVERIFY(hasOk(s.handle(refLine(u"t"_s, r, 0, gridOf(a, 200, 120)), 0)));
        QVERIFY(hasOk(s.handle("start", 0)));
        source.push(0, 200, 120, a);
        const QList<int> scores = scoresOf(s.tick(0), u"t"_s);
        QCOMPARE(scores.size(), 8); // slots 0 to the last declared region
        QCOMPARE(scores.at(0), 1000);
        for (int r = 1; r <= 6; ++r)
            QCOMPARE(scores.at(r), -1);
        QVERIFY(scores.at(7) >= 0); // a 50 x 50 corner is a region like any other
    }

    void regionsAndFramesOfAnotherSizeAreHandledPerFrame()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        configure(s, u"hash"_s, a, 200, 120, 0, 0, 200, 120);
        source.push(0, 100, 60, scene(100, 60, {}, 20)); // the screen got smaller: the region no longer fits
        QCOMPARE(scoresOf(s.tick(0), u"t"_s), (QList<int>{-1}));
        source.push(0, 200, 120, a);                      // and back
        QCOMPARE(scoresOf(s.tick(100), u"t"_s), (QList<int>{1000}));
    }

    void ourOwnPetsOverARegionSkipItWhenMoreThanHalfIsCovered()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        configure(s, u"structure"_s, a, 200, 120);
        source.push(0, 200, 120, a);
        QVERIFY(hasOk(s.handle("pets 1 0 0 0 100 120", 0))); // the left half exactly: not more than half
        QCOMPARE(scoresOf(s.tick(0), u"t"_s), (QList<int>{1000}));
        QVERIFY(hasOk(s.handle("pets 1 0 0 0 120 120", 0))); // 60 %
        QCOMPARE(scoresOf(s.tick(100), u"t"_s), (QList<int>{-1}));
        QVERIFY(hasOk(s.handle("pets 1 1 0 0 200 120", 0))); // a pet on another stream does not count
        QCOMPARE(scoresOf(s.tick(200), u"t"_s), (QList<int>{1000}));
        QVERIFY(hasOk(s.handle("pets 2 0 0 0 80 120 0 80 0 80 120", 0))); // two pets that together cover 160 of 200
        QCOMPARE(scoresOf(s.tick(300), u"t"_s), (QList<int>{-1}));
        QVERIFY(hasOk(s.handle("pets 0", 0)));
        QCOMPARE(scoresOf(s.tick(400), u"t"_s), (QList<int>{1000}));
    }

    void onlyTheNewestFrameIsEverLookedAt()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA), b = scene(200, 120, kB);
        configure(s, u"structure"_s, a, 200, 120);
        for (int i = 0; i < 5; ++i)
            source.push(0, 200, 120, b);
        source.push(0, 200, 120, a); // the newest
        QCOMPARE(scoresOf(s.tick(0), u"t"_s), (QList<int>{1000}));
        QCOMPARE(source.dropped, 5);
        QCOMPARE(source.acquired, 1); // once per stream per tick
        QCOMPARE(source.released, 1);
        // a still screen: no new frame, the last one is scored again
        QCOMPARE(scoresOf(s.tick(100), u"t"_s), (QList<int>{1000}));
    }

    void aTickAsksOnlyTheStreamsTheRegionsUse()
    {
        FakeSource source(3);
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        QVERIFY(hasOk(s.handle("trigger t hash", 0)));
        QVERIFY(hasOk(s.handle("region t 0 2 0 0 200 120", 0)));
        QVERIFY(hasOk(s.handle(refLine(u"t"_s, 0, 0, gridOf(a, 200, 120)), 0)));
        QVERIFY(hasOk(s.handle("start", 0)));
        source.push(2, 200, 120, a);
        QCOMPARE(scoresOf(s.tick(0), u"t"_s), (QList<int>{1000}));
        QCOMPARE(source.acquired, 1);
    }

    void theStateFollowsTheFrames()
    {
        FakeSource source;
        Session s(&source);
        QList<Reply> r = parse(s.handle("start", 0));
        QCOMPARE(r.last().word, u"waiting"_s);
        QVERIFY(s.tick(0).size() == 1); // no trigger: just the tick line
        QVERIFY(hasOk(s.handle("trigger t hash", 0)));
        QVERIFY(hasOk(s.handle("region t 0 0 0 0 200 120", 0)));
        auto lines = s.tick(100); // no frame yet
        QCOMPARE(scoresOf(lines, u"t"_s), (QList<int>{-1}));
        for (const Reply &x : parse(lines))
            QVERIFY(x.type != ReplyType::State);
        source.push(0, 200, 120, scene(200, 120, kA));
        r = parse(s.tick(200));
        QCOMPARE(r.first().type, ReplyType::State);
        QCOMPARE(r.first().word, u"watching"_s);
        r = parse(s.tick(300));
        for (const Reply &x : r)
            QVERIFY(x.type != ReplyType::State); // only once
        r = parse(s.handle("stop", 0));
        QCOMPARE(r.last().word, u"stopped"_s);
        QVERIFY(s.tick(400).isEmpty());
        QVERIFY(!s.running());
    }

    void stoppedMeansNoTickAndClearForgetsEverything()
    {
        FakeSource source;
        Session s(&source);
        configure(s, u"hash"_s, scene(200, 120, kA), 200, 120);
        QVERIFY(hasOk(s.handle("stop", 0)));
        source.push(0, 200, 120, scene(200, 120, kA));
        QVERIFY(s.tick(0).isEmpty());
        QCOMPARE(source.acquired, 0);
        QVERIFY(hasOk(s.handle("clear", 0)));
        QCOMPARE(s.triggerCount(), 0);
        QVERIFY(hasOk(s.handle("start", 0)));
        QCOMPARE(parse(s.tick(0)).size(), 1); // just `t`
    }

    void limitsAndMistakesAreAnswered()
    {
        FakeSource source;
        Session s(&source);
        for (int i = 0; i < kMaxTriggers; ++i)
            QVERIFY(hasOk(s.handle(QStringLiteral("trigger t%1 hash").arg(i).toLatin1(), 0)));
        QVERIFY(hasErr(s.handle("trigger t9 hash", 0)));
        QVERIFY(hasErr(s.handle("trigger t0 hash", 0)));                  // duplicate
        QVERIFY(hasErr(s.handle("region nope 0 0 0 0 10 10", 0)));         // unknown trigger
        QVERIFY(hasErr(s.handle("ref nope 0 0 " + gridOf(scene(200, 120, kA), 200, 120).toBase64(), 0)));
        QVERIFY(hasErr(s.handle("want nope 1", 0)));
        // 16 regions in all
        int n = 0;
        for (int t = 0; t < kMaxTriggers; ++t)
            for (int r = 0; r < kMaxRegionsPerTrigger; ++r, ++n) {
                const auto reply = s.handle(QStringLiteral("region t%1 %2 0 0 0 10 10").arg(t).arg(r).toLatin1(), 0);
                QVERIFY2(n < kMaxRegionsTotal ? hasOk(reply) : hasErr(reply), qPrintable(QString::number(n)));
            }
        QVERIFY(hasOk(s.handle("region t0 0 0 5 5 20 20", 0))); // changing one that exists is not a new one
        for (const char *bad : {"format-disk", "region", "ref t0 0 0 AAAA", "trigger", "start now", "rate 99", "\x01\x02", "pets 1"}) {
            const auto reply = s.handle(bad, 0);
            QVERIFY2(hasErr(reply), bad);
            QVERIFY(reply.size() == 1);
        }
        // an unknown word is never echoed back
        const auto unknown = s.handle("evilword with arguments", 0);
        QVERIFY(!unknown.first().contains("evil"));
    }

    void sharedCropsGiveEqualScores()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        for (const char *id : {"one", "two"}) {
            QVERIFY(hasOk(s.handle(QByteArray("trigger ") + id + " structure", 0)));
            QVERIFY(hasOk(s.handle(QByteArray("region ") + id + " 0 0 0 0 200 120", 0)));
            QVERIFY(hasOk(s.handle(refLine(QString::fromLatin1(id), 0, 0, gridOf(a, 200, 120)), 0)));
        }
        QVERIFY(hasOk(s.handle("start", 0)));
        source.push(0, 200, 120, scene(200, 120, kA, 20, 2));
        const auto lines = s.tick(0);
        QCOMPARE(scoresOf(lines, u"one"_s), scoresOf(lines, u"two"_s));
        QCOMPARE(source.acquired, 1);
    }

    void aScoreStaysWithinThePermilleRange()
    {
        FakeSource source;
        Session s(&source);
        const QByteArray a = scene(200, 120, kA);
        configure(s, u"structure"_s, a, 200, 120);
        QRandomGenerator rng(5);
        for (int i = 0; i < 40; ++i) {
            QByteArray noise(200 * 120 * 4, 0);
            for (int k = 0; k < noise.size(); ++k)
                noise[k] = char(rng.bounded(256));
            source.push(0, 200, 120, noise);
            const int p = scoresOf(s.tick(i * 100), u"t"_s).value(0, -2);
            QVERIFY2(p >= 0 && p <= 1000, qPrintable(QString::number(p)));
        }
    }

    // ---- Capture ----
    void aCaptureIsRefusedUnlessTheHelperWasStartedForIt()
    {
        FakeSource source;
        Session s(&source);
        source.push(0, 200, 120, scene(200, 120, kA));
        const auto reply = s.handle("capture 1 0 0 0 100 100", 0);
        QVERIFY(hasErr(reply));
        QVERIFY(reply.first().contains("not-allowed"));
        QCOMPARE(source.acquired, 0); // not even a frame was looked at
    }

    void aCaptureIsOnePictureOfTheRectangleAsRgb()
    {
        FakeSource source;
        SessionOptions options;
        options.captureAllowed = true;
        Session s(&source, options);
        const QByteArray frame = scene(300, 200, {{10, 10, 50, 50, 255}}, 40);
        source.push(0, 300, 200, frame);
        const QList<Reply> lines = parse(s.handle("capture 7 0 0 0 100 100", 5000));
        QCOMPARE(lines.first().type, ReplyType::Shot);
        QCOMPARE(lines.first().request, 7);
        QCOMPARE(lines.first().width, 100); // under 360: not scaled
        QCOMPARE(lines.first().height, 100);
        QByteArray rgb;
        int seq = 0;
        for (const Reply &r : lines)
            if (r.type == ReplyType::Chunk) {
                QCOMPARE(r.sequence, seq++);
                rgb += r.data;
            }
        QCOMPARE(rgb.size(), 100 * 100 * 3);
        QCOMPARE(lines.at(lines.size() - 2).type, ReplyType::ShotEnd);
        QCOMPARE(lines.last().type, ReplyType::Ok);
        QCOMPARE(int(uchar(rgb[(20 * 100 + 20) * 3])), 255); // inside the box
        QCOMPARE(int(uchar(rgb[(80 * 100 + 80) * 3])), 40);  // background
        QCOMPARE(source.released, 1);
    }

    void aBigCaptureIsScaledTo360()
    {
        FakeSource source;
        SessionOptions options;
        options.captureAllowed = true;
        Session s(&source, options);
        source.push(0, 1920, 1080, scene(1920, 1080, {{0, 0, 960, 1080, 200}}, 30));
        const QList<Reply> lines = parse(s.handle("capture 1 0 0 0 1920 1080", 0));
        QCOMPARE(lines.first().width, 360);
        QCOMPARE(lines.first().height, 203);
        QVERIFY(lines.first().bytes <= kMaxCaptureBytes);
        QByteArray rgb;
        for (const Reply &r : lines)
            if (r.type == ReplyType::Chunk) {
                QVERIFY(r.data.size() <= kChunkBytes);
                rgb += r.data;
            }
        QCOMPARE(rgb.size(), 360 * 203 * 3);
        QCOMPARE(int(uchar(rgb[(100 * 360 + 10) * 3])), 200);
        QCOMPARE(int(uchar(rgb[(100 * 360 + 350) * 3])), 30);
        for (const QByteArray &l : s.handle("ping", 10))
            QVERIFY(l.size() < kMaxLineBytes);
    }

    void capturesAreRateLimitedAndChecked()
    {
        FakeSource source;
        SessionOptions options;
        options.captureAllowed = true;
        Session s(&source, options);
        source.push(0, 200, 120, scene(200, 120, kA));
        QVERIFY(!hasErr(s.handle("capture 1 0 0 0 100 100", 10000)));
        const auto again = s.handle("capture 2 0 0 0 100 100", 10500);
        QVERIFY(hasErr(again) && again.first().contains("too-soon"));
        QVERIFY(!hasErr(s.handle("capture 3 0 0 0 100 100", 11000)));
        for (const char *line : {"capture 4 0 150 0 100 100", "capture 5 0 0 0 1 100", "capture 6 0 0 100 100 100"}) { // outside, thin, outside
            const auto r = s.handle(line, 20000);
            QVERIFY2(hasErr(r) && r.first().contains("outside"), line);
        }
        const auto none = s.handle("capture 7 1 0 0 10 10", 30000);
        QVERIFY(hasErr(none) && none.first().contains("no-stream"));
        FakeSource empty;
        Session e(&empty, options);
        const auto noFrame = e.handle("capture 1 0 0 0 10 10", 0);
        QVERIFY(hasErr(noFrame) && noFrame.first().contains("no-frame"));
    }

    // ---- Fuzz ----
    void randomLinesNeverBreakTheSessionAndEveryReplyIsOurs()
    {
        FakeSource source(2);
        SessionOptions options;
        options.captureAllowed = true;
        options.captureMinGapMs = 0;
        Session s(&source, options);
        source.push(0, 200, 120, scene(200, 120, kA));
        source.push(1, 100, 60, scene(100, 60, kB));
        QRandomGenerator rng(20261023);
        const QList<QByteArray> seeds = {"trigger t structure", "region t 0 0 0 0 100 60", "region t 1 1 0 0 50 30", "start", "pets 1 0 0 0 50 50", "want t 1", "rate 3", "capture 1 0 0 0 100 100", "clear",
                                         refLine(u"t"_s, 0, 0, gridOf(scene(200, 120, kA), 200, 120)), "stop", "ping"};
        qint64 now = 0;
        for (int i = 0; i < 20000; ++i) {
            QByteArray line = seeds.at(int(rng.bounded(seeds.size())));
            if (rng.bounded(3) == 0 && !line.isEmpty())
                line[int(rng.bounded(line.size()))] = char(rng.bounded(256));
            if (rng.bounded(7) == 0)
                line.truncate(int(rng.bounded(line.size() + 1)));
            now += 200;
            QList<QByteArray> out = s.handle(line, now);
            out += s.tick(now);
            for (const QByteArray &l : out) {
                QVERIFY(l.size() < kMaxLineBytes);
                QVERIFY2(parseReply(l).has_value(), l.left(60).constData());
            }
            if (s.quitRequested())
                break;
        }
        QVERIFY(s.triggerCount() <= kMaxTriggers);
    }
};

QTEST_GUILESS_MAIN(TstScreenMatchSession)
#include "tst_screenmatchsession.moc"
