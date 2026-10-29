#include "importablefiles.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace murasaki {

namespace {

constexpr int kMaxImportable = 50;

QString kindOf(const QString &suffix)
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("zip"))
        return QStringLiteral("zip");
    if (s == QLatin1String("mdaa"))
        return QStringLiteral("mdaa");
    for (const char *video : {"mp4", "m4v", "webm", "mkv", "mov", "avi"})
        if (s == QLatin1String(video))
            return QStringLiteral("video");
    return {};
}

void scan(const QString &canonicalRoot, const QDir &dir, int depth, QList<ImportableFile> &out)
{
    for (const QFileInfo &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (out.size() >= kMaxImportable)
            return;
        const QString real = entry.canonicalFilePath();
        if (real.isEmpty() || !(real == canonicalRoot || real.startsWith(canonicalRoot + QLatin1Char('/'))))
            continue;
        if (entry.isDir()) {
            if (depth < 1)
                scan(canonicalRoot, QDir(real), depth + 1, out);
            continue;
        }
        const QString kind = kindOf(entry.suffix());
        if (kind.isEmpty() || !QFileInfo(real).isFile())
            continue;
        out.append({real, entry.fileName(), kind, QFileInfo(real).size(), entry.lastModified().toMSecsSinceEpoch()});
    }
}

} // namespace

QList<ImportableFile> findImportableFiles(const QString &itemDir)
{
    QList<ImportableFile> files;
    const QString root = QFileInfo(itemDir).canonicalFilePath();
    if (!root.isEmpty())
        scan(root, QDir(root), 0, files);
    return files;
}

} // namespace murasaki
