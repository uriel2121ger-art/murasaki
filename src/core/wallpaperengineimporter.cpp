#include "wallpaperengineimporter.h"

#include "itemrisks.h"
#include "pathutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr qint64 maxProjectJsonBytes = 16 * 1024 * 1024;

QString dependencyToString(const QJsonValue &value)
{
    // The id ends up in paths and URLs, so only plain digits are accepted.
    static const QRegularExpression digits(u"\\A\\d+\\z"_s);
    if (value.isString()) {
        const QString text = value.toString();
        return digits.match(text).hasMatch() ? text : QString();
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number >= 0 && number < 9007199254740992.0 && number == std::floor(number))
            return QString::number(static_cast<qint64>(number));
    }
    return {};
}

} // namespace

std::optional<WallpaperItem> WallpaperEngineImporter::scanItem(const QString &itemDir)
{
    QFile file(itemDir + u"/project.json"_s);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const bool tooLarge = file.size() > maxProjectJsonBytes;
    QByteArray data = tooLarge ? QByteArray() : file.readAll();
    // Many Workshop project.json files start with a UTF-8 BOM, which QJsonDocument rejects.
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);

    const QFileInfo info(itemDir);
    WallpaperItem item;
    item.dir = info.absoluteFilePath();
    item.contentDir = item.dir;
    item.id = u"we:"_s + info.fileName();
    item.source = u"wallpaper-engine"_s;
    item.title = info.fileName();
    static const QRegularExpression digits(u"\\A\\d+\\z"_s);
    if (digits.match(info.fileName()).hasMatch())
        item.workshopId = info.fileName();

    item.risks = scanItemRisks(item.dir);

    if (tooLarge) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = u"project.json too large"_s;
        return item;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (!doc.isObject()) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = u"project.json: "_s
            + (error.error != QJsonParseError::NoError ? error.errorString() : u"not a JSON object"_s);
        return item;
    }
    const QJsonObject project = doc.object();

    if (const QString title = project.value(u"title"_s).toString(); !title.isEmpty())
        item.title = title;
    const QString type = project.value(u"type"_s).toString();
    item.kind = itemKindFromString(type);
    // An asset pack of the editor has no type; it is not broken, it is just not a wallpaper.
    if (type.isEmpty() && project.value(u"dependency"_s).isUndefined()
        && (project.value(u"category"_s).toString().compare(u"asset"_s, Qt::CaseInsensitive) == 0
            || project.value(u"file"_s).toString().compare(u"assets.json"_s, Qt::CaseInsensitive) == 0))
        item.kind = ItemKind::Asset;
    if (type.compare(u"application"_s, Qt::CaseInsensitive) == 0) {
        const QString entryName = project.value(u"file"_s).toString();
        item.risks.prepend({u"application"_s, entryName.isEmpty() ? u"project.json"_s : entryName});
    }
    item.contentRating = project.value(u"contentrating"_s).toString();
    for (const QJsonValue &tag : project.value(u"tags"_s).toArray())
        item.tags.append(tag.toString());
    const QString canonicalDir = QFileInfo(item.dir).canonicalFilePath();
    if (const QString preview = confinedPath(canonicalDir, project.value(u"preview"_s).toString());
        !preview.isEmpty() && QFileInfo(preview).isFile())
        item.previewPath = item.dir + preview.mid(canonicalDir.size());
    // Ids may be written as JSON strings or integral numbers made of digits; anything else makes the item invalid.
    item.dependencyId = dependencyToString(project.value(u"dependency"_s));
    item.presetValues = project.value(u"preset"_s).toObject();
    item.properties = parseWeProperties(project.value(u"general"_s).toObject().value(u"properties"_s).toObject());
    // Keep only {locale: {key: string}}; the project file is untrusted.
    const QJsonObject rawLocalization = project.value(u"general"_s).toObject().value(u"localization"_s).toObject();
    for (auto locale = rawLocalization.begin(); locale != rawLocalization.end(); ++locale) {
        QJsonObject texts;
        const QJsonObject source = locale.value().toObject();
        for (auto text = source.begin(); text != source.end(); ++text) {
            if (text.value().isString())
                texts.insert(text.key(), text.value());
        }
        if (!texts.isEmpty())
            item.localization.insert(locale.key().toLower(), texts);
    }
    const QString entry = project.value(u"file"_s).toString();
    const QString entryPath = confinedPath(canonicalDir, entry);
    if (!entryPath.isEmpty())
        item.entryFile = item.dir + entryPath.mid(canonicalDir.size());

    const QJsonValue dependency = project.value(u"dependency"_s);
    if (item.dependencyId.isEmpty() && !dependency.isUndefined() && !dependency.isNull()) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = u"invalid dependency id"_s;
        return item;
    }
    if (!item.dependencyId.isEmpty())
        return item; // a preset: validated by resolveDependencies()

    if (item.kind == ItemKind::Asset)
        return item; // nothing to start: no entry to check
    if (item.kind == ItemKind::Unknown) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = u"unsupported type: "_s + type;
    } else if (!entry.isEmpty() && entryPath.isEmpty()) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = u"entry path escapes item dir"_s;
    } else {
        // A scene's entry is usually packed inside <entry basename>.pkg.
        const bool entryExists = !item.entryFile.isEmpty() && QFileInfo(item.entryFile).isFile();
        bool packedScene = false;
        if (item.kind == ItemKind::Scene && !entryPath.isEmpty()) {
            const QFileInfo entryInfo(entryPath);
            // Same directory as the already confined entry, so it is confined too.
            const QString pack = entryInfo.path() + u'/' + entryInfo.completeBaseName() + u".pkg"_s;
            packedScene = QFileInfo(pack).isFile();
        }
        if (!entryExists && !packedScene) {
            item.status = ItemStatus::Invalid;
            item.statusDetail = u"entry file not found: "_s + entry;
        }
    }
    return item;
}

QList<WallpaperItem> WallpaperEngineImporter::scanDirectory(const QString &dir)
{
    QList<WallpaperItem> items;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (auto item = scanItem(entry.absoluteFilePath()))
            items.append(std::move(*item));
    }
    return items;
}

void WallpaperEngineImporter::resolveDependencies(QList<WallpaperItem> &items)
{
    QHash<QString, qsizetype> byWorkshopId;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const QString &id = items.at(i).workshopId;
        // With duplicate ids (e.g. Workshop plus myprojects), prefer a playable item.
        if (!id.isEmpty() && (!byWorkshopId.contains(id) || items.at(i).status == ItemStatus::Ok))
            byWorkshopId.insert(id, i);
    }

    for (qsizetype i = 0; i < items.size(); ++i) {
        WallpaperItem &item = items[i];
        if (item.dependencyId.isEmpty())
            continue;
        const auto found = byWorkshopId.constFind(item.dependencyId);
        if (found == byWorkshopId.cend()) {
            item.status = ItemStatus::MissingDependency;
            item.statusDetail = item.dependencyId;
            continue;
        }
        const WallpaperItem &base = items.at(*found);
        // Presets are only made of playable base items; a preset of a preset is not supported.
        if (*found == i || base.status != ItemStatus::Ok || !base.dependencyId.isEmpty()) {
            item.status = ItemStatus::Invalid;
            item.statusDetail = u"dependency "_s + base.id + u" is not a playable base item"_s;
            continue;
        }
        // The engine loads the base item, so kind and entry always come from it together with contentDir.
        item.kind = base.kind;
        item.entryFile = base.entryFile;
        item.contentDir = base.contentDir;
        // A preset that ships its own general.properties keeps them instead of the base's.
        if (item.properties.isEmpty())
            item.properties = base.properties;
        if (item.localization.isEmpty())
            item.localization = base.localization;
        applyPresetValues(item.properties, item.presetValues);
        item.status = ItemStatus::Ok;
        item.statusDetail.clear();
    }
}

} // namespace murasaki
