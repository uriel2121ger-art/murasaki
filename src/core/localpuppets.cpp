#include "localpuppets.h"

#include "puppet/manifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QString fromStd(const std::string &text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

QJsonArray problems(const std::vector<puppet::Problem> &list)
{
    QJsonArray out;
    for (const puppet::Problem &p : list)
        out.append(fromStd(p.text()));
    return out;
}

// What the checker says is not trusted (it is the confined process that reads hostile models): lists are cut to a few short strings, counts to a sane range, the canvas to what a picture can be.
QJsonArray cleanList(const QJsonArray &list)
{
    QJsonArray out;
    for (const QJsonValue &value : list) {
        if (out.size() >= 16)
            break;
        if (value.isString())
            out.append(value.toString().left(200));
    }
    return out;
}

QJsonValue cleanCount(const QJsonValue &value)
{
    if (!value.isDouble())
        return QJsonValue(QJsonValue::Null);
    const double v = value.toDouble();
    return v >= 0 && v <= 1e6 && v == std::floor(v) ? QJsonValue(int(v)) : QJsonValue(QJsonValue::Null);
}

PetVariant variantOf(const QString &name, const QString &file, int width, int height)
{
    PetVariant v;
    v.name = name;
    v.file = file;
    v.width = width;
    v.height = height;
    v.fps = 60;
    v.frames = 0; // not an animation file: nothing to decode, transcode or cache
    v.bytes = QFileInfo(file).size();
    return v;
}

// The manifest of a folder is parsed again only when the folder's top level changed (the library lists every puppet at each rescan, and a rescan also follows each answer of the checker).
puppet::ManifestResult cachedManifest(const QString &folder)
{
    static QMutex mutex;
    static QHash<QString, QPair<QString, puppet::ManifestResult>> cache;
    QByteArray signature;
    const QFileInfo dir(folder);
    signature += QByteArray::number(dir.lastModified().toMSecsSinceEpoch());
    for (const QFileInfo &entry : QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden, QDir::Name).mid(0, 256))
        signature += '|' + entry.fileName().toUtf8() + ':' + QByteArray::number(entry.size()) + ':' + QByteArray::number(entry.lastModified().toMSecsSinceEpoch());
    const QString key = QString::fromLatin1(QCryptographicHash::hash(signature, QCryptographicHash::Sha1).toHex());
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.constFind(folder);
        if (it != cache.cend() && it->first == key)
            return it->second;
    }
    puppet::ManifestResult result = puppet::loadManifest(folder.toStdString());
    QMutexLocker lock(&mutex);
    if (cache.size() > 256)
        cache.clear();
    cache.insert(folder, {key, result});
    return result;
}

} // namespace

QString LocalPuppetImporter::validate(const QString &path, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (path.isEmpty() || path.size() > 4096 || !QFileInfo(path).isAbsolute())
        return fail(u"path must be absolute"_s);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir())
        return fail(u"not a folder: "_s + path.left(200));
    const puppet::ManifestResult result = puppet::loadManifest(canonical.toStdString());
    if (!result.ok) {
        QStringList reasons;
        for (const puppet::Problem &p : result.reasons)
            reasons << fromStd(p.text());
        return fail(reasons.isEmpty() ? u"not a puppet folder"_s : reasons.join(u"; "_s).left(400));
    }
    return QString::fromStdString(result.manifest.folder);
}

QString LocalPuppetImporter::itemIdFor(const QString &canonicalPath)
{
    return u"local:puppet:"_s + QString::fromLatin1(QCryptographicHash::hash(canonicalPath.toUtf8(), QCryptographicHash::Sha1).toHex());
}

WallpaperItem LocalPuppetImporter::scanItem(const QString &folder, const QJsonObject &check)
{
    const QFileInfo info(folder);
    WallpaperItem item;
    item.id = itemIdFor(folder);
    item.source = u"local"_s;
    item.dir = folder;
    item.contentDir = folder;
    item.kind = ItemKind::Pet;
    item.format = u"puppet"_s;
    item.title = info.fileName();
    const puppet::ManifestResult result = cachedManifest(folder);
    if (!result.ok || !result.format) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = !info.isDir() ? info.fileName() + u": the folder is gone"_s : (result.reasons.empty() ? info.fileName() + u": not a puppet"_s : fromStd(result.reasons[0].text()));
        item.puppet = QJsonObject{{u"reasons"_s, problems(result.reasons)}, {u"degraded"_s, QJsonArray()}};
        return item;
    }
    const puppet::Manifest &m = result.manifest;
    const bool open = *result.format == puppet::Format::Open;
    item.entryFile = QDir(folder).filePath(fromStd(m.file));
    item.title = QFileInfo(fromStd(m.file)).completeBaseName();
    if (item.title.endsWith(u".model3"_s))
        item.title.chop(7);
    if (item.title.isEmpty())
        item.title = info.fileName();

    // The deeper check, when it has run: its counts, canvas and names replace the manifest's, and a refusal makes the item invalid.
    const bool checked = !check.isEmpty();
    QJsonArray reasons = problems(result.reasons), degraded = problems(result.degraded);
    if (checked) {
        reasons = cleanList(check.value(u"reasons"_s).toArray());
        degraded = cleanList(check.value(u"degraded"_s).toArray());
    }
    int width = 512, height = 512;
    const QJsonArray canvas = check.value(u"canvas"_s).toArray();
    if (canvas.size() == 2 && canvas.at(0).toInt() > 0 && canvas.at(1).toInt() > 0 && canvas.at(0).toInt() <= 16384 && canvas.at(1).toInt() <= 16384) {
        width = canvas.at(0).toInt();
        height = canvas.at(1).toInt();
    }
    QJsonArray groups, expressions, hitAreas;
    for (const puppet::MotionGroup &g : m.motionGroups)
        groups.append(QJsonObject{{u"name"_s, fromStd(g.name)}, {u"motions"_s, int(g.motions.size())}});
    for (const puppet::ExpressionRef &e : m.expressions)
        expressions.append(fromStd(e.name));
    for (const puppet::HitArea &h : m.hitAreas)
        hitAreas.append(fromStd(h.name));
    item.puppet = QJsonObject{
        {u"backend"_s, open ? u"open"_s : u"cubism"_s},
        {u"version"_s, m.version},
        {u"checked"_s, checked},
        {u"parameters"_s, cleanCount(check.value(u"parameters"_s))},
        {u"drawables"_s, cleanCount(check.value(u"drawables"_s))},
        {u"textures"_s, cleanCount(check.value(u"textures"_s)).isNull() ? QJsonValue(int(m.textures.size())) : cleanCount(check.value(u"textures"_s))},
        {u"physics"_s, cleanCount(check.value(u"physics"_s))},
        {u"canvas"_s, QJsonArray{width, height}},
        {u"motionGroups"_s, groups},
        {u"expressions"_s, expressions},
        {u"hitAreas"_s, hitAreas},
        {u"reasons"_s, reasons},
        {u"degraded"_s, degraded},
    };
    if (checked && !check.value(u"ok"_s).toBool(false)) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = reasons.isEmpty() ? u"puppet: not a puppet"_s : reasons.at(0).toString();
        return item;
    }
    if (!open) { // drawn only through a Cubism Core the user installs (Murasaki ships none): the item is listed, and cannot be shown
        item.status = ItemStatus::MissingDependency;
        item.statusDetail = fromStd(std::string(puppet::kNeedsCubismRuntime));
    }
    item.variants.append(variantOf(u"model"_s, item.entryFile, width, height));
    for (const puppet::MotionGroup &g : m.motionGroups)
        item.variants.append(variantOf(u"motion:"_s + fromStd(g.name), item.entryFile, width, height));
    for (const puppet::ExpressionRef &e : m.expressions)
        item.variants.append(variantOf(u"expression:"_s + fromStd(e.name), item.entryFile, width, height));
    return item;
}

} // namespace murasaki
