#include "pathutil.h"

#include <QDir>
#include <QFileInfo>

namespace murasaki {

QString confinedPath(const QString &canonicalDir, const QString &relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative))
        return {};
    const QString path = QDir::cleanPath(canonicalDir + u'/' + relative);
    if (!path.startsWith(canonicalDir + u'/'))
        return {};
    return path;
}

QString containedExistingFile(const QString &dir, const QString &relative)
{
    const QString canonicalDir = QFileInfo(dir).canonicalFilePath();
    if (canonicalDir.isEmpty())
        return {};
    const QString lexical = confinedPath(canonicalDir, relative);
    if (lexical.isEmpty())
        return {};
    const QString canonical = QFileInfo(lexical).canonicalFilePath();
    if (canonical.isEmpty() || !canonical.startsWith(canonicalDir + u'/') || !QFileInfo(canonical).isFile())
        return {};
    return canonical;
}

bool isInsideAnyRoot(const QString &canonicalPath, const QStringList &canonicalRoots)
{
    if (canonicalPath.isEmpty())
        return false;
    for (const QString &root : canonicalRoots) {
        if (root.isEmpty())
            continue;
        if (canonicalPath == root || canonicalPath.startsWith(root.endsWith(u'/') ? root : root + u'/'))
            return true;
    }
    return false;
}

} // namespace murasaki
