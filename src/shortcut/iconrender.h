#pragma once

// The icon of an application as the engine gets it: 256 x 256 RGBA8 (straight alpha), the picture scaled to fit and centred on a
// transparent canvas. `Icon=` of a desktop entry is a theme name or an absolute path; both are untrusted (any local program writes
// .desktop files): a path must resolve inside an icon directory, be a regular file of at most 4 MiB and decode within 4096 pixels a
// side and 16 Mpixel; vector files are only read through the theme engine (no SVG decoding of arbitrary paths here).

#include <QImage>
#include <QString>
#include <QStringList>

#include <optional>

namespace murasaki::shortcut {

inline constexpr int kIconSide = 256;
inline constexpr qint64 kMaxIconFileBytes = 4 * 1024 * 1024;
inline constexpr int kMaxIconSourceSide = 4096;
inline constexpr qint64 kMaxIconSourcePixels = 16 * 1000 * 1000;

// The directories an absolute icon path may be in.
QStringList iconRoots();

// Whether `spec` is an absolute path (as opposed to a theme name).
bool isIconPath(const QString &spec);

// A raster file at `path` (inside one of `roots`, resolved first), decoded within the limits. Thread safe. Nullopt otherwise.
std::optional<QImage> loadIconFile(const QString &path, const QStringList &roots);

// Where the icon called `name` (a plain name: no path separators, at most 256 bytes) is in the icon theme `theme` (with the themes it
// inherits, up to four levels, and hicolor) under the directories `searchPaths`, found by looking at the files themselves: the biggest
// png (or an svg when the build reads them) of the "apps" directories, then the pixmaps directory. Nothing of Qt's theme engine, which is
// not for a worker thread. Thread safe. Nullopt when there is none.
std::optional<QString> findThemeIcon(const QString &name, const QString &theme, const QStringList &searchPaths);

// A file found that way, decoded: a raster by loadIconFile, an SVG by QtSvg when the build has it (the file is at most
// kMaxIconFileBytes and drawn straight into kIconSide x kIconSide; nothing it references is fetched from a network). Thread safe.
std::optional<QImage> loadThemeIconFile(const QString &path, const QStringList &roots);

// `image` fitted into kIconSide x kIconSide: kIconSide * kIconSide * 4 bytes of RGBA8, or nothing for an empty image. Thread safe.
std::optional<QByteArray> fitIcon(const QImage &image);

} // namespace murasaki::shortcut
