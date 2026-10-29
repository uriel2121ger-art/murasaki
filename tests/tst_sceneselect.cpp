// Choosing the scene engine: the `sceneEngine` mode, the cached --check verdicts and the crash ledger.
// The engines are shell scripts that count their runs and print a fixed verdict.

#include "sceneengine.h"
#include "testutil.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A fake murasaki-scene-vk. `<path>.calls` gets one line per run (the arguments), `<path>.verdict` is printed on
// --check, `<path>.exit` (when present) is its exit status.
QString makeFakeVulkan(const QString &dir, const QString &name, const QByteArray &verdict)
{
    const QString path = dir + u'/' + name;
    testutil::writeFile(path,
                        "#!/bin/sh\n"
                        "echo \"$@\" >> \"$0.calls\"\n"
                        "[ -f \"$0.sleep\" ] && sleep \"$(cat \"$0.sleep\")\"\n"
                        "[ -f \"$0.exit\" ] && exit \"$(cat \"$0.exit\")\"\n"
                        "[ \"$1\" = \"--check\" ] && cat \"$0.verdict\"\n"
                        "exit 0\n");
    testutil::writeFile(path + u".verdict"_s, verdict);
    QFile(path).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

int calls(const QString &engine)
{
    QFile f(engine + u".calls"_s);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    return f.readAll().count('\n');
}

QString lastCall(const QString &engine)
{
    QFile f(engine + u".calls"_s);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> lines = f.readAll().trimmed().split('\n');
    return QString::fromUtf8(lines.last());
}

const QByteArray kOk = "{\"ok\":true,\"reasons\":[],\"engine\":\"fake\",\"version\":\"1\"}\n";
const QByteArray kNotOk = "{\"ok\":false,\"reasons\":[\"effects: M2\",\"particles: M3\"]}\n";

} // namespace

class TestSceneSelect : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_item;

    // Whether the choice started a check, or one is running or waiting.
    static bool checkStarted(const SceneEngineChoice &choice, const SceneCheckRunner &runner)
    {
        return choice.pending || runner.runningCount() > 0 || runner.queuedCount() > 0;
    }

    SceneEngineRequest request(SceneEngineMode mode, const QString &vulkan, SceneCheckCache *cache, SceneCheckRunner *runner = nullptr) const
    {
        SceneEngineRequest r;
        r.mode = mode;
        r.itemDir = m_item;
        r.assetsDir = u"/assets"_s;
        r.vulkanProgram = vulkan;
        r.cache = cache;
        r.runner = runner;
        return r;
    }

private slots:
    void init()
    {
        m_dir.remove();
        m_dir = QTemporaryDir();
        QVERIFY(m_dir.isValid());
        // hermetic: a murasaki-sandbox installed on the machine running the tests must not wrap the fake engines
        qputenv("MURASAKI_SANDBOX", m_dir.filePath(u"no-sandbox"_s).toLocal8Bit());
        m_item = m_dir.filePath(u"item"_s);
        testutil::writeFile(m_item + u"/project.json"_s, "{}");
        testutil::writeFile(m_item + u"/scene.pkg"_s, "pkg");
    }

    void modeStrings()
    {
        QCOMPARE(sceneEngineModeFromString(u"auto"_s), SceneEngineMode::Auto);
        QCOMPARE(sceneEngineModeFromString(u"vulkan"_s), SceneEngineMode::Vulkan);
        QCOMPARE(sceneEngineModeFromString(u"bogus"_s), SceneEngineMode::Auto); // the default
        QCOMPARE(sceneEngineModeFromString(QString()), SceneEngineMode::Auto);
    }

    // A spanned scene is drawn by the Vulkan engine only: there is no other engine to fall back to, and an engine that
    // cannot do it is refused with the reason.
    void aSpannedSceneNeedsTheVulkanEngine()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        const SceneEngineMode modes[] = {
            SceneEngineMode::Auto,
            SceneEngineMode::Vulkan,
        };
        for (const auto mode : modes) {
            SceneEngineRequest r = request(mode, vk, &cache);
            r.span = true;
            const auto choice = chooseSceneEngine(r);
            QCOMPARE(choice.program, vk);
            QVERIFY(choice.vulkan);
            QVERIFY(choice.refusal.isEmpty());
            QVERIFY(!choice.pending);
        }
        QCOMPARE(calls(vk), 0); // nothing was asked of the engine to choose it

        // no Vulkan engine installed
        SceneEngineRequest none = request(SceneEngineMode::Auto, QString(), &cache);
        none.span = true;
        const auto missing = chooseSceneEngine(none);
        QVERIFY(missing.program.isEmpty());
        QVERIFY(missing.refusal.contains(u"Spanning"_s));
        QVERIFY(missing.refusal.contains(u"murasaki-scene-vk"_s));

        // a cached verdict that says the scene is beyond it: the reason comes along
        const QString picky = makeFakeVulkan(m_dir.path(), u"picky"_s, kNotOk);
        const QString key = sceneCheckKey(m_item, picky, u"/assets"_s);
        cache.storeVerdict(m_item, key, SceneCheckVerdict{false, {u"effects: M2"_s}});
        SceneEngineRequest beyond = request(SceneEngineMode::Auto, picky, &cache);
        beyond.span = true;
        const auto refused = chooseSceneEngine(beyond);
        QVERIFY(refused.program.isEmpty());
        QVERIFY(refused.refusal.contains(u"Spanning"_s));
        QVERIFY(refused.refusal.contains(u"effects: M2"_s));

        // a Vulkan engine that crashed twice on the item is not replaced by another engine
        const QString crashy = makeFakeVulkan(m_dir.path(), u"crashy"_s, kOk);
        const QString crashKey = sceneCheckKey(m_item, crashy, u"/assets"_s);
        cache.recordCrash(m_item, crashKey);
        cache.recordCrash(m_item, crashKey);
        SceneEngineRequest crashed = request(SceneEngineMode::Auto, crashy, &cache);
        crashed.span = true;
        const auto gone = chooseSceneEngine(crashed);
        QVERIFY(gone.program.isEmpty());
        QVERIFY(gone.refusal.contains(u"crashed"_s));

        // without span nothing changes: the refusal stays empty
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache)).refusal.isEmpty());
    }


    void vulkanModeForcesTheEngineWithoutChecking()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kNotOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        const auto choice = chooseSceneEngine(request(SceneEngineMode::Vulkan, vk, &cache));
        QCOMPARE(choice.program, vk);
        QVERIFY(choice.vulkan);
        QCOMPARE(choice.label, u"Vulkan"_s);
        QCOMPARE(calls(vk), 0);

        // not installed: no program, with the reason
        const auto none = chooseSceneEngine(request(SceneEngineMode::Vulkan, QString(), &cache));
        QVERIFY(none.program.isEmpty());
        QVERIFY(none.label.contains(u"not installed"_s));
    }

    // Only the Vulkan engine installed: it runs every scene, whatever the mode; one its check said it cannot fully draw is named as
    // incomplete, with the reason, and an unchecked one is checked in the background so that the label can say so later.
    void withoutTheForkTheVulkanEngineRunsEveryScene()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kNotOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        const SceneEngineMode modes[] = {
            SceneEngineMode::Auto,
        };
        for (const auto mode : modes) {
            const auto choice = chooseSceneEngine(request(mode, vk, &cache));
            QCOMPARE(choice.program, vk);
            QVERIFY(choice.vulkan);
            QCOMPARE(choice.label, u"Vulkan"_s);
        }

        const QString key = sceneCheckKey(m_item, vk, u"/assets"_s);
        cache.storeVerdict(m_item, key, SceneCheckVerdict{false, {u"effects: M2"_s}});
        const auto incomplete = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache));
        QCOMPARE(incomplete.program, vk);
        QCOMPARE(incomplete.label, u"Vulkan (incomplete: effects: M2)"_s);

        // nothing installed at all
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Auto, QString(), &cache)).program.isEmpty());
    }

    void withoutTheForkAnUncheckedSceneIsCheckedInTheBackground()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kNotOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache, 15000);
        runner.setSandbox(false);
        const auto first = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QCOMPARE(first.program, vk);
        QVERIFY(!first.pending); // nothing waits for it
        QVERIFY(waitForVerdict(runner));
        const auto next = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY2(next.label.startsWith(u"Vulkan (incomplete: "_s), qPrintable(next.label));
    }

    // Waits for the next verdict the runner reports.
    static bool waitForVerdict(SceneCheckRunner &runner, int ms = 15000)
    {
        QSignalSpy spy(&runner, &SceneCheckRunner::verdictReady);
        return spy.count() > 0 || spy.wait(ms);
    }

    void autoNeverBlocksAndUsesTheVerdictOnTheNextChoice()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        testutil::writeFile(vk + u".sleep"_s, "1"); // a slow engine: the choice must not wait for it
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache, 15000);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        QElapsedTimer timer;
        timer.start();
        const auto first = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY2(timer.elapsed() < 500, "chooseSceneEngine blocked");

        QVERIFY(waitForVerdict(runner));
        const auto second = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(second.vulkan);
        QVERIFY(!second.pending);
        QCOMPARE(second.program, vk);
        QCOMPARE(second.label, u"Vulkan"_s);
        QCOMPARE(calls(vk), 1);
        QVERIFY(lastCall(vk).startsWith(u"--check "_s + m_item));
        QVERIFY(lastCall(vk).contains(u"--assets-dir /assets"_s));

        // later choices, also through a new cache object reading the file, ask nobody
        SceneCheckCache reloaded(m_dir.filePath(u"cache.json"_s));
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &reloaded, &runner)).vulkan);
        QCOMPARE(calls(vk), 1);
    }

    void oneCheckRunsPerKeyWhateverTheNumberOfAskers()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        testutil::writeFile(vk + u".sleep"_s, "0.3");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache, 15000);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        for (int i = 0; i < 5; ++i) {
            const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
            QVERIFY(checkStarted(choice, runner));
        }
        QVERIFY(waitForVerdict(runner));
        QTest::qWait(100);
        QCOMPARE(calls(vk), 1);
    }


#ifdef MURASAKI_SCENE_VK_PATH
    // The real engine's own --check decides: an item it cannot draw in full is labelled with the reason; one it can draw
    // goes to the Vulkan engine. (The crash path is the ledger test below, with fake engines.)
    void theRealEngineDecidesTheFallback()
    {
        qputenv("MURASAKI_SCENE_VK_CHECK_DEVICE", "0");
        const auto itemWith = [&](const char *name, const QByteArray &objects) {
            const QString dir = m_dir.filePath(QString::fromLatin1(name));
            testutil::writeFile(dir + u"/project.json"_s, "{\"type\":\"Scene\",\"file\":\"scene.json\"}");
            testutil::writeFile(dir + u"/scene.json"_s,
                                "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"orthogonalprojection\":{\"width\":1920,\"height\":1080}},"
                                "\"objects\":[" + objects + "]}");
            return dir;
        };
        const QString vk = QString::fromUtf8(MURASAKI_SCENE_VK_PATH);
        for (const bool supported : {false, true}) {
            SceneCheckCache cache(m_dir.filePath(u"real-cache.json"_s));
            SceneCheckRunner runner(&cache);
            runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
            SceneEngineRequest r = request(SceneEngineMode::Auto, vk, &cache, &runner);
            r.itemDir = supported ? itemWith("fine", "") : itemWith("particles", R"({"id":1,"name":"p","particle":"particles/none.json"})");
            r.assetsDir = m_dir.filePath(u"no-assets"_s);
            const auto first = chooseSceneEngine(r);
            QVERIFY(waitForVerdict(runner, 30000));
            const auto choice = chooseSceneEngine(r);
            if (supported) {
                QVERIFY2(choice.vulkan, qPrintable(choice.label));
                QCOMPARE(choice.label, u"Vulkan"_s);
            } else {
                QVERIFY(choice.vulkan);
                QVERIFY2(choice.label.startsWith(u"Vulkan (incomplete: "_s) && choice.label.contains(u"particles"_s), qPrintable(choice.label));
            }
        }
    }
#endif

    void aBurstOfChecksRunsTwoAtATimeAndDropsTheOldestWaiting()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        testutil::writeFile(vk + u".sleep"_s, "0.3");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache, 15000);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        for (int i = 1; i <= 8; ++i)
            runner.request(m_item + QString::number(i), QString(), vk, u"k%1"_s.arg(i));
        QCOMPARE(runner.runningCount(), kMaxParallelChecks);
        QCOMPARE(runner.queuedCount(), kMaxQueuedChecks);
        QVERIFY(runner.running(u"k8"_s)); // a queued key still counts as being checked
        QVERIFY(!runner.running(u"k3"_s)); // dropped: it was the oldest waiting
        QVERIFY(!runner.running(u"k4"_s));
        runner.request(m_item + u"5"_s, QString(), vk, u"k5"_s); // asked again: no second copy
        QCOMPARE(runner.queuedCount(), kMaxQueuedChecks);
        int widest = 0;
        QElapsedTimer timer;
        timer.start();
        while ((runner.runningCount() > 0 || runner.queuedCount() > 0) && timer.elapsed() < 20000) {
            widest = std::max(widest, runner.runningCount());
            QTest::qWait(20);
        }
        QCOMPARE(runner.runningCount(), 0);
        QCOMPARE(runner.queuedCount(), 0);
        QVERIFY(widest <= kMaxParallelChecks);
        QCOMPARE(calls(vk), 6); // k1 and k2, then k8, k7, k6 and k5; k3 and k4 never ran
    }

    void aFailedCheckIsBelievedBrieflyButARealNoStays()
    {
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        cache.storeFailure(m_item, u"timed-out"_s, u"--check timed out"_s, now);
        QVERIFY(cache.failure(u"timed-out"_s, now + 60 * 1000).has_value()); // a minute later: still believed
        QVERIFY(!cache.failure(u"timed-out"_s, now + 3 * 60 * 1000).has_value()); // three minutes later: ask again
        SceneCheckVerdict no;
        no.ok = false;
        no.reasons = {u"effects: M2"_s};
        cache.storeVerdict(m_item, u"real-no"_s, no);
        const auto kept = cache.verdict(u"real-no"_s);
        QVERIFY(kept.has_value());
        QVERIFY(!kept->ok); // a real "no" is not subject to the retry window
    }

    void withoutARunnerAutoOnlyUsesWhatIsCached()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, nullptr));
        QVERIFY(!choice.pending);
        QCOMPARE(calls(vk), 0);
        QCOMPARE(choice.program, vk);
    }

    void cachedVerdictsDieWithTheItemTheEngineOrTheAssets()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        const auto ask = [&] { // whether it had to check
            const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
            const bool checked = checkStarted(choice, runner);
            if (checked)
                waitForVerdict(runner);
            return checked;
        };
        ask();
        QCOMPARE(calls(vk), 1);
        QVERIFY(!ask());
        QCOMPARE(calls(vk), 1);

        // a file directly in the item changes
        QTest::qSleep(20);
        QFile touch(m_item + u"/scene.pkg"_s);
        QVERIFY(touch.open(QIODevice::WriteOnly | QIODevice::Append));
        touch.write("more");
        touch.close();
        QVERIFY(ask());
        QCOMPARE(calls(vk), 2);

        // a file in a subfolder changes, which the folder's own mtime does not show
        QTest::qSleep(20);
        testutil::writeFile(m_item + u"/sub/deep/file.bin"_s, "1");
        QTest::qSleep(20);
        testutil::writeFile(m_item + u"/sub/deep/file.bin"_s, "22");
        QVERIFY(ask());
        QCOMPARE(calls(vk), 3);

        // the Wallpaper Engine assets directory changes (the request carries /assets: use a real one)
        QVERIFY(QDir().mkpath(m_dir.filePath(u"assets"_s)));
        SceneEngineRequest withAssets = request(SceneEngineMode::Auto, vk, &cache, &runner);
        withAssets.assetsDir = m_dir.filePath(u"assets"_s);
        chooseSceneEngine(withAssets);
        waitForVerdict(runner);
        const int afterFirst = calls(vk);
        QTest::qSleep(20);
        testutil::writeFile(m_dir.filePath(u"assets/new.txt"_s), "x");
        QVERIFY(checkStarted(chooseSceneEngine(withAssets), runner));
        waitForVerdict(runner);
        QCOMPARE(calls(vk), afterFirst + 1);

        // the engine changes: a new binary at the same path
        QTest::qSleep(20);
        QFile engine(vk);
        QVERIFY(engine.open(QIODevice::WriteOnly | QIODevice::Append));
        engine.write("# new build\n");
        engine.close();
        QVERIFY(ask());
    }


    // The ledger of crashes with only the Vulkan engine: it keeps running the item, named as incomplete.
    void aTwiceCrashedItemIsNamedIncomplete()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        const QString cachePath = m_dir.filePath(u"cache.json"_s);
        SceneCheckCache cache(cachePath);
        SceneCheckRunner runner(&cache);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(waitForVerdict(runner));
        const auto first = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(first.vulkan);
        QCOMPARE(first.label, u"Vulkan"_s);
        QVERIFY(!first.key.isEmpty());
        QCOMPARE(cache.recordCrash(m_item, first.key), 1);
        QCOMPARE(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner)).label, u"Vulkan"_s); // one restart is allowed
        QCOMPARE(cache.recordCrash(m_item, first.key), 2);
        const auto after = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QCOMPARE(after.program, vk);
        QVERIFY2(after.label.startsWith(u"Vulkan (incomplete: "_s) && after.label.contains(u"crashed"_s), qPrintable(after.label));
        // the forced mode is subject to it too
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Vulkan, vk, &cache)).label.contains(u"crashed"_s));
        // and it is remembered
        SceneCheckCache reloaded(cachePath);
        SceneCheckRunner reloadedRunner(&reloaded);
        reloadedRunner.setSandbox(false);
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &reloaded, &reloadedRunner)).label.contains(u"crashed"_s));

        // a changed item gets another chance (and a new check)
        QTest::qSleep(20);
        testutil::writeFile(m_item + u"/scene.pkg"_s, "new content");
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &reloaded, &reloadedRunner));
        QVERIFY(waitForVerdict(reloadedRunner));
        QCOMPARE(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &reloaded, &reloadedRunner)).label, u"Vulkan"_s);
    }

    void theCheckRunsBehindTheSandboxHelperUnlessTurnedOff()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        // a stand-in helper: records its arguments, then runs what follows `--` like the real one does
        const QString helper = m_dir.filePath(u"helper"_s);
        testutil::writeFile(helper,
                            "#!/bin/sh\n"
                            "echo \"$@\" > \"$0.args\"\n"
                            "while [ \"$1\" != \"--\" ]; do shift; done\n"
                            "shift\n"
                            "exec \"$@\"\n");
        QFile(helper).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("MURASAKI_SANDBOX", helper.toLocal8Bit());
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache);
        runner.request(m_item, u"/assets"_s, vk, u"key"_s);
        QVERIFY(waitForVerdict(runner));
        QFile args(helper + u".args"_s);
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QString line = QString::fromLocal8Bit(args.readAll()).trimmed();
        QCOMPARE(line, u"--profile scene --item %1 --assets /assets -- %2 --check %1 --assets-dir /assets"_s.arg(m_item, vk));
        QCOMPARE(calls(vk), 1); // and the engine did run, behind it

        // switched off, the engine is started directly
        QFile::remove(helper + u".args"_s);
        SceneCheckRunner direct(&cache);
        direct.setSandbox(false);
        direct.request(m_item, u"/assets"_s, vk, u"key2"_s);
        QVERIFY(waitForVerdict(direct));
        QVERIFY(!QFile::exists(helper + u".args"_s));
        QCOMPARE(calls(vk), 2);
    }

    void failedChecksFallBackAndAreRememberedForADay()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, "this is not json\n");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(waitForVerdict(runner));
        auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(!choice.pending);
        QVERIFY2(cache.failure(choice.key).value_or(QString()).contains(u"no verdict"_s), qPrintable(choice.label));
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QCOMPARE(calls(vk), 1); // not asked again within the TTL

        // after the TTL it is asked again: age the failure by rewriting it
        const QString key = choice.key;
        QVERIFY(cache.failure(key).has_value());
        QVERIFY(!cache.failure(key, QDateTime::currentMSecsSinceEpoch() + kCheckFailureTtlMs + 1000).has_value());
        cache.storeFailure(m_item, key, u"old failure"_s, QDateTime::currentMSecsSinceEpoch() - kCheckFailureTtlMs - 1000);
        QVERIFY(checkStarted(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner)), runner));
        QVERIFY(waitForVerdict(runner));
        QCOMPARE(calls(vk), 2);

        // a failure survives a restart of the program
        SceneCheckCache reloaded(m_dir.filePath(u"cache.json"_s));
        QVERIFY(reloaded.failure(key).has_value());
    }

    void exitCodesAndMissingVerdictFieldsAreFailures()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        testutil::writeFile(vk + u".exit"_s, "3");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(waitForVerdict(runner));
        const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(cache.failure(choice.key).value_or(QString()).contains(u"exit code 3"_s));

        // a verdict line without "ok" is not a verdict
        QString error;
        QVERIFY(!parseSceneCheckOutput("{\"reasons\":[]}\n", &error).has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(!parseSceneCheckOutput(QByteArray(), &error).has_value());
    }

    void aHangingCheckTimesOutWithoutBlocking()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        testutil::writeFile(vk + u".sleep"_s, "30");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache, 300);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        QElapsedTimer timer;
        timer.start();
        QVERIFY(checkStarted(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner)), runner));
        QVERIFY(timer.elapsed() < 500);
        QVERIFY(waitForVerdict(runner, 5000));
        QVERIFY(timer.elapsed() < 5000);
        const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(!choice.pending);
        QVERIFY2(cache.failure(choice.key).value_or(QString()).contains(u"timed out"_s), qPrintable(choice.label));
    }

    void anEngineThatCannotBeRunIsAFailure()
    {
        const QString vk = m_dir.filePath(u"not-executable"_s);
        testutil::writeFile(vk, "x");
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneCheckRunner runner(&cache);
        runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
        chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY(waitForVerdict(runner));
        const auto choice = chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner));
        QVERIFY2(cache.failure(choice.key).value_or(QString()).contains(u"cannot run"_s), qPrintable(choice.label));
    }

    void extraOutputBeforeTheVerdictIsTolerated()
    {
        QString error;
        const auto verdict = parseSceneCheckOutput("log line\n" + kOk + "\n", &error);
        QVERIFY2(verdict.has_value(), qPrintable(error));
        QVERIFY(verdict->ok);
    }

    void withoutEnginesTheChoiceIsEmptyOrTheOtherOne()
    {
        SceneCheckCache cache(m_dir.filePath(u"cache.json"_s));
        SceneEngineRequest none = request(SceneEngineMode::Auto, QString(), &cache);
        QVERIFY(chooseSceneEngine(none).program.isEmpty());

        // auto with no other engine: the Vulkan engine is all there is, nothing to check against
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kNotOk);
        SceneEngineRequest onlyVk = request(SceneEngineMode::Auto, vk, &cache);
        const auto choice = chooseSceneEngine(onlyVk);
        QCOMPARE(choice.program, vk);
        QVERIFY(choice.vulkan);

    }

    void damagedCacheFileIsIgnored()
    {
        const QString path = m_dir.filePath(u"cache.json"_s);
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        for (const QByteArray &junk : {QByteArray("garbage"), QByteArray("[1]"), QByteArray("{\"version\":99,\"entries\":{}}"),
                                       QByteArray("{\"version\":1,\"entries\":{\"x\":5}}")}) {
            testutil::writeFile(path, junk);
            SceneCheckCache cache(path);
            SceneCheckRunner runner(&cache);
            runner.setSandbox(false); // the fake engines write beside themselves: no Landlock around them
            QVERIFY(checkStarted(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &cache, &runner)), runner)); // ignored, asked again
            QVERIFY(waitForVerdict(runner));
        }
        // and a cache that cannot be written (a directory in the way) still lets the choice through
        QVERIFY(QDir().mkpath(m_dir.filePath(u"blocked/cache.json"_s)));
        SceneCheckCache blocked(m_dir.filePath(u"blocked/cache.json"_s));
        SceneCheckRunner blockedRunner(&blocked);
        blockedRunner.setSandbox(false);
        QVERIFY(checkStarted(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &blocked, &blockedRunner)), blockedRunner));
        QVERIFY(waitForVerdict(blockedRunner));
        QVERIFY(chooseSceneEngine(request(SceneEngineMode::Auto, vk, &blocked, &blockedRunner)).vulkan); // kept in memory
    }

    void cacheStaysBounded()
    {
        const QString vk = makeFakeVulkan(m_dir.path(), u"vk"_s, kOk);
        const QString path = m_dir.filePath(u"cache.json"_s);
        SceneCheckCache cache(path, 20);
        for (int i = 0; i < 50; ++i)
            cache.storeVerdict(u"/item%1"_s.arg(i), u"key%1"_s.arg(i), SceneCheckVerdict{true, {}});
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto entries = QJsonDocument::fromJson(f.readAll()).object().value(u"entries"_s).toObject();
        QVERIFY(entries.size() > 0 && entries.size() <= 20);
    }
};

QTEST_GUILESS_MAIN(TestSceneSelect)
#include "tst_sceneselect.moc"
