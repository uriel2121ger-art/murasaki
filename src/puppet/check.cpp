#include "puppet/check.h"

#include "puppet/openformat.h"
#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace murasaki::puppet {

namespace {

constexpr size_t kMaxDetailBytes = 120;

void appendEscaped(std::string &out, std::string_view text)
{
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void appendCount(std::string &out, bool known, long long value)
{
    out += known ? std::to_string(value) : std::string("null");
}

void appendProblems(std::string &out, const std::vector<Problem> &problems)
{
    out += '[';
    bool first = true;
    for (const Problem &problem : problems) {
        if (!first)
            out += ',';
        first = false;
        appendEscaped(out, Problem{problem.name, tidyDetail(problem.detail)}.text());
    }
    out += ']';
}

} // namespace

ManifestResult checkItem(const std::string &folder)
{
    ManifestResult result = loadManifest(folder);
    if (!result.ok || !result.format || *result.format != Format::Open)
        return result;
    const OpenLoadResult loaded = OpenModel::loadFile(result.manifest.folder, result.manifest.file);
    if (!loaded.model) {
        result.ok = false;
        std::string_view name = kManifestInvalid;
        if (loaded.error.find("kMax") != std::string::npos)
            name = kTooLarge;
        else if (loaded.error.find("BC7") != std::string::npos || loaded.error.find("INP version") != std::string::npos)
            name = kUnsupportedFormat;
        result.reasons.push_back({name, loaded.error});
        return result;
    }
    result.degraded.erase(std::remove_if(result.degraded.begin(), result.degraded.end(), [](const Problem &p) { return p.name == kPayloadNotRead; }), result.degraded.end());
    Manifest &m = result.manifest;
    m.parameterCount = static_cast<int>(loaded.model->parameters().size());
    m.drawableCount = loaded.model->info().partCount;
    m.textureCount = static_cast<int>(loaded.model->info().textures.size());
    m.physicsCount = loaded.model->info().physicsNodes;
    const Bounds bounds = loaded.model->bounds();
    const double w = bounds.maxX - bounds.minX, h = bounds.maxY - bounds.minY;
    if (std::isfinite(w) && std::isfinite(h) && w > 0 && h > 0 && w < 1e7 && h < 1e7) {
        m.canvasWidth = std::max(1, static_cast<int>(std::lround(w)));
        m.canvasHeight = std::max(1, static_cast<int>(std::lround(h)));
    }
    return result;
}

std::string tidyDetail(std::string_view detail)
{
    // Whole characters only, at most kMaxDetailBytes bytes. Invalid UTF-8, C0 and C1 controls and the characters that hide or reorder text (zero-width, bidi marks and overrides, line and
    // paragraph separators, the byte order mark) become U+FFFD: a name from a hostile folder reaches the GUI through here.
    std::string out;
    size_t i = 0;
    while (i < detail.size()) {
        const unsigned char c = static_cast<unsigned char>(detail[i]);
        size_t length = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        std::uint32_t code = 0xFFFD;
        bool valid = length != 0 && i + length <= detail.size() && json::validUtf8(detail.substr(i, length));
        if (valid) {
            code = length == 1 ? c : length == 2 ? (c & 0x1Fu) : length == 3 ? (c & 0x0Fu) : (c & 0x07u);
            for (size_t k = 1; k < length; ++k)
                code = (code << 6) | (static_cast<unsigned char>(detail[i + k]) & 0x3Fu);
        } else {
            length = 1;
        }
        const bool hidden = code < 0x20 || (code >= 0x7F && code <= 0x9F) || (code >= 0x200B && code <= 0x200F) || (code >= 0x2028 && code <= 0x202E) || (code >= 0x2060 && code <= 0x206F)
            || code == 0xFEFF;
        const std::string_view piece = (valid && !hidden) ? detail.substr(i, length) : std::string_view("\xEF\xBF\xBD");
        if (out.size() + piece.size() > kMaxDetailBytes) {
            out += "...";
            return out;
        }
        out.append(piece);
        i += length;
    }
    return out;
}

std::string checkJson(const ManifestResult &result, std::string_view version)
{
    const Manifest &m = result.manifest;
    std::string out = "{\"ok\":";
    out += result.ok ? "true" : "false";
    out += ",\"format\":";
    if (result.format)
        out += *result.format == Format::Cubism ? "\"cubism\"" : "\"open\"";
    else
        out += "null";
    out += ",\"modelVersion\":";
    appendCount(out, result.format.has_value() && m.version != 0, m.version);
    out += ",\"parameters\":";
    appendCount(out, result.ok && m.parameterCount >= 0, m.parameterCount);
    out += ",\"drawables\":";
    appendCount(out, result.ok && m.drawableCount >= 0, m.drawableCount);
    out += ",\"textures\":";
    const bool cubism = result.ok && result.format && *result.format == Format::Cubism;
    if (cubism)
        appendCount(out, true, static_cast<long long>(m.textures.size()));
    else
        appendCount(out, result.ok && m.textureCount >= 0, m.textureCount);
    out += ",\"motions\":";
    appendCount(out, cubism, m.motionCount());
    out += ",\"expressions\":";
    appendCount(out, cubism, static_cast<long long>(m.expressions.size()));
    out += ",\"physics\":";
    appendCount(out, result.ok && m.physicsCount >= 0, m.physicsCount);
    out += ",\"canvas\":";
    if (result.ok && m.canvasWidth > 0 && m.canvasHeight > 0)
        out += "[" + std::to_string(m.canvasWidth) + "," + std::to_string(m.canvasHeight) + "]";
    else
        out += "null";
    out += ",\"reasons\":";
    appendProblems(out, result.reasons);
    out += ",\"degraded\":";
    appendProblems(out, result.degraded);
    out += ",\"engine\":\"murasaki-puppet\",\"version\":";
    appendEscaped(out, version);
    out += '}';
    return out;
}

} // namespace murasaki::puppet
