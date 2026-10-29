// The strict .npy / .npz reader (npyreader.h): every header field valid and invalid, every refusal by its name, limits checked before any data is read, truncated and trailing data,
// frames chosen from a stream in any chunking, an .npz in a ZIP (stored and deflate), and a seeded fuzz run over mutated files that must never crash.

#include "importfixtures.h"
#include "npyfixtures.h"
#include "npyreader.h"
#include "petimport.h"

#include <QBuffer>
#include <QRandomGenerator>
#include <QTest>

using namespace murasaki::import;
using namespace importfixtures;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

// A header text padded with spaces to a multiple of 64 bytes in all (numpy's habit) and ended by a newline.
QByteArray npyHeader(const QByteArray &dict, int major = 1, int minor = 0, bool pad = true)
{
    QByteArray text = dict;
    const int lengthBytes = major == 1 ? 2 : 4;
    if (pad)
        while ((10 + (lengthBytes - 2) + text.size() + 1) % 64 != 0)
            text.append(' ');
    text.append('\n');
    QByteArray out("\x93NUMPY", 6);
    out.append(char(major));
    out.append(char(minor));
    out += lengthBytes == 2 ? le16(quint16(text.size())) : le32(quint32(text.size()));
    return out + text;
}

QByteArray dictFor(const QByteArray &descr, const QByteArray &fortran, const QByteArray &shape)
{
    return "{'descr': '" + descr + "', 'fortran_order': " + fortran + ", 'shape': " + shape + ", }";
}

// Frame f, pixel i, channel c holds (f * 37 + i * 5 + c) & 255.
QByteArray pixels(int frames, int h, int w)
{
    QByteArray out(frames * h * w * 3, 0);
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < h * w * 3; ++i)
            out[f * h * w * 3 + i] = char(f * 37 + i * 5);
    return out;
}

QByteArray npy(int frames, int h, int w, int major = 1)
{
    return npyHeader(dictFor("|u1", "False", "(" + QByteArray::number(frames) + ", " + QByteArray::number(h) + ", " + QByteArray::number(w) + ", 3)"), major) + pixels(frames, h, w);
}

struct Read {
    bool ok = false;
    QString error;
    QList<int> indices;
    QList<QByteArray> frames;
    NpyShape shape;
};

Read readAll(const QByteArray &bytes, const QList<int> &wanted, int chunk = 0, const NpyLimits &limits = {}, long long known = -2)
{
    Read r;
    NpyFrameReader reader(
        limits, [&](const NpyShape &s) { r.shape = s; return wanted; },
        [&](int index, const uchar *rgb, int w, int h) {
            r.indices.append(index);
            r.frames.append(QByteArray(reinterpret_cast<const char *>(rgb), w * h * 3));
            return true;
        },
        known == -2 ? bytes.size() : known);
    bool ok = true;
    const int step = chunk > 0 ? chunk : int(bytes.size());
    for (qsizetype at = 0; ok && at < bytes.size(); at += step)
        ok = reader.feed(bytes.constData() + at, qsizetype(std::min<qsizetype>(step, bytes.size() - at)));
    ok = ok && reader.finish();
    r.ok = ok;
    r.error = reader.error();
    return r;
}

QByteArray npz(const QByteArray &npyBytes, bool deflate, const QByteArray &name = "frames.npy")
{
    return buildZip({npyfixtures::zipMember(name, npyBytes, deflate, false, false, 0)});
}

bool refusedAs(const Read &r, std::string_view name)
{
    return !r.ok && r.error.startsWith(nameOf(name));
}

} // namespace

class TstNpyReader : public QObject
{
    Q_OBJECT

private slots:
    void aWellFormedArrayDeliversTheChosenFramesOnce()
    {
        const QByteArray file = npy(6, 3, 4);
        const Read r = readAll(file, {4, 1, 4, 1});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.shape.frames, 6);
        QCOMPARE(r.shape.height, 3);
        QCOMPARE(r.shape.width, 4);
        QCOMPARE(r.indices, (QList<int>{1, 4})); // in index order, once each
        const QByteArray all = pixels(6, 3, 4);
        QCOMPARE(r.frames.at(0), all.mid(1 * 36, 36));
        QCOMPARE(r.frames.at(1), all.mid(4 * 36, 36));
    }

    void anyChunkingGivesTheSameFrames()
    {
        const QByteArray file = npy(5, 2, 7);
        const Read whole = readAll(file, {0, 2, 4});
        QVERIFY(whole.ok);
        for (const int chunk : {1, 2, 3, 7, 64, 100, 1000}) {
            const Read r = readAll(file, {0, 2, 4}, chunk);
            QVERIFY2(r.ok, qPrintable(QString("chunk %1: %2").arg(chunk).arg(r.error)));
            QCOMPARE(r.frames, whole.frames);
        }
    }

    void noFramesChosenStillChecksTheWholeFile()
    {
        QVERIFY(readAll(npy(3, 2, 2), {}).ok);
        QByteArray cut = npy(3, 2, 2);
        cut.chop(1);
        QVERIFY(refusedAs(readAll(cut, {}, 0, {}, -1), kNpyTruncated));
    }

    void versionsOneTwoAndThreeAreAccepted()
    {
        for (const int major : {1, 2, 3}) {
            const Read r = readAll(npy(2, 2, 2, major), {1});
            QVERIFY2(r.ok, qPrintable(QString("v%1: %2").arg(major).arg(r.error)));
            QCOMPARE(r.frames.size(), 1);
        }
    }

    void theGrammarAcceptsWhatNumpyWrites_data()
    {
        QTest::addColumn<QByteArray>("dict");
        const QByteArray shape = "(2, 2, 2, 3)";
        QTest::newRow("numpy's own") << dictFor("|u1", "False", shape);
        QTest::newRow("little endian marker") << dictFor("<u1", "False", shape);
        QTest::newRow("double quotes") << QByteArray("{\"descr\": \"|u1\", \"fortran_order\": False, \"shape\": (2, 2, 2, 3), }");
        QTest::newRow("other order") << QByteArray("{'shape': (2, 2, 2, 3), 'descr': '|u1', 'fortran_order': False}");
        QTest::newRow("no trailing comma") << QByteArray("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)}");
        QTest::newRow("tight") << QByteArray("{'descr':'|u1','fortran_order':False,'shape':(2,2,2,3)}");
        QTest::newRow("trailing comma in the shape") << QByteArray("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3,), }");
        QTest::newRow("spaces") << QByteArray("{ 'descr' : '|u1' , 'fortran_order' : False , 'shape' : ( 2 , 2 , 2 , 3 ) , }");
    }
    void theGrammarAcceptsWhatNumpyWrites()
    {
        QFETCH(QByteArray, dict);
        const Read r = readAll(npyHeader(dict) + pixels(2, 2, 2), {0, 1});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.frames.size(), 2);
    }

    void headerRefusalsHaveTheirOwnNames_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<QString>("reason");
        const auto row = [&](const char *name, const QByteArray &file, std::string_view reason) { QTest::newRow(name) << file << nameOf(reason); };
        const QByteArray body = pixels(2, 2, 2);
        const QByteArray shape = "(2, 2, 2, 3)";

        row("empty", QByteArray(), kNpyNotNpy);
        row("wrong magic", QByteArray("\x93NUMPX\x01\x00", 8) + QByteArray(100, 'x'), kNpyNotNpy);
        row("a pickle", QByteArray("\x80\x04\x95\x10", 4) + QByteArray(100, 'x'), kNpyNotNpy);
        row("a zip", QByteArray("PK\x03\x04", 4) + QByteArray(100, 'x'), kNpyNotNpy);
        row("version 4", npyHeader(dictFor("|u1", "False", shape), 4) + body, kNpyVersion);
        row("version 0", npyHeader(dictFor("|u1", "False", shape), 0) + body, kNpyVersion);
        row("version 1.1", npyHeader(dictFor("|u1", "False", shape), 1, 1) + body, kNpyVersion);
        row("version 2.1", npyHeader(dictFor("|u1", "False", shape), 2, 1) + body, kNpyVersion);

        QByteArray tooLarge("\x93NUMPY\x02\x00", 8);
        tooLarge += le32(70000) + QByteArray(70000, ' ');
        row("header over 64 KiB", tooLarge, kNpyHeaderTooLarge);
        QByteArray absurd("\x93NUMPY\x03\x00", 8);
        absurd += le32(0xFFFFFFFFu);
        row("header length 4 GiB", absurd, kNpyHeaderTooLarge);
        QByteArray zeroLength("\x93NUMPY\x01\x00", 8);
        zeroLength += le16(0);
        row("header length 0", zeroLength, kNpyHeaderMalformed);

        row("object dtype", npyHeader(dictFor("|O", "False", shape)) + body, kNpyDtype);
        row("float32", npyHeader(dictFor("<f4", "False", shape)) + body, kNpyDtype);
        row("int8", npyHeader(dictFor("|i1", "False", shape)) + body, kNpyDtype);
        row("big-endian u1", npyHeader(dictFor(">u1", "False", shape)) + body, kNpyDtype);
        row("uint16", npyHeader(dictFor("<u2", "False", shape)) + body, kNpyDtype);
        row("bool", npyHeader(dictFor("|b1", "False", shape)) + body, kNpyDtype);
        row("empty dtype", npyHeader(dictFor("", "False", shape)) + body, kNpyDtype);
        row("structured dtype", npyHeader(dictFor("[('a', '|u1')]", "False", shape)) + body, kNpyHeaderMalformed);
        row("fortran order", npyHeader(dictFor("|u1", "True", shape)) + body, kNpyFortranOrder);

        row("3-D", npyHeader(dictFor("|u1", "False", "(2, 2, 3)")) + body, kNpyShape);
        row("2-D", npyHeader(dictFor("|u1", "False", "(2, 3)")) + body, kNpyShape);
        row("1-D", npyHeader(dictFor("|u1", "False", "(12,)")) + body, kNpyShape);
        row("0-D", npyHeader(dictFor("|u1", "False", "()")) + body, kNpyShape);
        row("5-D", npyHeader(dictFor("|u1", "False", "(1, 2, 2, 2, 3)")) + body, kNpyShape);
        row("four channels", npyHeader(dictFor("|u1", "False", "(2, 2, 2, 4)")) + body, kNpyShape);
        row("one channel", npyHeader(dictFor("|u1", "False", "(2, 2, 2, 1)")) + body, kNpyShape);
        row("zero frames", npyHeader(dictFor("|u1", "False", "(0, 2, 2, 3)")), kNpyShape);
        row("zero height", npyHeader(dictFor("|u1", "False", "(2, 0, 2, 3)")), kNpyShape);
        row("zero width", npyHeader(dictFor("|u1", "False", "(2, 2, 0, 3)")), kNpyShape);

        row("1025 frames", npyHeader(dictFor("|u1", "False", "(1025, 2, 2, 3)")), kNpyOverLimit);
        row("height 4097", npyHeader(dictFor("|u1", "False", "(1, 4097, 2, 3)")), kNpyOverLimit);
        row("width 4097", npyHeader(dictFor("|u1", "False", "(1, 2, 4097, 3)")), kNpyOverLimit);
        row("over 256 MiB", npyHeader(dictFor("|u1", "False", "(1024, 400, 400, 3)")), kNpyOverLimit);
        row("max everything", npyHeader(dictFor("|u1", "False", "(1024, 4096, 4096, 3)")), kNpyOverLimit);
        row("2^40 frames", npyHeader(dictFor("|u1", "False", "(9999999999, 4096, 4096, 3)")), kNpyOverLimit);
        row("12 digits of 9", npyHeader(dictFor("|u1", "False", "(999999999999, 999999999999, 999999999999, 3)")), kNpyOverLimit);

        row("missing descr", npyHeader("{'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("missing shape", npyHeader("{'descr': '|u1', 'fortran_order': False}") + body, kNpyHeaderMalformed);
        row("missing fortran_order", npyHeader("{'descr': '|u1', 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("repeated key", npyHeader("{'descr': '|u1', 'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("extra key", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3), 'x': 1}") + body, kNpyHeaderMalformed);
        row("not a dict", npyHeader("[1, 2, 3]") + body, kNpyHeaderMalformed);
        row("code instead", npyHeader("__import__('os').system('true')") + body, kNpyHeaderMalformed);
        row("code in a value", npyHeader("{'descr': __import__('os'), 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("no closing brace", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)") + body, kNpyHeaderMalformed);
        row("text after the brace", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)} x") + body, kNpyHeaderMalformed);
        row("lowercase true", npyHeader("{'descr': '|u1', 'fortran_order': true, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("fortran 0", npyHeader("{'descr': '|u1', 'fortran_order': 0, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("leading zero", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (0003, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("leading zeros", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 000000000002, 2, 3)}") + body, kNpyHeaderMalformed);
        row("negative dimension", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2, -2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("python 2 long", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2L, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("hex dimension", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (0x2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("float dimension", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (2.0, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("13 digits", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (1000000000000, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("nine dimensions", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (1, 1, 1, 1, 1, 1, 1, 1, 3)}") + body, kNpyHeaderMalformed);
        row("an escape", npyHeader("{'de\\x73cr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("unterminated string", npyHeader("{'descr': '|u1, 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("non-ASCII", npyHeader("{'descr': '|u\xc3\xa9', 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("a control byte", npyHeader(QByteArray("{'descr': '|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)}\x01")) + body, kNpyHeaderMalformed);
        row("a tab", npyHeader("{'descr':\t'|u1', 'fortran_order': False, 'shape': (2, 2, 2, 3)}") + body, kNpyHeaderMalformed);
        row("no newline", npyHeader(dictFor("|u1", "False", shape)).chopped(1) + QByteArray(" ") + body, kNpyHeaderMalformed);
        row("empty braces", npyHeader("{}") + body, kNpyHeaderMalformed);
        row("empty shape tuple parts", npyHeader("{'descr': '|u1', 'fortran_order': False, 'shape': (,)}") + body, kNpyHeaderMalformed);
    }
    void headerRefusalsHaveTheirOwnNames()
    {
        QFETCH(QByteArray, file);
        QFETCH(QString, reason);
        const Read r = readAll(file, {0}, 0, {}, -1);
        QVERIFY2(!r.ok, "must be refused");
        QVERIFY2(r.error.startsWith(reason), qPrintable(QString("wanted \"%1\", got \"%2\"").arg(reason, r.error)));
        QVERIFY(r.frames.isEmpty()); // nothing is delivered from a refused file
        // the same file in one-byte chunks is refused for the same reason
        const Read slow = readAll(file, {0}, 1, {}, -1);
        QVERIFY(!slow.ok);
        QVERIFY2(slow.error.startsWith(reason) || (reason == nameOf(kNpyNotNpy) && !slow.error.isEmpty()), qPrintable(slow.error));
    }

    void limitsAreCheckedOnTheHeaderBeforeAnyDataIsRead()
    {
        // Only the header of a 3 GB array is given: the refusal comes from the header alone, and nothing is asked for or allocated.
        const QByteArray header = npyHeader(dictFor("|u1", "False", "(1024, 4096, 4096, 3)"));
        bool chosen = false;
        NpyFrameReader reader({}, [&](const NpyShape &) { chosen = true; return QList<int>{0}; }, [](int, const uchar *, int, int) { return true; }, -1);
        QVERIFY(!reader.feed(header.constData(), header.size()));
        QVERIFY(reader.error().startsWith(nameOf(kNpyOverLimit)));
        QVERIFY(!chosen);
        // a narrower limit set refuses a file the defaults take
        NpyLimits small;
        small.maxFrames = 4;
        QVERIFY(refusedAs(readAll(npy(5, 2, 2), {0}, 0, small), kNpyOverLimit));
        small = {};
        small.maxSide = 3;
        QVERIFY(refusedAs(readAll(npy(1, 4, 2), {0}, 0, small), kNpyOverLimit));
        small = {};
        small.maxDataBytes = 100;
        QVERIFY(refusedAs(readAll(npy(3, 4, 4), {0}, 0, small), kNpyOverLimit)); // 144 bytes
        small = {};
        small.maxHeaderBytes = 32;
        QVERIFY(refusedAs(readAll(npy(1, 2, 2), {0}, 0, small), kNpyHeaderTooLarge));
    }

    void aShapeThatDisagreesWithTheSizeIsRefusedBeforeTheData()
    {
        QByteArray file = npy(3, 2, 2);
        file.chop(5);
        bool delivered = false;
        NpyFrameReader reader({}, [](const NpyShape &) { return QList<int>{0}; }, [&](int, const uchar *, int, int) { delivered = true; return true; }, file.size());
        QVERIFY(!reader.feed(file.constData(), file.size()));
        QVERIFY(reader.error().startsWith(nameOf(kNpyTruncated)));
        QVERIFY(!delivered); // not even frame 0, which was complete
        const Read trailing = readAll(npy(3, 2, 2) + QByteArray("junk"), {0});
        QVERIFY(refusedAs(trailing, kNpyTrailingData));
        QVERIFY(trailing.frames.isEmpty());
    }

    void truncationIsFoundWithoutAKnownSize()
    {
        for (const int cut : {1, 11, 12, 13, 35, 36, 37}) {
            QByteArray file = npy(3, 2, 2);
            file.chop(cut);
            QVERIFY2(refusedAs(readAll(file, {0, 1, 2}, 0, {}, -1), kNpyTruncated), qPrintable(QString::number(cut)));
        }
        QByteArray headerOnly = npy(3, 2, 2).left(40);
        QVERIFY(!readAll(headerOnly, {0}, 0, {}, -1).ok);
        QVERIFY(refusedAs(readAll(npy(1, 1, 1).left(5), {0}, 0, {}, -1), kNpyNotNpy) || !readAll(npy(1, 1, 1).left(5), {0}, 0, {}, -1).ok);
    }

    void dataAfterTheArrayIsRefusedWithoutAKnownSize()
    {
        QVERIFY(refusedAs(readAll(npy(2, 2, 2) + QByteArray("x"), {0}, 0, {}, -1), kNpyTrailingData));
        QVERIFY(refusedAs(readAll(npy(2, 2, 2) + QByteArray(1000, 'x'), {0}, 7, {}, -1), kNpyTrailingData));
    }

    void aFrameOutsideTheArrayIsRefused()
    {
        QVERIFY(refusedAs(readAll(npy(3, 2, 2), {3}), kNpyShape));
        QVERIFY(refusedAs(readAll(npy(3, 2, 2), {-1}), kNpyShape));
    }

    void theCallerCanStop()
    {
        NpyFrameReader reader({}, [](const NpyShape &) { return QList<int>{0, 1}; }, [](int, const uchar *, int, int) { return false; }, -1);
        const QByteArray file = npy(3, 2, 2);
        QVERIFY(!reader.feed(file.constData(), file.size()));
        QVERIFY(reader.error().startsWith(nameOf(kCancelled)));
    }

    void aReadCanBeStoppedFromOutside()
    {
        int asked = 0;
        NpyFrameReader reader({}, [](const NpyShape &) { return QList<int>{0}; }, [](int, const uchar *, int, int) { return true; }, -1);
        reader.setAlive([&] { return ++asked < 3; });
        const QByteArray file = npy(3, 2, 2);
        QVERIFY(reader.feed(file.constData(), 10));
        QVERIFY(reader.feed(file.constData() + 10, 10));
        QVERIFY(!reader.feed(file.constData() + 20, 10));
        QVERIFY(reader.error().startsWith(nameOf(kCancelled)));
        // through the .npz path too
        QBuffer zip;
        zip.setData(npz(npy(3, 8, 8), false));
        zip.open(QIODevice::ReadOnly);
        QString error;
        QVERIFY(!readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{0}; }, [](int, const uchar *, int, int) { return true; }, &error, nullptr, [] { return false; }));
        QVERIFY2(error.startsWith(nameOf(kCancelled)), qPrintable(error));
    }

    void aDeviceIsReadToItsEnd()
    {
        QBuffer buffer;
        buffer.setData(npy(4, 3, 3));
        buffer.open(QIODevice::ReadOnly);
        QList<int> got;
        QString error;
        NpyShape shape;
        QVERIFY(readNpyFrames(&buffer, {}, [](const NpyShape &) { return QList<int>{2}; }, [&](int i, const uchar *, int, int) { got.append(i); return true; }, &error, &shape));
        QCOMPARE(got, (QList<int>{2}));
        QCOMPARE(shape.frames, 4);
        QBuffer cut;
        cut.setData(npy(4, 3, 3).chopped(2));
        cut.open(QIODevice::ReadOnly);
        QVERIFY(!readNpyFrames(&cut, {}, [](const NpyShape &) { return QList<int>{}; }, [](int, const uchar *, int, int) { return true; }, &error));
        QVERIFY(error.startsWith(nameOf(kNpyTruncated)));
    }

    // ---- .npz ----
    void anNpzOfEitherMethodIsRead_data()
    {
        QTest::addColumn<bool>("deflate");
        QTest::newRow("stored") << false;
        QTest::newRow("deflate") << true;
    }
    void anNpzOfEitherMethodIsRead()
    {
        QFETCH(bool, deflate);
        QBuffer zip;
        zip.setData(npz(npy(5, 3, 3), deflate));
        zip.open(QIODevice::ReadOnly);
        QList<int> got;
        QList<QByteArray> frames;
        QString error;
        QVERIFY2(readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{0, 4}; },
                               [&](int i, const uchar *rgb, int w, int h) { got.append(i); frames.append(QByteArray(reinterpret_cast<const char *>(rgb), w * h * 3)); return true; }, &error),
                 qPrintable(error));
        QCOMPARE(got, (QList<int>{0, 4}));
        QCOMPARE(frames.at(1), pixels(5, 3, 3).mid(4 * 27, 27));
    }

    void aFlatPictureInsideAnNpzDeflatesFarAndStillReads()
    {
        const QByteArray flat = npyHeader(dictFor("|u1", "False", "(20, 200, 200, 3)")) + QByteArray(20 * 200 * 200 * 3, char(9)); // 2.4 MB of one value
        QBuffer zip;
        zip.setData(npz(flat, true));
        zip.open(QIODevice::ReadOnly);
        int count = 0;
        QString error;
        QVERIFY2(readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{3}; }, [&](int, const uchar *, int, int) { ++count; return true; }, &error), qPrintable(error));
        QCOMPARE(count, 1);
    }

    void anNpzRefusalsHaveTheirNames_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<QString>("reason");
        const QByteArray good = npy(2, 2, 2);
        QTest::newRow("not a zip") << QByteArray(100, 'x') << nameOf(kNpzNotAZip);
        QTest::newRow("empty") << QByteArray() << nameOf(kNpzNotAZip);
        QTest::newRow("a bare npy") << good << nameOf(kNpzNotAZip);
        QTest::newRow("no frames member") << npz(good, false, "other.npy") << nameOf(kNpzNoFrames);
        QTest::newRow("no extension") << npz(good, false, "frames") << nameOf(kNpzNoFrames);
        QTest::newRow("a folder named frames.npy") << buildZip({npyfixtures::zipMember("frames.npy/", QByteArray(), true, false, false, 0)}) << nameOf(kNpzNoFrames);
        QTest::newRow("object array inside") << npz(npyHeader(dictFor("|O", "False", "(2, 2, 2, 3)")) + QByteArray("\x80\x04 cos\nsystem\n"), true) << nameOf(kNpyDtype);
        QTest::newRow("pickle inside") << npz(QByteArray("\x80\x04\x95 cos\nsystem\n(S'true'\ntR."), true) << nameOf(kNpyNotNpy);
        QTest::newRow("truncated array") << npz(good.chopped(3), true) << nameOf(kNpyTruncated);
        QTest::newRow("trailing bytes") << npz(good + QByteArray("zz"), true) << nameOf(kNpyTrailingData);
        QTest::newRow("bad crc") << buildZip({npyfixtures::zipMember("frames.npy", good, false, true, false, 0)}) << QString("zip: entry refused");
        QTest::newRow("a lying size") << buildZip({npyfixtures::zipMember("frames.npy", good, false, false, true, quint32(good.size() - 10))}) << QString("zip: entry refused");
    }
    void anNpzRefusalsHaveTheirNames()
    {
        QFETCH(QByteArray, file);
        QFETCH(QString, reason);
        QBuffer zip;
        zip.setData(file);
        zip.open(QIODevice::ReadOnly);
        QString error;
        QList<int> delivered;
        QVERIFY(!readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{0}; }, [&](int i, const uchar *, int, int) { delivered.append(i); return true; }, &error));
        QVERIFY2(error.startsWith(reason), qPrintable(QString("wanted \"%1\", got \"%2\"").arg(reason, error)));
    }

    void anNpzBombIsRefusedByItsDeclaredSize()
    {
        // A directory that declares a gigabyte for a member: refused at open, the data is never inflated.
        const QByteArray good = npy(1, 2, 2);
        QBuffer zip;
        zip.setData(buildZip({npyfixtures::zipMember("frames.npy", good, false, false, true, 1u << 30)}));
        zip.open(QIODevice::ReadOnly);
        QString error;
        QVERIFY(!readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{0}; }, [](int, const uchar *, int, int) { return true; }, &error));
        QVERIFY(!error.isEmpty());
    }

    // ---- Fuzz ----
    void mutatedFilesNeverCrashAndNeverLie()
    {
        QRandomGenerator rng(20261023);
        const QByteArray seeds[] = {npy(3, 4, 5), npy(2, 3, 3, 2), npy(2, 3, 3, 3), npyHeader("{'descr': '<u1', 'fortran_order': False, 'shape': (1, 2, 2, 3,), }") + pixels(1, 2, 2)};
        int accepted = 0, refused = 0;
        for (int i = 0; i < 4000; ++i) {
            QByteArray file = seeds[i % 4];
            const int edits = 1 + int(rng.bounded(4));
            for (int e = 0; e < edits && !file.isEmpty(); ++e) {
                switch (rng.bounded(5)) {
                case 0: file[int(rng.bounded(file.size()))] = char(rng.bounded(256)); break;
                case 1: file[int(rng.bounded(qMin<qsizetype>(file.size(), 140)))] = char(rng.bounded(256)); break; // favour the header
                case 2: file.truncate(int(rng.bounded(file.size() + 1))); break;
                case 3: file.insert(int(rng.bounded(file.size() + 1)), QByteArray(1 + int(rng.bounded(3)), char(rng.bounded(256)))); break;
                case 4: file.remove(int(rng.bounded(file.size())), 1 + int(rng.bounded(3))); break;
                }
            }
            const int chunk = rng.bounded(2) ? 0 : 1 + int(rng.bounded(40));
            const Read r = readAll(file, {0}, chunk, {}, rng.bounded(2) ? -1 : file.size());
            if (r.ok) {
                ++accepted;
                QVERIFY(r.error.isEmpty());
                QCOMPARE(r.frames.size(), r.indices.size());
                for (int k = 0; k < r.indices.size(); ++k) {
                    QVERIFY(r.indices.at(k) >= 0 && r.indices.at(k) < r.shape.frames);
                    QCOMPARE(r.frames.at(k).size(), int(r.shape.frameBytes()));
                }
            } else {
                ++refused;
                QVERIFY2(!r.error.isEmpty(), "a refusal must name its reason");
            }
        }
        qInfo() << "fuzz:" << accepted << "accepted," << refused << "refused";
        QVERIFY(accepted > 50 && refused > 1000); // the run exercised both outcomes
    }

    void mutatedNpzFilesNeverCrash()
    {
        QRandomGenerator rng(7);
        const QByteArray seeds[] = {npz(npy(3, 4, 5), false), npz(npy(3, 4, 5), true)};
        int refused = 0, accepted = 0;
        for (int i = 0; i < 2000; ++i) {
            QByteArray file = seeds[i % 2];
            const int edits = 1 + int(rng.bounded(3));
            for (int e = 0; e < edits && !file.isEmpty(); ++e) {
                switch (rng.bounded(3)) {
                case 0: file[int(rng.bounded(file.size()))] = char(rng.bounded(256)); break;
                case 1: file.truncate(int(rng.bounded(file.size() + 1))); break;
                case 2: file.insert(int(rng.bounded(file.size() + 1)), QByteArray(1, char(rng.bounded(256)))); break;
                }
            }
            QBuffer zip;
            zip.setData(file);
            zip.open(QIODevice::ReadOnly);
            QString error;
            const bool ok = readNpzFrames(&zip, {}, [](const NpyShape &) { return QList<int>{0, 2}; }, [](int, const uchar *, int, int) { return true; }, &error);
            QVERIFY2(ok || !error.isEmpty(), "a refusal must name its reason");
            refused += !ok;
            accepted += ok;
        }
        QVERIFY2(accepted > 0 && refused > 500, qPrintable(QString("%1 accepted, %2 refused").arg(accepted).arg(refused)));
    }
};

QTEST_GUILESS_MAIN(TstNpyReader)
#include "tst_npyreader.moc"
