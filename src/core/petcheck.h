#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <array>
#include <string_view>

namespace murasaki {

// `murasaki-overlay --check ITEM` and `murasakictl pets check ITEM`: feature detection for pets, the way
// `murasaki-scene-vk --check` is for scenes. It only reads headers (the same readers the importers use, never a
// whole file) and lists the item's directory; nothing is decoded, drawn or run.
//
// Output, one line on stdout, exit status 0 whenever a verdict was printed:
//   {"ok":true,"reasons":[],"degraded":[],"engine":"murasaki-overlay","version":"0.1.0"}
//   {"ok":false,"reasons":["not a readable animation: a.apng"],"degraded":[],...}
// complete: ok and nothing degraded. degraded: ok, with named gaps. refused: not ok, with reasons.
// The degraded entries are sorted. An entry is a name from the table below, alone or followed by ": " and the file or value it is about. A caller
// matches the part before the first ": ". docs/pets/check.md describes every name.
namespace petcheck {

enum class Verdict { Refused, Degraded };

struct Name {
    Verdict verdict;
    std::string_view text;
    std::string_view meaning;
};

inline constexpr std::string_view kItemNotFound = "item not found";
inline constexpr std::string_view kNoAnimation = "no animation file";
inline constexpr std::string_view kUnsupportedFileType = "unsupported file type";
inline constexpr std::string_view kUnsupportedContainer = "unsupported container";
inline constexpr std::string_view kNotReadable = "not a readable animation";
inline constexpr std::string_view kOutsideItem = "not a regular file inside the item folder";
inline constexpr std::string_view kTooLarge = "file over the size limit";
inline constexpr std::string_view kCanvasOverVideoLimit = "canvas over the video path limit";

inline constexpr std::string_view kOverCacheBudget = "exceeds the frame cache budget";
inline constexpr std::string_view kTooManyFiles = "too many files to examine";
inline constexpr std::string_view kVariantSkipped = "variant skipped";
inline constexpr std::string_view kTooManyFrames = "too many frames for the frame cache";
inline constexpr std::string_view kCanvasTooLarge = "canvas too large for the frame cache";
inline constexpr std::string_view kMdaaNotImported = "mdaa package not imported";
inline constexpr std::string_view kVideoNotImported = "video source not imported";
inline constexpr std::string_view kArchiveNotImported = "frame archive not imported";
inline constexpr std::string_view kAudioNotPlayed = "audio not played";
inline constexpr std::string_view kProgramNotRun = "program not run";

// The one table: every name this check can print, with what it means. docs/pets/check.md lists the same rows
// (a test keeps them equal).
inline constexpr std::array<Name, 18> kNames = {{
    {Verdict::Refused, kItemNotFound, "the item directory, file or anima:<id> does not exist"},
    {Verdict::Refused, kNoAnimation, "the item folder holds no apng, gif or webp file"},
    {Verdict::Refused, kUnsupportedFileType, "the file's extension is not one Murasaki reads"},
    {Verdict::Refused, kUnsupportedContainer,
     "the item holds only a container Murasaki does not import yet (mdaa, zip, a video, a live2d model)"},
    {Verdict::Refused, kNotReadable,
     "no animation of the item has a header that reads as an apng, gif or webp (broken, truncated or implausible)"},
    {Verdict::Refused, kOutsideItem, "an animation file is a symbolic link out of the item folder, or not a regular file"},
    {Verdict::Refused, kTooLarge, "every animation is over the 2 GiB a decoder opens at most"},
    {Verdict::Refused, kCanvasOverVideoLimit, "every animation has a canvas side over 16384 pixels, more than the video path plays"},
    {Verdict::Degraded, kOverCacheBudget,
     "the frames, at the size the overlay decodes them at most (650 px longest side), do not fit the default 128 MiB frame cache; the pet plays through the video path"},
    {Verdict::Degraded, kTooManyFiles, "the item has more animation files than the check examines (64); the rest were not looked at"},
    {Verdict::Degraded, kVariantSkipped, "some animation files of the item are unreadable, too large or over the video limit; the others play"},
    {Verdict::Degraded, kTooManyFrames, "more frames than the frame cache decodes; the pet plays through the video path"},
    {Verdict::Degraded, kCanvasTooLarge, "a canvas side over the frame cache's limit; the pet plays through the video path"},
    {Verdict::Degraded, kMdaaNotImported, "the item carries an .mdaa package; only its plain animations play until the user imports it (Import this package)"},
    {Verdict::Degraded, kVideoNotImported, "the item carries a video (mp4, webm, mkv, mov, avi); it is not turned into a pet until the user imports it (Import this file)"},
    {Verdict::Degraded, kArchiveNotImported, "the item carries a zip of frames or a sprite sheet source; it is not imported until the user imports it (Import this file)"},
    {Verdict::Degraded, kAudioNotPlayed, "the item carries a sound; pets are silent"},
    {Verdict::Degraded, kProgramNotRun, "the item ships a program (never run); the animations play"},
}};

} // namespace petcheck

struct PetCheckResult {
    bool ok = false;
    QStringList reasons;  // why it was refused
    QStringList degraded; // what an accepted item plays without
};

// Checks what `item` names: an item directory (a Workshop item of Anima Engine), an animation file (a local pet) or
// "anima:<id>", looked up in `animaDirs` (the Workshop content dirs). A relative path is taken from the current dir.
PetCheckResult checkPetItem(const QString &item, const QStringList &animaDirs);

// The Workshop content dirs of Anima Engine that exist on this machine: Steam's libraries and Murasaki's SteamCMD dir.
QStringList defaultAnimaDirs();

// The verdict as the one line above.
QByteArray petCheckJson(const PetCheckResult &result);

// Runs the check: prints the line to stdout and returns the exit status (0 once a verdict was printed).
int runPetCheck(const QString &item);

} // namespace murasaki
