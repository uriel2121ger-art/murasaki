#include "sceneengine.h"
#include "testutil.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki;

class TstSceneEngine : public QObject
{
    Q_OBJECT

    static QStringList propertyArgs(const QStringList &args)
    {
        QStringList out;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == QLatin1String("--set-property"))
                out << args[i + 1];
        return out;
    }

private slots:
    void initTestCase()
    {
        // The fake engines of these tests run as they are, whatever helper this machine has installed.
        qputenv("MURASAKI_SANDBOX", "/nonexistent/murasaki-sandbox");
    }

    void sandboxWrapsTheEngineBehindTheHelper()
    {
        const SandboxedCommand wrapped = sandboxSceneEngine(QStringLiteral("/bin/murasaki-sandbox"), QStringLiteral("/items/123"),
                                                            QStringLiteral("/we/assets"), QStringLiteral("/bin/engine"),
                                                            {QStringLiteral("--fps"), QStringLiteral("30")});
        QCOMPARE(wrapped.program, QStringLiteral("/bin/murasaki-sandbox"));
        QCOMPARE(wrapped.arguments, (QStringList{"--profile", "scene", "--item", "/items/123", "--assets", "/we/assets", "--",
                                                 "/bin/engine", "--fps", "30"}));
        const SandboxedCommand required = sandboxSceneEngine(QStringLiteral("h"), QStringLiteral("/i"), {}, QStringLiteral("e"), {}, true);
        QCOMPARE(required.arguments, (QStringList{"--profile", "scene", "--item", "/i", "--require", "--", "e"}));
        // Without assets, and an item named like an option can not be taken for one.
        const SandboxedCommand plain = sandboxSceneEngine(QStringLiteral("h"), QStringLiteral("-x"), {}, QStringLiteral("e"), {});
        QCOMPARE(plain.arguments, (QStringList{"--profile", "scene", "--item", "./-x", "--", "e"}));
    }

    void argumentsLayout()
    {
        SceneLaunch l;
        l.bridgeFd = 7;
        l.size = QSize(3840, 2160);
        l.outputName = QStringLiteral("DP-1");
        l.itemDir = QStringLiteral("/items/123");
        l.assetsDir = QStringLiteral("/we/assets");
        l.fill = QStringLiteral("fit");
        l.fps = 24;
        const QStringList expected{
            "--murasaki-bridge-fd", "7",        "--murasaki-size", "3840x2160", "--assets-dir", "/we/assets",
            "--volume", "0",        "--fps",    "24",              "--screen-root", "DP-1",      "--bg",
            "/items/123",           "--scaling", "fit"};
        QCOMPARE(sceneEngineArguments(l), expected);
    }

    void renderScaleIsPassedUnlessAutomatic()
    {
        SceneLaunch l;
        l.size = QSize(10, 10);
        l.itemDir = QStringLiteral("/i");
        QVERIFY(!sceneEngineArguments(l).contains(QStringLiteral("--render-scale"))); // automatic: the engine decides
        l.renderScale = 1.0;
        QCOMPARE(sceneEngineArguments(l).at(sceneEngineArguments(l).indexOf(QStringLiteral("--render-scale")) + 1), QStringLiteral("1"));
        l.renderScale = 0.75;
        QStringList args = sceneEngineArguments(l);
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--render-scale")) + 1), QStringLiteral("0.75"));
        l.renderScale = 0.5;
        args = sceneEngineArguments(l);
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--render-scale")) + 1), QStringLiteral("0.5"));
    }

    void theBufferIsScaledToTheRenderQuality()
    {
        // 2560x1440 at 75 % and 50 %: even sides, the same aspect
        QCOMPARE(scaledBufferSize({2560, 1440}, 0.75), QSize(1920, 1080));
        QCOMPARE(scaledBufferSize({2560, 1440}, 0.5), QSize(1280, 720));
        QCOMPARE(scaledBufferSize({2560, 1440}, 1.0), QSize(2560, 1440));
        // odd results are made even, never above the viewport
        QCOMPARE(scaledBufferSize({1001, 563}, 0.75).width() % 2, 0);
        QVERIFY(scaledBufferSize({1001, 563}, 0.75).width() <= 1001);
        QVERIFY(scaledBufferSize({1001, 563}, 1.0).width() <= 1001);
        // tiny viewports are not shrunk below themselves (nor below 64 when larger than that)
        QCOMPARE(scaledBufferSize({40, 30}, 0.25), QSize(40, 30));
        QCOMPARE(scaledBufferSize({100, 100}, 0.25), QSize(64, 64));
        // one pixel sides (a 1 x N output) stay what they are
        QCOMPARE(scaledBufferSize({1, 1080}, 0.5).width(), 1);
        QCOMPARE(scaledBufferSize({1920, 1}, 0.5).height(), 1);
        QCOMPARE(scaledBufferSize({1, 1}, 0.25), QSize(1, 1));
        // nothing in, nothing out; nonsense scales are 1
        QVERIFY(scaledBufferSize({}, 0.5).isEmpty());
        QCOMPARE(scaledBufferSize({800, 600}, std::nan("")), QSize(800, 600));
        QCOMPARE(scaledBufferSize({800, 600}, 0.0), QSize(200, 150)); // clamped to 25 %
    }

    void volumeIsAlwaysPassedNeverSilent()
    {
        SceneLaunch l;
        l.size = QSize(10, 10);
        l.itemDir = QStringLiteral("/i");
        l.volume = 64;
        QStringList args = sceneEngineArguments(l);
        QVERIFY(!args.contains(QStringLiteral("--silent")));
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--volume")) + 1), QStringLiteral("64"));
        l.volume = 9999;
        args = sceneEngineArguments(l);
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--volume")) + 1), QStringLiteral("128"));
        l.volume = -5;
        args = sceneEngineArguments(l);
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--volume")) + 1), QStringLiteral("0"));
        QVERIFY(!args.contains(QStringLiteral("--silent")));
    }

    void emptyAssetsDirAndDefaults()
    {
        SceneLaunch l;
        l.size = QSize(0, -3);
        l.itemDir = QStringLiteral("/i");
        l.fill = QStringLiteral("bogus");
        const QStringList args = sceneEngineArguments(l);
        QVERIFY(!args.contains(QStringLiteral("--assets-dir")));
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--murasaki-size")) + 1), QStringLiteral("1x1"));
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--screen-root")) + 1), QStringLiteral("murasaki"));
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--scaling")) + 1), QStringLiteral("fill"));
    }

    void optionLikeValuesCannotInjectOptions()
    {
        SceneLaunch l;
        l.size = QSize(10, 10);
        l.itemDir = QStringLiteral("-evil");
        l.outputName = QStringLiteral("--assets-dir");
        const QStringList args = sceneEngineArguments(l);
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--bg")) + 1), QStringLiteral("./-evil"));
        // An unsafe output name is not rewritten into another name: the launch falls back to the default and the
        // item refuses to start with it (sceneOutputNameValid).
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--screen-root")) + 1), QStringLiteral("murasaki"));
        QVERIFY(!sceneOutputNameValid(QStringLiteral("--assets-dir")));
        QVERIFY(!sceneOutputNameValid(QStringLiteral("a\nb")));
        QVERIFY(sceneOutputNameValid(QStringLiteral("DP-1")));
        QVERIFY(sceneOutputNameValid(QString()));
        l.assetsDir = QStringLiteral("-assets");
        const QStringList withAssets = sceneEngineArguments(l);
        QCOMPARE(withAssets.at(withAssets.indexOf(QStringLiteral("--assets-dir")) + 1), QStringLiteral("./-assets"));
    }

    void fillMapping()
    {
        QCOMPARE(bridgeScalingMode(QStringLiteral("fill")), 0u);
        QCOMPARE(bridgeScalingMode(QStringLiteral("fit")), 1u);
        QCOMPARE(bridgeScalingMode(QStringLiteral("stretch")), 2u);
        QCOMPARE(bridgeScalingMode(QStringLiteral("nonsense")), 0u);
    }

    void propertyEscaping()
    {
        SceneLaunch l;
        l.size = QSize(10, 10);
        l.itemDir = QStringLiteral("/i");
        l.properties = {
            {QStringLiteral("schemecolor"), QVariantList{0.5, 0.25, 1.0}},
            {QStringLiteral("bytes"), QVariantList{255, 0, 128}},
            {QStringLiteral("flag"), true},
            {QStringLiteral("off"), false},
            {QStringLiteral("count"), 3},
            {QStringLiteral("speed"), 1.5},
            {QStringLiteral("whole"), 2.0},
            {QStringLiteral("text"), QStringLiteral("a=b c \"quoted\" ünï")},
            {QStringLiteral("rgb"), QStringLiteral("0.1 0.2 0.3")},
            {QStringLiteral("bad=key"), 1},
            {QStringLiteral("-opt"), 1},
            {QString(), 1},
            {QStringLiteral("nested"), QVariantMap{{QStringLiteral("a"), 1}}},
        };
        QStringList got = propertyArgs(sceneEngineArguments(l));
        got.sort();
        QStringList want{
            "schemecolor=0.500000 0.250000 1.000000",
            "bytes=255 0 128",
            "flag=true",
            "off=false",
            "count=3",
            "speed=1.500000",
            "whole=2",
            "text=a=b c \"quoted\" ünï",
            "rgb=0.1 0.2 0.3",
        };
        want.sort();
        QCOMPARE(got, want);
    }

    void propertyValueNeverUsesExponent()
    {
        QCOMPARE(*scenePropertyValue(0.00000001), QStringLiteral("0.000000"));
        QCOMPARE(*scenePropertyValue(1e20), QStringLiteral("100000000000000000000.000000"));
        QVERIFY(!scenePropertyValue(QVariant()).has_value());
        QVERIFY(!scenePropertyValue(QVariantList{}).has_value());
        QVERIFY(!scenePropertyValue(QVariantList{QStringLiteral("x")}).has_value());
    }

    void propertiesJson()
    {
        const QVariantMap map{
            {QStringLiteral("a"), true},
            {QStringLiteral("b"), 2},
            {QStringLiteral("c"), QStringLiteral("0.1 0.2 0.3")},
            {QStringLiteral("d"), QVariantList{0.1, 0.2, 0.3}},
            {QStringLiteral("x=y"), 1},
            {QStringLiteral("nested"), QVariantMap{}},
        };
        const QJsonObject all = bridgePropertiesJson(map);
        QCOMPARE(all.size(), 4);
        QCOMPARE(all[QStringLiteral("a")], QJsonValue(true));
        QCOMPARE(all[QStringLiteral("b")], QJsonValue(2));
        QCOMPARE(all[QStringLiteral("c")], QJsonValue(QStringLiteral("0.1 0.2 0.3")));
        QVERIFY(all[QStringLiteral("d")].isArray());
        QCOMPARE(all[QStringLiteral("d")].toArray().size(), 3);
        const QJsonObject some = bridgePropertiesJson(map, {QStringLiteral("b"), QStringLiteral("nested")});
        QCOMPARE(some.keys(), QStringList{QStringLiteral("b")});
    }

    void backoffDelaysAndGiveUp()
    {
        using ms = std::chrono::milliseconds;
        RestartBackoff b; // 3 crashes in 60 s give up
        QCOMPARE(b.recordCrash(ms(0)), std::optional<ms>(ms(1000)));
        QCOMPARE(b.recordCrash(ms(1000)), std::optional<ms>(ms(2000)));
        QVERIFY(!b.recordCrash(ms(3000)).has_value());
    }

    void backoffGrowsToFourSecondsAndRepeats()
    {
        using ms = std::chrono::milliseconds;
        RestartBackoff b(6, std::chrono::seconds(60));
        const int expected[] = {1000, 2000, 4000, 4000, 4000};
        for (int i = 0; i < 5; ++i)
            QCOMPARE(b.recordCrash(ms(i * 100)), std::optional<ms>(ms(expected[i])));
        QVERIFY(!b.recordCrash(ms(600)).has_value());
    }

    void backoffForgetsOldCrashes()
    {
        using ms = std::chrono::milliseconds;
        RestartBackoff b;
        QVERIFY(b.recordCrash(ms(0)).has_value());
        QVERIFY(b.recordCrash(ms(1000)).has_value());
        // The third crash lands outside the 60 s window of the first two: the policy starts over.
        QCOMPARE(b.recordCrash(ms(62000)), std::optional<ms>(ms(1000)));
        QCOMPARE(b.recentCrashes(), 1);
    }

    void backoffResetClearsHistory()
    {
        using ms = std::chrono::milliseconds;
        RestartBackoff b;
        b.recordCrash(ms(0));
        b.recordCrash(ms(10));
        b.reset();
        QCOMPARE(b.recordCrash(ms(20)), std::optional<ms>(ms(1000)));
    }
};

QTEST_GUILESS_MAIN(TstSceneEngine)
#include "tst_sceneengine.moc"
