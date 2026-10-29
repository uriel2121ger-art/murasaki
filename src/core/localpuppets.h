#pragma once

#include "wallpaperitem.h"

#include <QJsonObject>
#include <QString>

namespace murasaki {

// Puppet pets: a folder the user picked that holds one Inochi2D `.inp` or one Cubism `*.model3.json` and what it names (docs/pets/puppets.md). The folder is not copied or converted; the store
// (a LocalPetStore on local-puppets.json) only remembers its canonical path. The item is "local:puppet:<sha1 of the canonical path>", source "local", kind Pet, format "puppet".
//
// The daemon validates the manifest only (loadManifest: names, sizes, the path rules, no links, the limits table); the deeper check runs in the sandbox (`murasaki-puppet --check`, PuppetChecker) and its
// answer is merged into the item. A puppet item has these variants, all with the model file as `file`, the canvas as its size and no frames: "model" (the base), then "motion:<Group>" for each motion
// group and "expression:<Name>" for each expression; a pet's states name them (docs/pets/states.md).
class LocalPuppetImporter
{
public:
    // The canonical path of a folder that can be a puppet, or an empty string with *error set (relative path, not a folder, the manifest's reasons).
    static QString validate(const QString &path, QString *error = nullptr);
    static QString itemIdFor(const QString &canonicalPath);
    // The item for the folder. `check` is the JSON object `murasaki-puppet --check` printed for it (empty: not run yet). A folder that vanished or no longer reads yields an Invalid item without
    // variants, so the user can see and remove it.
    static WallpaperItem scanItem(const QString &folder, const QJsonObject &check = {});
};

inline constexpr int kMaxLocalPuppets = 64;

} // namespace murasaki
