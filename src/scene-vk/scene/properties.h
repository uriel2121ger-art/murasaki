#pragma once

// User properties of the running wallpaper: what the Murasaki GUI sends in Properties messages and what
// `--set-property KEY=VALUE` sets at launch. Scene objects bind to a property by name (`"visible": {"user": "name",
// "value": true}`); the object's own value is the fallback when the property was never set.
//
// Values arrive as JSON (bool, number, string, or an array of numbers for colours and vectors) or, from the command
// line, as text in the engine's --set-property syntax: "true"/"false", a number, space separated numbers, or a plain
// string.

#include "util/math.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace murasaki::scene {

using PropertyValue = std::variant<bool, double, std::string, std::vector<double>>;

class PropertyStore
{
public:
    void set(std::string key, PropertyValue value);
    // `--set-property` text: "true"/"false" -> bool, one number -> number, several numbers -> vector, else string.
    void setFromText(std::string key, std::string_view text);
    // A JSON object of key -> value. Returns false (and changes nothing) when `json` is not an object. Values of
    // unsupported types (null, nested objects) are skipped.
    bool setFromJson(std::string_view json);

    const PropertyValue *find(const std::string &key) const;
    const std::map<std::string, PropertyValue> &values() const { return m_values; }
    // Bumps on every change that altered a value, so a renderer knows to redraw.
    uint64_t revision() const { return m_revision; }

    // The property as text, the way a condition compares it; empty when it was never set.
    std::optional<std::string> getText(const std::string &key) const;
    std::optional<bool> getBool(const std::string &key) const;
    std::optional<double> getNumber(const std::string &key) const;
    // Colours and vectors. A component above 1 is read as 0..255 and scaled, since the GUI may send either.
    std::optional<Vec3> getColor(const std::string &key) const;
    // The numbers of a vector property: an array, a string "x y z", or one number (a single entry, which a caller reads as the
    // value of every component; a one-element array is the same). More numbers than a caller needs are left for it to ignore.
    // Empty when the property is missing, empty, not numeric (a boolean), or has a part that is not a finite number ("nan 1").
    std::optional<std::vector<double>> getVector(const std::string &key) const;

private:
    std::map<std::string, PropertyValue> m_values;
    uint64_t m_revision = 0;
};

// A value that is either literal or bound to a user property, read from scene.json.
template <class T>
struct Bound {
    T value{};
    std::string user; // property name, empty when the value is literal
    // `{"user": {"name": ..., "condition": "2"}}`: the value is true while the property reads as `condition` (a combo's
    // choice, a checkbox as true/false, a number without decimals)
    std::string condition;
};

bool resolve(const Bound<bool> &b, const PropertyStore &props);
float resolve(const Bound<float> &b, const PropertyStore &props);
Vec3 resolve(const Bound<Vec3> &b, const PropertyStore &props);

} // namespace murasaki::scene
