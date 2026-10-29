#pragma once

#include "workshopwatcher.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace murasaki {

// Tells from Steam's local files alone (no network) whether a Workshop item is installed, downloading or absent.
//
// For every `steamapps/workshop` dir given (one per Steam library) and the app id, the state of item <id> is:
//   "downloading"  downloads/<appid>/<id> exists in any library, else
//   "installed"    <id> is listed in WorkshopItemsInstalled of appworkshop_<appid>.acf and
//                  content/<appid>/<id> exists in the same library, else
//   "none".
// A malformed or unreadable .acf counts as listing nothing (one qWarning per load); a missing .acf is normal.
// The .acf is parsed on construction and after every change; the directories are checked on each state() call.
//
// changed() is emitted, debounced (300 ms), after activity on the .acf files and downloads dirs. Their parent
// dirs are watched too, so creation and Steam's atomic replace of the .acf are noticed, and paths that
// disappear are re-added.
class WorkshopState : public QObject
{
    Q_OBJECT

public:
    static constexpr auto Installed = "installed";
    static constexpr auto Downloading = "downloading";
    static constexpr auto None = "none";

    explicit WorkshopState(const QStringList &workshopRoots, const QString &appId = QStringLiteral("431960"),
                           QObject *parent = nullptr);

    QString state(const QString &workshopId) const;

signals:
    void changed();

private:
    struct Library
    {
        QString root;
        QSet<QString> installed;
    };

    void reload();

    QString m_appId;
    QList<Library> m_libraries;
    WorkshopWatcher m_watcher;
};

} // namespace murasaki
