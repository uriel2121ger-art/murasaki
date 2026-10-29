#include "sceneenginelookup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace murasaki {
namespace {

QString executableFile(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
}


} // namespace


QString findVulkanSceneEngine()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_SCENE_VK");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    if (const QString onPath = QStandardPaths::findExecutable(QStringLiteral("murasaki-scene-vk")); !onPath.isEmpty())
        return onPath;
    const QString home = QDir::homePath();
    for (const QString &candidate : {home + QStringLiteral("/.local/bin/murasaki-scene-vk"),
                                     home + QStringLiteral("/.local/lib/murasaki/scene/murasaki-scene-vk")}) {
        if (const QString found = executableFile(candidate); !found.isEmpty())
            return found;
    }
    return {};
}

QString findSandboxHelper()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_SANDBOX");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    const QString name = QStringLiteral("murasaki-sandbox");
    if (QCoreApplication::instance()) {
        // Installed next to the program, or in a build tree where every target has its own directory (src/surface and
        // src/sandbox are siblings).
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString &candidate : {here + QLatin1Char('/') + name, here + QStringLiteral("/../sandbox/") + name}) {
            if (const QString found = executableFile(candidate); !found.isEmpty())
                return QDir::cleanPath(found);
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        return onPath;
    return executableFile(QDir::homePath() + QStringLiteral("/.local/bin/") + name);
}

QString findPetImportHelper()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_PETIMPORT");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    const QString name = QStringLiteral("murasaki-petimport");
    if (QCoreApplication::instance()) {
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString &candidate : {here + QLatin1Char('/') + name, here + QStringLiteral("/../import/") + name}) {
            if (const QString found = executableFile(candidate); !found.isEmpty())
                return QDir::cleanPath(found);
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        return onPath;
    return executableFile(QDir::homePath() + QStringLiteral("/.local/bin/") + name);
}

QString findPetAudioHelper()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_PETAUDIO");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    const QString name = QStringLiteral("murasaki-petaudio");
    if (QCoreApplication::instance()) {
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString &candidate : {here + QLatin1Char('/') + name, here + QStringLiteral("/../audio/") + name}) {
            if (const QString found = executableFile(candidate); !found.isEmpty())
                return QDir::cleanPath(found);
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        return onPath;
    return executableFile(QDir::homePath() + QStringLiteral("/.local/bin/") + name);
}

QString findScreenMatchHelper()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_SCREENMATCH");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    const QString name = QStringLiteral("murasaki-screenmatch");
    if (QCoreApplication::instance()) {
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString &candidate : {here + QLatin1Char('/') + name, here + QStringLiteral("/../screenmatch/") + name}) {
            if (const QString found = executableFile(candidate); !found.isEmpty())
                return QDir::cleanPath(found);
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        return onPath;
    return executableFile(QDir::homePath() + QStringLiteral("/.local/bin/") + name);
}

QString findPuppetHelper()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_PUPPET");
    if (!fromEnv.isEmpty())
        return executableFile(fromEnv);
    const QString name = QStringLiteral("murasaki-puppet");
    if (QCoreApplication::instance()) {
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString &candidate : {here + QLatin1Char('/') + name, here + QStringLiteral("/../puppet/") + name}) {
            if (const QString found = executableFile(candidate); !found.isEmpty())
                return QDir::cleanPath(found);
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        return onPath;
    return executableFile(QDir::homePath() + QStringLiteral("/.local/bin/") + name);
}

} // namespace murasaki
