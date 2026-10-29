#include "steamlocator.h"

#include "vdf.h"

#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

void addLibrary(QStringList &libraries, const QString &path)
{
    if (!QDir::isAbsolutePath(path) || !QFileInfo(path + u"/steamapps"_s).isDir())
        return;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty() && !libraries.contains(canonical))
        libraries.append(canonical);
}

bool isValidAppId(const QString &appId)
{
    if (appId.isEmpty())
        return false;
    for (const QChar c : appId) {
        if (c < u'0' || c > u'9')
            return false;
    }
    return true;
}

} // namespace

SteamLocator::SteamLocator(const QStringList &rootCandidates)
{
    for (const QString &root : rootCandidates) {
        addLibrary(m_libraries, root);
        const auto vdf = readVdfFile(root + u"/steamapps/libraryfolders.vdf"_s);
        if (!vdf)
            continue;
        const QVariantMap folders = vdfValue(*vdf, u"libraryfolders").toMap();
        // Keep Steam's order: numeric keys ascending, non-numeric keys last.
        QStringList keys = folders.keys();
        std::stable_sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
            bool okA = false, okB = false;
            const int na = a.toInt(&okA), nb = b.toInt(&okB);
            if (okA != okB)
                return okA;
            return okA && na < nb;
        });
        for (const QString &key : keys) {
            const QVariant entry = folders.value(key);
            // Current format: "0" { "path" "…" }. Legacy format: "1" "/path".
            const QString path = entry.typeId() == QMetaType::QVariantMap
                ? vdfValue(entry.toMap(), u"path").toString()
                : entry.toString();
            addLibrary(m_libraries, path);
        }
    }
}

QStringList SteamLocator::defaultRootCandidates()
{
    QStringList candidates;
    if (const QString env = qEnvironmentVariable("MURASAKI_STEAM_ROOT"); !env.isEmpty())
        candidates << env;
    const QString home = QDir::homePath();
    candidates << home + u"/.steam/root"_s << home + u"/.steam/steam"_s << home + u"/.local/share/Steam"_s
               << home + u"/.steam/debian-installation"_s
               << home + u"/.var/app/com.valvesoftware.Steam/.local/share/Steam"_s
               << home + u"/snap/steam/common/.local/share/Steam"_s;
    return candidates;
}

QString SteamLocator::appInstallDir(const QString &appId) const
{
    if (!isValidAppId(appId))
        return {};
    for (const QString &library : m_libraries) {
        const auto acf = readVdfFile(library + u"/steamapps/appmanifest_"_s + appId + u".acf"_s);
        if (!acf)
            continue;
        const QString installDir = vdfValue(vdfValue(*acf, u"AppState").toMap(), u"installdir").toString();
        if (installDir.isEmpty())
            continue;
        // installdir is always a single folder name. It may be a symlink to another disk, so the
        // canonical result is allowed to leave common/.
        if (installDir == u"."_s || installDir == u".."_s || installDir.contains(u'/') || installDir.contains(u'\\'))
            continue;
        const QFileInfo info(library + u"/steamapps/common/"_s + installDir);
        if (!info.isDir())
            continue;
        return info.canonicalFilePath();
    }
    return {};
}

QString SteamLocator::workshopDir(const QString &appId) const
{
    if (!isValidAppId(appId))
        return {};
    for (const QString &library : m_libraries) {
        const QString dir = library + u"/steamapps/workshop/content/"_s + appId;
        if (QFileInfo(dir).isDir())
            return dir;
    }
    return {};
}

QString SteamLocator::workshopManifest(const QString &appId) const
{
    if (!isValidAppId(appId))
        return {};
    for (const QString &library : m_libraries) {
        const QString file = library + u"/steamapps/workshop/appworkshop_"_s + appId + u".acf"_s;
        if (QFileInfo(file).isFile())
            return file;
    }
    return {};
}

QStringList SteamLocator::workshopDirCandidates(const QString &appId) const
{
    QStringList result;
    if (!isValidAppId(appId))
        return result;
    for (const QString &library : m_libraries)
        result << library + u"/steamapps/workshop/content/"_s + appId;
    return result;
}

QStringList SteamLocator::workshopManifestCandidates(const QString &appId) const
{
    QStringList result;
    if (!isValidAppId(appId))
        return result;
    for (const QString &library : m_libraries)
        result << library + u"/steamapps/workshop/appworkshop_"_s + appId + u".acf"_s;
    return result;
}

QStringList SteamLocator::workshopRoots() const
{
    QStringList result;
    for (const QString &library : m_libraries)
        result << library + u"/steamapps/workshop"_s;
    return result;
}

QString SteamLocator::steamCmdInstallDir()
{
    const QString fromEnv = qEnvironmentVariable("MURASAKI_STEAMCMD_DIR");
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/murasaki/steamcmd"_s;
}

QString SteamLocator::steamCmdWorkshopRoot(const QString &installDir)
{
    return installDir + u"/steamapps/workshop"_s;
}

QString SteamLocator::steamCmdContentDir(const QString &installDir, const QString &appId)
{
    return steamCmdWorkshopRoot(installDir) + u"/content/"_s + appId;
}

} // namespace murasaki
