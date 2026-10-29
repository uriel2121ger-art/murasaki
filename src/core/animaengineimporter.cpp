#include "animaengineimporter.h"

#include "animationinfo.h"
#include "itemrisks.h"
#include "pathutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

std::optional<WallpaperItem> AnimaEngineImporter::scanItem(const QString &itemDir)
{
    const QFileInfo info(itemDir);
    const QString canonicalDir = info.canonicalFilePath();
    if (canonicalDir.isEmpty())
        return std::nullopt;

    QStringList names;
    const QFileInfoList entries = QDir(itemDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (isAnimationFile(entry.absoluteFilePath()))
            names.append(entry.fileName());
    }
    if (names.isEmpty())
        return std::nullopt;
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int byCase = QString::compare(a, b, Qt::CaseInsensitive);
        return byCase != 0 ? byCase < 0 : a < b;
    });

    WallpaperItem item;
    item.dir = info.absoluteFilePath();
    item.contentDir = item.dir;
    item.id = u"anima:"_s + info.fileName();
    item.source = u"anima-engine"_s;
    item.title = info.fileName();
    item.kind = ItemKind::Pet;
    item.risks = scanItemRisks(item.dir);
    static const QRegularExpression digits(u"\\A\\d+\\z"_s);
    if (digits.match(info.fileName()).hasMatch())
        item.workshopId = info.fileName();

    QString firstProblem;
    for (const QString &name : std::as_const(names)) {
        // The file path goes to a decoder later: a symlink out of the item dir is not an animation of this item.
        const QString real = containedExistingFile(canonicalDir, name);
        if (real.isEmpty()) {
            if (firstProblem.isEmpty())
                firstProblem = name + u": not a regular file inside the item folder"_s;
            continue;
        }
        const auto animation = readAnimationInfo(real);
        if (!animation) {
            if (firstProblem.isEmpty())
                firstProblem = name + u": not a readable animation"_s;
            continue;
        }
        PetVariant variant;
        variant.name = QFileInfo(name).completeBaseName();
        variant.file = real; // canonical, so what the decoder opens is what was checked
        variant.width = animation->width;
        variant.height = animation->height;
        variant.fps = animation->fps;
        variant.frames = animation->frames;
        variant.bytes = QFileInfo(real).size();
        item.variants.append(std::move(variant));
    }

    if (item.variants.isEmpty()) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = firstProblem;
    }
    return item;
}

QList<WallpaperItem> AnimaEngineImporter::scanDirectory(const QString &dir)
{
    QList<WallpaperItem> items;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (auto item = scanItem(entry.absoluteFilePath()))
            items.append(std::move(*item));
    }
    return items;
}

} // namespace murasaki
