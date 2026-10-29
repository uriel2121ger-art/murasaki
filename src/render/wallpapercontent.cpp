#include "wallpapercontent.h"

#include "webapiscript.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

// A Wallpaper Engine color as the scene engine reads it: "r g b" (0..1 floats). Accepts that form, a number
// array, or #rrggbb.
QString sceneColor(const QJsonValue &value)
{
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue &component : value.toArray())
            parts << QString::number(component.toDouble(), 'g', 9);
        return parts.join(u' ');
    }
    const QString text = value.toString().simplified();
    static const QRegularExpression hex(u"^#([0-9a-fA-F]{6})$"_s);
    if (const auto match = hex.match(text); match.hasMatch()) {
        const uint rgb = match.captured(1).toUInt(nullptr, 16);
        return u"%1 %2 %3"_s.arg(((rgb >> 16) & 0xff) / 255.0, 0, 'g', 6)
            .arg(((rgb >> 8) & 0xff) / 255.0, 0, 'g', 6)
            .arg((rgb & 0xff) / 255.0, 0, 'g', 6);
    }
    return text;
}

// {key: value} for the scene engine: the property's default, overridden by presetValues, overridden by the
// assignment's overrides (null does not override). Labels and groups carry no value.
QJsonObject sceneUserProperties(const QJsonObject &item, const QJsonObject &overrides)
{
    const QJsonObject preset = item.value(u"presetValues"_s).toObject();
    QJsonObject result;
    for (const QJsonValue &entry : item.value(u"properties"_s).toArray()) {
        const QJsonObject property = entry.toObject();
        const QString key = property.value(u"key"_s).toString();
        const QString type = property.value(u"type"_s).toString();
        if (key.isEmpty() || type == u"label" || type == u"group")
            continue;
        QJsonValue value = property.value(u"value"_s);
        for (const QJsonObject *layer : {&preset, &overrides}) {
            const QJsonValue candidate = layer->value(key);
            if (!candidate.isUndefined() && !candidate.isNull())
                value = candidate;
        }
        if (value.isUndefined() || value.isNull())
            continue;
        if (type == u"color")
            value = sceneColor(value);
        result.insert(key, value);
    }
    return result;
}

} // namespace

WallpaperContent WallpaperContent::fromResolveReply(const QJsonObject &reply, const QString &assetsDir)
{
    WallpaperContent content;
    content.kind = u"none"_s;
    const QJsonObject assignment = reply.value(u"assignment"_s).toObject();
    if (assignment.isEmpty())
        return content;
    const QJsonObject item = reply.value(u"item"_s).toObject();

    content.itemId = assignment.value(u"itemId"_s).toString();
    content.fill = assignment.value(u"fill"_s).toString(u"fill"_s);
    content.muted = assignment.value(u"muted"_s).toBool();
    content.span = assignment.value(u"span"_s).toBool(); // only a real boolean: anything else is not span
    QJsonObject identity = assignment;
    identity.remove(u"muted"_s); // muting must not reload the wallpaper
    identity.remove(u"properties"_s); // property edits reach the running page instead (userProperties)
    QJsonObject identityDoc{{u"assignment"_s, identity}, {u"item"_s, item}};
    if (item.value(u"kind"_s).toString() == u"scene")
        identityDoc.insert(u"assetsDir"_s, assetsDir); // the engine reads it at launch
    const QByteArray fingerprint = QJsonDocument(identityDoc).toJson(QJsonDocument::Compact);
    content.key = QString::fromLatin1(QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha1).toHex());
    if (item.isEmpty())
        return content; // the assigned item left the library: black until it comes back

    content.contentDir = item.value(u"contentDir"_s).toString();
    content.entryFile = item.value(u"entryFile"_s).toString();
    content.previewPath = item.value(u"preview"_s).toString();
    const QString kind = item.value(u"kind"_s).toString();
    const bool ok = item.value(u"status"_s).toString() == u"ok";
    const bool hasEntry = !content.entryFile.isEmpty();
    if (ok && hasEntry && kind == u"video") {
        content.kind = u"video"_s;
    } else if (ok && hasEntry && kind == u"web") {
        content.kind = u"web"_s;
        content.networkAllowed = item.value(u"permissions"_s).toObject().value(u"network"_s).toBool(); // only a real true
        const QJsonObject overrides = assignment.value(u"properties"_s).toObject();
        content.script = buildWebApiScript(item, overrides);
        content.userProperties = webUserProperties(item, overrides);
        QStringList candidates{item.value(u"dir"_s).toString(), content.contentDir};
        for (const QJsonValue &entry : item.value(u"properties"_s).toArray()) {
            const QJsonObject property = entry.toObject();
            const QString type = property.value(u"type"_s).toString();
            const QString picked = overrides.value(property.value(u"key"_s).toString()).toString();
            if ((type == u"file" || type == u"directory") && QDir::isAbsolutePath(picked))
                candidates.append(picked);
        }
        for (const QString &candidate : candidates) {
            const QString canonical = candidate.isEmpty() ? QString() : QFileInfo(candidate).canonicalFilePath();
            if (!canonical.isEmpty() && !content.readableRoots.contains(canonical))
                content.readableRoots.append(canonical);
        }
    } else if (ok && kind == u"scene") {
        if (assetsDir.isEmpty()) {
            content.kind = u"preview"_s;
            content.reason = u"Wallpaper Engine assets not found"_s;
        } else {
            content.kind = u"scene"_s;
            content.assetsDir = assetsDir;
            content.sceneProperties = sceneUserProperties(item, assignment.value(u"properties"_s).toObject());
        }
    } else {
        content.kind = u"preview"_s; // broken items
    }
    return content;
}

QJsonObject changedUserProperties(const QJsonObject &before, const QJsonObject &after)
{
    QJsonObject changed;
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (!before.contains(it.key()) || before.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    }
    return changed;
}

QUrl itemEntryUrl(const QString &token, const QString &contentDir, const QString &entryFile)
{
    static const QRegularExpression tokenPattern(u"^[a-z0-9]+$"_s);
    if (contentDir.isEmpty() || entryFile.isEmpty() || !tokenPattern.match(token).hasMatch())
        return {};
    const QString relative = QDir(contentDir).relativeFilePath(entryFile);
    if (relative.isEmpty() || relative == u"." || relative == u".." || relative.startsWith(u"../"_s)
        || QDir::isAbsolutePath(relative))
        return {};
    QUrl url;
    url.setScheme(kItemScheme);
    url.setHost(token);
    if (url.host() != token) // the host is the capability: it must survive URL normalisation unchanged
        return {};
    url.setPath(u'/' + relative, QUrl::DecodedMode);
    return url;
}

} // namespace murasaki
