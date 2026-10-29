#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace murasaki {

inline constexpr int kMaxPermissionGrants = 4096;

// What the user allowed individual items to do ($XDG_CONFIG_HOME/murasaki/permissions.json). Everything is denied
// until granted. Today the only permission is "network": a web wallpaper may reach the internet.
class ItemPermissionStore : public QObject
{
    Q_OBJECT

public:
    explicit ItemPermissionStore(QString filePath, QObject *parent = nullptr);

    QString filePath() const { return m_path; }

    // Same contract as AssignmentStore::load: a missing file means no grants; an unreadable or invalid file is
    // renamed to <file>.bad-<UTC time> and the store starts empty (nothing allowed); if that rename fails, saving
    // stays disabled so the user's file is never overwritten.
    void load();

    bool network(const QString &itemId) const { return m_network.contains(itemId); }
    QSet<QString> networkItems() const { return m_network; }

    // Grants or revokes. Returns true when the state afterwards is the requested one (changed() fires only when it
    // differs from before). False with *error set for an invalid id, too many grants, or a failed write (then
    // nothing changed).
    bool setNetwork(const QString &itemId, bool allowed, QString *error = nullptr);

signals:
    void changed(const QString &itemId);

private:
    bool save() const;

    QString m_path;
    QSet<QString> m_network;
    bool m_saveBlocked = false;
};

} // namespace murasaki
