#include "propertymodel.h"

#include <QCoreApplication>
#include <QLocale>
#include <QMetaType>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr int kMaxConditionLength = 512;

// Common Wallpaper Engine keys that projects use without shipping a translation. Item-specific keys come with the
// item's own `general.localization`; whatever is in neither falls back to a humanized key.
struct BuiltinLabel
{
    const char *key;
    const char *text;
};
constexpr BuiltinLabel kBuiltinLabels[] = {
    {"ui_browse_properties_scheme_color", QT_TRANSLATE_NOOP("PropertyModel", "Scheme color")},
    {"ui_browse_properties_schemecolor", QT_TRANSLATE_NOOP("PropertyModel", "Scheme color")},
    {"ui_browse_properties_audio_volume", QT_TRANSLATE_NOOP("PropertyModel", "Volume")},
    {"ui_browse_properties_volume", QT_TRANSLATE_NOOP("PropertyModel", "Volume")},
    {"ui_browse_properties_speed", QT_TRANSLATE_NOOP("PropertyModel", "Speed")},
    {"ui_browse_properties_playback_rate", QT_TRANSLATE_NOOP("PropertyModel", "Playback speed")},
    {"ui_browse_properties_brightness", QT_TRANSLATE_NOOP("PropertyModel", "Brightness")},
    {"ui_browse_properties_contrast", QT_TRANSLATE_NOOP("PropertyModel", "Contrast")},
    {"ui_browse_properties_saturation", QT_TRANSLATE_NOOP("PropertyModel", "Saturation")},
    {"ui_browse_properties_hue", QT_TRANSLATE_NOOP("PropertyModel", "Hue")},
    {"ui_browse_properties_opacity", QT_TRANSLATE_NOOP("PropertyModel", "Opacity")},
    {"ui_browse_properties_background_color", QT_TRANSLATE_NOOP("PropertyModel", "Background color")},
    {"ui_browse_properties_color", QT_TRANSLATE_NOOP("PropertyModel", "Color")},
};

QString humanizeKey(const QString &key)
{
    QString text = key;
    for (const QString &prefix : {u"ui_browse_properties_"_s, u"ui_"_s}) {
        if (text.startsWith(prefix, Qt::CaseInsensitive)) {
            text.remove(0, prefix.size());
            break;
        }
    }
    text.replace(u'_', u' ');
    text = text.simplified();
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

// Looks `key` up in the localization tables: exact locale, same language, English, then any locale.
QString lookupLocalized(const QString &key, const QVariantMap &localization, const QString &localeName)
{
    if (localization.isEmpty())
        return {};
    const QString locale = localeName.toLower().replace(u'_', u'-');
    const QString language = locale.section(u'-', 0, 0);
    const auto find = [&](const QString &name) {
        return localization.value(name).toMap().value(key).toString();
    };
    QStringList order{locale};
    if (!language.isEmpty()) {
        order.append(language);
        for (auto it = localization.cbegin(); it != localization.cend(); ++it) {
            if (it.key().toLower().startsWith(language + u'-'))
                order.append(it.key());
        }
    }
    order << u"en-us"_s << u"en"_s;
    for (auto it = localization.cbegin(); it != localization.cend(); ++it)
        order.append(it.key());
    for (const QString &name : std::as_const(order)) {
        if (const QString text = find(name); !text.isEmpty())
            return text;
    }
    return {};
}

bool isEditableType(const QString &type)
{
    static const QSet<QString> types{u"slider"_s, u"bool"_s,      u"color"_s,    u"combo"_s,
                                     u"textinput"_s, u"file"_s, u"directory"_s, u"usershortcut"_s};
    return types.contains(type);
}

bool isTextType(const QString &type)
{
    return type == u"color"_s || type == u"textinput"_s || type == u"file"_s || type == u"directory"_s || type == u"usershortcut"_s;
}

bool isNumberVariant(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Long:
    case QMetaType::ULong:
        return true;
    default:
        return false;
    }
}

// Numbers, bools and numeric strings count as numbers; everything else does not.
bool asNumber(const QVariant &v, double &out)
{
    if (v.typeId() == QMetaType::Bool) {
        out = v.toBool() ? 1.0 : 0.0;
        return true;
    }
    if (isNumberVariant(v)) {
        out = v.toDouble();
        return std::isfinite(out);
    }
    if (v.typeId() == QMetaType::QString) {
        const QString s = v.toString().trimmed();
        if (s.isEmpty())
            return false;
        bool ok = false;
        out = s.toDouble(&ok);
        return ok && std::isfinite(out);
    }
    return false;
}

bool looseEqual(const QVariant &a, const QVariant &b)
{
    double x = 0;
    double y = 0;
    if (asNumber(a, x) && asNumber(b, y))
        return std::abs(x - y) <= 1e-9 * std::max({1.0, std::abs(x), std::abs(y)});
    return a.toString() == b.toString();
}

bool isIdentifier(const QString &key)
{
    static const QRegularExpression re(u"^[A-Za-z_][A-Za-z0-9_]*$"_s);
    // Reserved words and strict-mode restricted names cannot be parameter names.
    static const QSet<QString> reserved{
        u"break"_s,    u"case"_s,       u"catch"_s,    u"class"_s,   u"const"_s,      u"continue"_s, u"debugger"_s,
        u"default"_s,  u"delete"_s,     u"do"_s,       u"else"_s,    u"enum"_s,       u"export"_s,   u"extends"_s,
        u"false"_s,    u"finally"_s,    u"for"_s,      u"function"_s, u"if"_s,        u"import"_s,   u"in"_s,
        u"instanceof"_s, u"new"_s,      u"null"_s,     u"return"_s,  u"super"_s,      u"switch"_s,   u"this"_s,
        u"throw"_s,    u"true"_s,       u"try"_s,      u"typeof"_s,  u"var"_s,        u"void"_s,     u"while"_s,
        u"with"_s,     u"yield"_s,      u"let"_s,      u"static"_s,  u"implements"_s, u"interface"_s, u"package"_s,
        u"private"_s,  u"protected"_s,  u"public"_s,   u"await"_s,   u"eval"_s,       u"arguments"_s};
    return re.match(key).hasMatch() && !reserved.contains(key);
}

// Token whitelist for conditions. They come from project files anyone can publish, so nothing is handed to
// the JS engine unless every token is known: identifiers that are property keys (read only as `key.value`)
// or true/false/null/undefined, number and plain string literals, == != === !== < > <= >= && || ! and
// parentheses. No calls, member access other than `.value`, comments, assignments or statements.
bool isAllowedCondition(const QString &expr, const QSet<QString> &keys)
{
    if (expr.size() > kMaxConditionLength)
        return false;
    static const QSet<QString> literals{u"true"_s, u"false"_s, u"null"_s, u"undefined"_s};
    const qsizetype n = expr.size();
    auto isIdStart = [](QChar c) { return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || c == u'_'; };
    auto isDigit = [](QChar c) { return c >= u'0' && c <= u'9'; };
    auto isIdChar = [&](QChar c) { return isIdStart(c) || isDigit(c); };
    auto skipSpace = [&](qsizetype i) {
        while (i < n && (expr.at(i) == u' ' || expr.at(i) == u'\t' || expr.at(i) == u'\n' || expr.at(i) == u'\r'))
            ++i;
        return i;
    };

    // `operand` is true after an identifier, literal or ')': the next token must then be an operator or ')'.
    bool operand = false;
    int depth = 0;
    qsizetype i = skipSpace(0);
    while (i < n) {
        const QChar c = expr.at(i);
        if (isIdStart(c)) {
            if (operand)
                return false;
            qsizetype j = i + 1;
            while (j < n && isIdChar(expr.at(j)))
                ++j;
            const QString word = expr.mid(i, j - i);
            const bool isKey = keys.contains(word);
            if (!isKey && !literals.contains(word))
                return false;
            j = skipSpace(j);
            if (j < n && expr.at(j) == u'.') {
                if (!isKey)
                    return false;
                j = skipSpace(j + 1);
                if (expr.mid(j, 5) != u"value"_s || (j + 5 < n && isIdChar(expr.at(j + 5))))
                    return false;
                j += 5;
            }
            operand = true;
            i = skipSpace(j);
        } else if (isDigit(c)) {
            if (operand)
                return false;
            qsizetype j = i;
            while (j < n && isDigit(expr.at(j)))
                ++j;
            if (j + 1 < n && expr.at(j) == u'.' && isDigit(expr.at(j + 1))) {
                ++j;
                while (j < n && isDigit(expr.at(j)))
                    ++j;
            }
            if (j < n && (expr.at(j) == u'e' || expr.at(j) == u'E')) {
                qsizetype k = j + 1;
                if (k < n && (expr.at(k) == u'+' || expr.at(k) == u'-'))
                    ++k;
                if (k < n && isDigit(expr.at(k))) {
                    while (k < n && isDigit(expr.at(k)))
                        ++k;
                    j = k;
                }
            }
            if (j < n && (isIdChar(expr.at(j)) || expr.at(j) == u'.'))
                return false;
            operand = true;
            i = skipSpace(j);
        } else if (c == u'"' || c == u'\'') {
            if (operand)
                return false;
            qsizetype j = i + 1;
            while (j < n && expr.at(j) != c) {
                if (expr.at(j) == u'\\' || expr.at(j) == u'\n' || expr.at(j) == u'\r')
                    return false;
                ++j;
            }
            if (j >= n || j - i - 1 > 256)
                return false;
            operand = true;
            i = skipSpace(j + 1);
        } else if (c == u'(') {
            if (operand)
                return false; // a call
            ++depth;
            i = skipSpace(i + 1);
        } else if (c == u')') {
            if (!operand || --depth < 0)
                return false;
            i = skipSpace(i + 1);
        } else {
            static const QStringList binary{u"==="_s, u"!=="_s, u"=="_s, u"!="_s, u"<="_s,
                                            u">="_s,  u"&&"_s,  u"||"_s, u"<"_s,  u">"_s};
            qsizetype len = 0;
            bool isBinary = false;
            for (const QString &op : binary) {
                if (expr.mid(i, op.size()) == op) {
                    len = op.size();
                    isBinary = true;
                    break;
                }
            }
            if (!isBinary) {
                if (c != u'!')
                    return false;
                len = 1;
            }
            // Binary operators need a left operand, '!' must not have one.
            if (isBinary != operand)
                return false;
            operand = false;
            i = skipSpace(i + len);
        }
    }
    return operand && depth == 0;
}

QString stripDollars(QString expr)
{
    expr.remove(u'$');
    return expr;
}

QString decodeEntities(const QString &in)
{
    static const QRegularExpression entity(u"&(#[xX][0-9A-Fa-f]+|#[0-9]+|[A-Za-z][A-Za-z0-9]*);"_s);
    QString out;
    qsizetype last = 0;
    auto it = entity.globalMatch(in);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1);
        QString replacement;
        if (name.startsWith(u'#')) {
            bool ok = false;
            const uint code = name.at(1) == u'x' || name.at(1) == u'X' ? name.mid(2).toUInt(&ok, 16)
                                                                       : name.mid(1).toUInt(&ok, 10);
            if (ok && QChar::isPrint(code))
                replacement = QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        } else if (name == u"amp"_s) {
            replacement = u"&"_s;
        } else if (name == u"lt"_s) {
            replacement = u"<"_s;
        } else if (name == u"gt"_s) {
            replacement = u">"_s;
        } else if (name == u"quot"_s) {
            replacement = u"\""_s;
        } else if (name == u"apos"_s) {
            replacement = u"'"_s;
        } else if (name == u"nbsp"_s) {
            replacement = u" "_s;
        }
        if (replacement.isEmpty())
            continue; // unknown entity stays as written
        out += in.mid(last, m.capturedStart() - last) + replacement;
        last = m.capturedEnd();
    }
    return out + in.mid(last);
}

} // namespace

PropertyModel::PropertyModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

PropertyModel::~PropertyModel() = default;

bool PropertyModel::conditionAllowed(const QString &expr, const QSet<QString> &keys) const
{
    return isAllowedCondition(expr, keys);
}

int PropertyModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant PropertyModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case KeyRole:
    case Qt::DisplayRole:
        return row.key;
    case TypeRole:
        return row.type;
    case TextRole:
        return row.text;
    case ValueRole:
        return row.current;
    case MinRole:
        return row.min;
    case MaxRole:
        return row.max;
    case StepRole:
        return row.step;
    case EffectiveStepRole:
        return effectiveStep(row.min, row.max, row.step);
    case OptionsRole:
        return row.options;
    case VisibleRole:
        return row.visible;
    default:
        return {};
    }
}

QHash<int, QByteArray> PropertyModel::roleNames() const
{
    return {
        {KeyRole, "key"},
        {TypeRole, "type"},
        {TextRole, "text"},
        {ValueRole, "value"},
        {MinRole, "min"},
        {MaxRole, "max"},
        {StepRole, "step"},
        {EffectiveStepRole, "effectiveStep"},
        {OptionsRole, "options"},
        {VisibleRole, "visible"},
    };
}

QString PropertyModel::plainText(const QString &html)
{
    static const QRegularExpression lineBreak(u"<\\s*(br|/p|/div|/li)\\b[^>]*>"_s,
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tag(u"</?[A-Za-z!][^>]*>"_s);
    QString text = html;
    text.replace(lineBreak, u" "_s);
    text.replace(tag, QString());
    // Decode after removing tags so an encoded "&lt;b&gt;" stays literal text.
    text = decodeEntities(text);
    return text.simplified();
}

double PropertyModel::effectiveStep(double min, double max, double step)
{
    if (step > 0)
        return step;
    if (min == std::floor(min) && max == std::floor(max))
        return 1.0;
    const double derived = (max - min) / 100.0;
    return derived > 0 ? derived : 1.0;
}

QVariant PropertyModel::normalized(const QString &type, const QVariant &value, const Row &row) const
{
    if (type == u"slider"_s) {
        double number = row.min;
        asNumber(value, number);
        return number;
    }
    if (type == u"bool"_s) {
        if (value.typeId() == QMetaType::QString) {
            const QString s = value.toString().trimmed().toLower();
            return s == u"true"_s || s == u"1"_s;
        }
        return value.toBool();
    }
    if (type == u"combo"_s) {
        if (!value.isValid() || value.isNull())
            return row.options.isEmpty() ? QVariant(QString()) : row.options.first().toMap().value(u"value"_s);
        return value;
    }
    return value.isValid() && !value.isNull() ? QVariant(value.toString()) : QVariant(QString());
}

bool PropertyModel::sameValue(const Row &row, const QVariant &a, const QVariant &b) const
{
    if (row.type == u"color"_s) {
        // "r g b" floats; compare by component so "1 0 0" equals "1.0 0.0 0.0".
        const QStringList pa = a.toString().simplified().split(u' ', Qt::SkipEmptyParts);
        const QStringList pb = b.toString().simplified().split(u' ', Qt::SkipEmptyParts);
        if (pa.size() == pb.size()) {
            bool same = true;
            for (qsizetype i = 0; i < pa.size() && same; ++i) {
                bool okA = false;
                bool okB = false;
                const double x = pa.at(i).toDouble(&okA);
                const double y = pb.at(i).toDouble(&okB);
                same = okA && okB ? std::abs(x - y) <= 1e-4 : pa.at(i) == pb.at(i);
            }
            if (same)
                return true;
        }
        return false;
    }
    if (isTextType(row.type))
        return a.toString() == b.toString();
    return looseEqual(a, b);
}

void PropertyModel::load(const QJsonObject &item, const QJsonObject &assignmentOverrides)
{
    load(item.toVariantMap(), assignmentOverrides.toVariantMap());
}

void PropertyModel::setLocale(const QString &localeName)
{
    m_locale = localeName;
}

QString PropertyModel::resolveLabel(const QString &raw, const QVariantMap &localization, const QString &localeName,
                                    const QString &fallbackKey)
{
    if (raw.isEmpty())
        return humanizeKey(fallbackKey);
    if (const QString found = lookupLocalized(raw, localization, localeName); !found.isEmpty())
        return plainText(found);
    if (!raw.startsWith(u"ui_"_s, Qt::CaseInsensitive))
        return plainText(raw);
    const QString lower = raw.toLower();
    for (const BuiltinLabel &label : kBuiltinLabels) {
        if (lower == QLatin1StringView(label.key))
            return QCoreApplication::translate("PropertyModel", label.text);
    }
    return humanizeKey(raw);
}

void PropertyModel::load(const QVariantMap &item, const QVariantMap &assignmentOverrides)
{
    const int oldCount = rowCount();
    beginResetModel();
    rebuild(item, assignmentOverrides);
    updateVisibility();
    endResetModel();
    if (rowCount() != oldCount)
        emit countChanged();
}

void PropertyModel::rebuild(const QVariantMap &item, const QVariantMap &assignmentOverrides)
{
    m_rows.clear();
    m_scope.clear();
    m_compiled.clear();
    m_overrides.clear();

    const QVariantMap preset = item.value(u"presetValues"_s).toMap();
    const QVariantMap localization = item.value(u"localization"_s).toMap();
    const QString localeName = m_locale.isEmpty() ? QLocale::system().name() : m_locale;
    struct Ordered
    {
        double order;
        int index;
        Row row;
    };
    QList<Ordered> ordered;
    QSet<QString> seen;
    const QVariantList properties = item.value(u"properties"_s).toList();
    for (int i = 0; i < properties.size(); ++i) {
        const QVariantMap p = properties.at(i).toMap();
        const QString key = p.value(u"key"_s).toString();
        if (key.isEmpty())
            continue;
        const QString type = p.value(u"type"_s).toString().toLower();

        Row row;
        row.key = key;
        row.type = type;
        row.text = resolveLabel(p.value(u"text"_s).toString(), localization, localeName, key);
        row.min = p.value(u"min"_s).toDouble();
        row.max = p.value(u"max"_s).toDouble();
        row.step = p.value(u"step"_s).toDouble();
        row.condition = p.value(u"condition"_s).toString();
        for (const QVariant &o : p.value(u"options"_s).toList()) {
            const QVariantMap om = o.toMap();
            row.options.append(QVariantMap{{u"label"_s,
                                            resolveLabel(om.value(u"label"_s).toString(), localization, localeName,
                                                         om.value(u"value"_s).toString())},
                                           {u"value"_s, om.value(u"value"_s)}});
        }

        // default -> preset (null keeps the default) -> assignment override
        QVariant base = p.value(u"value"_s);
        const QVariant presetValue = preset.value(key);
        if (presetValue.isValid() && !presetValue.isNull())
            base = presetValue;

        if (isIdentifier(key) && !seen.contains(key)) {
            seen.insert(key);
            m_scope.append({key, base}); // replaced below for editable rows
        }
        if (!isEditableType(type))
            continue;
        row.base = normalized(type, base, row);
        const QVariant override = assignmentOverrides.value(key);
        row.current = override.isValid() && !override.isNull() ? normalized(type, override, row) : row.base;
        ordered.append({p.value(u"order"_s).toDouble(), i, row});
    }
    // Override keys that are not rows (removed or non-editable properties) are carried through unchanged.
    for (auto it = assignmentOverrides.cbegin(); it != assignmentOverrides.cend(); ++it) {
        const bool isRow = std::any_of(ordered.cbegin(), ordered.cend(),
                                       [&](const Ordered &o) { return o.row.key == it.key(); });
        if (!isRow)
            m_overrides.insert(it.key(), it.value());
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Ordered &a, const Ordered &b) { return a.order < b.order; });
    for (Ordered &o : ordered)
        m_rows.append(std::move(o.row));
}

void PropertyModel::setValue(int row, const QVariant &value)
{
    if (row < 0 || row >= m_rows.size())
        return;
    Row &r = m_rows[row];
    const QVariant next = normalized(r.type, value, r);
    const bool changed = !sameValue(r, r.current, next);
    if (changed) {
        r.current = next;
        emit dataChanged(index(row), index(row), {ValueRole});
        for (int other : updateVisibility())
            emit dataChanged(index(other), index(other), {VisibleRole});
        emit edited();
    }
}

QVariantMap PropertyModel::overrides() const
{
    QVariantMap out = m_overrides;
    for (const Row &row : m_rows) {
        if (!sameValue(row, row.current, row.base))
            out.insert(row.key, row.current);
    }
    return out;
}

void PropertyModel::resetToDefaults()
{
    bool changed = !m_overrides.isEmpty();
    m_overrides.clear();
    QList<int> valueRows;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (!(r.current.metaType() == r.base.metaType() && r.current == r.base)) {
            r.current = r.base;
            valueRows.append(i);
            changed = true;
        }
    }
    for (int i : valueRows)
        emit dataChanged(index(i), index(i), {ValueRole});
    for (int i : updateVisibility())
        emit dataChanged(index(i), index(i), {VisibleRole});
    if (changed)
        emit edited();
}

void PropertyModel::ensureEngine()
{
    if (m_engine)
        return;
    m_engine = std::make_unique<QJSEngine>();
    QJSValue global = m_engine->globalObject();
    // Keep a way to freeze the scope objects, then strip the global object down to harmless built-ins.
    m_freeze = m_engine->evaluate(u"Object.freeze"_s);
    // Whatever a condition can still reach is made immutable so one row cannot change what another sees.
    m_engine->evaluate(
        u"(function(){ var o = Object.prototype;"
        u" delete String.prototype.repeat; delete String.prototype.padStart; delete String.prototype.padEnd;"
        u" ['__defineGetter__','__defineSetter__','__lookupGetter__','__lookupSetter__'].forEach(function(n){ delete o[n]; });"
        u" [Math, Number, String, Boolean, Array, Number.prototype, String.prototype, Boolean.prototype,"
        u"  Array.prototype, Function.prototype, o].forEach(function(x){ Object.freeze(x); }); })()"_s);
    static const QSet<QString> keep{u"Math"_s,    u"Number"_s,   u"String"_s,   u"Boolean"_s,   u"isNaN"_s,
                                    u"isFinite"_s, u"parseInt"_s, u"parseFloat"_s, u"NaN"_s, u"Infinity"_s,
                                    u"undefined"_s};
    const QJSValue names = m_engine->evaluate(u"Object.getOwnPropertyNames(this)"_s);
    const quint32 length = names.property(u"length"_s).toUInt();
    for (quint32 i = 0; i < length; ++i) {
        const QString name = names.property(i).toString();
        if (!keep.contains(name))
            global.setProperty(name, QJSValue(QJSValue::UndefinedValue));
    }
}

QJSValue PropertyModel::compile(const QString &condition)
{
    const auto cached = m_compiled.constFind(condition);
    if (cached != m_compiled.constEnd())
        return *cached;

    QJSValue function(QJSValue::UndefinedValue);
    const QString expr = stripDollars(condition);
    QSet<QString> keys;
    for (const ScopeEntry &entry : std::as_const(m_scope))
        keys.insert(entry.key);
    if (!conditionAllowed(expr, keys)) {
        if (!m_warned.contains(condition)) {
            m_warned.insert(condition);
            qWarning().noquote() << "PropertyModel: ignoring unsafe condition expression:" << condition.left(80);
        }
    } else {
        QStringList names;
        for (const ScopeEntry &entry : std::as_const(m_scope))
            names << entry.key;
        // The newline keeps a trailing "//" in the expression from swallowing the closing parenthesis.
        const QString source = u"(function("_s + names.join(u',') + u"){ \"use strict\"; return !!("_s + expr
                               + u"\n); })"_s;
        const QJSValue compiled = m_engine->evaluate(source);
        if (compiled.isCallable())
            function = compiled;
    }
    m_compiled.insert(condition, function);
    return function;
}

bool PropertyModel::evaluate(const QString &condition, const QJSValueList &args)
{
    const QJSValue function = compile(condition);
    if (!function.isCallable())
        return true;
    const QJSValue result = function.call(args);
    if (result.isError() || !result.isBool())
        return true;
    return result.toBool();
}

QList<int> PropertyModel::updateVisibility()
{
    QList<int> changed;
    bool anyCondition = false;
    for (const Row &row : std::as_const(m_rows))
        anyCondition = anyCondition || !row.condition.isEmpty();
    if (!anyCondition) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (!m_rows[i].visible) {
                m_rows[i].visible = true;
                changed.append(i);
            }
        }
        return changed;
    }

    ensureEngine();
    QHash<QString, QVariant> effective;
    for (const Row &row : std::as_const(m_rows))
        effective.insert(row.key, row.current);
    // One frozen {value} object per key; frozen so an expression cannot alter what later ones see.
    QJSValueList args;
    for (const ScopeEntry &entry : std::as_const(m_scope)) {
        QJSValue object = m_engine->newObject();
        object.setProperty(u"value"_s, m_engine->toScriptValue(effective.value(entry.key, entry.value)));
        args.append(m_freeze.call({object}));
    }
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &row = m_rows[i];
        const bool visible = row.condition.trimmed().isEmpty() ? true : evaluate(row.condition, args);
        if (visible != row.visible) {
            row.visible = visible;
            changed.append(i);
        }
    }
    return changed;
}

} // namespace murasaki
