#pragma once

#include <QString>
#include <QStringList>

namespace murasaki {

// Joins a relative path onto a canonical dir, lexically only. Returns empty when relative is empty or absolute,
// or when the result leaves the dir.
QString confinedPath(const QString &canonicalDir, const QString &relative);

// confinedPath plus symlink resolution, for serving files to untrusted content: returns the canonical path of an
// existing regular file that is really inside dir (dir itself may be a symlink), else empty.
QString containedExistingFile(const QString &dir, const QString &relative);

// True when canonicalPath is one of canonicalRoots or inside one. Both sides must already be canonical (symlinks
// resolved); an empty path is never inside.
bool isInsideAnyRoot(const QString &canonicalPath, const QStringList &canonicalRoots);

} // namespace murasaki
