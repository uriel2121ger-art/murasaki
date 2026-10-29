#pragma once

#include "petlimits.h"
#include "wallpaperitem.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace murasaki {

// Pets from animation files the user picked (GIF, WebP or APNG). They are the same item type as Workshop pets:
// "local:<sha1 of the canonical path>", source "local", kind Pet, one variant. Nothing is copied or linked; the
// store only remembers the paths.
class LocalPetImporter
{
public:
    // The canonical path of a file that can be a local pet, or an empty string with *error set (relative path,
    // missing file, unknown extension, header that does not read as an animation).
    static QString validate(const QString &path, QString *error = nullptr);
    static QString itemIdFor(const QString &canonicalPath);
    // A pet item for the file. A file that vanished or no longer reads yields an Invalid item without variants, so
    // the user can see and remove it.
    static WallpaperItem scanItem(const QString &path);
};

// The remembered files ($XDG_CONFIG_HOME/murasaki/local-pets.json), written atomically.
class LocalPetStore : public QObject
{
    Q_OBJECT

public:
    explicit LocalPetStore(QString filePath, QObject *parent = nullptr);
    // How an item id is made from a stored path (for remove()); the default is LocalPetImporter::itemIdFor. The puppet folders' store uses LocalPuppetImporter's.
    void setItemIdFunction(std::function<QString(const QString &)> idOf) { m_idOf = std::move(idOf); }

    // A missing file means no local pets; an unreadable or invalid file is renamed to <file>.bad-<UTC time> and the
    // store starts empty (saving stays disabled if that rename fails, so the user's file is never overwritten).
    void load();

    QStringList files() const { return m_files; }
    // The path that was stored under the item id. nullopt when there is none, or when saving failed (then
    // *error says so and nothing changed).
    std::optional<QString> remove(const QString &itemId, QString *error = nullptr);
    // False without signalling when the file is stored already, the limit is reached or the file could not be
    // saved (the store is unchanged; *error says why, empty for "already stored").
    bool add(const QString &canonicalPath, QString *error = nullptr);

signals:
    void changed();

private:
    bool save() const;

    QString m_path;
    QStringList m_files;
    bool m_saveBlocked = false;
    std::function<QString(const QString &)> m_idOf;
};

inline constexpr int kMaxLocalPets = 256;
// Larger files are refused: the header readers are cheap, but a decoder opens the file later.
inline constexpr qint64 kMaxLocalPetBytes = kMaxPetFileBytes;

} // namespace murasaki
