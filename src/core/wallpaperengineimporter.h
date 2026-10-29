#pragma once

#include "wallpaperitem.h"

#include <optional>

namespace murasaki {

// Turns Wallpaper Engine item directories (Workshop or myprojects) into WallpaperItems.
class WallpaperEngineImporter
{
public:
    // nullopt if the dir has no project.json (e.g. a download in progress).
    static std::optional<WallpaperItem> scanItem(const QString &itemDir);
    // Every item directly under dir. Presets stay unresolved until resolveDependencies().
    static QList<WallpaperItem> scanDirectory(const QString &dir);
    // Points presets at their base items and applies preset values (see Task 7).
    static void resolveDependencies(QList<WallpaperItem> &items);
};

} // namespace murasaki
