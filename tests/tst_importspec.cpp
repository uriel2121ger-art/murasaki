// The job description (importspec.h): strict parsing, defaults, canonical form, names and natural order.

#include "imagesequence.h"
#include "importspec.h"
#include "petimport.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTest>

using namespace murasaki::import;

class TstImportSpec : public QObject
{
    Q_OBJECT

private slots:
    void aSheetSpecKeepsAnimasDefaults()
    {
        const SpecResult r = parseImportSpec(R"({"kind":"sheet","sources":["/tmp/a.png"],"name":"Neko"})");
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.spec->sheet.frameWidth, 64);
        QCOMPARE(r.spec->sheet.frameCount, 4);
        QCOMPARE(r.spec->sheet.startFrame, 1);
        QCOMPARE(effectiveFrameMs(*r.spec), 100);
    }
    void imagesDefaultToTheUsersFps()
    {
        const SpecResult r = parseImportSpec(R"({"kind":"images","sources":["/a/1.png","/a/2.png"]})");
        QVERIFY(r.ok());
        QCOMPARE(effectiveFrameMs(*r.spec), 42); // 24 fps
        const SpecResult f = parseImportSpec(R"({"kind":"zip","sources":["/a.zip"],"fps":10})");
        QCOMPARE(effectiveFrameMs(*f.spec), 100);
    }
    void refusalsNameTheSpec()
    {
        const char *bad[] = {
            "", "[]", "{}", R"({"kind":"movie","sources":["/a"]})", R"({"kind":"sheet"})", R"({"kind":"sheet","sources":[]})",
            R"({"kind":"sheet","sources":["relative.png"]})", R"({"kind":"sheet","sources":["/a","/b"]})", R"({"kind":"sheet","sources":[5]})",
            R"({"kind":"sheet","sources":["/a"],"frameWidth":0})", R"({"kind":"sheet","sources":["/a"],"frameWidth":"64"})",
            R"({"kind":"sheet","sources":["/a"],"frameWidth":64.5})", R"({"kind":"sheet","sources":["/a"],"frameWidth":9000})",
            R"({"kind":"sheet","sources":["/a"],"frameCount":20001})", R"({"kind":"sheet","sources":["/a"],"paddingLeft":-1})",
            R"({"kind":"sheet","sources":["/a"],"frameMs":0})", R"({"kind":"sheet","sources":["/a"],"frameMs":1e9})",
            R"({"kind":"images","sources":["/a","/b"],"fps":0})", R"({"kind":"images","sources":["/a","/b"],"fps":500})",
            R"({"kind":"images","sources":["/a","/b"],"fps":10,"frameMs":50})", R"({"kind":"images","sources":["/a","/b"],"sizePolicy":"stretch"})",
            R"({"kind":"images","sources":["/a","/b"],"name":5})",
        };
        for (const char *json : bad) {
            const SpecResult r = parseImportSpec(json);
            QVERIFY2(!r.ok(), json);
            QVERIFY2(r.error.startsWith(QString::fromUtf8(kSpecRefused.data(), qsizetype(kSpecRefused.size()))), json);
        }
        QVERIFY(!parseImportSpec(QByteArray(70000, ' ')).ok());
        QVERIFY(!parseImportSpec(QByteArray("{\"kind\":\"sheet\",\"sources\":[\"/a") + QByteArray(1, '\0') + QByteArray("b\"]}")).ok());
    }
    void outputOptionsAreStrict()
    {
        const SpecResult d = parseImportSpec(R"j({"kind":"images","sources":["/a","/b"]})j");
        QCOMPARE(d.spec->sizePolicy, SizePolicy::Fit); // the user's answer: letterbox by default
        QVERIFY(d.spec->format == OutputFormat::Apng && !d.spec->colorKey.enabled && !d.spec->keepSource && !d.spec->keepAudio);
        const SpecResult w = parseImportSpec(R"j({"kind":"gif","sources":["/a"],"format":"webp","webpQuality":80,"keepSource":true,"colorKey":{"color":"00ff00","tolerance":30}})j");
        QVERIFY2(w.ok(), qPrintable(w.error));
        QVERIFY(w.spec->format == OutputFormat::Webp && w.spec->keepSource && w.spec->colorKey.enabled);
        QCOMPARE(w.spec->colorKey.green, quint8(255));
        QCOMPARE(w.spec->colorKey.tolerance, 0.3);
        QCOMPARE(w.spec->colorKey.softness, 0.1);
        for (const char *bad : {R"j({"kind":"gif","sources":["/a"],"format":"mp4"})j", R"j({"kind":"gif","sources":["/a"],"webpQuality":0})j",
                                R"j({"kind":"gif","sources":["/a"],"keepSource":"yes"})j", R"j({"kind":"gif","sources":["/a"],"colorKey":{"color":"zzzzzz"}})j",
                                R"j({"kind":"gif","sources":["/a"],"colorKey":{"color":"00ff00","tolerance":101}})j", R"j({"kind":"gif","sources":["/a"],"colorKey":5})j"})
            QVERIFY2(!parseImportSpec(bad).ok(), bad);
        QVERIFY(canonicalJson(*d.spec) != canonicalJson(*w.spec));
    }
    void unknownKeysAreIgnored()
    {
        QVERIFY(parseImportSpec(R"({"kind":"gif","sources":["/a.gif"],"future":{"x":1}})").ok());
    }
    void theCanonicalFormIsStable()
    {
        const SpecResult a = parseImportSpec(R"({"sources":["/a.png"],"kind":"sheet","frameWidth":32,"name":"N"})");
        const SpecResult b = parseImportSpec(R"({"name":"N","kind":"sheet","sources":["/a.png"],"frameWidth":32,"frameCount":4})");
        QCOMPARE(canonicalJson(*a.spec), canonicalJson(*b.spec));
        const SpecResult c = parseImportSpec(R"({"name":"N","kind":"sheet","sources":["/a.png"],"frameWidth":33})");
        QVERIFY(canonicalJson(*a.spec) != canonicalJson(*c.spec));
    }
    void outputNamesCannotEscape()
    {
        QCOMPARE(sanitiseOutputName("Neko walk"), QStringLiteral("Neko walk"));

        for (const QString &name : {QStringLiteral("../../x"), QStringLiteral("/abs"), QStringLiteral("a\\b"), QStringLiteral(".hidden"), QStringLiteral("   "), QString(),
                                    QStringLiteral(".."), QStringLiteral("x\ny"), QString(300, QLatin1Char('a')), QStringLiteral("日本語のペット")}) {
            const QString out = sanitiseOutputName(name);
            QVERIFY(!out.isEmpty());
            QVERIFY(out.toUtf8().size() <= 64);
            QVERIFY(!out.contains(QLatin1Char('/')) && !out.contains(QLatin1Char('\\')) && !out.contains(QStringLiteral("..")) && !out.contains(QLatin1Char('\n')));
            QVERIFY(!out.startsWith(QLatin1Char('.')));
        }
    }
    void naturalOrderPutsTwoBeforeTen()
    {
        QStringList names{"f10.png", "f2.png", "F1.png", "f02.png", "f1.png", "a.png", "f001.png"};
        std::sort(names.begin(), names.end(), naturalLess);
        QCOMPARE(names.first(), QStringLiteral("a.png"));
        QVERIFY(names.indexOf("f2.png") < names.indexOf("f10.png"));
        QVERIFY(names.indexOf("f001.png") < names.indexOf("f2.png"));
        QRandomGenerator rng(5);
        for (int i = 0; i < 2000; ++i) { // a strict weak order: irreflexive and asymmetric
            const QString a = QString::number(rng.bounded(300)) + (rng.bounded(2) ? "x" : "X"), b = QString::number(rng.bounded(300)) + "x";
            QVERIFY(!naturalLess(a, a));
            QVERIFY(!(naturalLess(a, b) && naturalLess(b, a)));
        }
    }
    void sequencePlansFollowThePolicy()
    {
        const QList<ImageDim> same{{10, 20}, {10, 20}, {10, 20}};
        QVERIFY(planSequence(same, SizePolicy::First).ok());
        const QList<ImageDim> mixed{{10, 20}, {30, 10}};
        QVERIFY(planSequence(mixed, SizePolicy::First).error.startsWith(QString::fromUtf8(kImagesSizesDiffer.data())));
        const SequencePlan fit = planSequence(mixed, SizePolicy::Fit);
        QVERIFY(fit.ok());
        QCOMPARE(fit.canvasWidth, 30);
        QCOMPARE(fit.canvasHeight, 20);
        QCOMPARE(fit.placements.at(0), (Placement{10, 0, 10, 20}));
        QCOMPARE(fit.placements.at(1), (Placement{0, 5, 30, 10}));
        const SequencePlan scale = planSequence(mixed, SizePolicy::Scale);
        QCOMPARE(scale.canvasWidth, 10);
        QCOMPARE(scale.placements.at(1), (Placement{0, 0, 10, 20}));
    }
    void sequenceLimitsAreCheckedOnHeadersOnly()
    {
        QVERIFY(planSequence({{1, 1}}, SizePolicy::First).error.startsWith(QString::fromUtf8(kImagesFewer.data())));
        QVERIFY(planSequence({}, SizePolicy::First).error.startsWith(QString::fromUtf8(kImagesFewer.data())));
        QVERIFY(planSequence(QList<ImageDim>(kMaxFrames + 1, ImageDim{1, 1}), SizePolicy::First).error.startsWith(QString::fromUtf8(kImagesTooMany.data())));
        QVERIFY(planSequence(QList<ImageDim>(kMaxFrames, ImageDim{1, 1}), SizePolicy::First).ok());
        QVERIFY(planSequence({{40000, 40000}, {40000, 40000}}, SizePolicy::First).error.startsWith(QString::fromUtf8(kNotAnImage.data())));
        QVERIFY(planSequence({{0, 5}, {1, 1}}, SizePolicy::Fit).error.startsWith(QString::fromUtf8(kNotAnImage.data())));
        QVERIFY(planSequence({{-3, 5}, {1, 1}}, SizePolicy::Scale).error.startsWith(QString::fromUtf8(kNotAnImage.data())));
    }
    void theDocumentListsTheTableAndOnlyTheTable()
    {
        QFile doc(QStringLiteral(PETS_IMPORT_DOC));
        QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/import.md is missing");
        const QString text = QString::fromUtf8(doc.readAll());
        const int start = text.indexOf(QStringLiteral("## Names"));
        const int end = text.indexOf(QStringLiteral("## Daemon calls"));
        QVERIFY(start > 0 && end > start);
        const QString section = text.mid(start, end - start);
        for (const Name &n : kNames) {
            const QString row = QStringLiteral("| `") + QString::fromUtf8(n.text.data(), qsizetype(n.text.size())) + QStringLiteral("` |");
            QVERIFY2(section.contains(row), qPrintable(row));
        }
        int rows = 0;
        for (const QString &line : section.split(QLatin1Char('\n')))
            rows += line.startsWith(QStringLiteral("| `"));
        QCOMPARE(rows, int(kNames.size()));
    }
    void everyNameIsUniqueAndDocumented()
    {
        QSet<QString> seen;
        for (const Name &n : kNames) {
            seen.insert(QString::fromUtf8(n.text.data(), qsizetype(n.text.size())));
            QVERIFY(!n.meaning.empty());
        }
        QCOMPARE(seen.size(), qsizetype(kNames.size()));
    }
};

QTEST_GUILESS_MAIN(TstImportSpec)
#include "tst_importspec.moc"
