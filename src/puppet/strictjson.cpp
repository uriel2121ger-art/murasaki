#include "puppet/strictjson.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace murasaki::puppet::json {

const Value *Value::find(std::string_view key) const
{
    if (kind != Kind::Object)
        return nullptr;
    for (const auto &member : members)
        if (member.first == key)
            return &member.second;
    return nullptr;
}

bool validUtf8(std::string_view bytes)
{
    size_t i = 0;
    const size_t n = bytes.size();
    const auto at = [&](size_t k) { return static_cast<unsigned char>(bytes[k]); };
    while (i < n) {
        const unsigned char c = at(i);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        std::uint32_t code = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
            code = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            code = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            code = c & 0x07;
        } else {
            return false;
        }
        if (i + length > n)
            return false;
        for (size_t k = 1; k < length; ++k) {
            if ((at(i + k) & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (at(i + k) & 0x3F);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

namespace {

class Parser
{
public:
    Parser(std::string_view text, const Limits &limits) : m_text(text), m_limits(limits) {}

    ParseResult run()
    {
        ParseResult result;
        if (m_text.substr(0, 3) == "\xEF\xBB\xBF")
            m_pos = 3;
        if (!validUtf8(m_text.substr(m_pos))) {
            result.error = "not valid UTF-8";
            return result;
        }
        skipSpace();
        if (!value(result.value, 1)) {
            result.error = m_error;
            result.offset = m_pos;
            result.value = {};
            return result;
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            result.error = "text after the value";
            result.offset = m_pos;
            result.value = {};
            return result;
        }
        result.ok = true;
        return result;
    }

private:
    bool fail(const char *why)
    {
        if (m_error.empty())
            m_error = why;
        return false;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++m_pos;
        }
    }

    bool count()
    {
        if (++m_values > m_limits.maxValues)
            return fail("too many values");
        return true;
    }

    bool value(Value &out, long long depth)
    {
        if (!count())
            return false;
        if (m_pos >= m_text.size())
            return fail("unexpected end");
        const char c = m_text[m_pos];
        switch (c) {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out.kind = Kind::String;
            return string(out.text);
        case 't':
            return literal("true", out, Kind::Bool, true);
        case 'f':
            return literal("false", out, Kind::Bool, false);
        case 'n':
            return literal("null", out, Kind::Null, false);
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return number(out);
            return fail("unexpected character");
        }
    }

    bool literal(std::string_view word, Value &out, Kind kind, bool boolean)
    {
        if (m_text.substr(m_pos, word.size()) != word)
            return fail("unexpected character");
        m_pos += word.size();
        out.kind = kind;
        out.boolean = boolean;
        return true;
    }

    bool number(Value &out)
    {
        const size_t start = m_pos;
        size_t p = m_pos;
        const auto digit = [&](size_t k) { return k < m_text.size() && m_text[k] >= '0' && m_text[k] <= '9'; };
        if (m_text[p] == '-')
            ++p;
        if (!digit(p))
            return fail("bad number");
        if (m_text[p] == '0') {
            ++p;
            if (digit(p))
                return fail("leading zero");
        } else {
            while (digit(p))
                ++p;
        }
        if (p < m_text.size() && m_text[p] == '.') {
            ++p;
            if (!digit(p))
                return fail("bad number");
            while (digit(p))
                ++p;
        }
        if (p < m_text.size() && (m_text[p] == 'e' || m_text[p] == 'E')) {
            ++p;
            if (p < m_text.size() && (m_text[p] == '+' || m_text[p] == '-'))
                ++p;
            if (!digit(p))
                return fail("bad number");
            while (digit(p))
                ++p;
        }
        double parsed = 0;
        const auto [end, error] = std::from_chars(m_text.data() + start, m_text.data() + p, parsed);
        if (error != std::errc() || end != m_text.data() + p || !std::isfinite(parsed))
            return fail("number out of range");
        m_pos = p;
        out.kind = Kind::Number;
        out.number = parsed;
        return true;
    }

    static void appendUtf8(std::string &out, std::uint32_t code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(std::uint32_t &out)
    {
        if (m_pos + 4 > m_text.size())
            return fail("bad escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_text[m_pos + k];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("bad escape");
        }
        m_pos += 4;
        return true;
    }

    bool string(std::string &out)
    {
        ++m_pos; // the opening quote
        out.clear();
        while (true) {
            if (m_pos >= m_text.size())
                return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"') {
                ++m_pos;
                return true;
            }
            if (c < 0x20)
                return fail("control character in a string");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++m_pos;
            } else {
                ++m_pos;
                if (m_pos >= m_text.size())
                    return fail("unterminated string");
                const char e = m_text[m_pos++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!hex4(code))
                        return false;
                    if (code >= 0xDC00 && code <= 0xDFFF)
                        return fail("lone surrogate");
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (m_text.substr(m_pos, 2) != "\\u")
                            return fail("lone surrogate");
                        m_pos += 2;
                        if (!hex4(low))
                            return false;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail("lone surrogate");
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, code);
                    break;
                }
                default:
                    return fail("bad escape");
                }
            }
            if (static_cast<long long>(out.size()) > m_limits.maxStringBytes)
                return fail("string too long");
        }
    }

    bool array(Value &out, long long depth)
    {
        if (depth > m_limits.maxDepth)
            return fail("nested too deep");
        out.kind = Kind::Array;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
                return false;
            if (item.kind == Kind::Number && out.items.empty()) {
                out.numbers.push_back(item.number);
            } else {
                if (!out.numbers.empty()) { // a number list that turns out to be mixed: unpack it
                    for (const double number : out.numbers) {
                        Value n;
                        n.kind = Kind::Number;
                        n.number = number;
                        out.items.push_back(std::move(n));
                    }
                    out.numbers.clear();
                    out.numbers.shrink_to_fit();
                }
                out.items.push_back(std::move(item));
            }
            skipSpace();
            if (m_pos >= m_text.size())
                return fail("unexpected end");
            const char c = m_text[m_pos++];
            if (c == ']')
                return true;
            if (c != ',')
                return fail("expected , or ]");
        }
    }

    bool object(Value &out, long long depth)
    {
        if (depth > m_limits.maxDepth)
            return fail("nested too deep");
        out.kind = Kind::Object;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        std::unordered_set<std::string> seen;
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return fail("expected a key");
            std::string key;
            if (!string(key))
                return false;
            if (!seen.insert(key).second)
                return fail("repeated key");
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return fail("expected :");
            ++m_pos;
            skipSpace();
            Value member;
            if (!value(member, depth + 1))
                return false;
            out.members.emplace_back(std::move(key), std::move(member));
            skipSpace();
            if (m_pos >= m_text.size())
                return fail("unexpected end");
            const char c = m_text[m_pos++];
            if (c == '}')
                return true;
            if (c != ',')
                return fail("expected , or }");
        }
    }

    std::string_view m_text;
    Limits m_limits;
    size_t m_pos = 0;
    long long m_values = 0;
    std::string m_error;
};

} // namespace

ParseResult parse(std::string_view text, const Limits &limits)
{
    return Parser(text, limits).run();
}

} // namespace murasaki::puppet::json
