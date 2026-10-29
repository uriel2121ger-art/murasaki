#include "importfolders.h"

#include <QDir>

namespace murasaki::import {

int cleanStaleImportParts(const QString &outRoot)
{
    int removed = 0;
    const QDir root(outRoot);
    for (const QString &name : root.entryList({QStringLiteral("*.part")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QDir(root.filePath(name)).removeRecursively())
            ++removed;
    }
    return removed;
}

} // namespace murasaki::import
