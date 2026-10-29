#include "weproperty.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

struct TypeName {
    WeProperty::Type type;
    QLatin1String name;
};

constexpr TypeName kTypeNames[] = {
    {WeProperty::Type::Label, QLatin1String("label")},
    {WeProperty::Type::Group, QLatin1String("group")},
    {WeProperty::Type::Bool, QLatin1String("bool")},
    {WeProperty::Type::Slider, QLatin1String("slider")},
    {WeProperty::Type::Color, QLatin1String("color")},
    {WeProperty::Type::Combo, QLatin1String("combo")},
    {WeProperty::Type::TextInput, QLatin1String("textinput")},
    {WeProperty::Type::File, QLatin1String("file")},
    {WeProperty::Type::Directory, QLatin1String("directory")},
    {WeProperty::Type::UserShortcut, QLatin1String("usershortcut")},
};

// Reads numbers, numeric strings and bools as double; anything else (or non-finite) is 0.
double jsonToDouble(const QJsonValue &value)
{
    double result = 0;
    if (value.isDouble()) {
        result = value.toDouble();
    } else if (value.isBool()) {
        result = value.toBool() ? 1.0 : 0.0;
    } else if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        if (ok)
            result = parsed;
    }
    return std::isfinite(result) ? result : 0.0;
}

} // namespace

QString weTypeToString(WeProperty::Type type)
{
    for (const TypeName &entry : kTypeNames) {
        if (entry.type == type)
            return entry.name;
    }
    return u"unknown"_s;
}

WeProperty::Type weTypeFromString(const QString &type)
{
    for (const TypeName &entry : kTypeNames) {
        if (type.compare(entry.name, Qt::CaseInsensitive) == 0)
            return entry.type;
    }
    return WeProperty::Type::Unknown;
}

QList<WeProperty> parseWeProperties(const QJsonObject &properties)
{
    QList<WeProperty> result;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject obj = it.value().toObject();
        WeProperty property;
        property.key = it.key();
        // Entries without a type are informational text (credits, links).
        property.type = obj.contains(u"type"_s) ? weTypeFromString(obj.value(u"type"_s).toString())
                                                : WeProperty::Type::Label;
        property.text = obj.value(u"text"_s).toString();
        property.value = obj.value(u"value"_s).toVariant();
        property.min = jsonToDouble(obj.value(u"min"_s));
        property.max = jsonToDouble(obj.value(u"max"_s));
        property.step = jsonToDouble(obj.value(u"step"_s));
        // Wallpaper Engine strips '$' from conditions before evaluating them.
        property.condition = obj.value(u"condition"_s).toString().remove(u'$');
        property.order = jsonToDouble(obj.value(u"order"_s));
        const QJsonArray options = obj.value(u"options"_s).toArray();
        for (const QJsonValue &option : options) {
            const QJsonObject o = option.toObject();
            property.options.append({o.value(u"label"_s).toString(), o.value(u"value"_s).toVariant()});
        }
        result.append(property);
    }
    // Ties break by key because QJsonObject does not keep file order.
    std::stable_sort(result.begin(), result.end(), [](const WeProperty &a, const WeProperty &b) {
        return a.order != b.order ? a.order < b.order : a.key < b.key;
    });
    return result;
}

void applyPresetValues(QList<WeProperty> &properties, const QJsonObject &preset)
{
    for (WeProperty &property : properties) {
        const QJsonValue value = preset.value(property.key);
        if (!value.isUndefined() && !value.isNull())
            property.value = value.toVariant();
    }
}

QJsonObject WeProperty::toJson() const
{
    QJsonObject json{
        {u"key"_s, key},
        {u"type"_s, weTypeToString(type)},
        {u"text"_s, text},
        {u"value"_s, QJsonValue::fromVariant(value)},
        {u"order"_s, order},
    };
    if (type == Type::Slider) {
        json.insert(u"min"_s, min);
        json.insert(u"max"_s, max);
        json.insert(u"step"_s, step);
    }
    if (!options.isEmpty()) {
        QJsonArray array;
        for (const WeOption &option : options)
            array.append(QJsonObject{{u"label"_s, option.label}, {u"value"_s, QJsonValue::fromVariant(option.value)}});
        json.insert(u"options"_s, array);
    }
    if (!condition.isEmpty())
        json.insert(u"condition"_s, condition);
    return json;
}

} // namespace murasaki
