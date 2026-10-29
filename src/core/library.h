#pragma once

#include "wallpaperitem.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

#include <optional>

namespace murasaki {

// Merged index of every known item. Rescans are synchronous (project.json only) and report a diff.
class Library : public QObject
{
    Q_OBJECT

public:
    explicit Library(QObject *parent = nullptr);

    void setWorkshopDirs(const QStringList &dirs) { m_workshopDirs = dirs; }
    QStringList workshopDirs() const { return m_workshopDirs; }
    // Anima Engine (pet) Workshop content dirs, one per Steam library. Pet ids are "anima:<dir name>", so they
    // never collide with "we:" ids; the duplicate rule below applies within each kind.
    void setAnimaDirs(const QStringList &dirs) { m_animaDirs = dirs; }
    QStringList animaDirs() const { return m_animaDirs; }

    // Files of local pets (canonical paths, see LocalPetImporter). Their ids are "local:<hash>".
    void setLocalPetFiles(const QStringList &files) { m_localPetFiles = files; }
    // The puppet folders the user added (LocalPuppetImporter), and where the answer of their deeper check is found (the daemon's PuppetChecker; empty object: not run).
    void setLocalPuppetFolders(const QStringList &folders) { m_localPuppetFolders = folders; }
    QStringList localPuppetFolders() const { return m_localPuppetFolders; }
    void setPuppetCheckProvider(std::function<QJsonObject(const QString &)> provider) { m_puppetCheck = std::move(provider); }
    QStringList localPetFiles() const { return m_localPetFiles; }

    // Called for every pet item after it is scanned and before it is compared with the previous scan, so the
    // daemon can fill in Workshop metadata (title, tags, preview). A rescan picks up what it changes.
    void setPetDecorator(std::function<void(WallpaperItem &)> decorator) { m_petDecorator = std::move(decorator); }

    // Sorted by title (case-insensitive), then id.
    QList<WallpaperItem> items() const;
    std::optional<WallpaperItem> item(const QString &id) const;

public slots:
    // Ids are "we:<dir name>", so the same name in two Workshop dirs collides: among colliding entries the first with status Ok wins, else the first.
    void rescan();

signals:
    // Sorted id lists; emitted only when at least one is non-empty.
    void itemsChanged(const QStringList &added, const QStringList &changed, const QStringList &removed);

private:
    QStringList m_workshopDirs;
    QStringList m_animaDirs;
    QStringList m_localPetFiles;
    QStringList m_localPuppetFolders;
    std::function<QJsonObject(const QString &)> m_puppetCheck;
    std::function<void(WallpaperItem &)> m_petDecorator;
    QHash<QString, WallpaperItem> m_items;
    QHash<QString, QByteArray> m_fingerprints; // compact JSON per id, used for diffing
};

} // namespace murasaki
