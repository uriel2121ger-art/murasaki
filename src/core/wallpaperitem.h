#pragma once

#include "itemrisks.h"
#include "weproperty.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace murasaki {

// Asset is not a wallpaper: a pack of pieces (clocks, visualizers) for the Wallpaper Engine editor. It has no "type" in its
// project.json, only "category": "Asset" and "file": "assets.json".
enum class ItemKind { Unknown, Scene, Video, Web, Pet, Asset };
enum class ItemStatus { Ok, MissingDependency, Invalid };

QString itemKindToString(ItemKind kind);
ItemKind itemKindFromString(const QString &kind); // case-insensitive
QString itemStatusToString(ItemStatus status);

// A variant at least this large (either side, in pixels) or this big (bytes) is transcoded to a cheaper
// format before it is shown as a pet.
inline constexpr int kHeavyVariantPixels = 800;
inline constexpr qint64 kHeavyVariantBytes = 50LL * 1024 * 1024;

// One animation file of a pet item (Anima Engine items may ship several).
struct PetVariant {
    QString name; // file name without extension
    QString file; // absolute path
    int width = 0;
    int height = 0;
    double fps = 0;
    int frames = 0;
    qint64 bytes = 0;

    bool isHeavy() const
    {
        return width >= kHeavyVariantPixels || height >= kHeavyVariantPixels || bytes >= kHeavyVariantBytes;
    }
    QJsonObject toJson() const;
};

struct WallpaperItem {
    QString id;          // "we:<item dir name>", or "anima:<item dir name>" for pets
    QString source;      // "wallpaper-engine" or "anima-engine"
    QString dir;         // absolute item dir
    QString title;
    ItemKind kind = ItemKind::Unknown;
    QString entryFile;   // absolute; presets point into their base item
    QString contentDir;  // dir an engine must load; a preset's base item dir
    QString previewPath; // absolute, or empty
    QStringList tags;
    QString contentRating;
    QString workshopId;
    QString dependencyId; // base item Workshop id, for presets
    QJsonObject presetValues;
    QList<WeProperty> properties;
    // general.localization: {locale: {key: text}}, locales lowercase as written by Wallpaper Engine ("en-us").
    QJsonObject localization;
    QList<PetVariant> variants; // pets only: the animation files, sorted by name
    // "puppet" for a puppet pet (a model that is drawn by murasaki-puppet, not an animation file; docs/pets/puppets.md); empty for every other item.
    QString format;
    // A puppet's facts: backend, version, counts, motion groups, expressions, hit areas, canvas, reasons and degraded names (the item's "puppet" object, API revision 20).
    QJsonObject puppet;
    // Programs found in the item folder, or project.json type application. Murasaki never runs them.
    QList<ItemRisk> risks;
    ItemStatus status = ItemStatus::Ok;
    QString statusDetail;

    // Light form for listings: no paths to entry files, no properties, no variants (pets get variantCount).
    QJsonObject toSummaryJson() const;
    QJsonObject toJson() const;
};

} // namespace murasaki
