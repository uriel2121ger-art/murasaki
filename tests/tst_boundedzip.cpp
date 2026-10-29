// BoundedZip against well-formed and hostile archives built by importfixtures.h.

#include "boundedzip.h"
#include "importfixtures.h"
#include "petimport.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>

using namespace murasaki::import;
using namespace importfixtures;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

ZipMember member(const QByteArray &name, const QByteArray &data, bool deflate = true)
{
    ZipMember m;
    m.name = name;
    m.data = data;
    m.deflate = deflate;
    return m;
}

struct Opened {
    std::optional<BoundedZip> zip;
    QString error;
    QBuffer device;
};

// Opens `bytes` in `out`; the buffer must outlive the reader.
void openIn(Opened &out, const QByteArray &bytes, const ZipLimits &limits = {})
{
    out.device.setData(bytes);
    QVERIFY(out.device.open(QIODevice::ReadOnly));
    out.zip = BoundedZip::open(&out.device, limits, &out.error);
}

} // namespace

#define OPEN(var, bytes, ...) \
    Opened var;               \
    openIn(var, bytes, ##__VA_ARGS__)

#define REFUSED(bytes, wanted, ...)                                                          \
    do {                                                                                     \
        OPEN(o, bytes, ##__VA_ARGS__);                                                       \
        QVERIFY2(!o.zip.has_value(), "accepted");                                            \
        QVERIFY2(o.error.startsWith(nameOf(wanted)), qPrintable(o.error));                   \
    } while (false)

class TstBoundedZip : public QObject
{
    Q_OBJECT

private slots:
    void wellFormedArchiveReadsBackStoredAndDeflated()
    {
        QByteArray big(100000, 0);
        QRandomGenerator rng(7);
        for (char &c : big)
            c = char('a' + rng.bounded(16)); // compresses about 2:1, well under the ratio limit
        OPEN(o, buildZip({member("a/b.txt", "hello hello hello hello", true), member("c.bin", "raw", false), member("big", big)}));
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
        QCOMPARE(o.zip->entries().size(), 3);
        QCOMPARE(o.zip->entries().at(0).name, QStringLiteral("a/b.txt"));
        QString error;
        QCOMPARE(o.zip->read(0, &error).value(), QByteArray("hello hello hello hello"));
        QCOMPARE(o.zip->read(1, &error).value(), QByteArray("raw"));
        QCOMPARE(o.zip->read(2, &error).value(), big);
        QCOMPARE(o.zip->sha256(2, &error).value(), QCryptographicHash::hash(big, QCryptographicHash::Sha256));
        QBuffer sink;
        QVERIFY(sink.open(QIODevice::WriteOnly));
        QVERIFY(o.zip->extractTo(2, &sink, &error));
        QCOMPARE(sink.data(), big);
    }
    void directoriesAreListedAndFlagged()
    {
        OPEN(o, buildZip({member("dir/", {}, false), member("dir/x", "x", false)}));
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
        QVERIFY(o.zip->entries().at(0).isDirectory);
        QVERIFY(!o.zip->entries().at(1).isDirectory);
    }
    void notAZipAndTruncatedFilesAreRefused()
    {
        REFUSED(QByteArray(), kZipNotAZip);
        REFUSED(QByteArray("PK"), kZipNotAZip);
        REFUSED(QByteArray(5000, 'x'), kZipNotAZip);
        const QByteArray good = buildZip({member("a", "abcabcabc")});
        REFUSED(good.left(good.size() - 1), kZipNotAZip);
        REFUSED(good.left(good.size() - 22), kZipNotAZip);
        REFUSED(good + QByteArray(1, 'x'), kZipNotAZip); // trailing junk after the end record
    }
    void aCommentThatHidesAnotherEndRecordIsRefused()
    {
        const QByteArray inner = buildZip({member("evil", "x", false)});
        REFUSED(buildZip({member("good", "x", false)}, inner), kZipNotAZip);
        // a plain comment is fine
        OPEN(o, buildZip({member("good", "x", false)}, "just a comment"));
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
    }
    void entryCountThatDisagreesWithTheDirectoryIsRefused()
    {
        REFUSED(buildZip({member("a", "x", false)}, {}, 5), kZipNotAZip);
        REFUSED(buildZip({member("a", "x", false), member("b", "y", false)}, {}, 1), kZipNotAZip);
        ZipLimits few;
        few.maxEntries = 2;
        REFUSED(buildZip({member("a", "1", false), member("b", "2", false), member("c", "3", false)}), kZipOverTheLimit, few);
    }
    void hostileNamesAreRefused_data()
    {
        QTest::addColumn<QByteArray>("name");
        for (const char *n : {"../x", "a/../../x", "/etc/passwd", "C:/x", "c:\\x", "a//b", "./x", "a/./b", "a\\..\\b", "tab\there", "new\nline", "bell\a",
                              "\xe2\x80\xae" "gpj.exe", "\xe2\x80\xab" "x", "..", "a/.."})
            QTest::newRow(n) << QByteArray(n);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("nul") << QByteArray("x\0y", 3);
        QTest::newRow("long") << QByteArray(600, 'a');
    }
    void hostileNamesAreRefused()
    {
        QFETCH(QByteArray, name);
        ZipMember m = member(name, "data", false);
        for (const char c : name)
            if (uchar(c) >= 0x80)
                m.flags = 0x0800; // the UTF-8 flag: these bytes are the bidirectional controls, not CP437 letters
        REFUSED(buildZip({m}), kZipEntryRefused);
    }
    void duplicateNamesAfterNormalisationAndCaseAreRefused()
    {
        REFUSED(buildZip({member("A.png", "1", false), member("a.png", "2", false)}), kZipEntryRefused);
        REFUSED(buildZip({member("a/b", "1", false), member("a/b", "2", false)}), kZipEntryRefused);
        REFUSED(buildZip({member("a\\b", "1", false), member("a/b", "2", false)}), kZipEntryRefused);
    }
    void encryptedSymlinkAndOddMethodsAreRefused()
    {
        ZipMember enc = member("e", "x", false);
        enc.flags = 1;
        REFUSED(buildZip({enc}), kZipEntryRefused);
        ZipMember link = member("l", "target", false);
        link.externalAttributes = 0120777u << 16;
        REFUSED(buildZip({link}), kZipEntryRefused);
        ZipMember bzip = member("b", "x", false);
        bzip.method = 12;
        REFUSED(buildZip({bzip}), kZipEntryRefused);
        ZipMember lzma = member("m", "x", false);
        lzma.method = 14;
        REFUSED(buildZip({lzma}), kZipEntryRefused);
    }
    void sizesAndRatiosOverTheLimitsAreRefused()
    {
        ZipLimits small;
        small.maxMemberBytes = 1000;
        REFUSED(buildZip({member("a", QByteArray(5000, 'a'))}), kZipOverTheLimit, small);
        ZipLimits total;
        total.maxTotalBytes = 3000;
        REFUSED(buildZip({member("a", QByteArray(2000, 'a'), false), member("b", QByteArray(2000, 'b'), false)}), kZipOverTheLimit, total);
        // a bomb: 10 MB of zeros in a few KB
        REFUSED(buildZip({member("bomb", QByteArray(10 * 1000 * 1000, '\0'))}), kZipOverTheLimit);
        // a tiny member is exempt from the ratio
        OPEN(o, buildZip({member("t", QByteArray(3000, '\0'))}));
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
    }
    void aBigMemberIsStreamedNotHeld()
    {
        const QByteArray big(3 * 1000 * 1000, 'q');
        ZipLimits limits;
        limits.maxMemoryBytes = 1000000;
        limits.maxRatio = 100000;
        OPEN(o, buildZip({member("big", big)}), limits);
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
        QString error;
        QVERIFY(!o.zip->read(0, &error).has_value()); // over what read() keeps
        QVERIFY(!error.isEmpty());
        QCOMPARE(o.zip->sha256(0, &error).value(), QCryptographicHash::hash(big, QCryptographicHash::Sha256));
    }
    void lyingSizesAndChecksumsAreCaught()
    {
        QString error;
        {
            ZipMember lie = member("a", QByteArray(500, 'x'));
            lie.lieAboutSize = true;
            lie.declaredSize = 100; // says 100, inflates to 500: the cap stops it
            OPEN(o, buildZip({lie}));
            if (o.zip) {
                QVERIFY(!o.zip->read(0, &error).has_value());
                QVERIFY(!error.isEmpty());
            }
        }
        {
            ZipMember lie = member("a", QByteArray(100, 'x'));
            lie.lieAboutSize = true;
            lie.declaredSize = 900; // says 900, delivers 100
            OPEN(o, buildZip({lie}));
            if (o.zip)
                QVERIFY(!o.zip->read(0, &error).has_value());
        }
        {
            ZipMember bad = member("a", "payload payload", false);
            bad.badCrc = true;
            OPEN(o, buildZip({bad}));
            QVERIFY2(o.zip.has_value(), qPrintable(o.error));
            QVERIFY(!o.zip->read(0, &error).has_value());
            QVERIFY(error.contains(QStringLiteral("crc"), Qt::CaseInsensitive));
            QBuffer sink;
            QVERIFY(sink.open(QIODevice::WriteOnly));
            QVERIFY(!o.zip->extractTo(0, &sink, &error));
            QVERIFY(!o.zip->sha256(0, &error).has_value());
        }
        {
            ZipMember missing = member("a", "payload", false);
            missing.noLocalHeader = true;
            OPEN(o, buildZip({missing}));
            if (o.zip)
                QVERIFY(!o.zip->read(0, &error).has_value());
        }
    }
    void aGarbageDeflateStreamFailsCleanly()
    {
        // a stored member whose method field (local header and directory) is patched to deflate: the bytes are not a deflate stream
        QByteArray bytes = buildZip({member("a", QByteArray(64, 'x'), false)});
        bytes[8] = 8;
        bytes[bytes.indexOf("PK\x01\x02") + 10] = 8;
        OPEN(o, bytes);
        if (o.zip) {
            QString error;
            QVERIFY(!o.zip->read(0, &error).has_value());
            QVERIFY(!error.isEmpty());
        }
    }
    void outOfRangeIndexesAreNotCrashes()
    {
        OPEN(o, buildZip({member("a", "x", false)}));
        QVERIFY(o.zip);
        QString error;
        QVERIFY(!o.zip->read(-1, &error).has_value());
        QVERIFY(!o.zip->read(1, &error).has_value());
        QVERIFY(!o.zip->sha256(99, &error).has_value());
    }
    void aCentralDirectoryCannotAskForTheWholeFile()
    {
        QByteArray bytes = buildZip({member("a", QByteArray(60000, 'x'), false)});
        const int end = bytes.size() - 22;
        qToLittleEndian<quint32>(quint32(end), bytes.data() + end + 12); // directory size = the whole file before the end record
        qToLittleEndian<quint32>(0, bytes.data() + end + 16);            // directory offset 0
        {
            OPEN(o, bytes);
            QVERIFY(!o.zip.has_value()); // refused (by size when it is large, by shape when it is small)
        }
        ZipLimits small;
        small.maxDirectoryBytes = 40;
        REFUSED(buildZip({member("a", "x", false)}), kZipOverTheLimit, small);
    }
    void theLocalHeaderMustAgreeWithTheDirectory()
    {
        ZipMember renamed = member("a.png", "data data", false);
        renamed.hasLocalName = true;
        renamed.localName = "b.png";
        REFUSED(buildZip({renamed}), kZipEntryRefused);
        ZipMember shorter = member("abc.png", "data data", false);
        shorter.hasLocalName = true;
        shorter.localName = "a.png";
        REFUSED(buildZip({shorter}), kZipEntryRefused);
        ZipMember flags = member("a", "data", false);
        flags.flags = 0x0800; // the directory says UTF-8 ... patched below in the local header only
        QByteArray bytes = buildZip({flags});
        bytes[6] = 0;
        bytes[7] = 0;
        REFUSED(bytes, kZipEntryRefused);
    }
    void aLongLocalExtraFieldCannotMoveTheDataUnderAnotherMember()
    {
        // The second member's local header is placed so that its data (after a huge extra field) would start inside the first member's bytes.
        ZipMember first = member("first.bin", QByteArray(500, 'f'), false);
        ZipMember second = member("second.bin", QByteArray(100, 's'), false);
        QByteArray bytes = buildZip({first, second});
        // find the second local header and make its extra length point back over nothing useful: the declared data range then runs into the directory
        const int local = bytes.indexOf("PK\x03\x04", 4);
        qToLittleEndian<quint16>(60000, bytes.data() + local + 28);
        const QByteArray out = bytes;
        OPEN(o, out);
        QVERIFY(!o.zip.has_value());
    }
    void anExtraFieldThatKeepsTheRangesApartIsFine()
    {
        ZipMember a = member("a", "aaaa", false);
        a.localExtra = QByteArray(30, 'e');
        OPEN(o, buildZip({a, member("b", "bbbb", false)}));
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
        QString error;
        QCOMPARE(o.zip->read(0, &error).value(), QByteArray("aaaa"));
        QCOMPARE(o.zip->read(1, &error).value(), QByteArray("bbbb"));
    }
    void cp437NamesAreAcceptedWithoutTheUtf8Flag()
    {
        OPEN(o, buildZip({member(QByteArray("caf\x82.png"), "data", false)})); // CP437 e-acute
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
    }
    void anOversizeMemberCanBeSkippedInsteadOfRefusingTheArchive()
    {
        ZipLimits limits;
        limits.maxMemberBytes = 1000;
        limits.skipOversize = true;
        OPEN(o, buildZip({member("big.mp4", QByteArray(5000, 'v'), false), member("ok.png", "small", false)}), limits);
        QVERIFY2(o.zip.has_value(), qPrintable(o.error));
        QVERIFY(o.zip->entries().at(0).tooBig);
        QVERIFY(!o.zip->entries().at(1).tooBig);
        QString error;
        QVERIFY(!o.zip->read(0, &error).has_value());
        QVERIFY(error.contains(QStringLiteral("skipped")));
        QBuffer sink;
        QVERIFY(sink.open(QIODevice::WriteOnly));
        QVERIFY(!o.zip->extractTo(0, &sink, &error));
        QVERIFY(!o.zip->sha256(0, &error).has_value());
        QCOMPARE(o.zip->read(1, &error).value(), QByteArray("small"));
    }
    void normaliseNameRule()
    {
        QString why;
        QCOMPARE(normaliseZipName("a\\b/c.png", true, 512, &why), QStringLiteral("a/b/c.png"));
        QCOMPARE(normaliseZipName("dir/", true, 512, &why), QStringLiteral("dir/"));
        QVERIFY(normaliseZipName("../a", true, 512, &why).isEmpty());
        QVERIFY(!why.isEmpty());
    }
    void mutatedArchivesNeverCrashAndNeverEscape()
    {
        // fixed seed: flips bytes of a valid archive; every outcome must be a refusal or a clean read, and every accepted name must stay relative
        const QByteArray base = buildZip({member("frames/0001.png", QByteArray(3000, 'a')), member("frames/0002.png", QByteArray(2000, 'b'), false), member("meta.json", "{}")});
        QRandomGenerator rng(20261021);
        int accepted = 0;
        for (int round = 0; round < 3000; ++round) {
            QByteArray mutated = base;
            const int flips = 1 + int(rng.bounded(4));
            for (int i = 0; i < flips; ++i)
                mutated[int(rng.bounded(quint32(mutated.size())))] = char(rng.bounded(256));
            if (rng.bounded(10) == 0)
                mutated.truncate(int(rng.bounded(quint32(mutated.size()))));
            OPEN(o, mutated);
            if (!o.zip)
                continue;
            ++accepted;
            for (int i = 0; i < o.zip->entries().size(); ++i) {
                const QString &name = o.zip->entries().at(i).name;
                QVERIFY(!name.startsWith(QLatin1Char('/')));
                QVERIFY(!name.split(QLatin1Char('/')).contains(QStringLiteral("..")));
                QString error;
                (void)o.zip->read(i, &error);
            }
        }
        QVERIFY(accepted > 0); // some mutations leave a valid archive
    }
};

QTEST_GUILESS_MAIN(TstBoundedZip)
#include "tst_boundedzip.moc"
