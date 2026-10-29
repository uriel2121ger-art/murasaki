#pragma once

#include <QByteArrayView>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace murasaki {

// Parses Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf) into nested QVariantMaps.
// Values are QString; blocks are QVariantMap (QVariantMap sorts keys, so original key order is lost). Returns nullopt on malformed input.
std::optional<QVariantMap> parseVdf(QByteArrayView text);
std::optional<QVariantMap> readVdfFile(const QString &path);

// KeyValues keys are case-insensitive ("LibraryFolders" vs "libraryfolders"). Returns an invalid QVariant if absent.
QVariant vdfValue(const QVariantMap &map, QStringView key);

} // namespace murasaki
