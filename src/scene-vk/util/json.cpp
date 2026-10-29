#include "util/json.h"

#include <charconv>
#include <cmath>
#include <format>

namespace murasaki::scene {

const Json *Json::find(std::string_view key) const
{
    if (m_type != Type::Object) {
        return nullptr;
    }
    for (const auto &[name, value] : m_object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string Json::stringOr(std::string_view key, std::string fallback) const
{
    const Json *v = find(key);
    return v && v->isString() ? v->m_string : fallback;
}

double Json::numberOr(std::string_view key, double fallback) const
{
    const Json *v = find(key);
    return v && v->isNumber() ? v->m_number : fallback;
}

bool Json::boolOr(std::string_view key, bool fallback) const
{
    const Json *v = find(key);
    return v && v->isBool() ? v->m_bool : fallback;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser
{
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Json> run(std::string *error)
    {
        // UTF-8 byte order mark
        if (m_text.substr(0, 3) == "\xEF\xBB\xBF") {
            m_pos = 3;
        }
        std::optional<Json> value = parseValue(0);
        if (value) {
            skipSpace();
            if (m_pos != m_text.size()) {
                value.reset();
                fail("unexpected data after the document");
            }
        }
        if (!value && error) {
            *error = m_error;
        }
        return value;
    }

private:
    void fail(const std::string &what)
    {
        if (m_error.empty()) {
            m_error = std::format("{} at offset {}", what, m_pos);
        }
    }

    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool consume(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) == word) {
            m_pos += word.size();
            return true;
        }
        return false;
    }

    std::optional<Json> parseValue(int depth)
    {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return std::nullopt;
        }
        skipSpace();
        if (m_pos >= m_text.size()) {
            fail("unexpected end of input");
            return std::nullopt;
        }
        const char c = m_text[m_pos];
        switch (c) {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"': {
            auto s = parseString();
            return s ? std::optional<Json>(Json(std::move(*s))) : std::nullopt;
        }
        case 't':
            if (consume("true")) {
                return Json(true);
            }
            break;
        case 'f':
            if (consume("false")) {
                return Json(false);
            }
            break;
        case 'n':
            if (consume("null")) {
                return Json();
            }
            break;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber();
            }
            break;
        }
        fail("unexpected character");
        return std::nullopt;
    }

    std::optional<Json> parseNumber()
    {
        const size_t start = m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                ++m_pos;
            } else {
                break;
            }
        }
        double value = 0.0;
        const auto [end, ec] = std::from_chars(m_text.data() + start, m_text.data() + m_pos, value);
        if (ec != std::errc() || end != m_text.data() + m_pos || !std::isfinite(value)) {
            m_pos = start;
            fail("bad number");
            return std::nullopt;
        }
        return Json(value);
    }

    static void appendUtf8(std::string &out, uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<uint32_t> parseHex4()
    {
        if (m_pos + 4 > m_text.size()) {
            fail("short \\u escape");
            return std::nullopt;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                fail("bad \\u escape");
                return std::nullopt;
            }
        }
        return value;
    }

    std::optional<std::string> parseString()
    {
        ++m_pos; // opening quote
        std::string out;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                break;
            }
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
                auto cp = parseHex4();
                if (!cp) {
                    return std::nullopt;
                }
                if (*cp >= 0xD800 && *cp < 0xDC00 && consume("\\u")) {
                    auto low = parseHex4();
                    if (!low) {
                        return std::nullopt;
                    }
                    if (*low >= 0xDC00 && *low < 0xE000) {
                        *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                    }
                }
                appendUtf8(out, *cp);
                break;
            }
            default:
                fail("bad escape");
                return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Json> parseArray(int depth)
    {
        ++m_pos; // [
        Json::Array items;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return Json(std::move(items));
        }
        while (true) {
            auto value = parseValue(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            items.push_back(std::move(*value));
            skipSpace();
            if (m_pos >= m_text.size()) {
                fail("unterminated array");
                return std::nullopt;
            }
            const char c = m_text[m_pos++];
            if (c == ']') {
                return Json(std::move(items));
            }
            if (c != ',') {
                --m_pos;
                fail("expected ',' or ']'");
                return std::nullopt;
            }
            // WE's own files sometimes end a list with a comma (effects/fluidsimulation): accepted.
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return Json(std::move(items));
            }
        }
    }

    std::optional<Json> parseObject(int depth)
    {
        ++m_pos; // {
        Json::Object members;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return Json(std::move(members));
        }
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                fail("expected a member name");
                return std::nullopt;
            }
            auto key = parseString();
            if (!key) {
                return std::nullopt;
            }
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++m_pos;
            auto value = parseValue(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            members.emplace_back(std::move(*key), std::move(*value));
            skipSpace();
            if (m_pos >= m_text.size()) {
                fail("unterminated object");
                return std::nullopt;
            }
            const char c = m_text[m_pos++];
            if (c == '}') {
                return Json(std::move(members));
            }
            if (c != ',') {
                --m_pos;
                fail("expected ',' or '}'");
                return std::nullopt;
            }
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return Json(std::move(members));
            }
        }
    }

    std::string_view m_text;
    size_t m_pos = 0;
    std::string m_error;
};

} // namespace

std::optional<Json> parseJson(std::string_view text, std::string *error)
{
    return Parser(text).run(error);
}

} // namespace murasaki::scene
