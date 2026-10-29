#include "mddoparser.h"

#include "petimport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringDecoder>

#include <cmath>
#include <tuple>

namespace murasaki::import {
namespace {

struct Value {
    enum Kind { Int, Float, Str, Bool, None, List } kind = None;
    qint64 integer = 0;
    double real = 0;
    QString text;
    bool boolean = false;
    QList<Value> items;
};

class Literal
{
public:
    Literal(const QString &text, const MddoLimits &limits)
        : m_text(text)
        , m_limits(limits)
    {
    }

    std::optional<Value> parse(QString *reason)
    {
        m_reason = reason;
        auto value = parseValue(0);
        if (!value)
            return std::nullopt;
        skip();
        if (m_pos != m_text.size()) {
            fail(QStringLiteral("unexpected text after the value"));
            return std::nullopt;
        }
        return value;
    }

private:
    void fail(const QString &message)
    {
        if (m_reason && m_reason->isEmpty())
            *m_reason = message;
    }
    void skip()
    {
        while (m_pos < m_text.size() && (m_text.at(m_pos) == QLatin1Char(' ') || m_text.at(m_pos) == QLatin1Char('\t')))
            ++m_pos;
    }
    bool atEnd() const { return m_pos >= m_text.size(); }

    std::optional<Value> parseValue(int depth)
    {
        skip();
        if (atEnd()) {
            fail(QStringLiteral("a value is missing"));
            return std::nullopt;
        }
        const QChar c = m_text.at(m_pos);
        if (c == QLatin1Char('[') || c == QLatin1Char('('))
            return parseSequence(depth);
        if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
            return parseString();
        if (c == QLatin1Char('+') || c == QLatin1Char('-') || c.isDigit() || c == QLatin1Char('.'))
            return parseNumber();
        for (const auto &[word, kind, truth] : {std::tuple{QLatin1String("True"), Value::Bool, true}, std::tuple{QLatin1String("False"), Value::Bool, false},
                                                std::tuple{QLatin1String("None"), Value::None, false}}) {
            if (m_text.mid(m_pos, word.size()) == word && (m_pos + word.size() == m_text.size() || !m_text.at(m_pos + word.size()).isLetterOrNumber())
                && (m_pos + word.size() == m_text.size() || m_text.at(m_pos + word.size()) != QLatin1Char('_'))) {
                m_pos += word.size();
                Value v;
                v.kind = kind;
                v.boolean = truth;
                return v;
            }
        }
        fail(QStringLiteral("not a literal (names, calls and operators are not accepted)"));
        return std::nullopt;
    }

    std::optional<Value> parseSequence(int depth)
    {
        if (depth >= m_limits.maxDepth) {
            fail(QStringLiteral("nested too deeply"));
            return std::nullopt;
        }
        const QChar open = m_text.at(m_pos++);
        const QChar close = open == QLatin1Char('[') ? QLatin1Char(']') : QLatin1Char(')');
        Value list;
        list.kind = Value::List;
        bool comma = false;
        for (;;) {
            skip();
            if (atEnd()) {
                fail(QStringLiteral("a bracket is not closed"));
                return std::nullopt;
            }
            if (m_text.at(m_pos) == close) {
                ++m_pos;
                break;
            }
            if (!list.items.isEmpty() && !comma) {
                fail(QStringLiteral("a comma is missing"));
                return std::nullopt;
            }
            if (list.items.size() >= m_limits.maxItems) {
                fail(QStringLiteral("too many items"));
                return std::nullopt;
            }
            auto item = parseValue(depth + 1);
            if (!item)
                return std::nullopt;
            list.items.append(*item);
            skip();
            comma = false;
            if (!atEnd() && m_text.at(m_pos) == QLatin1Char(',')) {
                ++m_pos;
                comma = true;
            }
        }
        // (x) is x, as in Python; (x,) and () are tuples
        if (open == QLatin1Char('(') && list.items.size() == 1 && !comma)
            return list.items.first();
        return list;
    }

    std::optional<Value> parseString()
    {
        const QChar quote = m_text.at(m_pos++);
        // a triple quote, or a doubled quote used to start a longer string, is not accepted
        if (m_pos + 1 < m_text.size() && m_text.at(m_pos) == quote && m_text.at(m_pos + 1) == quote) {
            fail(QStringLiteral("triple-quoted strings are not accepted"));
            return std::nullopt;
        }
        QString out;
        for (;;) {
            if (atEnd()) {
                fail(QStringLiteral("a string is not closed"));
                return std::nullopt;
            }
            const QChar c = m_text.at(m_pos++);
            if (c == quote)
                break;
            if (c.unicode() < 0x20 && c != QLatin1Char('\t')) {
                fail(QStringLiteral("a control character in a string"));
                return std::nullopt;
            }
            if (c != QLatin1Char('\\')) {
                out.append(c);
            } else {
                if (atEnd()) {
                    fail(QStringLiteral("a string is not closed"));
                    return std::nullopt;
                }
                const QChar e = m_text.at(m_pos++);
                switch (e.unicode()) {
                case '\\': out.append(QLatin1Char('\\')); break;
                case '\'': out.append(QLatin1Char('\'')); break;
                case '"': out.append(QLatin1Char('"')); break;
                case 'n': out.append(QLatin1Char('\n')); break;
                case 'r': out.append(QLatin1Char('\r')); break;
                case 't': out.append(QLatin1Char('\t')); break;
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                    // Python reads \NNN as an octal code (\012 is a line feed): a different meaning from a NUL followed by digits, so neither is guessed.
                    fail(QStringLiteral("octal escapes are not read"));
                    return std::nullopt;
                case 'x':
                case 'u': {
                    const int digits = e == QLatin1Char('x') ? 2 : 4;
                    bool ok = m_pos + digits <= m_text.size();
                    uint code = 0;
                    for (int i = 0; ok && i < digits; ++i) {
                        const int digit = QString(m_text.at(m_pos + i)).toInt(&ok, 16);
                        code = code * 16 + uint(digit);
                    }
                    if (!ok) {
                        fail(QStringLiteral("a bad \\x or \\u escape"));
                        return std::nullopt;
                    }
                    m_pos += digits;
                    out.append(QChar(ushort(code)));
                    break;
                }
                default:
                    fail(QStringLiteral("an escape this reader does not accept"));
                    return std::nullopt;
                }
            }
            if (out.toUtf8().size() > m_limits.maxStringBytes) {
                fail(QStringLiteral("a string over the limit"));
                return std::nullopt;
            }
        }
        // adjacent literals ("a" "b") would be concatenated by Python: refused
        skip();
        if (!atEnd() && (m_text.at(m_pos) == QLatin1Char('\'') || m_text.at(m_pos) == QLatin1Char('"'))) {
            fail(QStringLiteral("neighbouring strings are not joined"));
            return std::nullopt;
        }
        Value v;
        v.kind = Value::Str;
        v.text = out;
        return v;
    }

    std::optional<Value> parseNumber()
    {
        const qsizetype start = m_pos;
        if (m_text.at(m_pos) == QLatin1Char('+') || m_text.at(m_pos) == QLatin1Char('-'))
            ++m_pos;
        qsizetype digitsBefore = 0, digitsAfter = 0;
        while (!atEnd() && m_text.at(m_pos).isDigit() && m_text.at(m_pos).unicode() < 0x80) {
            ++m_pos;
            ++digitsBefore;
        }
        bool real = false;
        if (!atEnd() && m_text.at(m_pos) == QLatin1Char('.')) {
            real = true;
            ++m_pos;
            while (!atEnd() && m_text.at(m_pos).isDigit() && m_text.at(m_pos).unicode() < 0x80) {
                ++m_pos;
                ++digitsAfter;
            }
        }
        if (digitsBefore + digitsAfter == 0) {
            fail(QStringLiteral("not a number"));
            return std::nullopt;
        }
        if (!atEnd() && (m_text.at(m_pos) == QLatin1Char('e') || m_text.at(m_pos) == QLatin1Char('E'))) {
            real = true;
            ++m_pos;
            if (!atEnd() && (m_text.at(m_pos) == QLatin1Char('+') || m_text.at(m_pos) == QLatin1Char('-')))
                ++m_pos;
            qsizetype exponentDigits = 0;
            while (!atEnd() && m_text.at(m_pos).isDigit() && m_text.at(m_pos).unicode() < 0x80) {
                ++m_pos;
                ++exponentDigits;
            }
            if (exponentDigits == 0 || exponentDigits > 4) {
                fail(QStringLiteral("a bad exponent"));
                return std::nullopt;
            }
        }
        if (!atEnd() && (m_text.at(m_pos).isLetterOrNumber() || m_text.at(m_pos) == QLatin1Char('_') || m_text.at(m_pos) == QLatin1Char('.'))) {
            fail(QStringLiteral("not a plain decimal number"));
            return std::nullopt;
        }
        const QString text = m_text.mid(start, m_pos - start);
        Value v;
        bool ok = false;
        if (real) {
            v.kind = Value::Float;
            v.real = text.toDouble(&ok);
            if (!ok || !std::isfinite(v.real)) {
                fail(QStringLiteral("a number out of range"));
                return std::nullopt;
            }
        } else {
            if (digitsBefore > 15) {
                fail(QStringLiteral("a number out of range"));
                return std::nullopt;
            }
            v.kind = Value::Int;
            v.integer = text.toLongLong(&ok);
            if (!ok) {
                fail(QStringLiteral("a number out of range"));
                return std::nullopt;
            }
        }
        return v;
    }

    QString m_text;
    MddoLimits m_limits;
    qsizetype m_pos = 0;
    QString *m_reason = nullptr;
};

double numberOf(const Value &v) { return v.kind == Value::Int ? double(v.integer) : v.real; }
bool isNumber(const Value &v) { return v.kind == Value::Int || v.kind == Value::Float; }

// An item as a Value, from a JSON value of the same shapes.
std::optional<Value> fromJson(const QJsonValue &json, int depth, const MddoLimits &limits)
{
    if (depth > limits.maxDepth)
        return std::nullopt;
    Value v;
    if (json.isArray()) {
        const QJsonArray array = json.toArray();
        if (array.size() > limits.maxItems || depth >= limits.maxDepth) // the same nesting limit as the literal reader
            return std::nullopt;
        v.kind = Value::List;
        for (const QJsonValue &item : array) {
            auto child = fromJson(item, depth + 1, limits);
            if (!child)
                return std::nullopt;
            v.items.append(*child);
        }
    } else if (json.isString()) {
        v.kind = Value::Str;
        v.text = json.toString();
        if (v.text.toUtf8().size() > limits.maxStringBytes)
            return std::nullopt;
    } else if (json.isBool()) {
        v.kind = Value::Bool;
        v.boolean = json.toBool();
    } else if (json.isNull()) {
        v.kind = Value::None;
    } else if (json.isDouble()) {
        const double d = json.toDouble();
        if (!std::isfinite(d))
            return std::nullopt;
        // JSON has one number type: 1 and 1.0 are an int and a float in Python's json, and 1e2 is a float. Qt cannot tell; an integral value written
        // without a point or exponent is taken for an int by the line's own text (the caller passes `integralText`), so here: integral means int.
        if (d == std::floor(d) && std::fabs(d) < 1e15) {
            v.kind = Value::Int;
            v.integer = qint64(d);
        } else {
            v.kind = Value::Float;
            v.real = d;
        }
    } else {
        return std::nullopt;
    }
    return v;
}

// The position string "N-(x,y)": split once on '-' ; N other than 1 means no move; 1 means a literal tuple of at least two numbers follows.
std::optional<MddoAction> positionFromString(MddoAction action, const QString &text, const MddoLimits &limits, QString *reason)
{
    const qsizetype dash = text.indexOf(QLatin1Char('-'));
    if (dash < 0) {
        *reason = QStringLiteral("the position text has no '-'");
        return std::nullopt;
    }
    bool ok = false;
    const qint64 n = text.left(dash).trimmed().toLongLong(&ok);
    if (!ok) {
        *reason = QStringLiteral("the position text does not start with a number");
        return std::nullopt;
    }
    if (n != 1) {
        action.hasPosition = false;
        return action;
    }
    QString inner;
    Literal literal(text.mid(dash + 1).trimmed(), limits);
    auto value = literal.parse(&inner);
    if (!value || value->kind != Value::List || value->items.size() < 2 || !isNumber(value->items.at(0)) || !isNumber(value->items.at(1))) {
        *reason = QStringLiteral("invalid coordinates");
        return std::nullopt;
    }
    action.x = numberOf(value->items.at(0));
    action.y = numberOf(value->items.at(1));
    if (std::fabs(action.x) > limits.maxCoordinate || std::fabs(action.y) > limits.maxCoordinate) {
        *reason = QStringLiteral("coordinates out of range");
        return std::nullopt;
    }
    action.hasPosition = true;
    return action;
}

} // namespace

std::optional<MddoAction> parseMddoLine(const QByteArray &line, int lineNumber, QString *reason, const MddoLimits &limits)
{
    QString local;
    QString *why = reason ? reason : &local;
    why->clear();
    if (line.size() > limits.maxLineBytes) {
        *why = QStringLiteral("line over %1 bytes").arg(limits.maxLineBytes);
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(line);
    if (decoder.hasError()) {
        *why = QStringLiteral("not valid UTF-8");
        return std::nullopt;
    }
    for (const QChar c : text) {
        if (c.unicode() == 0 || (c.unicode() < 0x20 && c != QLatin1Char('\t') && c != QLatin1Char('\r'))) {
            *why = QStringLiteral("a control character");
            return std::nullopt;
        }
    }
    // JSON first (strict: Qt rejects comments and trailing commas), else the literal grammar
    std::optional<Value> value;
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &jsonError);
    if (jsonError.error == QJsonParseError::NoError && document.isArray()) {
        value = fromJson(QJsonValue(document.array()), 0, limits);
        if (!value) {
            *why = QStringLiteral("over the limits of a line");
            return std::nullopt;
        }
    } else {
        Literal literal(text.trimmed(), limits);
        value = literal.parse(why);
        if (!value)
            return std::nullopt;
    }
    if (value->kind != Value::List || value->items.size() != 3) {
        *why = QStringLiteral("a line must be a list or tuple of exactly three items");
        return std::nullopt;
    }
    const Value &activation = value->items.at(0), &path = value->items.at(1), &position = value->items.at(2);
    if (activation.kind != Value::Int || (activation.integer != 0 && activation.integer != 1)) {
        *why = QStringLiteral("the activation must be the integer 0 or 1");
        return std::nullopt;
    }
    if (path.kind != Value::Str || path.text.trimmed().isEmpty()) {
        *why = QStringLiteral("the path must be a text");
        return std::nullopt;
    }
    // Decoded, not as written: \x00, \u0000 and the JSON escapes arrive here as real characters, and a NUL or a control in a path truncates or hides it.
    for (qsizetype i = 0; i < path.text.size(); ++i) {
        const char16_t c = path.text.at(i).unicode();
        const bool pair = QChar::isHighSurrogate(c) && i + 1 < path.text.size() && QChar::isLowSurrogate(path.text.at(i + 1).unicode());
        if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0) || (QChar::isSurrogate(c) && !pair)) {
            *why = QStringLiteral("the path has a control character");
            return std::nullopt;
        }
        if (pair)
            ++i;
    }
    MddoAction action;
    action.line = lineNumber;
    action.show = activation.integer == 1;
    action.path = path.text;
    if (position.kind == Value::Str)
        return positionFromString(action, position.text, limits, why);
    if (position.kind == Value::List && position.items.size() >= 2 && isNumber(position.items.at(0)) && isNumber(position.items.at(1))) {
        action.x = numberOf(position.items.at(0));
        action.y = numberOf(position.items.at(1));
        if (std::fabs(action.x) > limits.maxCoordinate || std::fabs(action.y) > limits.maxCoordinate) {
            *why = QStringLiteral("coordinates out of range");
            return std::nullopt;
        }
        action.hasPosition = true;
        return action;
    }
    *why = QStringLiteral("the position must be (x, y) or a text \"N-(x,y)\"");
    return std::nullopt;
}

MddoTable parseMddo(const QByteArray &file, const MddoLimits &limits)
{
    MddoTable table;
    if (file.size() > limits.maxFileBytes) {
        table.error = QString::fromUtf8(kMdaaTableRefused.data(), qsizetype(kMdaaTableRefused.size())) + QStringLiteral(": over %1 bytes").arg(limits.maxFileBytes);
        return table;
    }
    QByteArray data = file;
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    qsizetype pos = 0;
    int lineNumber = 0;
    while (pos <= data.size()) {
        qsizetype end = data.indexOf('\n', pos);
        if (end < 0)
            end = data.size();
        QByteArray line = data.mid(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        const bool remnant = end == data.size() && line.isEmpty(); // what follows the last newline is not a line
        if (lineNumber > limits.maxLines && !remnant) {
            table.actions.clear();
            table.refused.clear();
            table.error = QString::fromUtf8(kMdaaTableRefused.data(), qsizetype(kMdaaTableRefused.size())) + QStringLiteral(": over %1 lines").arg(limits.maxLines);
            return table;
        }
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        QString reason;
        if (const auto action = parseMddoLine(line, lineNumber, &reason, limits))
            table.actions.append(*action);
        else
            table.refused.append({lineNumber, reason});
        if (pos > data.size())
            break;
    }
    return table;
}

} // namespace murasaki::import
