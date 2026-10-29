#pragma once

// The text protocol between the overlay and the player process (docs/pets/audio.md). One command per line on the player's stdin, one answer per
// line on its stdout. Lines are at most kMaxLineBytes; anything that does not parse closes the player. Assets are ids, never paths.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace murasaki::audio {

inline constexpr std::size_t kMaxLineBytes = 256;

struct Command {
    enum class Kind { Play, Stop, Cancel, Pause, Resume, Gain, Master, Quit } kind = Kind::Quit;
    int pet = 0;
    unsigned generation = 0;
    std::string asset;            // Play
    long long startFrame = 0;     // Play
    int gain = 0;                 // Play, Gain, Master: 0..1000 (thousandths)
    bool loop = false;            // Play
    long long atNs = 0;           // Play: CLOCK_MONOTONIC
};

// Parses one line (without its newline); nullopt for anything that is not exactly a command of the protocol.
std::optional<Command> parseCommand(std::string_view line);
// The line for a command, without the newline.
std::string formatCommand(const Command &command);

} // namespace murasaki::audio
