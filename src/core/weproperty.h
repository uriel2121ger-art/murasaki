#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariant>

namespace murasaki {

struct WeOption {
    QString label;
    QVariant value;
};

// One entry of project.json general.properties.
struct WeProperty {
    enum class Type { Unknown, Label, Group, Bool, Slider, Color, Combo, TextInput, File, Directory, UserShortcut };

    QString key;
    Type type = Type::Unknown;
    QString text;
    QVariant value;
    double min = 0;
    double max = 0;
    double step = 0;
    QList<WeOption> options;
    QString condition;
    double order = 0;

    QJsonObject toJson() const;
};

QString weTypeToString(WeProperty::Type type);
WeProperty::Type weTypeFromString(const QString &type);

// Parses general.properties into a list sorted by (order, key).
QList<WeProperty> parseWeProperties(const QJsonObject &properties);

// Overrides values with a preset's {key: value} map. Null preset values keep the default.
void applyPresetValues(QList<WeProperty> &properties, const QJsonObject &preset);

} // namespace murasaki
