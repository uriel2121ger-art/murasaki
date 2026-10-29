#pragma once

// What the host tells the engine about the media that plays (bridge/mediaview.h), as the engine keeps it: the player's state,
// the position extrapolated from the last report, and the colours of the cover. Everything the host sent is untrusted data:
// parsing bounds every string and number.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::scene {

enum class MediaPlayback { Stopped = 0, Playing = 1, Paused = 2 }; // MediaPlaybackEvent.PLAYBACK_*

inline constexpr size_t kMaxMediaText = 256;  // bytes of a title, artist, album...
inline constexpr size_t kMaxMediaGenres = 8;

struct MediaState {
    bool enabled = false; // a player is followed
    MediaPlayback playback = MediaPlayback::Stopped;
    std::string title;
    std::string artist;
    std::string albumTitle;
    std::string albumArtist;
    std::vector<std::string> genres;
    double position = 0.0; // seconds, at the moment the report arrived
    double rate = 1.0;
    double duration = 0.0; // seconds; 0 when unknown

    // What the properties event shows: everything but the status, playback and timeline.
    bool sameProperties(const MediaState &o) const
    {
        return title == o.title && artist == o.artist && albumTitle == o.albumTitle && albumArtist == o.albumArtist && genres == o.genres;
    }
};

// The state out of the host's JSON; nullopt when it is not a JSON object. Unknown keys are ignored, bad types leave the default,
// text is cut to kMaxMediaText bytes at a character boundary and loses its control characters, genres are cut to kMaxMediaGenres,
// numbers are finite and clamped (position and duration 0..86400 s, rate 0..16).
std::optional<MediaState> parseMediaState(std::string_view json);

// The position `elapsed` seconds after the report: moving at `rate` only while playing, never past a known duration.
double extrapolatedPosition(const MediaState &state, double elapsed);

// The five colours MediaThumbnailEvent carries, each as r, g, b in 0..1 (an assumption, docs/scene-vk/scripts.md): the picture's
// opaque pixels (alpha >= 128) are binned in 4 x 4 x 4 colour cells; the primary colour is the mean of the fullest cell, the
// secondary and tertiary the means of the next fullest cells at least 0.25 away (in RGB) from the colours already taken, falling
// back to the primary; the text colour is black over a light primary (luma above 0.5) and white over a dark one; the high contrast
// colour is the one among the secondary, tertiary and text colours whose luma is the farthest from the primary's. A picture with
// no opaque pixel gives mid grey for all.
struct MediaColors {
    std::array<float, 3> primary{0.5f, 0.5f, 0.5f};
    std::array<float, 3> secondary{0.5f, 0.5f, 0.5f};
    std::array<float, 3> tertiary{0.5f, 0.5f, 0.5f};
    std::array<float, 3> text{1.0f, 1.0f, 1.0f};
    std::array<float, 3> highContrast{1.0f, 1.0f, 1.0f};
};
MediaColors mediaColors(uint32_t width, uint32_t height, const uint8_t *rgba);

} // namespace murasaki::scene
