#pragma once

#include <QString>
#include <QStringList>

namespace murasaki {

// Finds Steam library folders and, per app, its install dir and Workshop content dir.
// libraries() and appInstallDir() are canonical (symlinks resolved). workshopDir() and workshopManifest()
// are a canonical library path plus a fixed suffix.
// appInstallDir() rejects installdir values that are not a single folder name, but follows a symlinked
// common/<installdir> to wherever it points. App ids must be ASCII digits; results are empty when not found.
class SteamLocator
{
public:
    explicit SteamLocator(const QStringList &rootCandidates);

    // $MURASAKI_STEAM_ROOT first, then the usual native, Debian, Flatpak and Snap locations.
    static QStringList defaultRootCandidates();

    QStringList libraries() const { return m_libraries; }
    QString appInstallDir(const QString &appId) const;
    QString workshopDir(const QString &appId) const;
    QString workshopManifest(const QString &appId) const;

    // Where the Workshop content dir / appworkshop manifest of an app would be in every library, in library
    // order, whether or not they exist yet (Steam creates them on the first subscription). Empty for an
    // invalid app id. Meant for watching, so items subscribed later are picked up.
    QStringList workshopDirCandidates(const QString &appId) const;
    QStringList workshopManifestCandidates(const QString &appId) const;

    // <library>/steamapps/workshop of every library (the dir holding appworkshop_<id>.acf, content/ and
    // downloads/), whether or not it exists yet. Independent of the app id.
    QStringList workshopRoots() const;

    // Where SteamCMD downloads for Murasaki ($MURASAKI_STEAMCMD_DIR, else $XDG_DATA_HOME/murasaki/steamcmd): its
    // force_install_dir. Its Workshop root (steamapps/workshop, holding the .acf and content/) and the content dir of
    // an app in it are read like Steam's, whether or not they exist yet.
    static QString steamCmdInstallDir();
    static QString steamCmdWorkshopRoot(const QString &installDir);
    static QString steamCmdContentDir(const QString &installDir, const QString &appId);

private:
    QStringList m_libraries;
};

} // namespace murasaki
