#include "vdf.h"

#include <QFile>

namespace murasaki {
namespace {

// Real files nest ~4 levels; this bounds recursion on malformed input.
constexpr int kMaxDepth = 64;

// Explicit ASCII whitespace (locale-independent): ' ' and \t..\r.
constexpr bool isSpace(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

class Lexer
{
public:
    enum class Kind { String, Open, Close, End, Error };
    struct Token {
        Kind kind;
        QString text;
    };

    explicit Lexer(QByteArrayView text)
        : m_text(text)
    {
    }

    Token next()
    {
        for (;;) {
            skipSpaceAndComments();
            if (m_pos >= m_text.size())
                return {Kind::End, {}};
            const char c = m_text[m_pos];
            if (c == '{') {
                ++m_pos;
                return {Kind::Open, {}};
            }
            if (c == '}') {
                ++m_pos;
                return {Kind::Close, {}};
            }
            if (c == '[') {
                // Platform conditional such as [$WIN32]: Murasaki ignores them.
                while (m_pos < m_text.size() && m_text[m_pos] != ']')
                    ++m_pos;
                if (m_pos >= m_text.size())
                    return {Kind::Error, {}};
                ++m_pos;
                continue; // loop instead of recursing: many conditionals must not grow the stack
            }
            if (c == '"')
                return quoted();
            return bare();
        }
    }

private:
    void skipSpaceAndComments()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (isSpace(c)) {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
            } else {
                return;
            }
        }
    }

    Token quoted()
    {
        ++m_pos; // opening quote
        QByteArray out;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"')
                return {Kind::String, QString::fromUtf8(out)};
            if (c == '\\' && m_pos < m_text.size()) {
                const char escaped = m_text[m_pos++];
                switch (escaped) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                default: // unknown escapes keep just the following character
                    out += escaped;
                }
                continue;
            }
            out += c;
        }
        return {Kind::Error, {}};
    }

    Token bare()
    {
        const qsizetype start = m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (isSpace(c) || c == '{' || c == '}' || c == '"')
                break;
            ++m_pos;
        }
        return {Kind::String, QString::fromUtf8(m_text.sliced(start, m_pos - start))};
    }

    QByteArrayView m_text;
    qsizetype m_pos = 0;
};

// Reads "key value" and "key { ... }" pairs until the matching '}' (nested) or end of input (top level).
bool parseBlock(Lexer &lexer, QVariantMap &out, bool nested, int depth)
{
    if (depth > kMaxDepth)
        return false;
    for (;;) {
        const Lexer::Token key = lexer.next();
        if (key.kind == Lexer::Kind::End)
            return !nested;
        if (key.kind == Lexer::Kind::Close)
            return nested;
        if (key.kind != Lexer::Kind::String)
            return false;

        const Lexer::Token value = lexer.next();
        if (value.kind == Lexer::Kind::String) {
            out.insert(key.text, value.text);
        } else if (value.kind == Lexer::Kind::Open) {
            QVariantMap child;
            if (!parseBlock(lexer, child, true, depth + 1))
                return false;
            out.insert(key.text, child);
        } else {
            return false;
        }
    }
}

} // namespace

std::optional<QVariantMap> parseVdf(QByteArrayView text)
{
    if (text.startsWith("\xEF\xBB\xBF"))
        text = text.sliced(3);
    Lexer lexer(text);
    QVariantMap root;
    if (!parseBlock(lexer, root, false, 0))
        return std::nullopt;
    return root;
}

std::optional<QVariantMap> readVdfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseVdf(file.readAll());
}

QVariant vdfValue(const QVariantMap &map, QStringView key)
{
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().compare(key, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return {};
}

} // namespace murasaki
