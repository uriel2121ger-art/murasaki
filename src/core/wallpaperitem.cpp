#include "wallpaperitem.h"

#include <QJsonArray>

using namespace Qt::StringLiterals;

namespace murasaki {

QString itemKindToString(ItemKind kind)
{
    switch (kind) {
    case ItemKind::Scene:
        return u"scene"_s;
    case ItemKind::Video:
        return u"video"_s;
    case ItemKind::Web:
        return u"web"_s;
    case ItemKind::Pet:
        return u"pet"_s;
    case ItemKind::Asset:
        return u"asset"_s;
    case ItemKind::Unknown:
        break;
    }
    return u"unknown"_s;
}

ItemKind itemKindFromString(const QString &kind)
{
    const QString lower = kind.toLower();
    if (lower == u"scene"_s)
        return ItemKind::Scene;
    if (lower == u"video"_s)
        return ItemKind::Video;
    if (lower == u"web"_s)
        return ItemKind::Web;
    if (lower == u"pet"_s)
        return ItemKind::Pet;
    if (lower == u"asset"_s)
        return ItemKind::Asset;
    return ItemKind::Unknown;
}

QString itemStatusToString(ItemStatus status)
{
    switch (status) {
    case ItemStatus::Ok:
        return u"ok"_s;
    case ItemStatus::MissingDependency:
        return u"missing-dependency"_s;
    case ItemStatus::Invalid:
        break;
    }
    return u"invalid"_s;
}

QJsonObject PetVariant::toJson() const
{
    return QJsonObject{
        {u"name"_s, name},
        {u"file"_s, file},
        {u"width"_s, width},
        {u"height"_s, height},
        {u"fps"_s, fps},
        {u"frames"_s, frames},
        {u"bytes"_s, bytes},
        {u"heavy"_s, isHeavy()},
    };
}

QJsonObject WallpaperItem::toSummaryJson() const
{
    QJsonObject summary{
        {u"id"_s, id},
        {u"source"_s, source},
        {u"title"_s, title},
        {u"kind"_s, itemKindToString(kind)},
        {u"status"_s, itemStatusToString(status)},
        {u"statusDetail"_s, statusDetail},
        {u"preview"_s, previewPath},
        {u"tags"_s, QJsonArray::fromStringList(tags)},
        {u"contentRating"_s, contentRating},
        {u"workshopId"_s, workshopId},
        {u"dependencyId"_s, dependencyId},
    };
    QJsonArray riskList;
    for (const ItemRisk &risk : risks)
        riskList.append(risk.toJson());
    summary.insert(u"risks"_s, riskList);
    if (kind == ItemKind::Pet)
        summary.insert(u"variantCount"_s, int(variants.size()));
    if (!format.isEmpty())
        summary.insert(u"format"_s, format);
    return summary;
}

QJsonObject WallpaperItem::toJson() const
{
    QJsonObject json = toSummaryJson();
    json.insert(u"dir"_s, dir);
    json.insert(u"entryFile"_s, entryFile);
    json.insert(u"contentDir"_s, contentDir);
    json.insert(u"presetValues"_s, presetValues);
    json.insert(u"localization"_s, localization);
    QJsonArray props;
    for (const WeProperty &property : properties)
        props.append(property.toJson());
    json.insert(u"properties"_s, props);
    if (kind == ItemKind::Pet) {
        QJsonArray list;
        for (const PetVariant &variant : variants)
            list.append(variant.toJson());
        json.insert(u"variants"_s, list);
    }
    if (!format.isEmpty())
        json.insert(u"puppet"_s, puppet);
    return json;
}

} // namespace murasaki
