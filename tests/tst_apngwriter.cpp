// ApngWriter against an independent decoder (importfixtures.h) and against the header reader the program uses (readApngInfo).

#include "animationinfo.h"
#include "apngwriter.h"
#include "importfixtures.h"
#include "petimport.h"

#include <QBuffer>
#include <QRandomGenerator>
#include <QTemporaryFile>
#include <QTest>

using namespace murasaki;
using namespace murasaki::import;
using namespace importfixtures;

namespace {

QByteArray noise(int width, int height, quint32 seed)
{
    QByteArray out(width * height * 4, 0);
    QRandomGenerator rng(seed);
    for (char &c : out)
        c = char(rng.bounded(256));
    return out;
}

// Smooth content, so that the filters other than none win on some rows.
QByteArray gradient(int width, int height, int shift)
{
    QByteArray out(width * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uchar *p = reinterpret_cast<uchar *>(out.data()) + (y * width + x) * 4;
            p[0] = uchar(x * 3 + shift);
            p[1] = uchar(y * 5 + shift);
            p[2] = uchar((x + y) * 2);
            p[3] = uchar(x % 7 == 0 ? 128 : 255);
        }
    }
    return out;
}

} // namespace

class TstApngWriter : public QObject
{
    Q_OBJECT

private slots:
    void framesRoundTripBitForBit()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY2(writer.begin(37, 23), qPrintable(writer.error()));
        const QList<QByteArray> frames{gradient(37, 23, 0), noise(37, 23, 1), gradient(37, 23, 90), QByteArray(37 * 23 * 4, '\0'), noise(37, 23, 2)};
        const QList<int> delays{100, 33, 0, 20, 65535};
        for (int i = 0; i < frames.size(); ++i)
            QVERIFY2(writer.addFrame(reinterpret_cast<const uchar *>(frames.at(i).constData()), 37 * 4, delays.at(i)), qPrintable(writer.error()));
        QVERIFY2(writer.finish(), qPrintable(writer.error()));
        QCOMPARE(writer.frames(), 5);
        const DecodedApng decoded = decodeApng(buffer.data());
        QVERIFY2(decoded.ok, qPrintable(decoded.problem));
        QCOMPARE(decoded.declaredFrames, 5);
        QCOMPARE(decoded.width, 37);
        QCOMPARE(decoded.height, 23);
        QCOMPARE(decoded.frames, frames);
        for (int i = 0; i < delays.size(); ++i)
            QCOMPARE(decoded.delaysMs.at(i), double(delays.at(i)));
    }
    void aStrideWiderThanTheRowIsHonoured()
    {
        QByteArray padded(20 * 4 * 3, '\x7f'); // 3 rows of 20 pixels, the picture is the left 5
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 5; ++x)
                for (int c = 0; c < 4; ++c)
                    padded[y * 80 + x * 4 + c] = char(y * 50 + x * 10 + c);
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(5, 3));
        QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(padded.constData()), 80, 40));
        QVERIFY(writer.finish());
        const DecodedApng decoded = decodeApng(buffer.data());
        QVERIFY2(decoded.ok, qPrintable(decoded.problem));
        for (int y = 0; y < 3; ++y)
            QCOMPARE(decoded.frames.at(0).mid(y * 20, 20), padded.mid(y * 80, 20));
    }
    void longDelaysKeepTheirValueToTheCentisecond()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(1, 1));
        const QByteArray px(4, 'a');
        for (const int ms : {65536, 120000, 65535000, 99999999})
            QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(px.constData()), 4, ms));
        QVERIFY(writer.finish());
        const DecodedApng decoded = decodeApng(buffer.data());
        QVERIFY2(decoded.ok, qPrintable(decoded.problem));
        QCOMPARE(decoded.delaysMs.at(0), 65540.0);   // 6554 cs
        QCOMPARE(decoded.delaysMs.at(1), 120000.0);
        QCOMPARE(decoded.delaysMs.at(2), 655350.0);  // the most a centisecond fraction holds
        QCOMPARE(decoded.delaysMs.at(3), 655350.0);  // clamped
    }
    void theProgramsOwnHeaderReaderAcceptsTheFile()
    {
        QTemporaryFile file;
        QVERIFY(file.open());
        ApngWriter writer(&file);
        QVERIFY(writer.begin(64, 48));
        const QByteArray frame = gradient(64, 48, 3);
        for (int i = 0; i < 12; ++i)
            QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 64 * 4, 100));
        QVERIFY(writer.finish());
        file.flush();
        file.seek(0);
        const auto info = readApngInfo(&file);
        QVERIFY(info);
        QCOMPARE(info->frames, 12);
        QCOMPARE(info->width, 64);
        QCOMPARE(info->height, 48);
        QCOMPARE(info->fps, 10.0);
    }
    void manyTinyFramesStayWithinTheChunkBudget()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(1, 1));
        const QByteArray px("\x01\x02\x03\xff", 4);
        for (int i = 0; i < 5000; ++i)
            QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(px.constData()), 4, 10));
        QVERIFY(writer.finish());
        buffer.seek(0);
        const auto info = readApngInfo(&buffer);
        QVERIFY(info);
        QCOMPARE(info->frames, 5000);
    }
    void aBigFrameIsSplitIntoChunksAndStillDecodes()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(700, 600));
        const QByteArray frame = noise(700, 600, 9); // incompressible: more than one 256 KiB chunk
        QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 700 * 4, 50));
        QVERIFY(writer.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 700 * 4, 50));
        QVERIFY(writer.finish());
        QVERIFY(buffer.data().count("IDAT") >= 4);
        QVERIFY(buffer.data().count("fdAT") >= 4);
        const DecodedApng decoded = decodeApng(buffer.data());
        QVERIFY2(decoded.ok, qPrintable(decoded.problem));
        QCOMPARE(decoded.frames.at(1), frame);
    }
    void limitsAreCheckedBeforeAnythingIsAllocated()
    {
        for (const auto &size : {std::pair{0, 10}, std::pair{10, 0}, std::pair{-1, 5}, std::pair{kMaxCanvasSide + 1, 10}, std::pair{10, 100000}}) {
            QBuffer buffer;
            QVERIFY(buffer.open(QIODevice::ReadWrite));
            ApngWriter writer(&buffer);
            QVERIFY(!writer.begin(size.first, size.second));
            QVERIFY(!writer.error().isEmpty());
            QVERIFY(buffer.data().isEmpty() || buffer.data().size() < 8);
        }
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(4, 4));
        const QByteArray row(16, 'x');
        QVERIFY(!writer.addFrame(nullptr, 16, 10));
        ApngWriter second(&buffer);
        QVERIFY(second.begin(4, 4));
        QVERIFY(!second.addFrame(reinterpret_cast<const uchar *>(row.constData()), 15, 10)); // a stride shorter than a row
        QVERIFY(!second.addFrame(reinterpret_cast<const uchar *>(row.constData()), 16, 10)); // the writer is dead after an error
    }
    void anOutputOverTheBudgetFailsAndSaysSo()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer, 3000);
        QVERIFY(writer.begin(100, 100));
        const QByteArray frame = noise(100, 100, 4);
        bool failed = false;
        for (int i = 0; i < 10 && !failed; ++i)
            failed = !writer.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 400, 10);
        QVERIFY(failed);
        QVERIFY(writer.error().contains(QStringLiteral("budget")));
        QVERIFY(buffer.data().size() <= 3000);
        QVERIFY(!writer.finish());
    }
    void noFramesIsNotAnAnimation()
    {
        QBuffer buffer;
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        ApngWriter writer(&buffer);
        QVERIFY(writer.begin(4, 4));
        QVERIFY(!writer.finish());
    }
    void aSequentialOrReadOnlyDeviceIsRefused()
    {
        QBuffer readOnly;
        readOnly.setData(QByteArray(10, 'a'));
        QVERIFY(readOnly.open(QIODevice::ReadOnly));
        ApngWriter writer(&readOnly);
        QVERIFY(!writer.begin(4, 4));
    }
    void aFailingDeviceIsReported()
    {
        class Failing : public QBuffer
        {
        public:
            qint64 writeData(const char *data, qint64 len) override { return ++calls > 6 ? -1 : QBuffer::writeData(data, len); }
            int calls = 0;
        } device;
        QVERIFY(device.open(QIODevice::ReadWrite));
        ApngWriter writer(&device);
        const QByteArray frame = noise(8, 8, 3);
        bool failed = !writer.begin(8, 8);
        for (int i = 0; i < 5 && !failed; ++i)
            failed = !writer.addFrame(reinterpret_cast<const uchar *>(frame.constData()), 32, 10);
        QVERIFY(failed || !writer.finish());
        QVERIFY(writer.error().contains(QStringLiteral("cannot write")));
    }
};

QTEST_GUILESS_MAIN(TstApngWriter)
#include "tst_apngwriter.moc"
