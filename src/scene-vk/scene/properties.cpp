#include "scene/properties.h"

#include <cmath>
#include <sstream>

#include "util/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace murasaki::scene {

namespace {

bool parseNumber(std::string_view text, double &out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

std::vector<std::string_view> splitSpaces(std::string_view text)
{
    std::vector<std::string_view> parts;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        const size_t start = i;
        while (i < text.size() && text[i] != ' ') {
            ++i;
        }
        if (i > start) {
            parts.push_back(text.substr(start, i - start));
        }
    }
    return parts;
}

} // namespace

void PropertyStore::set(std::string key, PropertyValue value)
{
    auto it = m_values.find(key);
    if (it != m_values.end() && it->second == value) {
        return;
    }
    m_values[std::move(key)] = std::move(value);
    ++m_revision;
}

void PropertyStore::setFromText(std::string key, std::string_view text)
{
    if (text == "true" || text == "false") {
        set(std::move(key), text == "true");
        return;
    }
    const auto parts = splitSpaces(text);
    std::vector<double> numbers;
    for (std::string_view part : parts) {
        double v = 0;
        if (!parseNumber(part, v)) {
            numbers.clear();
            break;
        }
        numbers.push_back(v);
    }
    if (numbers.size() == 1) {
        set(std::move(key), numbers[0]);
    } else if (numbers.size() > 1) {
        set(std::move(key), std::move(numbers));
    } else {
        set(std::move(key), std::string(text));
    }
}

bool PropertyStore::setFromJson(std::string_view json)
{
    const auto parsed = parseJson(json);
    if (!parsed || !parsed->isObject()) {
        return false;
    }
    for (const auto &[key, value] : parsed->members()) {
        if (value.isBool()) {
            set(key, value.asBool());
        } else if (value.isNumber()) {
            set(key, value.asNumber());
        } else if (value.isString()) {
            set(key, value.asString());
        } else if (value.isArray()) {
            std::vector<double> numbers;
            bool ok = true;
            for (const Json &item : value.items()) {
                ok = ok && item.isNumber();
                numbers.push_back(item.asNumber());
            }
            if (ok && !numbers.empty()) {
                set(key, std::move(numbers));
            }
        }
    }
    return true;
}

const PropertyValue *PropertyStore::find(const std::string &key) const
{
    const auto it = m_values.find(key);
    return it == m_values.end() ? nullptr : &it->second;
}

std::optional<bool> PropertyStore::getBool(const std::string &key) const
{
    const PropertyValue *v = find(key);
    if (!v) {
        return std::nullopt;
    }
    if (const bool *b = std::get_if<bool>(v)) {
        return *b;
    }
    if (const double *d = std::get_if<double>(v)) {
        return *d != 0.0;
    }
    if (const std::string *s = std::get_if<std::string>(v)) {
        return *s == "true" || *s == "1";
    }
    return std::nullopt;
}

std::optional<double> PropertyStore::getNumber(const std::string &key) const
{
    const PropertyValue *v = find(key);
    if (!v) {
        return std::nullopt;
    }
    if (const double *d = std::get_if<double>(v)) {
        return *d;
    }
    if (const bool *b = std::get_if<bool>(v)) {
        return *b ? 1.0 : 0.0;
    }
    if (const std::string *s = std::get_if<std::string>(v)) {
        double out = 0;
        if (parseNumber(*s, out)) {
            return out;
        }
    }
    return std::nullopt;
}

std::optional<Vec3> PropertyStore::getColor(const std::string &key) const
{
    const PropertyValue *v = find(key);
    if (!v) {
        return std::nullopt;
    }
    std::vector<double> c;
    if (const auto *vec = std::get_if<std::vector<double>>(v)) {
        c = *vec;
    } else if (const std::string *s = std::get_if<std::string>(v)) {
        for (std::string_view part : splitSpaces(*s)) {
            double d = 0;
            if (!parseNumber(part, d)) {
                return std::nullopt;
            }
            c.push_back(d);
        }
    }
    if (c.size() < 3) {
        return std::nullopt;
    }
    const double scale = (c[0] > 1.0 || c[1] > 1.0 || c[2] > 1.0) ? 1.0 / 255.0 : 1.0;
    return Vec3{static_cast<float>(c[0] * scale), static_cast<float>(c[1] * scale), static_cast<float>(c[2] * scale)};
}

std::optional<std::vector<double>> PropertyStore::getVector(const std::string &key) const
{
    const PropertyValue *v = find(key);
    if (!v) {
        return std::nullopt;
    }
    std::vector<double> out;
    if (const auto *vec = std::get_if<std::vector<double>>(v)) {
        out = *vec;
    } else if (const double *d = std::get_if<double>(v)) {
        out.push_back(*d);
    } else if (const std::string *s = std::get_if<std::string>(v)) {
        for (std::string_view part : splitSpaces(*s)) {
            double d = 0;
            if (!parseNumber(part, d)) {
                return std::nullopt;
            }
            out.push_back(d);
        }
    }
    for (double d : out) {
        if (!std::isfinite(d)) {
            return std::nullopt;
        }
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> PropertyStore::getText(const std::string &key) const
{
    const PropertyValue *v = find(key);
    if (!v) {
        return std::nullopt;
    }
    if (const bool *b = std::get_if<bool>(v)) {
        return std::string(*b ? "true" : "false");
    }
    if (const double *d = std::get_if<double>(v)) {
        if (std::floor(*d) == *d && std::fabs(*d) < 1e15) {
            return std::to_string(static_cast<long long>(*d));
        }
        std::ostringstream out;
        out << *d;
        return out.str();
    }
    if (const std::string *s = std::get_if<std::string>(v)) {
        return *s;
    }
    std::ostringstream out;
    const auto &vec = std::get<std::vector<double>>(*v);
    for (size_t i = 0; i < vec.size(); ++i) {
        out << (i ? " " : "") << vec[i];
    }
    return out.str();
}

bool resolve(const Bound<bool> &b, const PropertyStore &props)
{
    if (!b.condition.empty()) {
        if (const auto text = props.getText(b.user)) {
            return *text == b.condition;
        }
        return b.value;
    }
    if (!b.user.empty()) {
        if (const auto v = props.getBool(b.user)) {
            return *v;
        }
    }
    return b.value;
}

float resolve(const Bound<float> &b, const PropertyStore &props)
{
    if (!b.user.empty()) {
        if (const auto v = props.getNumber(b.user)) {
            return static_cast<float>(*v);
        }
    }
    return b.value;
}

Vec3 resolve(const Bound<Vec3> &b, const PropertyStore &props)
{
    if (!b.user.empty()) {
        if (const auto v = props.getColor(b.user)) {
            return *v;
        }
    }
    return b.value;
}

} // namespace murasaki::scene
