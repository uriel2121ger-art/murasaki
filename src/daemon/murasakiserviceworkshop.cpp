#include "murasakiservice.h"
#include "murasakiserviceinternal.h"

#include "automationmanager.h"

#include "localpets.h"
#include "localpuppets.h"
#include "petaudiomanager.h"
#include "puppetchecker.h"
#include "importablefiles.h"

#include "constants.h"
#include "playlistmanager.h"
#include "shortcut/launchassigned.h"
#include "shortcut/shortcutpolicy.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr qint64 kAutoImportMaxBytes = 256LL << 20; // automatic imports skip bigger files

} // namespace

void MurasakiService::autoImportWorkshopFiles()
{
    if (!m_imports || !m_localPets)
        return;
    const ImportSettings &prefs = m_settings->importSettings();
    const bool files = prefs.workshopFiles == u"auto"_s, packages = prefs.workshopMdaa == u"auto"_s;
    if (!files && !packages)
        return;
    bool changed = false;
    bool queueFull = false;
    for (const WallpaperItem &item : m_library->items()) {
        if (item.source != u"anima-engine"_s || item.kind != ItemKind::Pet)
            continue;
        for (const ImportableFile &file : findImportableFiles(item.dir)) {
            // A ZIP and a video follow importWorkshopFiles, a package its own setting.
            if (!(((file.kind == u"zip"_s || file.kind == u"video"_s) && files) || (file.kind == u"mdaa"_s && packages)))
                continue;
            const QString key = file.path + u'\n' + QString::number(file.size) + u'\n' + QString::number(file.modified);
            if (m_autoImported.contains(key))
                continue;
            // A big file is never imported behind the user's back: it stays "Import this file" on the item page.
            if (file.size > kAutoImportMaxBytes)
                continue;
            QJsonObject spec{{u"kind"_s, file.kind}, {u"name"_s, item.title.left(100)}, {u"sources"_s, QJsonArray{file.path}}, {u"format"_s, prefs.format}};
            if (file.kind == u"zip"_s) {
                spec.insert(u"fps"_s, prefs.fps);
                spec.insert(u"sizePolicy"_s, prefs.sizePolicy);
            }
            if (file.kind == u"video"_s) {
                spec.insert(u"maxSide"_s, prefs.videoMaxSide);
                spec.insert(u"maxFps"_s, prefs.videoMaxFps);
                if (prefs.keepAudio)
                    spec.insert(u"keepAudio"_s, true);
            }
            if (prefs.keepSource)
                spec.insert(u"keepSource"_s, true);
            QString problem;
            if (!m_imports->submit(QJsonDocument(spec).toJson(QJsonDocument::Compact), &problem).isEmpty()) {
                m_autoImported.insert(key);
                changed = true;
            } else {
                queueFull = true; // the next finished job brings this file back
                break;
            }
        }
        if (queueFull)
            break; // (not only this item: nothing more fits now)
    }
    if (changed && !m_autoImportedFile.isEmpty()) {
        QSaveFile out(m_autoImportedFile);
        const QByteArray bytes = QJsonDocument(QJsonArray::fromStringList(QStringList(m_autoImported.begin(), m_autoImported.end()))).toJson(QJsonDocument::Compact);
        // If this cannot be saved the files are imported again at the next start (and a pet the user removed comes back): say so.
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
            qWarning().noquote() << "murasakid: cannot save" << m_autoImportedFile << "; automatic imports will be repeated at the next start";
    }
}

void MurasakiService::attachSteamCmd(SteamCmdManager *steamcmd)
{
    m_steamcmd = steamcmd;
    connect(m_steamcmd, &SteamCmdManager::changed, this, &MurasakiService::SteamCmdChanged);
}

QString MurasakiService::SteamCmdStatus() const
{
    if (!m_steamcmd) // a daemon without the manager reports what a machine without SteamCMD does
        return QString::fromUtf8(QJsonDocument(QJsonObject{{u"available"_s, false},
                                                           {u"program"_s, QString()},
                                                           {u"state"_s, u"unavailable"_s},
                                                           {u"user"_s, QString()},
                                                           {u"prompt"_s, QString()},
                                                           {u"message"_s, QString()},
                                                           {u"jobs"_s, QJsonArray()}})
                                     .toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QJsonDocument(m_steamcmd->toJson()).toJson(QJsonDocument::Compact));
}

void MurasakiService::SteamCmdLogin(const QString &user)
{
    if (!m_steamcmd) {
        rejectCall(u"SteamCMD is not available"_s);
        return;
    }
    QString problem;
    if (!m_steamcmd->login(user, &problem))
        rejectCall(problem);
}

void MurasakiService::SteamCmdSubmit(const QString &kind, const QString &secret)
{
    if (!m_steamcmd) {
        rejectCall(u"SteamCMD is not available"_s);
        return;
    }
    QString problem;
    if (!m_steamcmd->submit(kind, secret, &problem))
        rejectCall(problem);
}

void MurasakiService::SteamCmdCancelLogin()
{
    if (m_steamcmd)
        m_steamcmd->cancelLogin();
}

void MurasakiService::SteamCmdLogout()
{
    if (m_steamcmd)
        m_steamcmd->logout();
}

void MurasakiService::SteamCmdDownload(const QString &appId, const QString &itemId)
{
    if (!m_steamcmd) {
        rejectCall(u"SteamCMD is not available"_s);
        return;
    }
    QString problem;
    if (!m_steamcmd->download(appId, itemId, &problem))
        rejectCall(problem);
}

void MurasakiService::SteamCmdCancel(const QString &itemId)
{
    if (m_steamcmd)
        m_steamcmd->cancel(itemId);
}

} // namespace murasaki
