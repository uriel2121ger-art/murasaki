#include "sceneenginelookup.h"
#include "testutil.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSceneEngineLookup : public QObject
{
    Q_OBJECT

    static QString makeExecutable(const QString &path)
    {
        testutil::writeFile(path, "#!/bin/sh\n");
        QFile(path).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }

private slots:
    void init()
    {
        m_oldPath = qgetenv("PATH");
        m_oldHome = qgetenv("HOME");
        qunsetenv("MURASAKI_SCENE_ENGINE");
        qunsetenv("MURASAKI_SCENE_VK");
        qunsetenv("MURASAKI_SANDBOX");
    }
    void cleanup()
    {
        qputenv("PATH", m_oldPath);
        qputenv("HOME", m_oldHome);
        qunsetenv("MURASAKI_SCENE_ENGINE");
        qunsetenv("MURASAKI_SCENE_VK");
        qunsetenv("MURASAKI_SANDBOX");
    }

    void sandboxHelperIsLookedUpLikeTheEngines()
    {
        QTemporaryDir home;
        QTemporaryDir bin;
        QVERIFY(home.isValid() && bin.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("PATH", bin.path().toUtf8());
        QVERIFY(findSandboxHelper().isEmpty());
        const QString local = makeExecutable(home.filePath(u".local/bin/murasaki-sandbox"_s));
        QCOMPARE(findSandboxHelper(), local);
        const QString onPath = makeExecutable(bin.filePath(u"murasaki-sandbox"_s));
        QCOMPARE(findSandboxHelper(), onPath);
        // An explicit choice is the only candidate.
        const QString chosen = makeExecutable(home.filePath(u"chosen"_s));
        qputenv("MURASAKI_SANDBOX", chosen.toUtf8());
        QCOMPARE(findSandboxHelper(), chosen);
        qputenv("MURASAKI_SANDBOX", home.filePath(u"missing"_s).toUtf8());
        QVERIFY(findSandboxHelper().isEmpty());
    }


    void vulkanEngineEnvironmentIsExplicit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString exe = makeExecutable(dir.filePath(u"vk"_s));
        qputenv("HOME", dir.path().toUtf8());
        makeExecutable(dir.filePath(u".local/bin/murasaki-scene-vk"_s));
        qputenv("MURASAKI_SCENE_VK", exe.toUtf8());
        QCOMPARE(findVulkanSceneEngine(), exe);
        qputenv("MURASAKI_SCENE_VK", dir.filePath(u"missing"_s).toUtf8());
        QVERIFY(findVulkanSceneEngine().isEmpty()); // an explicit override never falls back
        qunsetenv("MURASAKI_SCENE_VK");
    }

    void vulkanEngineSearchOrder()
    {
        QTemporaryDir home;
        QTemporaryDir bin;
        QVERIFY(home.isValid() && bin.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("PATH", bin.path().toUtf8());
        qunsetenv("MURASAKI_SCENE_VK");
        QVERIFY(findVulkanSceneEngine().isEmpty());
        testutil::writeFile(home.filePath(u".local/bin/murasaki-scene-vk"_s), "x"); // not executable
        QVERIFY(findVulkanSceneEngine().isEmpty());
        const QString lib = makeExecutable(home.filePath(u".local/lib/murasaki/scene/murasaki-scene-vk"_s));
        QCOMPARE(findVulkanSceneEngine(), lib);
        const QString local = makeExecutable(home.filePath(u".local/bin/murasaki-scene-vk"_s));
        QCOMPARE(findVulkanSceneEngine(), local);
        const QString onPath = makeExecutable(bin.filePath(u"murasaki-scene-vk"_s));
        QCOMPARE(findVulkanSceneEngine(), onPath);
    }

private:
    QByteArray m_oldPath;
    QByteArray m_oldHome;
};

QTEST_GUILESS_MAIN(TestSceneEngineLookup)
#include "tst_sceneenginelookup.moc"
