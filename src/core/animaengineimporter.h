#pragma once

#include "wallpaperitem.h"

#include <optional>

namespace murasaki {

// Turns Anima Engine Workshop item directories (Steam app 3474900) into pet WallpaperItems.
// An item is "anima:<dir name>" with kind Pet. Every apng, gif or webp file directly inside the dir whose header
// reads as an animation is a variant, sorted by name; files that are still downloading or broken are skipped.
class AnimaEngineImporter
{
public:
    // nullopt if the dir holds no file with an animation extension (e.g. a download in progress). An item whose
    // animation files are all unreadable is returned with status Invalid.
    static std::optional<WallpaperItem> scanItem(const QString &itemDir);
    // Every item directly under dir.
    static QList<WallpaperItem> scanDirectory(const QString &dir);
};

} // namespace murasaki
