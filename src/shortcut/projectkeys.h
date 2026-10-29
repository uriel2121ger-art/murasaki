#pragma once

// The `usershortcut` properties of a wallpaper item: the keys of general.properties in its project.json whose type is "usershortcut".
// The host reads them itself (never from the scene's own data, which is untrusted too): size capped, keys checked.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace murasaki::shortcut {

inline constexpr qint64 kMaxProjectBytes = 4 * 1024 * 1024;

// The keys (valid keys only, at most kMaxSlots, in file order of the object's keys sorted) of `properties`, a `general.properties` object.
QStringList userShortcutKeys(const QJsonObject &properties);

// The same read from <itemDir>/project.json (nothing when the file is missing, over kMaxProjectBytes or not JSON).
QStringList userShortcutKeys(const QString &itemDir);

} // namespace murasaki::shortcut
