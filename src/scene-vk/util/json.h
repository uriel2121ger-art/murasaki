#pragma once

// A small JSON reader for scene.json, models, materials and project.json. Wallpaper Engine files are plain JSON
// with a few habits worth knowing: a UTF-8 byte order mark, tab indentation, and numbers written in any of the
// forms printf produces ("0.7", "-0.000", "1e-05").
//
// Objects keep the order of their members, which the scene needs (draw order is file order). A comma before a
// closing bracket is accepted: some of WE's own effect files have one.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace murasaki::scene {

class Json
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    explicit Json(bool b) : m_type(Type::Bool), m_bool(b) {}
    explicit Json(double n) : m_type(Type::Number), m_number(n) {}
    explicit Json(std::string s) : m_type(Type::String), m_string(std::move(s)) {}
    explicit Json(Array a) : m_type(Type::Array), m_array(std::move(a)) {}
    explicit Json(Object o) : m_type(Type::Object), m_object(std::move(o)) {}

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool fallback = false) const { return isBool() ? m_bool : fallback; }
    double asNumber(double fallback = 0.0) const { return isNumber() ? m_number : fallback; }
    const std::string &asString() const { return m_string; } // empty unless isString()
    const Array &items() const { return m_array; }          // empty unless isArray()
    const Object &members() const { return m_object; }      // empty unless isObject()

    // Member lookup; nullptr when this is not an object or the key is absent. Keys match exactly.
    const Json *find(std::string_view key) const;
    // Convenience readers that tolerate a missing member or a different type.
    std::string stringOr(std::string_view key, std::string fallback = {}) const;
    double numberOr(std::string_view key, double fallback) const;
    bool boolOr(std::string_view key, bool fallback) const;

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    Array m_array;
    Object m_object;
};

// Parses a whole document. On failure returns nullopt and, if `error` is given, says where it went wrong.
// Nesting is limited to 64 levels.
std::optional<Json> parseJson(std::string_view text, std::string *error = nullptr);

} // namespace murasaki::scene
