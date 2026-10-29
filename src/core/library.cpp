#include "library.h"

#include "animaengineimporter.h"
#include "localpets.h"
#include "localpuppets.h"
#include "wallpaperengineimporter.h"

#include <QJsonDocument>

#include <algorithm>
#include <utility>

namespace murasaki {

Library::Library(QObject *parent)
    : QObject(parent)
{
}

QList<WallpaperItem> Library::items() const
{
    QList<WallpaperItem> list = m_items.values();
    std::sort(list.begin(), list.end(), [](const WallpaperItem &a, const WallpaperItem &b) {
        const int byTitle = QString::compare(a.title, b.title, Qt::CaseInsensitive);
        return byTitle != 0 ? byTitle < 0 : a.id < b.id;
    });
    return list;
}

std::optional<WallpaperItem> Library::item(const QString &id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.cend())
        return std::nullopt;
    return *it;
}

void Library::rescan()
{
    QList<WallpaperItem> scanned;
    QHash<QString, qsizetype> indexById; // id -> index into scanned
    for (const QString &dir : std::as_const(m_workshopDirs)) {
        QList<WallpaperItem> found = WallpaperEngineImporter::scanDirectory(dir);
        for (WallpaperItem &item : found) {
            const auto it = indexById.constFind(item.id);
            if (it == indexById.cend()) {
                indexById.insert(item.id, scanned.size());
                scanned.append(std::move(item));
            } else if (scanned[*it].status != ItemStatus::Ok && item.status == ItemStatus::Ok) {
                scanned[*it] = std::move(item); // same rule as the importer: prefer a playable duplicate
            }
        }
    }
    WallpaperEngineImporter::resolveDependencies(scanned);
    // Pets come after the dependency pass: they have no presets and must not take part in it.
    for (const QString &dir : std::as_const(m_animaDirs)) {
        QList<WallpaperItem> found = AnimaEngineImporter::scanDirectory(dir);
        for (WallpaperItem &item : found) {
            if (m_petDecorator)
                m_petDecorator(item);
            const auto it = indexById.constFind(item.id);
            if (it == indexById.cend()) {
                indexById.insert(item.id, scanned.size());
                scanned.append(std::move(item));
            } else if (scanned[*it].status != ItemStatus::Ok && item.status == ItemStatus::Ok) {
                scanned[*it] = std::move(item);
            }
        }
    }
    for (const QString &file : std::as_const(m_localPetFiles)) {
        WallpaperItem item = LocalPetImporter::scanItem(file);
        if (!indexById.contains(item.id)) {
            indexById.insert(item.id, scanned.size());
            scanned.append(std::move(item));
        }
    }

    for (const QString &folder : std::as_const(m_localPuppetFolders)) {
        WallpaperItem item = LocalPuppetImporter::scanItem(folder, m_puppetCheck ? m_puppetCheck(folder) : QJsonObject());
        if (!indexById.contains(item.id)) {
            indexById.insert(item.id, scanned.size());
            scanned.append(std::move(item));
        }
    }

    QHash<QString, WallpaperItem> items;
    QHash<QString, QByteArray> fingerprints;
    QStringList added, changed, removed;
    // Fingerprints only cover project.json-derived data: edits to content/preview files at the same path go unnoticed.
    for (WallpaperItem &item : scanned) {
        const QByteArray fingerprint = QJsonDocument(item.toJson()).toJson(QJsonDocument::Compact);
        const auto old = m_fingerprints.constFind(item.id);
        if (old == m_fingerprints.cend())
            added.append(item.id);
        else if (*old != fingerprint)
            changed.append(item.id);
        fingerprints.insert(item.id, fingerprint);
        items.insert(item.id, std::move(item));
    }
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        if (!items.contains(it.key()))
            removed.append(it.key());
    }

    m_items = std::move(items);
    m_fingerprints = std::move(fingerprints);
    if (added.isEmpty() && changed.isEmpty() && removed.isEmpty())
        return;
    added.sort();
    changed.sort();
    removed.sort();
    emit itemsChanged(added, changed, removed);
}

} // namespace murasaki
