#pragma once

// The limits of the pets' sound (docs/pets/audio.md), without Qt: the player and the mixer include this and nothing else of the core.

#include <array>
#include <string_view>

namespace murasaki {

inline constexpr int kDefaultPetAudioVolume = 75; // Anima's own default
inline constexpr int kMaxPetAudioOffsetMs = 5000;
inline constexpr int kPetAudioAssetIdLength = 64;  // lowercase hex of a SHA-256
inline constexpr int kMaxPetAudioVoices = 16;      // sounds mixed at once
// The longest sound an import accepts, and the biggest normalised asset (10 minutes of stereo 16-bit 48 kHz is 115 MB).
inline constexpr long long kMaxPetAudioMs = 10LL * 60 * 1000;
inline constexpr long long kMinPetAudioMs = 20;
inline constexpr long long kMaxPetAudioAssetBytes = 128LL << 20;
inline constexpr long long kMaxPetAudioSourceBytes = 256LL << 20;
inline constexpr int kPetAudioRate = 48000;        // the normalised asset: PCM 16-bit, stereo, this rate
inline constexpr int kMaxPetAudioChannels = 8;     // of a source; downmixed

// The names (docs/pets/audio.md). A refusal or a state is one of these, alone or followed by ": " and a detail.
inline constexpr std::string_view kAudioNotAudio = "audio: not audio";
inline constexpr std::string_view kAudioContainerNotAllowed = "audio: container not allowed";
inline constexpr std::string_view kAudioNoStream = "audio: no audio stream";
inline constexpr std::string_view kAudioDecoderMissing = "audio: decoder not available";
inline constexpr std::string_view kAudioTooLong = "audio: too long";
inline constexpr std::string_view kAudioTooShort = "audio: too short";
inline constexpr std::string_view kAudioOverTheLimit = "audio: over the size limit";
inline constexpr std::string_view kAudioTimeout = "audio: time limit reached";
inline constexpr std::string_view kAudioUnavailable = "audio unavailable";
inline constexpr std::string_view kAudioFailed = "audio failed";
inline constexpr std::string_view kAudioAssetMissing = "audio asset missing";
inline constexpr std::string_view kAudioAssetInvalid = "audio asset invalid";
inline constexpr std::string_view kAudioNoLoop = "audio: animation does not loop";
inline constexpr std::string_view kAudioClickNeedsClickable = "audio: click needs a clickable pet";
inline constexpr std::string_view kAudioDownmixed = "audio: downmixed";
inline constexpr std::string_view kAudioResampled = "audio: resampled";

struct PetAudioName {
    std::string_view text;
    std::string_view meaning;
};

inline constexpr auto kPetAudioNames = std::to_array<PetAudioName>({
    {kAudioNotAudio, "a source file is not audio this build reads (the detail is libav's reason), or it is damaged before its first sample"},
    {kAudioContainerNotAllowed, "the file is a container the audio import does not take (the detail names it)"},
    {kAudioNoStream, "the file holds no audio stream (a video without a sound track, cover art only)"},
    {kAudioDecoderMissing, "this build has no decoder for the audio's codec, or the codec is not on the allow-list"},
    {kAudioTooLong, "the sound is longer than 10 minutes"},
    {kAudioTooShort, "the sound is shorter than 20 milliseconds"},
    {kAudioOverTheLimit, "the source file is over 256 MiB, or the normalised sound would pass 128 MiB"},
    {kAudioTimeout, "decoding the audio took longer than the time limit (5 minutes)"},
    {kAudioUnavailable, "the pet has a sound but the system has no PipeWire to play it"},
    {kAudioFailed, "the sound player stopped and was not started again (it failed three times in a minute)"},
    {kAudioAssetMissing, "the placement names an asset that is not in the store"},
    {kAudioAssetInvalid, "the asset in the store is not a sound of the normalised format"},
    {kAudioNoLoop, "the mode is animation_loop but the animation is a still image or a single frame, so it never wraps"},
    {kAudioClickNeedsClickable, "the mode is click but the pet is click-through, so it never gets a click"},
    {kAudioDownmixed, "informational: the sound had more than two channels and was mixed down to stereo"},
    {kAudioResampled, "informational: the sound was converted to 48 kHz"},
});

} // namespace murasaki
