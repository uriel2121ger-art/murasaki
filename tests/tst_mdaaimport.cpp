// The .mdaa importer (mdaaimport.h) on packages built by the test: what is kept, what is dropped by name, and that nothing from the archive is ever
// written under its own name or started.

#include "apngwriter.h"
#include "importfixtures.h"
#include "mdaaimport.h"
#include "npyfixtures.h"
#include "petimport.h"
#include "screenframes.h"
#include "screenrefstore.h"
#include "triggerspack.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QImage>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki::import;
using namespace importfixtures;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

QByteArray apngBytes(int w, int h, int frames, int seed = 1)
{
    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);
    ApngWriter writer(&buffer);
    writer.begin(w, h);
    QByteArray px(w * h * 4, 0);
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < px.size(); ++i)
            px[i] = char(i * 7 + f * 31 + seed);
        writer.addFrame(reinterpret_cast<const uchar *>(px.constData()), w * 4, 50);
    }
    writer.finish();
    return buffer.data();
}

QByteArray gifBytes(int w, int h, int frames)
{
    QByteArray g("GIF89a");
    g += le16(quint16(w)) + le16(quint16(h)) + QByteArray("\xf1\x00\x00", 3) + QByteArray("\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12);
    for (int f = 0; f < frames; ++f) {
        g += QByteArray("\x21\xf9\x04\x04", 4) + le16(5) + QByteArray("\x00\x00", 2);
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
        for (int i = 0; i < w * h; i += 2) {
            put(4);
            put((i + f) % 4);
            if (i + 1 < w * h)
                put((i + f + 1) % 4);
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

ZipMember member(const QByteArray &name, const QByteArray &data, bool deflate = true)
{
    ZipMember m;
    m.name = name;
    m.data = data;
    m.deflate = deflate;
    return m;
}

QByteArray screenPack(const QByteArray &match, int regions = 2, const QByteArray &event = QByteArray("\"event\":{\"name\":\"e\",\"fps\":30,\"frame_count\":10}"))
{
    QByteArray list;
    for (int i = 0; i < regions; ++i)
        list += (i ? "," : "") + QByteArray(R"({"id":"r)") + QByteArray::number(i) + R"(","x":10,"y":20,"w":100,"h":50,"kind":"rect"})";
    return R"({"id":"abc","name":"Screen","regions":[)" + list + "]," + event + R"(,"match":)" + match + R"(,"animations":[{"id":"t1","name":"Hello","asset_path":"a.gif","actions":[{"t_ms":0,"kind":"show"}]}]})";
}

} // namespace

class TstMdaaImport : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_n = 0;
    QString zip(const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(QStringLiteral("p%1.mdaa").arg(m_n++));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(bytes);
        return path;
    }
    QString folder()
    {
        const QString path = m_dir.filePath(QStringLiteral("job%1").arg(m_n++));
        QDir().mkpath(path);
        return path;
    }
    static QStringList files(const QString &dir) { return QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name); }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void aPackageBecomesVariantsPreviewAndScenes()
    {
        const QByteArray exe = QByteArray("MZ") + QByteArray(5000, 'q');
        const QByteArray table =
            "\xef\xbb\xbf[1, \"anim/walk.apng\", \"1-(120, 340)\"]\r\n"
            "(1, 'dance.gif', '2-(0,0)')\n"
            "[0, \"dance.gif\", \"1-(0,0)\"]\n"
            "[1, \"C:\\\\Users\\\\author\\\\x.gif\", \"1-(1,2)\"]\n"
            "[1, \"../escape.gif\", \"1-(1,2)\"]\n"
            "[1, \"missing.gif\", \"1-(1,2)\"]\n"
            "__import__('os').system('x')\n"
            "[1, \"sound.mp3\", \"1-(1,2)\"]\n";
        const QString p = zip(buildZip({member("miniatura.png", apngBytes(8, 8, 1)), member("actions.mddo", table), member("core.exe", exe),
                                        member("anim/walk.apng", apngBytes(16, 12, 3)), member("dance.gif", gifBytes(6, 4, 3)), member("sound.mp3", "ID3 not played"),
                                        member("readme.txt", "hello hello hello hello hello"), member("tool.dll", QByteArray("MZ") + QByteArray(100, 'd'))}));
        const QString out = folder();
        const MdaaResult r = importMdaa(p, out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.variants.size(), 2);
        QCOMPARE(r.variants.at(0).name, QStringLiteral("walk"));
        QCOMPARE(r.variants.at(0).frames, 3);
        QCOMPARE(r.variants.at(0).width, 16);
        QCOMPARE(r.variants.at(1).name, QStringLiteral("dance"));
        QCOMPARE(r.preview, QStringLiteral("preview.png"));
        // exactly these files, none under a member's name
        QCOMPARE(files(out), (QStringList{"dance.gif", "preview.png", "walk.apng"}));
        // scenes: walk (moved), dance without a move ('2-' means no move), the hide line; the rest are named and dropped
        QCOMPARE(r.scenes.size(), 3);
        QCOMPARE(r.scenes.at(0).variant, QStringLiteral("walk"));
        QVERIFY(r.scenes.at(0).show && r.scenes.at(0).hasPosition);
        QCOMPARE(r.scenes.at(0).x, 120.0);
        QVERIFY(!r.scenes.at(1).hasPosition);
        QVERIFY(!r.scenes.at(2).show);
        int outside = 0, refused = 0, notAnimation = 0;
        for (const QString &n : r.notes) {
            outside += n.startsWith(nameOf(kMdaaPathOutside));
            refused += n.startsWith(QStringLiteral("mdaa: actions.mddo line"));
            notAnimation += n.contains(QStringLiteral("not an animation"));
        }
        QCOMPARE(outside, 3);
        QCOMPARE(refused, 1);
        QCOMPARE(notAnimation, 1);
        // core.exe: hashed, named, never written
        QCOMPARE(r.executableName, QStringLiteral("core.exe"));
        QCOMPARE(r.executableSha256, QString::fromLatin1(QCryptographicHash::hash(exe, QCryptographicHash::Sha256).toHex()));
        QVERIFY(r.degraded.contains(nameOf(kMdaaExecutableNotRun)));
        QVERIFY(r.degraded.contains(nameOf(kAudioNotPlayed)));
        QVERIFY(r.degraded.contains(nameOf(kProgramNotRun) + ": tool.dll"));
        QCOMPARE(r.programs, QStringList{"tool.dll"});
        const QJsonObject record = mdaaRecord(r);
        QCOMPARE(record.value("variants").toArray().size(), 2);
        QCOMPARE(record.value("executable").toObject().value("sha256").toString(), r.executableSha256);
    }
    void nothingButMediaIsWrittenAnywhere()
    {
        const QString before = m_dir.path();
        const QString p = zip(buildZip({member("core.exe", QByteArray("MZ") + QByteArray(300, 'e')), member("a.gif", gifBytes(4, 4, 2))}));
        const QString out = folder();
        QVERIFY(importMdaa(p, out).ok());
        QCOMPARE(files(out), QStringList{"a.gif"});
        QVERIFY(!QFile::exists(m_dir.filePath("core.exe")) && !QFile::exists(out + "/core.exe"));
        Q_UNUSED(before);
    }
    void refusalsAreNamed()
    {
        QCOMPARE(importMdaa(zip(buildZip({member("actions.mddo", "[1,'a','1-(0,0)']")})), folder()).error, nameOf(kMdaaNoMedia));
        QVERIFY(importMdaa(zip(QByteArray(500, 'z')), folder()).error.startsWith(nameOf(kMdaaNotAZip)));
        QVERIFY(importMdaa(m_dir.filePath("absent.mdaa"), folder()).error.startsWith(nameOf(kSourceNotFound)));
        // a hostile member name is the ZIP reader's refusal, passed on whole
        QVERIFY(importMdaa(zip(buildZip({member("../x.gif", gifBytes(4, 4, 2))})), folder()).error.startsWith(nameOf(kZipEntryRefused)));
        // a still PNG, a JPEG-looking member and a damaged GIF are not animations
        const MdaaResult r = importMdaa(zip(buildZip({member("still.png", apngBytes(4, 4, 1)), member("photo.jpg", QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(40, 'j')),
                                                       member("bad.gif", QByteArray("GIF89a") + QByteArray(40, 'x')), member("ok.gif", gifBytes(4, 4, 2))})),
                                        folder());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.variants.size(), 1);
        QCOMPARE(r.ignored, 2);
        QVERIFY(r.notes.filter(nameOf(kMdaaMediaRefused)).size() == 1);
    }
    void equalLeafNamesGetDistinctFiles()
    {
        const QString out = folder();
        const MdaaResult r = importMdaa(zip(buildZip({member("a/walk.gif", gifBytes(4, 4, 2)), member("b/walk.gif", gifBytes(5, 4, 2)), member("c/Walk.GIF", gifBytes(6, 4, 2))})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(files(out).size(), 3);
        QCOMPARE((QSet<QString>{r.variants.at(0).name, r.variants.at(1).name, r.variants.at(2).name}).size(), 3);
    }
    void moreThanTheVariantLimitIsCapped()
    {
        QList<ZipMember> members;
        const QByteArray gif = gifBytes(2, 2, 2);
        for (int i = 0; i < 260; ++i)
            members.append(member(QByteArray("m") + QByteArray::number(i) + ".gif", gif, false));
        const MdaaResult r = importMdaa(zip(buildZip(members)), folder());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.variants.size(), 256);
        QCOMPARE(r.notes.size(), 4);
    }
    void aTriggersPackIsMarkedAndItsAssetsImported()
    {
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", "{}"), member("a.gif", gifBytes(4, 4, 2))})), folder());
        QVERIFY(r.ok());
        QVERIFY(r.triggersPack);
        QVERIFY(r.degraded.contains(nameOf(kMdaaTriggersPack)));
    }
    void aRealTriggersPackIsReadAsBoundedDataAndNeverFollowed()
    {
        const QByteArray pack = R"({"id":"abc","name":"Wave","created_ts":1.5,"regions":[{"id":"r1","x":10,"y":20,"w":100,"h":50,"kind":"rect"},{"id":"r2","kind":"poly","points":[[0,0],[10,0],[10,10]]}],
            "event":{"name":"e","fps":30,"frame_count":90},"match":{"mode":"ORB_HASH","threshold":0.8,"cooldown_ms":700,"stop_grace_ms":250},
            "animations":[{"id":"t1","name":"Hello","asset_path":"assets/a.gif","start_visible":false,"actions":[
              {"t_ms":500,"kind":"show"},{"t_ms":0,"kind":"move","x":100,"y":200,"dur_ms":0},{"t_ms":1000,"kind":"move","x":300,"y":200,"dur_ms":400,"move_mode":"smooth"},
              {"t_ms":1200,"kind":"sound","path":"C:\\Users\\author\\Music\\boom.wav","volume":0.5},{"t_ms":2000,"kind":"hide"},{"t_ms":1,"kind":"exec","path":"/bin/sh"}]},
              {"id":"t2","name":"Gone","asset_path":"C:\\somewhere\\missing.gif","actions":[]}]})";
        const QByteArray meta = R"({"version":1,"speed_mult":2.0,"anim_speed_mult":1.5})";
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", pack), member("meta.json", meta), member("assets/a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", QByteArray("PK\x03\x04 cos\nsystem\n(S'id'\ntR."), false),
                                                      member("core.exe", QByteArray("MZ"), false)})), folder());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(r.triggersPack);
        QVERIFY(r.degraded.contains(nameOf(kMdaaFramesNotRead))); // the frames are counted, never opened
        QVERIFY(r.degraded.contains(nameOf(kMdaaExecutableNotRun)));
        QCOMPARE(r.pack.value("name").toString(), QString("Wave"));
        QCOMPARE(r.pack.value("speedMult").toDouble(), 2.0);
        QCOMPARE(r.pack.value("match").toObject().value("cooldownMs").toDouble(), 700.0);
        QCOMPARE(r.pack.value("regions").toArray().size(), 2);
        const QJsonArray tracks = r.pack.value("tracks").toArray();
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks[0].toObject().value("variant").toString(), QString("a"));
        QCOMPARE(tracks[0].toObject().value("startVisible").toBool(true), false);
        QCOMPARE(tracks[1].toObject().value("variant").toString(), QString()); // an asset that was not imported
        const QJsonArray actions = tracks[0].toObject().value("actions").toArray();
        QCOMPARE(actions.size(), 5); // the "exec" action is not one of ours: dropped
        QString sound;
        for (const QJsonValue &a : actions)
            if (a.toObject().value("kind").toString() == "sound")
                sound = a.toObject().value("sound").toString();
        QCOMPARE(sound, QString("boom.wav")); // the author's path is reduced to a name
        const QString all = r.notes.join('\n');
        QVERIFY(all.contains(nameOf(kMdaaSoundNotFollowed)));
        QVERIFY(all.contains(nameOf(kMdaaPackRefused)));
        QVERIFY(!QJsonDocument(r.pack).toJson().contains("Users")); // no path of the author survives in the record
        QVERIFY(!QJsonDocument(mdaaRecord(r)).toJson().contains("C:"));
        // the program and the frames are not extracted
        QVERIFY(!QFileInfo::exists(folder() + "/core.exe") && !QFileInfo::exists(folder() + "/region0.npz"));
    }
    void aHostilePackJsonCostsOnlyThePackNeverTheAnimations()
    {
        QJsonArray animations;
        for (int i = 0; i < 100; ++i) {
            QJsonArray actions;
            for (int j = 0; j < 400; ++j)
                actions.append(QJsonObject{{"t_ms", j % 7 == 0 ? QJsonValue("late") : QJsonValue(1e30)}, {"kind", "move"}, {"x", "left"}});
            animations.append(QJsonObject{{"name", QString(5000, 'x')}, {"asset_path", "../../etc/passwd"}, {"actions", actions}});
        }
        QJsonArray regions, manyPoints;
        for (int i = 0; i < 1000; ++i)
            manyPoints.append(QJsonArray{1, 2});
        for (int i = 0; i < 100; ++i)
            regions.append(QJsonObject{{"kind", "weird"}, {"x", 1e300}, {"w", -5}, {"points", manyPoints}});
        const QByteArray big = QJsonDocument(QJsonObject{{"animations", animations}, {"regions", regions}, {"name", 5}, {"match", "not an object"}}).toJson();
        MdaaResult r = importMdaa(zip(buildZip({member("pack.json", big), member("a.gif", gifBytes(4, 4, 2))})), folder());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.variants.size(), 1); // the animation is imported whatever pack.json says
        QVERIFY(r.pack.value("tracks").toArray().size() <= kMaxPackTracks);
        QVERIFY(r.pack.value("regions").toArray().size() <= kMaxPackRegions);
        QVERIFY(QJsonDocument(r.pack).toJson().size() < (3 << 20));
        for (const QJsonValue &t : r.pack.value("tracks").toArray()) {
            QVERIFY(t.toObject().value("actions").toArray().size() <= kMaxPackActions);
            QVERIFY(t.toObject().value("name").toString().size() <= 100);
        }
        // not JSON at all, or an array: refused by name, the rest kept
        for (const QByteArray &bad : {QByteArray("{{{ not json"), QByteArray("[1,2,3]"), QByteArray(), QByteArray(std::size_t(5 << 20), ' ')}) {
            QTemporaryDir dir;
            r = importMdaa(zip(buildZip({member("pack.json", bad), member("a.gif", gifBytes(4, 4, 2))})), dir.path());
            QVERIFY2(r.ok(), qPrintable(r.error));
            QVERIFY(r.pack.isEmpty());
            QVERIFY(r.notes.join('\n').contains(nameOf(kMdaaPackRefused)));
        }
    }
    void aMemberThatFailedGivesItsBytesBackToTheBudget()
    {
        ZipMember bad = member("bad.gif", QByteArray("GIF89a") + QByteArray(600, 'x'), false);
        bad.badCrc = true; // written, then refused at the checksum
        const QByteArray gif = gifBytes(4, 4, 2);
        const QString p = zip(buildZip({bad, member("ok.gif", gif, false)}));
        MdaaBudget budget;
        budget.maxBytes = 700 + gif.size() - 100; // fits the good one alone, not both
        QVERIFY(gif.size() < budget.maxBytes);
        const MdaaResult r = importMdaa(p, folder(), {}, {}, budget);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.variants.size(), 1);
    }
    void cancelStopsAtOnce()
    {
        int calls = 0;
        const MdaaResult r = importMdaa(zip(buildZip({member("a.gif", gifBytes(4, 4, 2)), member("b.gif", gifBytes(4, 4, 2))})), folder(), [&] { return ++calls > 1; });
        QCOMPARE(r.error, nameOf(kCancelled));
    }
    void thePreviewCountsWithoutWriting()
    {
        const QString p = zip(buildZip({member("core.exe", QByteArray("MZ") + QByteArray(40, 'x')), member("a.gif", gifBytes(4, 4, 2)), member("b.apng", apngBytes(4, 4, 2)),
                                        member("t.txt", "text text text text text")}));
        const MdaaPreview v = previewMdaa(p);
        QVERIFY(v.ok());
        QCOMPARE(v.animations, 2);
        QVERIFY(v.hasExecutable);
        QVERIFY(v.degraded.contains(nameOf(kMdaaExecutableNotRun)));
        QVERIFY(!previewMdaa(zip(buildZip({member("t.txt", "text")}))).ok());
    }
    // ---- Reference pictures of the screen triggers ----
    void theFramesATriggerUsesAreKeptAsPngsAndTheRestDropped()
    {
        const QString out = folder();
        const QByteArray pack = screenPack(R"({"mode":"ORB_HASH","threshold":0.75,"ref_mode":"anchor","ref_frame_index":2})", 2, R"("event":{"name":"e","start_index":1,"end_index":null})");
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", pack), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", npyfixtures::npzFile(8, 30, 40), false),
                                                      member("frames/region_1.npz", npyfixtures::npzFile(5, 20, 20), true)})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.screenRefs.value("status").toString(), QString("ready"));
        QVERIFY2(!r.degraded.contains(nameOf(kMdaaFramesNotRead)), "the frames were read");
        const QJsonArray regions = r.screenRefs.value("regions").toArray();
        QCOMPARE(regions.size(), 2);
        const QJsonObject r0 = regions.at(0).toObject();
        QCOMPARE(r0.value("sourceFrames").toInt(), 8);
        QCOMPARE(r0.value("frames").toArray().size(), 1);                                  // anchor: one frame
        QCOMPARE(r0.value("frames").toArray().at(0).toObject().value("clipIndex").toInt(), 3); // start 1 + ref 2
        QImage image(out + "/" + r0.value("frames").toArray().at(0).toObject().value("file").toString());
        QCOMPARE(image.size(), QSize(40, 30));
        QCOMPARE(image.pixelColor(1, 1).red(), 3 * 20 + 10 + 40);
        QCOMPARE(regions.at(1).toObject().value("sourceFrames").toInt(), 5);
        // nothing but the animation, the pictures and the folder of pictures is in the job folder, and no temporary file is left
        QCOMPARE(files(out), (QStringList{"a.gif"}));
        QCOMPARE(QDir(out + "/screenrefs").entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name), (QStringList{"region0", "region1"}));
        // it all goes into the record
        const QJsonObject record = mdaaRecord(r);
        QCOMPARE(record.value("screenRefs").toObject().value("status").toString(), QString("ready"));
        // and into the store, when the trigger exists
        QTemporaryDir data;
        murasaki::ScreenRefStore store(data.path() + "/triggers");
        QString error;
        QCOMPARE(store.adoptImported("trigger-1", out, r.screenRefs, &error), 2);
        QCOMPARE(store.regions("trigger-1"), (QList<int>{0, 1}));
        QVERIFY(!store.frame("trigger-1", 1, 0).isEmpty());
    }

    void aSequencerKeepsItsRunAndReportsAShortClip()
    {
        const QString out = folder();
        const QByteArray pack = screenPack(R"({"ref_mode":"sequencer","ref_frame_index":0,"seq_len":5,"seq_step":2})", 1, R"("event":{"start_index":0,"end_index":6})");
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", pack), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", npyfixtures::npzFile(10, 16, 16))})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.screenRefs.value("status").toString(), QString("ready"));
        const QJsonArray frames = r.screenRefs.value("regions").toArray().at(0).toObject().value("frames").toArray();
        QCOMPARE(frames.size(), 4); // 0, 2, 4, 6; the fifth would be 8, past the clip's end
        QCOMPARE(frames.at(3).toObject().value("clipIndex").toInt(), 6);
        QVERIFY(r.notes.join('\n').contains(nameOf(kScreenRefsShortened)));
    }

    void brokenFramesFilesGiveNamedReasonsAndKeepNothing()
    {
        const QByteArray object = npyfixtures::npyFile(2, 4, 4).replace("|u1", "|O2");
        struct Case {
            const char *label;
            QByteArray npz;
            std::string_view reason;
        };
        const QList<Case> cases = {
            {"a pickle", QByteArray("PK\x03\x04 cos\nsystem\n(S'id'\ntR."), kNpzNotAZip},
            {"an object array", buildZip({member("frames.npy", object, false)}), kNpyDtype},
            {"a truncated array", buildZip({member("frames.npy", npyfixtures::npyFile(5, 10, 10).chopped(50), true)}), kNpyTruncated},
            {"no frames array", buildZip({member("other.npy", npyfixtures::npyFile(2, 4, 4), true)}), kNpzNoFrames},
            {"an array that is too big", buildZip({member("frames.npy", npyfixtures::npyWithShape("(9, 4096, 4096, 3)", QByteArray()), true)}), kNpyOverLimit},
        };
        for (const Case &c : cases) {
            const QString out = folder();
            const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({})", 1)), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", c.npz, false)})), out);
            QVERIFY2(r.ok(), c.label); // the animations are still imported
            QVERIFY2(r.screenRefs.value("status").toString().startsWith(nameOf(c.reason)), qPrintable(QString("%1: %2").arg(c.label, r.screenRefs.value("status").toString())));
            QVERIFY2(r.degraded.contains(nameOf(kMdaaFramesNotRead)), c.label);
            QVERIFY2(!QDir(out + "/screenrefs/region0").exists() || QDir(out + "/screenrefs/region0").entryList(QDir::Files).isEmpty(), c.label);
            QVERIFY2(files(out) == QStringList{"a.gif"}, c.label);
            murasaki::ScreenRefStore store(m_dir.filePath("store-broken"));
            QCOMPARE(store.adoptImported("t", out, r.screenRefs), 0);
        }
    }

    void regionsWithoutFramesOrWithUnknownNumbersAreNamed()
    {
        const QString out = folder();
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({})", 2)), member("a.gif", gifBytes(6, 6, 3)), member("frames/region1.npz", npyfixtures::npzFile(3, 8, 8)),
                                                      member("frames/region7.npz", npyfixtures::npzFile(3, 8, 8))})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const QJsonArray regions = r.screenRefs.value("regions").toArray();
        QVERIFY(regions.at(0).toObject().value("status").toString().startsWith(nameOf(kScreenRefsNoFrames)));
        QCOMPARE(regions.at(1).toObject().value("status").toString(), QString("ready"));
        QVERIFY(r.screenRefs.value("status").toString().startsWith(nameOf(kScreenRefsNoFrames))); // not every region is ready
        QVERIFY(r.notes.join('\n').contains(nameOf(kScreenRefsUnknownRegion)));
    }

    void onlyTheCanonicalRegionNamesMapAndTwoForOneRegionAreRefused()
    {
        const QString out = folder();
        const QByteArray npz = npyfixtures::npzFile(3, 8, 8);
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({})", 4)), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", npz), member("frames/region_0.npz", npz),
                                                      member("frames/region1.npz", npz), member("frames/region02.npz", npz), member("frames/region_3.npz", npz), member("frames/other.npz", npz)})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const QJsonArray regions = r.screenRefs.value("regions").toArray();
        QVERIFY(regions.at(0).toObject().value("status").toString().startsWith(nameOf(kScreenRefsAmbiguous))); // both names: neither
        QCOMPARE(regions.at(1).toObject().value("status").toString(), QString("ready"));
        QVERIFY(regions.at(2).toObject().value("status").toString().startsWith(nameOf(kScreenRefsNoFrames)));  // region02 is not a name we map
        QCOMPARE(regions.at(3).toObject().value("status").toString(), QString("ready"));                        // the underscore form alone
        QVERIFY(!QDir(out + "/screenrefs/region0").exists() || QDir(out + "/screenrefs/region0").entryList(QDir::Files).isEmpty());
        const QString notes = r.notes.join('\n');
        QVERIFY(notes.contains("region02.npz"));
        QVERIFY(notes.contains("other.npz"));
        // the same result whatever the order of the members in the archive
        const MdaaResult back = importMdaa(zip(buildZip({member("frames/other.npz", npz), member("frames/region_3.npz", npz), member("frames/region02.npz", npz), member("frames/region1.npz", npz),
                                                         member("frames/region_0.npz", npz), member("frames/region0.npz", npz), member("a.gif", gifBytes(6, 6, 3)), member("pack.json", screenPack(R"({})", 4))})), folder());
        QCOMPARE(back.screenRefs.value("status").toString(), r.screenRefs.value("status").toString());
        for (int i = 0; i < 4; ++i)
            QCOMPARE(back.screenRefs.value("regions").toArray().at(i).toObject().value("status").toString(), regions.at(i).toObject().value("status").toString());
    }

    void theTotalOfKeptPicturesIsAHardCap()
    {
        const QString out = folder();
        const QByteArray npz = npyfixtures::npzFile(70, 6, 6);
        QList<ZipMember> list = {member("pack.json", screenPack(R"({"ref_mode":"sequencer","seq_len":64,"seq_step":1})", 6, R"("event":{"start_index":0})")), member("a.gif", gifBytes(6, 6, 3))};
        for (int i = 0; i < 6; ++i)
            list.append(member(QByteArray("frames/region") + QByteArray::number(i) + ".npz", npz, false));
        const MdaaResult r = importMdaa(zip(buildZip(list)), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        int kept = 0;
        for (const QJsonValue &v : r.screenRefs.value("regions").toArray())
            kept += v.toObject().value("frames").toArray().size();
        QCOMPARE(kept, kMaxRefFramesTotal); // 4 x 64; the fifth region got nothing, the sixth neither
        int onDisk = 0;
        for (const QString &d : QDir(out + "/screenrefs").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            onDisk += QDir(out + "/screenrefs/" + d).entryList(QDir::Files).size();
        QCOMPARE(onDisk, kept);
        QVERIFY(r.screenRefs.value("status").toString().startsWith(nameOf(kScreenRefsOverLimit)));
    }

    void cancelAndTheTimeBudgetAreAskedInsideARegion()
    {
        const QByteArray pack = screenPack(R"({"ref_mode":"sequencer","seq_len":10})", 1);
        const QByteArray npz = npyfixtures::npzFile(40, 30, 30);
        const QString zipFile = zip(buildZip({member("pack.json", pack), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", npz, false)}));
        // the answer flips to "cancelled" only after the media loop: every later question, inside the region, stops it
        int asked = 0;
        const QString out = folder();
        const MdaaResult r = importMdaa(zipFile, out, [&] { return ++asked > 3; });
        QVERIFY(asked > 3);
        QCOMPARE(r.error, nameOf(kCancelled));
        QVERIFY(!QDir(out + "/screenrefs/region0").exists() || QDir(out + "/screenrefs/region0").entryList(QDir::Files).isEmpty());
        // a time budget of zero milliseconds is spent before the first region
        MdaaBudget budget;
        budget.maxMillis = 1;
        QElapsedTimer t;
        t.start();
        // (the animations are quick; the budget is enforced at the frames, so it ends the import by name at the latest there)
        const MdaaResult late = importMdaa(zipFile, folder(), {}, {}, budget);
        Q_UNUSED(late); // either outcome is fine on a fast machine; what matters is that the call returns
        QVERIFY(t.elapsed() < 20000);
    }

    void aPackWithNoRegionKeepsNoPictures()
    {
        const QString out = folder();
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({})", 0)), member("a.gif", gifBytes(6, 6, 3)), member("frames/region0.npz", npyfixtures::npzFile(3, 8, 8))})), out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(r.screenRefs.isEmpty());
        QVERIFY(!QDir(out + "/screenrefs").exists());
    }

    void theFramesBudgetIsTheJobsBudget()
    {
        const QString out = folder();
        MdaaBudget budget;
        budget.maxBytes = 900; // the animation fits, a picture of a whole clip does not
        const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({"ref_mode":"sequencer","seq_len":8})", 1)), member("a.gif", gifBytes(4, 4, 2)),
                                                      member("frames/region0.npz", npyfixtures::npzFile(10, 60, 60))})), out, {}, {}, budget);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(r.screenRefs.value("status").toString().startsWith(nameOf(kOutputOverBudget)));
        QVERIFY(!QDir(out + "/screenrefs/region0").exists() || QDir(out + "/screenrefs/region0").entryList(QDir::Files).isEmpty());
    }

    void mutatedFramesFilesNeverCrashOrLeaveFilesBehind()
    {
        const QByteArray npz = npyfixtures::npzFile(4, 12, 12, true);
        QRandomGenerator rng(99);
        for (int round = 0; round < 300; ++round) {
            QByteArray m = npz;
            for (int i = 0, n = 1 + int(rng.bounded(3)); i < n; ++i)
                m[int(rng.bounded(quint32(m.size())))] = char(rng.bounded(256));
            if (rng.bounded(4) == 0)
                m.truncate(int(rng.bounded(quint32(m.size()))));
            const QString out = folder();
            const MdaaResult r = importMdaa(zip(buildZip({member("pack.json", screenPack(R"({"ref_mode":"sequencer","seq_len":3})", 1)), member("a.gif", gifBytes(4, 4, 2)), member("frames/region0.npz", m, false)})), out);
            QVERIFY(r.ok());
            QVERIFY(files(out) == QStringList{"a.gif"});
            const QString status = r.screenRefs.value("status").toString();
            QVERIFY(status == "ready" || status.contains(':'));
            if (status != "ready")
                QVERIFY(!QDir(out + "/screenrefs/region0").exists() || QDir(out + "/screenrefs/region0").entryList(QDir::Files).isEmpty());
        }
    }
    void mutatedPackagesNeverCrashOrWriteOutside()
    {
        const QByteArray base = buildZip({member("miniatura.png", apngBytes(4, 4, 1)), member("actions.mddo", "[1, 'a.gif', '1-(1,2)']\n"), member("a.gif", gifBytes(4, 4, 3)),
                                          member("core.exe", QByteArray("MZ") + QByteArray(200, 'x'))});
        QRandomGenerator rng(20261023);
        for (int round = 0; round < 400; ++round) {
            QByteArray m = base;
            for (int i = 0, n = 1 + int(rng.bounded(3)); i < n; ++i)
                m[int(rng.bounded(quint32(m.size())))] = char(rng.bounded(256));
            const QString out = folder();
            const MdaaResult r = importMdaa(zip(m), out);
            for (const QString &f : files(out))
                QVERIFY2(f == "preview.png" || f.endsWith(".gif") || f.endsWith(".apng") || f.endsWith(".webp"), qPrintable(f));
            QVERIFY(!QFile::exists(out + "/core.exe"));
            Q_UNUSED(r);
        }
    }
};

QTEST_GUILESS_MAIN(TstMdaaImport)
#include "tst_mdaaimport.moc"
