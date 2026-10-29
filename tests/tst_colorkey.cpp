// The colour key (colorkey.h): a pure function of the pixel and the key.

#include "colorkey.h"
#include "keyedsink.h"

#include <QRandomGenerator>
#include <QTest>

using namespace murasaki::import;

namespace {

QByteArray px(int r, int g, int b, int a = 255)
{
    return QByteArray(1, char(r)) + QByteArray(1, char(g)) + QByteArray(1, char(b)) + QByteArray(1, char(a));
}

QByteArray keyed(QByteArray pixel, const ColorKey &key)
{
    applyColorKey(reinterpret_cast<uchar *>(pixel.data()), 1, 1, 4, key);
    return pixel;
}

ColorKey green(double tol, double soft)
{
    ColorKey k;
    k.enabled = true;
    k.tolerance = tol;
    k.softness = soft;
    return k;
}

// Records what a sink is given.
class Capture final : public murasaki::import::FrameSink
{
public:
    bool begin(int w, int h) override
    {
        width = w;
        height = h;
        return true;
    }
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override
    {
        QByteArray f;
        for (int y = 0; y < height; ++y)
            f.append(reinterpret_cast<const char *>(rgba + stride * y), width * 4);
        got.append(f);
        delays.append(delayMs);
        return true;
    }
    bool finish() override { return true; }
    int frames() const override { return int(got.size()); }
    long long bytesWritten() const override { return 0; }
    QString error() const override { return {}; }
    bool overBudget() const override { return false; }
    int width = 0, height = 0;
    QList<QByteArray> got;
    QList<int> delays;
};

} // namespace

class TstColorKey : public QObject
{
    Q_OBJECT

private slots:
    void disabledChangesNothing()
    {
        ColorKey k = green(1, 0);
        k.enabled = false;
        QCOMPARE(keyed(px(0, 255, 0), k), px(0, 255, 0));
    }
    void theExactKeyIsTransparentAtZeroTolerance()
    {
        QCOMPARE(uchar(keyed(px(0, 255, 0), green(0, 0)).at(3)), uchar(0));
        QCOMPARE(keyed(px(0, 254, 0), green(0, 0)), px(0, 254, 0));
    }
    void farColoursAreUntouched()
    {
        QCOMPARE(keyed(px(255, 0, 0, 200), green(0.3, 0.1)), px(255, 0, 0, 200));
        QCOMPARE(keyed(px(255, 0, 255), green(0.2, 0.0)), px(255, 0, 255));
    }
    void fullToleranceRemovesEverything()
    {
        QCOMPARE(uchar(keyed(px(255, 0, 255), green(1, 0)).at(3)), uchar(0));
    }
    void softnessRampIsMonotonic()
    {
        int last = -1;
        for (int r = 0; r <= 255; r += 5) {
            const int a = uchar(keyed(px(r, 255, 0), green(0.05, 0.4)).at(3));
            QVERIFY2(a >= last, "alpha must not fall as the colour moves away from the key");
            last = a;
        }
        QCOMPARE(last, 255);
    }
    void existingAlphaIsNeverRaised()
    {
        for (int r = 0; r <= 255; r += 15)
            QVERIFY(uchar(keyed(px(r, 200, 10, 90), green(0.1, 0.3)).at(3)) <= 90);
    }
    void rowsAreSeparatedByTheStrideNotTheWidth()
    {
        QByteArray two = px(0, 255, 0) + px(1, 2, 3) + QByteArray(8, 'x') /*padding*/ + px(0, 255, 0) + px(1, 2, 3);
        applyColorKey(reinterpret_cast<uchar *>(two.data()), 2, 2, 16, green(0, 0));
        QCOMPARE(uchar(two.at(3)), uchar(0));
        QCOMPARE(uchar(two.at(11)), uchar('x'));
        QCOMPARE(uchar(two.at(16 + 3)), uchar(0));
        QCOMPARE(uchar(two.at(16 + 7)), uchar(255));
    }
    void theKeyedSinkKeysEveryFrameAndLeavesTheSourceAlone()
    {
        Capture capture;
        KeyedSink sink(capture, green(0, 0));
        QVERIFY(sink.begin(2, 2));
        // 2x2, with a stride wider than the row: a green pixel and a red one in each row.
        const QByteArray rows = px(0, 255, 0) + px(255, 0, 0) + QByteArray(4, 'x') + px(255, 0, 0) + px(0, 255, 0) + QByteArray(4, 'x');
        QByteArray frame = rows;
        for (int delay : {40, 70})
            QVERIFY(sink.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 12, delay));
        QVERIFY(sink.finish());
        QCOMPARE(frame, rows); // the caller's buffer is untouched
        QCOMPARE(capture.delays, (QList<int>{40, 70}));
        for (const QByteArray &out : capture.got) {
            QCOMPARE(out.size(), 16);
            QCOMPARE(uchar(out.at(3)), uchar(0));  // green
            QCOMPARE(uchar(out.at(7)), uchar(255)); // red
            QCOMPARE(uchar(out.at(11)), uchar(255));
            QCOMPARE(uchar(out.at(15)), uchar(0));
        }
    }
    void aDisabledKeyedSinkPassesTheFrameAsItIs()
    {
        Capture capture;
        ColorKey off;
        KeyedSink sink(capture, off);
        sink.begin(1, 1);
        const QByteArray p = px(0, 255, 0);
        sink.addFrame(reinterpret_cast<const uchar *>(p.constData()), 4, 10);
        QCOMPARE(capture.got.value(0), p);
    }
    void outOfRangeSettingsAreClamped()
    {
        QByteArray p = px(10, 20, 30);
        applyColorKey(reinterpret_cast<uchar *>(p.data()), 0, 5, 4, green(5, -3)); // nothing to do, no crash
        QCOMPARE(uchar(keyed(px(0, 255, 0), green(-4, -1)).at(3)), uchar(0));
    }
};

QTEST_GUILESS_MAIN(TstColorKey)
#include "tst_colorkey.moc"
