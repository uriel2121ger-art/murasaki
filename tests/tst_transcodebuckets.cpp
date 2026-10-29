#include "transcodebuckets.h"

#include <QTest>

#include <limits>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestTranscodeBuckets : public QObject
{
    Q_OBJECT

private slots:
    void bucketsAreTheDocumentedOnes();
    void displayedLongestSideFollowsTheAspectRatio();
    void needIsTheScaledCeilingCappedBySourceAndMax();
    void bucketForNeedIsTheSmallestThatFits();
    void bucketForNeedCapsAtTheLargest_data();
    void bucketForNeedCapsAtTheLargest();
    void stageBucketsServeWhatAPetCannot();
    void aStagesNeedIsTheLongestSideOfTheDrawnAnimation();
    void outputGeometryIsParsedAndValidated();
    void pickDonePrefersTheSmallestThatFitsThenTheLargest();
    void keepNeverDropsTheBucketInUseAndLimitsTheRest();
    void outputScalesAreValidated();
    void outputScalesAreValidated_data();
    void atMostThirtyTwoOutputs();
};

void TestTranscodeBuckets::bucketsAreTheDocumentedOnes()
{
    QCOMPARE(kTranscodeBuckets, (std::array<int, 4>{256, 384, 512, 650}));
    QCOMPARE(kTranscodeMaxSide, 650);
    QVERIFY(isTranscodeBucket(384));
    QVERIFY(!isTranscodeBucket(385));
}

void TestTranscodeBuckets::displayedLongestSideFollowsTheAspectRatio()
{
    QCOMPARE(displayedLongestSide(300, QSize(1000, 500)), 300.0); // wide: the width is the longest side
    QCOMPARE(displayedLongestSide(300, QSize(500, 1000)), 600.0); // tall: the height follows
    QCOMPARE(displayedLongestSide(300, QSize()), 300.0);          // unknown size
}

void TestTranscodeBuckets::needIsTheScaledCeilingCappedBySourceAndMax()
{
    QCOMPARE(transcodeNeed(300, 1.0, 2000), 300);
    QCOMPARE(transcodeNeed(300, 1.25, 2000), 375);
    QCOMPARE(transcodeNeed(300.4, 1.0, 2000), 301);
    QCOMPARE(transcodeNeed(300.0000001, 1.0, 2000), 300); // floating point noise does not cost a pixel
    QCOMPARE(transcodeNeed(300, 2.0, 2000), 600);
    QCOMPARE(transcodeNeed(900, 2.0, 2000), 650);         // cap
    QCOMPARE(transcodeNeed(900, 1.0, 400), 400);          // never above the source
    QCOMPARE(transcodeNeed(900, 1.0, 0), 650);            // unknown source size
    QCOMPARE(transcodeNeed(0, 1.0, 400), 1);
    QCOMPARE(transcodeNeed(300, std::numeric_limits<double>::infinity(), 2000), 650);
}

void TestTranscodeBuckets::bucketForNeedIsTheSmallestThatFits()
{
    QCOMPARE(bucketForNeed(1), 256);
    QCOMPARE(bucketForNeed(256), 256);
    QCOMPARE(bucketForNeed(257), 384);
    QCOMPARE(bucketForNeed(384), 384);
    QCOMPARE(bucketForNeed(385), 512);
    QCOMPARE(bucketForNeed(513), 650);
}

void TestTranscodeBuckets::bucketForNeedCapsAtTheLargest_data()
{
    QTest::addColumn<int>("need");
    QTest::newRow("max") << 3840;
    QTest::newRow("above") << 3841;
    QTest::newRow("far above") << 100000;
}

void TestTranscodeBuckets::bucketForNeedCapsAtTheLargest()
{
    QFETCH(int, need);
    QCOMPARE(bucketForNeed(need), 3840); // (a pet's own need never exceeds 650: transcodeNeed caps it; a stage's goes to 3840)
}

void TestTranscodeBuckets::stageBucketsServeWhatAPetCannot()
{
    QCOMPARE(kStageBuckets, (std::array<int, 5>{960, 1280, 1920, 2560, 3840}));
    QCOMPARE(kStageMaxSide, 3840);
    for (const int bucket : {960, 1280, 1920, 2560, 3840, 650, 256})
        QVERIFY(isTranscodeBucket(bucket));
    QVERIFY(!isTranscodeBucket(1000));
    QCOMPARE(bucketForNeed(651), 960);
    QCOMPARE(bucketForNeed(961), 1280);
    QCOMPARE(bucketForNeed(1920), 1920);
    QCOMPARE(bucketForNeed(2561), 3840);
    QCOMPARE(bucketForNeed(650), 650); // a pet's own needs are as before
    QCOMPARE(transcodeNeed(900, 2.0, 5000), 650); // and a pet is still never converted larger
    // two buckets of a variant stay: the biggest and the one a placement needs
    QCOMPARE(bucketsToKeep({960, 650, 3840}, {900}), (QList<int>{3840, 960}));
}

void TestTranscodeBuckets::aStagesNeedIsTheLongestSideOfTheDrawnAnimation()
{
    // a 1920x1080 picture of a 1000x500 animation (contain on 1920x1080 is 1920x960): its long side is 1920 px at scale 1, 2880 at 1.5
    QCOMPARE(stageNeed(QSizeF(1920, 960), QSize(1000, 500), 0, 1.0, 5000), 1920);
    QCOMPARE(stageNeed(QSizeF(1920, 960), QSize(1000, 500), 0, 1.5, 5000), 2880);
    QCOMPARE(stageNeed(QSizeF(1920, 960), QSize(1000, 500), 0, 4.0, 5000), 3840); // capped
    QCOMPARE(stageNeed(QSizeF(1920, 960), QSize(1000, 500), 0, 1.0, 1200), 1200); // never above the source
    // turned a quarter: the footprint is 500x1000, so the picture 960x1920 is covered by the same animation
    QCOMPARE(stageNeed(QSizeF(960, 1920), QSize(1000, 500), 90, 1.0, 5000), 1920);
    // stretch: the larger factor wins, so neither side is under-sized
    QCOMPARE(stageNeed(QSizeF(1920, 1080), QSize(1000, 500), 0, 1.0, 5000), 2160);
    // unusable input asks for the most, never crashes
    QCOMPARE(stageNeed(QSizeF(), QSize(10, 10), 0, 1.0, 100), kStageMaxSide);
    QCOMPARE(stageNeed(QSizeF(10, 10), QSize(), 0, 1.0, 0), kStageMaxSide);
    QCOMPARE(stageNeed(QSizeF(10, 10), QSize(10, 10), 0, std::numeric_limits<double>::quiet_NaN(), 0), kStageMaxSide);
}

void TestTranscodeBuckets::outputGeometryIsParsedAndValidated()
{
    QString problem;
    auto info = parseOutputInfo(u"{\"DP-1\": {\"scale\": 1.5, \"x\": -1920, \"y\": 0, \"width\": 1920, \"height\": 1080}, \"HDMI-A-1\": 2}"_s, &problem);
    QVERIFY2(info, qPrintable(problem));
    QCOMPARE(info->value(u"DP-1"_s).scale, 1.5);
    QCOMPARE(info->value(u"DP-1"_s).rect, QRect(-1920, 0, 1920, 1080));
    QVERIFY(info->value(u"HDMI-A-1"_s).rect.isNull()); // a plain number is a scale and nothing more
    QCOMPARE(parseOutputScales(u"{\"DP-1\": {\"scale\": 1.5, \"x\": 0, \"y\": 0, \"width\": 10, \"height\": 10}}"_s)->value(u"DP-1"_s), 1.5);
    for (const char *bad : {"{\"A\": {\"scale\": 1}}", "{\"A\": {\"scale\": 1, \"x\": 0, \"y\": 0, \"width\": 0, \"height\": 5}}",
                            "{\"A\": {\"scale\": 1, \"x\": 0, \"y\": 0, \"width\": 16385, \"height\": 5}}",
                            "{\"A\": {\"scale\": 1, \"x\": 0.5, \"y\": 0, \"width\": 5, \"height\": 5}}",
                            "{\"A\": {\"scale\": 1, \"x\": 200000, \"y\": 0, \"width\": 5, \"height\": 5}}",
                            "{\"A\": {\"scale\": 9, \"x\": 0, \"y\": 0, \"width\": 5, \"height\": 5}}",
                            "{\"A\": {\"scale\": \"1\", \"x\": 0, \"y\": 0, \"width\": 5, \"height\": 5}}",
                            "{\"A\": {\"scale\": 1, \"x\": \"0\", \"y\": 0, \"width\": 5, \"height\": 5}}"}) {
        QVERIFY2(!parseOutputInfo(QString::fromUtf8(bad), &problem), bad);
        QVERIFY(!problem.isEmpty());
    }
}

void TestTranscodeBuckets::pickDonePrefersTheSmallestThatFitsThenTheLargest()
{
    QVERIFY(!pickDoneBucket({}, 300));
    QCOMPARE(*pickDoneBucket({650, 384, 256}, 300), 384);
    QCOMPARE(*pickDoneBucket({650, 384, 256}, 384), 384);
    QCOMPARE(*pickDoneBucket({650, 256}, 300), 650);
    QCOMPARE(*pickDoneBucket({256, 384}, 600), 384); // nothing fits: the largest there is
    QCOMPARE(*pickDoneBucket({256}, 650), 256);
}

void TestTranscodeBuckets::keepNeverDropsTheBucketInUseAndLimitsTheRest()
{
    // Newest first: 384 was converted last. The biggest copy always stays: it serves every smaller pet.
    QCOMPARE(bucketsToKeep({384, 256, 650}, {300}), (QList<int>{650, 384}));      // the biggest + the one in use
    QCOMPARE(bucketsToKeep({384, 256, 650}, {600}), (QList<int>{650, 384}));
    QCOMPARE(bucketsToKeep({384, 256, 650}, {200, 600}), (QList<int>{650, 256})); // two placements, both in use
    QCOMPARE(bucketsToKeep({384, 256, 650}, {200, 300, 600}), (QList<int>{650, 256, 384})); // in use beats the limit
    QCOMPARE(bucketsToKeep({384, 256, 650}, {}), (QList<int>{650, 384}));         // nothing placed: the biggest and the newest
    // a pet shrunk after 650 was converted, then 512 and 384: the 650 copy is not the one to evict
    QCOMPARE(bucketsToKeep({384, 512, 650}, {100}), (QList<int>{650, 384}));
    QCOMPARE(bucketsToKeep({256}, {600}), (QList<int>{256}));                     // the biggest there is, though too small
    QCOMPARE(bucketsToKeep({}, {300}), QList<int>());
}

void TestTranscodeBuckets::outputScalesAreValidated_data()
{
    QTest::addColumn<QString>("json");
    QTest::addColumn<bool>("valid");
    QTest::newRow("empty object") << u"{}"_s << true;
    QTest::newRow("two outputs") << u"{\"DP-1\":1.0,\"HDMI-A-1\":2}"_s << true;
    QTest::newRow("wildcard") << u"{\"*\":1.5}"_s << true;
    QTest::newRow("low edge") << u"{\"DP-1\":0.5}"_s << true;
    QTest::newRow("high edge") << u"{\"DP-1\":4}"_s << true;
    QTest::newRow("too low") << u"{\"DP-1\":0.49}"_s << false;
    QTest::newRow("too high") << u"{\"DP-1\":4.01}"_s << false;
    QTest::newRow("zero") << u"{\"DP-1\":0}"_s << false;
    QTest::newRow("negative") << u"{\"DP-1\":-1}"_s << false;
    QTest::newRow("string scale") << u"{\"DP-1\":\"1\"}"_s << false;
    QTest::newRow("bool scale") << u"{\"DP-1\":true}"_s << false;
    QTest::newRow("null scale") << u"{\"DP-1\":null}"_s << false;
    QTest::newRow("empty name") << u"{\"\":1}"_s << false;
    QTest::newRow("long name") << (u"{\""_s + QString(201, u'x') + u"\":1}"_s) << false;
    QTest::newRow("array") << u"[1]"_s << false;
    QTest::newRow("not json") << u"nope"_s << false;
    QTest::newRow("empty string") << QString() << false;
}

void TestTranscodeBuckets::outputScalesAreValidated()
{
    QFETCH(QString, json);
    QFETCH(bool, valid);
    QString problem;
    const auto scales = parseOutputScales(json, &problem);
    QCOMPARE(scales.has_value(), valid);
    QCOMPARE(problem.isEmpty(), valid);
}

void TestTranscodeBuckets::atMostThirtyTwoOutputs()
{
    const auto json = [](int count) {
        QStringList entries;
        for (int i = 0; i < count; ++i)
            entries << u"\"out-%1\":1"_s.arg(i);
        return u"{"_s + entries.join(u',') + u"}"_s;
    };
    QVERIFY(parseOutputScales(json(32)).has_value());
    QVERIFY(!parseOutputScales(json(33)).has_value());
}

QTEST_GUILESS_MAIN(TestTranscodeBuckets)
#include "tst_transcodebuckets.moc"
