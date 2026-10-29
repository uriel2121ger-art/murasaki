#include "media/mediastate.h"

#include "util/json.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

namespace {

// Text cut at a UTF-8 character boundary, control characters (and DEL) dropped, tabs and newlines made spaces.
std::string cleanText(const std::string &in)
{
    std::string out;
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\t' || c == '\n' || c == '\r') {
            out += ' ';
        } else if (c >= 0x20 && c != 0x7F) {
            out += ch;
        }
    }
    if (out.size() > kMaxMediaText) {
        size_t cut = kMaxMediaText;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut; // not in the middle of a character
        }
        out.resize(cut);
    }
    return out;
}

double clampedNumber(const Json *v, double fallback, double lo, double hi)
{
    if (!v || !v->isNumber() || !std::isfinite(v->asNumber())) {
        return fallback;
    }
    return std::clamp(v->asNumber(), lo, hi);
}

float luma(const std::array<float, 3> &c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

float distance(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

} // namespace

std::optional<MediaState> parseMediaState(std::string_view json)
{
    const auto parsed = parseJson(json);
    if (!parsed || !parsed->isObject()) {
        return std::nullopt;
    }
    MediaState s;
    if (const Json *v = parsed->find("enabled"); v && v->isBool()) {
        s.enabled = v->asBool();
    }
    if (const Json *v = parsed->find("state"); v && v->isString()) {
        s.playback = v->asString() == "playing" ? MediaPlayback::Playing : v->asString() == "paused" ? MediaPlayback::Paused : MediaPlayback::Stopped;
    }
    const auto text = [&](const char *key, std::string &out) {
        if (const Json *v = parsed->find(key); v && v->isString()) {
            out = cleanText(v->asString());
        }
    };
    text("title", s.title);
    text("artist", s.artist);
    text("albumTitle", s.albumTitle);
    text("albumArtist", s.albumArtist);
    if (const Json *v = parsed->find("genres"); v && v->isArray()) {
        for (const Json &g : v->items()) {
            if (s.genres.size() >= kMaxMediaGenres) {
                break;
            }
            if (g.isString()) {
                s.genres.push_back(cleanText(g.asString()));
            }
        }
    }
    s.position = clampedNumber(parsed->find("position"), 0.0, 0.0, 86400.0);
    s.rate = clampedNumber(parsed->find("rate"), 1.0, 0.0, 16.0);
    s.duration = clampedNumber(parsed->find("duration"), 0.0, 0.0, 86400.0);
    if (!s.enabled) {
        s = MediaState{}; // no player: nothing else means anything
    }
    return s;
}

double extrapolatedPosition(const MediaState &state, double elapsed)
{
    double position = state.position;
    if (state.playback == MediaPlayback::Playing && elapsed > 0.0) {
        position += elapsed * state.rate;
    }
    if (state.duration > 0.0) {
        position = std::min(position, state.duration);
    }
    return std::max(0.0, position);
}

MediaColors mediaColors(uint32_t width, uint32_t height, const uint8_t *rgba)
{
    MediaColors out;
    if (!rgba || width == 0 || height == 0) {
        return out;
    }
    struct Cell {
        uint64_t count = 0;
        uint64_t sum[3] = {0, 0, 0};
    };
    std::array<Cell, 64> cells{};
    const uint64_t pixels = uint64_t{width} * height;
    for (uint64_t i = 0; i < pixels; ++i) {
        const uint8_t *p = rgba + i * 4;
        if (p[3] < 128) {
            continue;
        }
        Cell &c = cells[static_cast<size_t>((p[0] >> 6) * 16 + (p[1] >> 6) * 4 + (p[2] >> 6))];
        ++c.count;
        for (int k = 0; k < 3; ++k) {
            c.sum[k] += p[k];
        }
    }
    // fullest first; the cell index breaks ties, so the result does not depend on the sort
    std::array<size_t, 64> order;
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cells[a].count != cells[b].count ? cells[a].count > cells[b].count : a < b; });
    const auto mean = [&](size_t i) {
        const Cell &c = cells[i];
        return std::array<float, 3>{static_cast<float>(c.sum[0]) / (255.0f * static_cast<float>(c.count)),
                                    static_cast<float>(c.sum[1]) / (255.0f * static_cast<float>(c.count)),
                                    static_cast<float>(c.sum[2]) / (255.0f * static_cast<float>(c.count))};
    };
    if (cells[order[0]].count == 0) {
        return out; // nothing opaque
    }
    out.primary = mean(order[0]);
    std::vector<std::array<float, 3>> taken{out.primary};
    std::array<std::array<float, 3>, 2> next{out.primary, out.primary};
    size_t found = 0;
    for (size_t k = 1; k < order.size() && found < 2 && cells[order[k]].count > 0; ++k) {
        const auto candidate = mean(order[k]);
        if (std::all_of(taken.begin(), taken.end(), [&](const auto &t) { return distance(candidate, t) >= 0.25f; })) {
            next[found++] = candidate;
            taken.push_back(candidate);
        }
    }
    out.secondary = next[0];
    out.tertiary = found > 1 ? next[1] : out.primary;
    const float l = luma(out.primary);
    out.text = l > 0.5f ? std::array<float, 3>{0.0f, 0.0f, 0.0f} : std::array<float, 3>{1.0f, 1.0f, 1.0f};
    out.highContrast = out.text;
    float best = std::fabs(luma(out.text) - l);
    for (const auto &c : {out.secondary, out.tertiary}) {
        if (std::fabs(luma(c) - l) > best) {
            best = std::fabs(luma(c) - l);
            out.highContrast = c;
        }
    }
    return out;
}

} // namespace murasaki::scene
