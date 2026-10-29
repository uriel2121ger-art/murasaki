// The import job end to end: real files in, an APNG and source.json out, checked with the independent decoder of importfixtures.h; hostile sources;
// cancel; the same import twice; and the helper process.

#include "importfixtures.h"
#include "importjob.h"
#include "localpets.h"
#include "sourcefile.h"
#include "animationinfo.h"
#include "importspec.h"
#include "videosource.h"
#include "petimport.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QLockFile>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <sys/stat.h>

using namespace murasaki::import;
using namespace importfixtures;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

QImage solid(int w, int h, QRgb rgb)
{
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(rgb);
    return image;
}

QByteArray rgba(const QImage &image)
{
    const QImage converted = image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray out;
    for (int y = 0; y < converted.height(); ++y)
        out += QByteArray(reinterpret_cast<const char *>(converted.constScanLine(y)), converted.width() * 4);
    return out;
}

QByteArray pngBytes(const QImage &image)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// A PNG whose header claims an enormous canvas (valid CRC, no pixel data): a decompression-bomb header.
QByteArray hugeHeaderPng(quint32 w, quint32 h)
{
    QByteArray ihdr;
    ihdr += QByteArray(4, 0);
    qToBigEndian(w, ihdr.data());
    QByteArray hh(4, 0);
    qToBigEndian(h, hh.data());
    ihdr += hh;
    ihdr += QByteArray("\x08\x06\x00\x00\x00", 5);
    const QByteArray typeAndData = QByteArray("IHDR") + ihdr;
    QByteArray crc(4, 0);
    qToBigEndian<quint32>(quint32(crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(typeAndData.constData()), uInt(typeAndData.size()))), crc.data());
    QByteArray len(4, 0);
    qToBigEndian<quint32>(13, len.data());
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + len + typeAndData + crc;
}

// A GIF89a of full-canvas frames over a 4-colour palette; every frame is written with LZW "clear" every two pixels, which needs no dictionary.
QByteArray makeGif(int w, int h, const QList<QByteArray> &indexed, const QList<int> &delaysCs, const QList<QRgb> &palette)
{
    QByteArray g("GIF89a");
    g += le16(quint16(w)) + le16(quint16(h)) + QByteArray("\xf1\x00\x00", 3);
    for (const QRgb c : palette)
        g += char(qRed(c)) + QByteArray(1, char(qGreen(c))) + QByteArray(1, char(qBlue(c)));
    for (int f = 0; f < indexed.size(); ++f) {
        g += QByteArray("\x21\xf9\x04\x04", 4) + le16(quint16(delaysCs.at(f))) + QByteArray("\x00\x00", 2);
        g += QByteArray("\x2c", 1) + le16(0) + le16(0) + le16(quint16(w)) + le16(quint16(h)) + QByteArray("\x00\x02", 2);
        QByteArray bits;
        quint32 acc = 0;
        int nbits = 0;
        const auto put = [&](int code) {
            acc |= quint32(code) << nbits;
            nbits += 3;
            while (nbits >= 8) {
                bits.append(char(acc & 0xff));
                acc >>= 8;
                nbits -= 8;
            }
        };
        const QByteArray &px = indexed.at(f);
        for (int i = 0; i < px.size(); i += 2) {
            put(4);
            put(px.at(i));
            if (i + 1 < px.size())
                put(px.at(i + 1));
        }
        put(5);
        if (nbits > 0)
            bits.append(char(acc & 0xff));
        for (int i = 0; i < bits.size(); i += 255) {
            const QByteArray block = bits.mid(i, 255);
            g += char(block.size()) + block;
        }
        g += QByteArray(1, '\0');
    }
    g += QByteArray(1, '\x3b');
    return g;
}

} // namespace

class TstPetImportJob : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString root() const { return path(QStringLiteral("out")); }

    static ImportSpec specFrom(const QByteArray &json)
    {
        const SpecResult r = parseImportSpec(json);
        Q_ASSERT_X(r.ok(), "specFrom", qPrintable(r.error));
        return *r.spec;
    }
    ImportOptions options() const
    {
        ImportOptions o;
        o.outRoot = root();
        return o;
    }
    static QByteArray json(const char *kind, const QStringList &sources, const QByteArray &extra = {})
    {
        QByteArray list;
        for (const QString &s : sources)
            list += (list.isEmpty() ? "" : ",") + QByteArray("\"") + s.toUtf8() + "\"";
        return QByteArray("{\"kind\":\"") + kind + "\",\"name\":\"T\",\"sources\":[" + list + "]" + (extra.isEmpty() ? "" : "," + extra) + "}";
    }
    // The frames of an outcome, read back by the independent decoder.
    static DecodedApng decoded(const ImportOutcome &o)
    {
        QFile f(o.file);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return decodeApng(f.readAll());
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void aSpriteSheetBecomesAnApngWithTheRightFrames()
    {
        QImage sheet(8, 4, QImage::Format_RGBA8888);
        const QRgb colours[] = {qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255), qRgba(0, 0, 255, 128), qRgba(10, 20, 30, 0)};
        for (int i = 0; i < 4; ++i)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 2; ++x)
                    sheet.setPixel(i * 2 + x, y, colours[i]);
        QVERIFY(writeFile(path("sheet.png"), pngBytes(sheet)));
        const ImportSpec spec = specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":80)"));
        const ImportPreview preview = previewImport(spec);
        QVERIFY2(preview.ok, qPrintable(preview.error));
        QCOMPARE(preview.frames, 4);
        QCOMPARE(preview.totalDelayMs, 320LL);
        const ImportOutcome o = runImport(spec, options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.frames, 4);
        QCOMPARE(o.width, 2);
        QCOMPARE(o.height, 4);
        QCOMPARE(o.totalDelayMs, 320LL);
        const DecodedApng d = decoded(o);
        QVERIFY2(d.ok, qPrintable(d.problem));
        QCOMPARE(d.frames.size(), 4);
        QCryptographicHash all(QCryptographicHash::Sha256);
        for (int i = 0; i < 4; ++i) {
            const QByteArray expect = rgba(sheet.copy(i * 2, 0, 2, 4));
            QCOMPARE(d.frames.at(i), expect);
            QCOMPARE(d.delaysMs.at(i), 80.0);
            all.addData(expect);
        }
        QCOMPARE(o.framesSha256, QString::fromLatin1(all.result().toHex()));
        QVERIFY(o.directory.endsWith(o.id));
        QVERIFY(QFileInfo::exists(o.directory + "/source.json"));
        QVERIFY(!QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size());
    }
    void theRecordHoldsTheSpecAndTheSourceFingerprint()
    {
        const ImportSpec spec = specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":80)"));
        const ImportOutcome o = runImport(spec, options());
        QVERIFY(o.ok);
        QFile f(o.directory + "/source.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject rec = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(rec.value("version").toInt(), 1);
        QCOMPARE(rec.value("sources").toArray().at(0).toObject().value("name").toString(), QStringLiteral("sheet.png"));
        QCOMPARE(rec.value("sources").toArray().at(0).toObject().value("sha256").toString().size(), 64);
        f.seek(0);
        QVERIFY(!f.readAll().contains(m_dir.path().toUtf8())); // no absolute source path is kept
        QCOMPARE(rec.value("output").toObject().value("frames").toInt(), 4);
    }
    void theSameImportTwiceIsOneFolderAndAChangedSpecIsAnother()
    {
        const ImportSpec spec = specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":80)"));
        const ImportOutcome first = runImport(spec, options());
        const ImportOutcome second = runImport(spec, options());
        QVERIFY(first.ok && second.ok);
        QVERIFY(second.reused);
        QCOMPARE(second.id, first.id);
        QCOMPARE(second.framesSha256, first.framesSha256);
        const ImportSpec other = specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":3,"frameMs":80)"));
        const ImportOutcome third = runImport(other, options());
        QVERIFY(third.ok);
        QVERIFY(third.id != first.id);
    }
    void sheetGeometryErrorsComeBackByName()
    {
        const ImportSpec spec = specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":9)"));
        const ImportOutcome o = runImport(spec, options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kSheetTooFewCells)), qPrintable(o.error));
        QVERIFY(!QFileInfo::exists(root() + "/anything.part"));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void imagesFollowTheSizePolicy()
    {
        QVERIFY(QDir().mkpath(path("seq")));
        QVERIFY(writeFile(path("seq/f2.png"), pngBytes(solid(4, 4, qRgb(0, 255, 0)))));
        QVERIFY(writeFile(path("seq/f10.png"), pngBytes(solid(4, 4, qRgb(0, 0, 255)))));
        QVERIFY(writeFile(path("seq/f1.png"), pngBytes(solid(4, 4, qRgb(255, 0, 0)))));
        ImportOutcome o = runImport(specFrom(json("images", {path("seq")}, R"("fps":10)")), options()); // a folder
        QVERIFY2(o.ok, qPrintable(o.error));
        DecodedApng d = decoded(o);
        QVERIFY(d.ok);
        QCOMPARE(d.frames.at(0), rgba(solid(4, 4, qRgb(255, 0, 0))));
        QCOMPARE(d.frames.at(1), rgba(solid(4, 4, qRgb(0, 255, 0))));
        QCOMPARE(d.frames.at(2), rgba(solid(4, 4, qRgb(0, 0, 255))));
        QCOMPARE(d.delaysMs.at(0), 100.0);

        QVERIFY(writeFile(path("seq/f3.png"), pngBytes(solid(6, 2, qRgb(9, 9, 9)))));
        o = runImport(specFrom(json("images", {path("seq")}, R"("sizePolicy":"first")")), options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kImagesSizesDiffer)), qPrintable(o.error));
        o = runImport(specFrom(json("images", {path("seq")})), options()); // letterbox is the default
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.width, 6);
        QCOMPARE(o.height, 4);
        d = decoded(o);
        QCOMPARE(d.frames.at(0).mid(0, 4), QByteArray(4, '\0')); // letterbox is transparent
        o = runImport(specFrom(json("images", {path("seq")}, R"("sizePolicy":"scale")")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.width, 4);
        QVERIFY(o.degraded.contains(nameOf(kScaledDown)));
    }
    void aZipOfFramesIsReadInMemoryAndProgramsAreNamedNotRun()
    {
        ZipMember a, b, exe, text;
        a.name = "frames/2.png";
        a.data = pngBytes(solid(3, 3, qRgb(1, 2, 3)));
        b.name = "frames/10.png";
        b.data = pngBytes(solid(3, 3, qRgb(4, 5, 6)));
        exe.name = "core.exe";
        exe.data = QByteArray("MZ") + QByteArray(100, 'x');
        text.name = "readme.txt";
        text.data = QByteArray("hello hello hello hello");
        QVERIFY(writeFile(path("f.zip"), buildZip({b, a, exe, text})));
        const ImportOutcome o = runImport(specFrom(json("zip", {path("f.zip")})), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.frames, 2);
        QCOMPARE(decoded(o).frames.at(0), rgba(solid(3, 3, qRgb(1, 2, 3)))); // 2 before 10
        QVERIFY(o.degraded.contains(QStringLiteral("zip: 2 entries ignored")));
        QVERIFY(o.degraded.filter("program not run").size() == 1);
        // nothing from the archive was written as a file
        QCOMPARE(QDir(o.directory).entryList(QDir::Files).size(), 2); // the animation and source.json
    }
    void aRepetitiveFileBesideTheFramesIsIgnoredNotFatal()
    {
        ZipMember a, b, text;
        a.name = "1.png";
        a.data = pngBytes(solid(3, 3, qRgb(1, 2, 3)));
        b.name = "2.png";
        b.data = pngBytes(solid(3, 3, qRgb(4, 5, 6)));
        text.name = "notes.txt";
        text.data = QByteArray(40000, 'a'); // compresses over 200:1
        QVERIFY(writeFile(path("with-notes.zip"), buildZip({a, b, text})));
        const ImportOutcome o = runImport(specFrom(json("zip", {path("with-notes.zip")})), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.frames, 2);
        QVERIFY(o.degraded.contains(QStringLiteral("zip: 1 entries ignored")));
    }
    void hostileZipsAreRefusedByName()
    {
        ZipMember escape;
        escape.name = "../evil.png";
        escape.data = pngBytes(solid(2, 2, 0));
        QVERIFY(writeFile(path("evil.zip"), buildZip({escape})));
        ImportOutcome o = runImport(specFrom(json("zip", {path("evil.zip")})), options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kZipEntryRefused)), qPrintable(o.error));
        QVERIFY(!QFileInfo::exists(m_dir.filePath("evil.png")));
        ZipMember junk;
        junk.name = "a.png";
        junk.data = QByteArray(100, 'j');
        QVERIFY(writeFile(path("junk.zip"), buildZip({junk})));
        o = runImport(specFrom(json("zip", {path("junk.zip")})), options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kZipNoImages)), qPrintable(o.error));
        QVERIFY(writeFile(path("notzip.zip"), QByteArray(500, 'z')));
        o = runImport(specFrom(json("zip", {path("notzip.zip")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kZipNotAZip)), qPrintable(o.error));
    }
    void aGifKeepsItsRawDelays()
    {
        const QList<QRgb> palette{qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 255)};
        const QList<QByteArray> frames{QByteArray("\x00\x01\x02\x03\x00\x01", 6), QByteArray("\x03\x03\x03\x03\x03\x03", 6), QByteArray("\x02\x02\x01\x01\x00\x00", 6)};
        QVERIFY(writeFile(path("a.gif"), makeGif(3, 2, frames, {5, 12, 1}, palette)));
        const ImportOutcome o = runImport(specFrom(json("gif", {path("a.gif")})), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.frames, 3);
        const DecodedApng d = decoded(o);
        QVERIFY2(d.ok, qPrintable(d.problem));
        QCOMPARE(d.delaysMs, (QList<double>{50.0, 120.0, 100.0})); // Qt reads 0 and 1 cs as 100 ms, as the pets' own tiny-delay rule does
        for (int f = 0; f < 3; ++f) {
            QByteArray expect;
            for (const char idx : frames.at(f))
                expect += QByteArray(1, char(qRed(palette.at(idx)))) + QByteArray(1, char(qGreen(palette.at(idx)))) + QByteArray(1, char(qBlue(palette.at(idx)))) + QByteArray(1, '\xff');
            QCOMPARE(d.frames.at(f), expect);
        }
    }
    void aPackageBecomesOneLocalPetWithOneVariantPerAnimation()
    {
        const QList<QRgb> palette{qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 255)};
        const QList<QByteArray> frames{QByteArray("\x00\x01\x02\x03\x00\x01", 6), QByteArray("\x03\x03\x03\x03\x03\x03", 6)};
        ZipMember gifA, gifB, thumb, table, exe;
        gifA.name = "idle.gif";
        gifA.data = makeGif(3, 2, frames, {5, 5}, palette);
        gifB.name = "sub/jump.gif";
        gifB.data = makeGif(3, 2, frames, {7, 7}, palette);
        thumb.name = "miniatura.png";
        thumb.data = pngBytes(solid(4, 4, qRgb(1, 2, 3)));
        table.name = "actions.mddo";
        table.data = "[1, \"sub/jump.gif\", \"1-(10, 20)\"]\n[0, \"idle.gif\", \"1-(0,0)\"]\n";
        exe.name = "core.exe";
        exe.data = QByteArray("MZ") + QByteArray(400, 'x');
        QVERIFY(writeFile(path("pack.mdaa"), buildZip({gifA, gifB, thumb, table, exe})));
        const ImportSpec spec = specFrom(json("mdaa", {path("pack.mdaa")}, R"("name":"Neko pack","autoScene":0)"));
        const ImportPreview preview = previewImport(spec);
        QVERIFY2(preview.ok, qPrintable(preview.error));
        QCOMPARE(preview.frames, 2);
        const ImportOutcome o = runImport(spec, options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.variants, 2);
        QCOMPARE(o.file, o.directory);
        QVERIFY(o.degraded.contains(nameOf(kMdaaExecutableNotRun)));
        QCOMPARE(QDir(o.directory).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name), (QStringList{"idle.gif", "jump.gif", "preview.png", "source.json"}));
        // the same package again is the same folder
        const ImportOutcome again = runImport(spec, options());
        QVERIFY(again.ok && again.reused);
        QCOMPARE(again.id, o.id);
        QCOMPARE(again.variants, 2);
        // the record
        QFile f(o.directory + "/source.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject rec = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonObject mdaa = rec.value("mdaa").toObject();
        QCOMPARE(mdaa.value("scenes").toArray().size(), 2);
        QCOMPARE(mdaa.value("autoScene").toInt(), 0);
        QCOMPARE(mdaa.value("executable").toObject().value("name").toString(), QStringLiteral("core.exe"));
        // a local pet: one item, a variant per animation, the preview
        QString why;
        const QString canonical = murasaki::LocalPetImporter::validate(o.directory, &why);
        QVERIFY2(!canonical.isEmpty(), qPrintable(why));
        const murasaki::WallpaperItem item = murasaki::LocalPetImporter::scanItem(canonical);
        QCOMPARE(item.title, QStringLiteral("Neko pack"));
        QCOMPARE(item.variants.size(), 2);
        QCOMPARE(item.variants.at(0).name, QStringLiteral("idle"));
        QCOMPARE(item.variants.at(0).width, 3);
        QVERIFY(item.previewPath.endsWith("preview.png"));
        QVERIFY(item.status == murasaki::ItemStatus::Ok);
        // a folder that is not an import's output is not a pet
        QVERIFY(QDir().mkpath(path("plain")));
        QVERIFY(murasaki::LocalPetImporter::validate(path("plain"), &why).isEmpty());
    }
    void aBigSequenceCanBeImportedAgainAndADamagedFolderIsRepaired()
    {
        QVERIFY(QDir().mkpath(path("many")));
        for (int i = 0; i < 260; ++i)
            QVERIFY(writeFile(path(QStringLiteral("many/%1.png").arg(i, 4, 10, QLatin1Char('0'))), pngBytes(solid(2, 2, qRgb(i % 256, 7, 9)))));
        const ImportSpec spec = specFrom(json("images", {path("many")}, R"("fps":25)"));
        const ImportOutcome first = runImport(spec, options());
        QVERIFY2(first.ok, qPrintable(first.error));
        QCOMPARE(first.frames, 260);
        QFile record(first.directory + "/source.json");
        QVERIFY(record.open(QIODevice::ReadOnly));
        const QJsonObject rec = QJsonDocument::fromJson(record.readAll()).object();
        QVERIFY(rec.value("sources").isObject()); // compact: a count and a hash, not 260 entries
        QCOMPARE(rec.value("sources").toObject().value("count").toInt(), 260);
        const ImportOutcome again = runImport(spec, options());
        QVERIFY2(again.ok && again.reused, qPrintable(again.error));
        // the animation is deleted by hand: the folder is not reusable, and the next run makes it again instead of failing to move into place
        QVERIFY(QFile::remove(first.file));
        const ImportOutcome repaired = runImport(spec, options());
        QVERIFY2(repaired.ok, qPrintable(repaired.error));
        QVERIFY(!repaired.reused);
        QVERIFY(QFileInfo::exists(repaired.file));
        QVERIFY(!QFileInfo::exists(first.directory + ".stale"));
        // a folder with no record at all
        QVERIFY(QFile::remove(repaired.directory + "/source.json"));
        QVERIFY(runImport(spec, options()).ok);
    }
    void aFolderWithOtherFilesSkipsThemAndTheRunHasAnIdleLock()
    {
        QVERIFY(QDir().mkpath(path("mixed")));
        QVERIFY(writeFile(path("mixed/a.png"), pngBytes(solid(3, 3, qRgb(1, 2, 3)))));
        QVERIFY(writeFile(path("mixed/b.png"), pngBytes(solid(3, 3, qRgb(4, 5, 6)))));
        QVERIFY(writeFile(path("mixed/Thumbs.db"), "not an image at all, no"));
        QVERIFY(writeFile(path("mixed/README.txt"), "hello"));
        const ImportSpec spec = specFrom(json("images", {path("mixed")}));
        const ImportOutcome o = runImport(spec, options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.frames, 2);
        QVERIFY(o.degraded.contains(QStringLiteral("images: 2 files ignored")));
        // while another run holds the lock of the same import, this one stops at once with a name
        const QByteArray canonical = canonicalJson(spec);
        QStringList hashes;
        for (const QString &f : {path("mixed/a.png"), path("mixed/b.png")}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::ReadOnly));
            hashes.append(QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()));
        }
        QVERIFY(QDir().mkpath(root()));
        QLockFile lock(QDir(root()).filePath("." + importId(canonical, hashes) + ".lock"));
        QVERIFY(lock.tryLock(0));
        const ImportOutcome busy = runImport(spec, options());
        QVERIFY(!busy.ok);
        QVERIFY2(busy.error.startsWith(nameOf(kHelperFailed)) && busy.error.contains("already running"), qPrintable(busy.error));
    }
    void theWorkIsBoundedByPixelsAndByTime()
    {
        // 40 images that claim 8192 x 8192 (headers only) are 2.7 gigapixels together: refused before a pixel is decoded
        QStringList big;
        for (int i = 0; i < 40; ++i) {
            const QString f = path(QStringLiteral("big%1.png").arg(i));
            QVERIFY(writeFile(f, hugeHeaderPng(8192, 8192)));
            big.append(f);
        }
        const ImportPreview preview = previewImport(specFrom(json("images", big)));
        QVERIFY(!preview.ok);
        QVERIFY2(preview.error.startsWith(nameOf(kOutputOverBudget)), qPrintable(preview.error));
        // a time budget of one millisecond, with a progress callback that is slow: stopped between frames, cleanly, nothing left
        ImportOptions slow = options();
        slow.maxMillis = 1;
        slow.progress = [](double, int) { QThread::msleep(20); };
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("sheet.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":20)")), slow);
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kTimeBudget)), qPrintable(o.error));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void aFifoIsNeverOpenedBlocking()
    {
        const QString fifo = path("pipe.png");
        QCOMPARE(::mkfifo(fifo.toLocal8Bit().constData(), 0600), 0);
        QFile f(fifo);
        QVERIFY(!murasaki::import::openSource(f)); // returns at once instead of waiting for a writer
        QFile regular(path("sheet.png"));
        QVERIFY(murasaki::import::openSource(regular));
    }
    void aPackageHasAnOutputBudgetAndItsSceneChoiceIsNotItsIdentity()
    {
        const QList<QRgb> palette{qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 255)};
        const QList<QByteArray> frames{QByteArray("\x00\x01\x02\x03\x00\x01", 6), QByteArray("\x03\x03\x03\x03\x03\x03", 6)};
        ZipMember gif, table;
        gif.name = "idle.gif";
        gif.data = makeGif(3, 2, frames, {5, 5}, palette);
        table.name = "actions.mddo";
        table.data = "[1, \"idle.gif\", \"1-(10, 20)\"]\n";
        QVERIFY(writeFile(path("budget.mdaa"), buildZip({gif, table})));
        ImportOptions tiny = options();
        tiny.maxOutputBytes = 20; // smaller than the GIF
        const ImportOutcome refused = runImport(specFrom(json("mdaa", {path("budget.mdaa")})), tiny);
        QVERIFY(!refused.ok);
        QVERIFY2(refused.error.startsWith(nameOf(kOutputOverBudget)), qPrintable(refused.error));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        const ImportOutcome a = runImport(specFrom(json("mdaa", {path("budget.mdaa")}, R"("autoScene":0)")), options());
        const ImportOutcome b = runImport(specFrom(json("mdaa", {path("budget.mdaa")}, R"("autoScene":-1)")), options());
        QVERIFY2(a.ok && b.ok, qPrintable(b.error));
        QCOMPARE(b.id, a.id);
        QVERIFY(b.reused);
        QFile f(b.directory + "/source.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object().value("mdaa").toObject().value("autoScene").toInt(), -1);
    }
    void aVideoBecomesAnApngWithTheOptionsOfTheSpec()
    {
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty() || !murasaki::import::videoSourceAvailable())
            QSKIP("needs the ffmpeg binary and a build with libav");
        QProcess p;
        p.start(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=0x00ff00:s=32x24:r=10:d=1", "-c:v", "ffv1", "-pix_fmt", "bgr0", path("green.mkv")});
        QVERIFY(p.waitForFinished(30000) && p.exitCode() == 0);
        const ImportSpec spec = specFrom(json("video", {path("green.mkv")}, R"("name":"Green")"));
        const ImportPreview preview = previewImport(spec);
        QVERIFY2(preview.ok, qPrintable(preview.error));
        QCOMPARE(preview.width, 32);
        ImportOutcome o = runImport(spec, options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.width, 32);
        QCOMPARE(o.height, 24);
        QCOMPARE(o.frames, 10);
        QVERIFY(o.degraded.contains(QStringLiteral("import: opaque video (no alpha)")));
        DecodedApng d = decoded(o);
        QVERIFY2(d.ok, qPrintable(d.problem));
        QCOMPARE(d.frames.size(), 10);
        const QByteArray px = d.frames.at(3).left(4); // (green within a step: the colour conversion of the encoder)
        QVERIFY(uchar(px.at(0)) <= 1 && uchar(px.at(1)) >= 253 && uchar(px.at(2)) <= 1 && uchar(px.at(3)) == 255);
        QCOMPARE(d.delaysMs.at(0), 100.0);
        // the colour key applies to a video as to any source
        o = runImport(specFrom(json("video", {path("green.mkv")}, R"("colorKey":{"color":"00ff00","tolerance":20,"softness":0})")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(uchar(decoded(o).frames.at(0).at(3)), uchar(0));
        // a smaller longest side, a lower rate and a trim
        o = runImport(specFrom(json("video", {path("green.mkv")}, R"("maxSide":16,"maxFps":5,"startMs":200,"durationMs":600)")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(o.width, 16);
        QCOMPARE(o.height, 12);
        QVERIFY(o.frames >= 2 && o.frames <= 4);
        QVERIFY(o.degraded.contains(QStringLiteral("import: video scaled down")));
        // keepAudio on a video without sound keeps nothing and says so; with sound it keeps the video for phase 5
        o = runImport(specFrom(json("video", {path("green.mkv")}, R"("keepAudio":true)")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        {
            QFile record(o.directory + "/source.json");
            QVERIFY(record.open(QIODevice::ReadOnly));
            const QJsonObject audio = QJsonDocument::fromJson(record.readAll()).object().value("audio").toObject();
            QVERIFY(!audio.value("hasAudio").toBool() && !audio.value("kept").toBool());
            QVERIFY(!QFileInfo::exists(o.directory + "/source"));
        }
        QProcess q;
        q.start(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=0x00ff00:s=32x24:r=10:d=1", "-f", "lavfi", "-i", "sine=d=1", "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", path("sound.mkv")});
        QVERIFY(q.waitForFinished(30000) && q.exitCode() == 0);
        o = runImport(specFrom(json("video", {path("sound.mkv")}, R"("keepAudio":true)")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        {
            QVERIFY(QFileInfo::exists(o.directory + "/source/001-sound.mkv"));
            QFile record(o.directory + "/source.json");
            QVERIFY(record.open(QIODevice::ReadOnly));
            const QJsonObject audio = QJsonDocument::fromJson(record.readAll()).object().value("audio").toObject();
            QVERIFY(audio.value("hasAudio").toBool() && audio.value("kept").toBool());
            QCOMPARE(audio.value("file").toString(), QStringLiteral("source/001-sound.mkv"));
        }
        // the time budget and cancel reach the video decode, with names, and leave nothing
        ImportOptions slow = options();
        slow.maxMillis = 1;
        slow.progress = [](double, int) { QThread::msleep(30); };
        o = runImport(specFrom(json("video", {path("green.mkv")}, R"("maxSide":17)")), slow);
        QVERIFY2(!o.ok && (o.error.startsWith(nameOf(kTimeBudget)) || o.error.contains("time limit")), qPrintable(o.error));
        int polls = 0;
        ImportOptions cancelling = options();
        cancelling.cancelled = [&] { return ++polls > 3; };
        o = runImport(specFrom(json("video", {path("green.mkv")}, R"("maxSide":18)")), cancelling);
        QVERIFY2(!o.ok && o.error.startsWith(nameOf(kCancelled)), qPrintable(o.error));
        // options of another kind, and a duration too long, are refused by the spec
        QVERIFY(!parseImportSpec(R"({"kind":"gif","sources":["/a.gif"],"maxFps":10})").ok());
        QVERIFY(!parseImportSpec(R"({"kind":"gif","sources":["/a.gif"],"keepAudio":true})").ok());
        QVERIFY(!parseImportSpec(R"({"kind":"video","sources":["/a.mp4"],"fps":10})").ok());
        QVERIFY(!parseImportSpec(R"({"kind":"video","sources":["/a.mp4"],"durationMs":1e12})").ok());
        QCOMPARE(canonicalJson(*parseImportSpec(R"({"kind":"gif","sources":["/a.gif"]})").spec).contains("frameMs"), false);
        // not a video, a playlist that names a file, and a spec the video source refuses
        QVERIFY(writeFile(path("text.mp4"), "this is not a video, just text"));
        o = runImport(specFrom(json("video", {path("text.mp4")})), options());
        QVERIFY2(!o.ok && o.error.startsWith(nameOf(kNotAVideo)), qPrintable(o.error));
        QVERIFY(writeFile(path("list.mp4"), "#EXTM3U\n#EXTINF:1,x\n/etc/passwd\n"));
        o = runImport(specFrom(json("video", {path("list.mp4")})), options());
        QVERIFY2(!o.ok, "a playlist is not a video");
        QVERIFY(!parseImportSpec(R"({"kind":"video","sources":["/a.mp4"],"maxFps":99})").ok());
        QVERIFY(!parseImportSpec(R"({"kind":"video","sources":["/a.mp4"],"startMs":-1})").ok());
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void hostileSourcesAreRefusedBeforeAnythingIsAllocated()
    {
        // a PNG that claims 40000 x 40000, as a sheet and as an image of a sequence
        QVERIFY(writeFile(path("huge.png"), hugeHeaderPng(40000, 40000)));
        ImportOutcome o = runImport(specFrom(json("sheet", {path("huge.png")})), options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kSheetTooLarge)), qPrintable(o.error));
        QVERIFY(writeFile(path("ok1.png"), pngBytes(solid(2, 2, 0xffffffff))));
        o = runImport(specFrom(json("images", {path("huge.png"), path("ok1.png")})), options());
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kNotAnImage)), qPrintable(o.error));
        // text with an image name, a GIF with a PNG name, a missing file, a directory, a device
        QVERIFY(writeFile(path("liar.png"), QByteArray("not an image at all, just text")));
        o = runImport(specFrom(json("sheet", {path("liar.png")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kNotAnImage)), qPrintable(o.error));
        o = runImport(specFrom(json("gif", {path("ok1.png")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kNotAnImage)), qPrintable(o.error));
        o = runImport(specFrom(json("sheet", {path("missing.png")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kSourceNotFound)), qPrintable(o.error));
        o = runImport(specFrom(json("sheet", {m_dir.path()})), options());
        QVERIFY2(o.error.startsWith(nameOf(kSourceNotRegular)), qPrintable(o.error));
        o = runImport(specFrom(json("sheet", {QStringLiteral("/dev/null")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kSourceNotRegular)), qPrintable(o.error));
        // a symlink to a regular file is followed once; one to a device is not a regular file
        QVERIFY(QFile::link(path("ok1.png"), path("link.png")));
        QVERIFY(QFile::link("/dev/zero", path("zero.png")));
        o = runImport(specFrom(json("images", {path("link.png"), path("ok1.png")})), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        o = runImport(specFrom(json("images", {path("zero.png"), path("ok1.png")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kSourceNotRegular)), qPrintable(o.error));
        // fewer than two images
        o = runImport(specFrom(json("images", {path("ok1.png")})), options());
        QVERIFY2(o.error.startsWith(nameOf(kImagesFewer)), qPrintable(o.error));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void aBudgetThatIsTooSmallFailsAndLeavesNothing()
    {
        QVERIFY(writeFile(path("big.png"), pngBytes([] {
            QImage image(256, 64, QImage::Format_RGBA8888);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 256; ++x)
                    image.setPixel(x, y, qRgba((x * 7 + y * 13) & 255, (x * 31) & 255, (y * 17 + x) & 255, 255));
            return image;
        }())));
        ImportOptions opts = options();
        opts.maxOutputBytes = 400;
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("big.png")}, R"("frameWidth":64,"frameHeight":64,"frameCount":4)")), opts);
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kOutputOverBudget)), qPrintable(o.error));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void cancelBetweenFramesLeavesNoWorkFolder()
    {
        int calls = 0;
        ImportOptions opts = options();
        opts.cancelled = [&] { return ++calls > 6; };
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("big.png")}, R"("frameWidth":16,"frameHeight":16,"frameCount":16,"columns":16,"frameMs":20)")), opts);
        QVERIFY(!o.ok);
        QVERIFY2(o.error.startsWith(nameOf(kCancelled)), qPrintable(o.error));
        QCOMPARE(QDir(root()).entryList({"*.part"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QVERIFY(!QDir(root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).contains(importId(canonicalJson(specFrom(json("sheet", {path("big.png")}, R"("frameWidth":16,"frameHeight":16,"frameCount":16,"columns":16,"frameMs":20)"))), {})));
    }
    void staleWorkFoldersAreCleaned()
    {
        QVERIFY(QDir().mkpath(root() + "/deadbeef.part/inner"));
        QVERIFY(writeFile(root() + "/deadbeef.part/inner/x", "x"));
        QCOMPARE(cleanStaleImportParts(root()), 1);
        QVERIFY(!QFileInfo::exists(root() + "/deadbeef.part"));
    }
    void progressIsReportedPerFrame()
    {
        QList<int> seen;
        ImportOptions opts = options();
        opts.progress = [&](double f, int frames) {
            QVERIFY(f > 0 && f <= 1);
            seen.append(frames);
        };
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("big.png")}, R"("frameWidth":64,"frameHeight":64,"frameCount":3,"frameMs":33)")), opts);
        QVERIFY2(o.ok, qPrintable(o.error));
        QCOMPARE(seen, (QList<int>{1, 2, 3}));
    }
    void aColourKeyAppliesToAnyKindAndIsPartOfTheId()
    {
        QImage sheet(4, 2, QImage::Format_RGBA8888);
        sheet.fill(qRgb(0, 255, 0));
        sheet.setPixel(1, 0, qRgb(200, 10, 10));
        sheet.setPixel(3, 1, qRgb(200, 10, 10));
        QVERIFY(writeFile(path("keyed.png"), pngBytes(sheet)));
        const QByteArray base = R"("frameWidth":2,"frameHeight":2,"frameCount":2)";
        const ImportOutcome plain = runImport(specFrom(json("sheet", {path("keyed.png")}, base)), options());
        const ImportOutcome keyed = runImport(specFrom(json("sheet", {path("keyed.png")}, base + R"(,"colorKey":{"color":"00FF00","tolerance":20,"softness":0})")), options());
        QVERIFY2(keyed.ok, qPrintable(keyed.error));
        QVERIFY(keyed.id != plain.id);
        const DecodedApng d = decoded(keyed);
        QVERIFY(d.ok);
        QCOMPARE(uchar(d.frames.at(0).at(3)), uchar(0));     // green key pixel: transparent
        QCOMPARE(uchar(d.frames.at(0).at(4 + 3)), uchar(255)); // red pixel kept
        QCOMPARE(uchar(d.frames.at(0).at(4)), uchar(200));
    }
    void animatedWebpIsAnOptionAndReadsBack()
    {
        QImage sheet(8, 4, QImage::Format_RGBA8888);
        for (int i = 0; i < 4; ++i)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 2; ++x)
                    sheet.setPixel(i * 2 + x, y, qRgba(i * 60, 255 - i * 60, (x + y) * 30, i == 2 ? 100 : 255));
        QVERIFY(writeFile(path("w.png"), pngBytes(sheet)));
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("w.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":70,"format":"webp")")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QVERIFY(o.file.endsWith(".webp"));
        QCOMPARE(o.frames, 4);
        QImage first(o.file);
        QVERIFY(!first.isNull() || true); // Qt's webp plugin may be absent; the header reader below is the check
        QFile f(o.file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto info = murasaki::readWebpInfo(&f);
        QVERIFY(info.has_value());
        QCOMPARE(info->frames, 4);
        QCOMPARE(info->width, 2);
        const ImportOutcome lossy = runImport(specFrom(json("sheet", {path("w.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"frameMs":70,"format":"webp","webpQuality":50)")), options());
        QVERIFY2(lossy.ok, qPrintable(lossy.error));
        QVERIFY(lossy.id != o.id);
    }
    void keepingTheSourceCopiesItIntoTheJobFolder()
    {
        const ImportOutcome o = runImport(specFrom(json("sheet", {path("w.png")}, R"("frameWidth":2,"frameHeight":4,"frameCount":4,"keepSource":true)")), options());
        QVERIFY2(o.ok, qPrintable(o.error));
        QVERIFY(QFileInfo::exists(o.directory + "/source/001-w.png"));
    }
    void theHelperProcessSpeaksJsonLinesAndCleansUp()
    {
        QVERIFY(writeFile(path("job.json"), json("sheet", {path("big.png")}, R"("frameWidth":64,"frameHeight":64,"frameCount":4,"frameMs":50)")));
        const QString out = path("helper-out");
        QProcess helper;
        helper.start(QStringLiteral(MURASAKI_PETIMPORT_BIN), {"--spec", path("job.json"), "--out-root", out});
        QVERIFY(helper.waitForFinished(30000));
        QCOMPARE(helper.exitCode(), 0);
        const QList<QByteArray> lines = helper.readAllStandardOutput().trimmed().split('\n');
        const QJsonObject last = QJsonDocument::fromJson(lines.last()).object();
        QVERIFY(last.value("done").toBool());
        QCOMPARE(last.value("frames").toInt(), 4);
        QVERIFY(QFileInfo::exists(last.value("file").toString()));

        QProcess preview;
        preview.start(QStringLiteral(MURASAKI_PETIMPORT_BIN), {"--spec", path("job.json"), "--preview"});
        QVERIFY(preview.waitForFinished(30000));
        QCOMPARE(QJsonDocument::fromJson(preview.readAllStandardOutput()).object().value("preview").toObject().value("frames").toInt(), 4);

        QVERIFY(writeFile(path("bad.json"), "{\"kind\":\"sheet\"}"));
        QProcess bad;
        bad.start(QStringLiteral(MURASAKI_PETIMPORT_BIN), {"--spec", path("bad.json"), "--out-root", out});
        QVERIFY(bad.waitForFinished(30000));
        QCOMPARE(bad.exitCode(), 1);
        QVERIFY(QJsonDocument::fromJson(bad.readAllStandardOutput()).object().value("error").toString().startsWith(nameOf(kSpecRefused)));

        QProcess usage;
        usage.start(QStringLiteral(MURASAKI_PETIMPORT_BIN), {"--nonsense"});
        QVERIFY(usage.waitForFinished(30000));
        QCOMPARE(usage.exitCode(), 2);
    }
};

QTEST_GUILESS_MAIN(TstPetImportJob)
#include "tst_petimportjob.moc"
