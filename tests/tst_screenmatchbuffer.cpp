// The helper's judgement of a buffer a PipeWire server hands over (src/screenmatch/bufferguard.*): every refusal, and the exact edges of the accepted ones.

#include "screenmatch/bufferguard.h"

#include <QTest>

using namespace murasaki::screenmatch;
using murasaki::ScreenPixelFormat;

Q_DECLARE_METATYPE(murasaki::screenmatch::BufferVerdict)

namespace {

uchar g_memory[16];

// A good 4x3 BGRx buffer: 16 bytes a row, 48 bytes, at offset 0 of a 64 byte mapping.
BufferInput good()
{
    BufferInput in;
    in.data = g_memory;
    in.hasChunk = true;
    in.offset = 0;
    in.size = 48;
    in.stride = 16;
    in.maxSize = 64;
    in.formatKnown = true;
    in.format = ScreenPixelFormat::Bgra32;
    in.width = 4;
    in.height = 3;
    return in;
}

BufferVerdict check(const BufferInput &in, Frame *frame = nullptr)
{
    Frame scratch;
    return checkBuffer(in, frame ? frame : &scratch);
}

} // namespace

class TstScreenMatchBuffer : public QObject
{
    Q_OBJECT

private slots:
    void aGoodBufferFillsTheFrame()
    {
        BufferInput in = good();
        in.offset = 8;
        in.size = 56;
        Frame frame;
        QCOMPARE(check(in, &frame), BufferVerdict::Ok); // 8 + 56 = 64: exactly the mapping
        QCOMPARE(frame.width, 4);
        QCOMPARE(frame.height, 3);
        QCOMPARE(frame.stride, qsizetype(16));
        QCOMPARE(frame.data, static_cast<const uchar *>(g_memory) + 8);
        QCOMPARE(frame.size, qsizetype(56));
        in = good();
        in.format = ScreenPixelFormat::Rgb24; // 3 bytes a pixel: a 12 byte row
        in.stride = 12;
        in.size = 36;
        QCOMPARE(check(in), BufferVerdict::Ok);
    }
    void aRefusalLeavesTheFrameAlone()
    {
        BufferInput in = good();
        in.corrupted = true;
        Frame frame;
        frame.width = 77;
        QCOMPARE(check(in, &frame), BufferVerdict::Corrupted);
        QCOMPARE(frame.width, 77);
        QCOMPARE(frame.data, static_cast<const uchar *>(nullptr));
    }
    void noMappingNoChunkAndACorruptedChunkAreRefused()
    {
        BufferInput in = good();
        in.data = nullptr;
        QCOMPARE(check(in), BufferVerdict::NoMapping);
        in = good();
        in.mapFailed = true; // MAP_FAILED is not null
        QCOMPARE(check(in), BufferVerdict::NoMapping);
        in = good();
        in.hasChunk = false;
        QCOMPARE(check(in), BufferVerdict::NoChunk);
        in = good();
        in.corrupted = true;
        QCOMPARE(check(in), BufferVerdict::Corrupted);
    }
    void aDmaBufIsReadOnlyWhenAskedFor()
    {
        BufferInput in = good();
        in.dmaBuf = true;
        QCOMPARE(check(in), BufferVerdict::DmaBufRefused); // the server was told shared memory only
        in.allowDmaBuf = true;
        QCOMPARE(check(in), BufferVerdict::Ok);            // --buffers any (experimental)
        in.data = nullptr;
        QCOMPARE(check(in), BufferVerdict::NoMapping);     // and the rest of the checks still apply
    }
    void theChunkMustLieInsideTheMapping_data()
    {
        QTest::addColumn<qint64>("offset");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<bool>("ok");
        QTest::newRow("exactly the end") << qint64(16) << qint64(48) << true;
        QTest::newRow("one past the end") << qint64(17) << qint64(48) << false;
        QTest::newRow("offset beyond the mapping") << qint64(65) << qint64(1) << false;
        QTest::newRow("offset at the end, nothing left") << qint64(64) << qint64(48) << false;
        QTest::newRow("negative offset") << qint64(-1) << qint64(48) << false;
        QTest::newRow("size 0") << qint64(0) << qint64(0) << false;
        QTest::newRow("negative size") << qint64(0) << qint64(-48) << false;
        QTest::newRow("size beyond the mapping") << qint64(0) << qint64(65) << false;
        QTest::newRow("offset + size wraps 32 bits") << qint64(0xfffffff0) << qint64(0x20) << false;
        QTest::newRow("offset + size wraps 64 bits") << qint64(0x7ffffffffffffff0) << qint64(0x7fffffffffffff00) << false;
    }
    void theChunkMustLieInsideTheMapping()
    {
        QFETCH(qint64, offset);
        QFETCH(qint64, size);
        QFETCH(bool, ok);
        BufferInput in = good();
        in.offset = offset;
        in.size = size;
        QCOMPARE(check(in) == BufferVerdict::Ok, ok);
    }
    void theStrideMustCoverARowAndTheChunkTheLastOne_data()
    {
        QTest::addColumn<qint64>("stride");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<BufferVerdict>("verdict");
        QTest::newRow("tight") << qint64(16) << qint64(48) << BufferVerdict::Ok;
        QTest::newRow("padded rows, the last one short") << qint64(20) << qint64(56) << BufferVerdict::Ok; // 2 * 20 + 16
        QTest::newRow("zero") << qint64(0) << qint64(48) << BufferVerdict::BadStride;
        QTest::newRow("negative (upside down)") << qint64(-16) << qint64(48) << BufferVerdict::BadStride;
        QTest::newRow("less than a row") << qint64(15) << qint64(48) << BufferVerdict::BadStride;
        QTest::newRow("absurd") << qint64(0x7fffffffffff) << qint64(48) << BufferVerdict::BadStride;
        QTest::newRow("the last row ends outside the chunk") << qint64(20) << qint64(55) << BufferVerdict::TooSmall;
        QTest::newRow("a stride that only the chunk size forbids") << qint64(1 << 30) << qint64(48) << BufferVerdict::TooSmall;
    }
    void theStrideMustCoverARowAndTheChunkTheLastOne()
    {
        QFETCH(qint64, stride);
        QFETCH(qint64, size);
        QFETCH(BufferVerdict, verdict);
        BufferInput in = good();
        in.stride = stride;
        in.size = size;
        in.maxSize = 1 << 20;
        QCOMPARE(check(in), verdict);
    }
    void aFormatChangeBetweenTwoBuffersIsJudgedByTheNewFormat()
    {
        // the same chunk the server sent for 4x3 is wrong for a stream that is now 8x3 or in an unknown format
        BufferInput in = good();
        in.width = 8; // a row is now 32 bytes: the stride of 16 no longer covers it
        QCOMPARE(check(in), BufferVerdict::BadStride);
        in = good();
        in.formatKnown = false;
        QCOMPARE(check(in), BufferVerdict::UnknownFormat);
        in = good();
        in.format = ScreenPixelFormat::Rgb24; // 3 bytes a pixel: 12 bytes a row fit the stride, but 3 rows of 16 are not what the chunk holds if it is smaller
        in.size = 31;
        QCOMPARE(check(in), BufferVerdict::TooSmall);
        in = good();
        in.height = 4; // taller: the chunk now ends before the last row
        QCOMPARE(check(in), BufferVerdict::TooSmall);
    }
    void theNegotiatedSizeIsBounded()
    {
        for (const qint64 width : {qint64(0), qint64(-4), qint64(16385), qint64(1) << 40}) {
            BufferInput in = good();
            in.width = width;
            QCOMPARE(check(in), BufferVerdict::BadSize);
        }
        BufferInput in = good();
        in.height = 0;
        QCOMPARE(check(in), BufferVerdict::BadSize);
    }
};

QTEST_GUILESS_MAIN(TstScreenMatchBuffer)
#include "tst_screenmatchbuffer.moc"
