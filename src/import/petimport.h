#pragma once

// Limits and names of the pet import (docs/pets/import.md). The limits reuse petlimits.h; the names are what a refused or degraded import says, in one
// table with the document that lists them (a test keeps both equal), as petcheck.h and petbehaviours.h do.

#include "petlimits.h"

#include <array>
#include <string_view>

namespace murasaki::import {

// Hard caps of an import. The frame count and the canvas are the frame cache's (petlimits.h); the rest bounds a source before anything is allocated.
inline constexpr int kMaxFrames = kMaxPetFrames;
inline constexpr int kMaxCanvasSide = kMaxPetCanvasSide;
inline constexpr int kMaxSheetSide = 16384;                          // a sheet image, per side
inline constexpr long long kMaxSheetPixels = 100LL * 1000 * 1000;    // 100 megapixels
inline constexpr long long kMaxSourceBytes = kMaxPetFileBytes;       // one source file
inline constexpr long long kMaxOutputBytes = 1LL << 30;              // the animation an import writes
inline constexpr long long kMaxTotalPixels = 2'000'000'000;           // frames x canvas of one import: the work is bounded, not only each piece
inline constexpr long long kMaxImageAllocationMiB = 256;             // QImageReader's allocation limit for one image

// The names. A refusal is one of these, alone or followed by ": " and a detail (the line, the file); a caller matches the part before the first ": ".
inline constexpr std::string_view kSheetFrameIsZero = "sheet: frame size is zero";
inline constexpr std::string_view kSheetFrameLargerThanSheet = "sheet: frame larger than the sheet";
inline constexpr std::string_view kSheetTooFewCells = "sheet: the sheet holds fewer frames than asked";
inline constexpr std::string_view kSheetTooManyFrames = "sheet: too many frames";
inline constexpr std::string_view kSheetPaddingOutside = "sheet: padding outside the sheet";
inline constexpr std::string_view kSheetTooLarge = "sheet: sheet over the size limit";
inline constexpr std::string_view kSheetBadStart = "sheet: start frame is not a frame of the sheet";
inline constexpr std::string_view kZipNotAZip = "zip: not a zip";
inline constexpr std::string_view kZipEntryRefused = "zip: entry refused";
inline constexpr std::string_view kZipOverTheLimit = "zip: over the size limit";
inline constexpr std::string_view kMdaaLineRefused = "mdaa: actions.mddo line refused";
inline constexpr std::string_view kMdaaTableRefused = "mdaa: actions.mddo refused";
inline constexpr std::string_view kMdaaNotAZip = "mdaa: not a zip";
inline constexpr std::string_view kMdaaNoMedia = "mdaa: no media";
inline constexpr std::string_view kMdaaPathOutside = "mdaa: path outside the package";
inline constexpr std::string_view kMdaaMediaRefused = "mdaa: media refused";
inline constexpr std::string_view kMdaaTriggersPack = "mdaa: triggers pack, timelines not applied";
inline constexpr std::string_view kMdaaPackRefused = "mdaa: pack refused";
inline constexpr std::string_view kMdaaSoundNotFollowed = "mdaa: sound not followed";
inline constexpr std::string_view kMdaaFramesNotRead = "mdaa: frames not read";
inline constexpr std::string_view kNpyNotNpy = "npy: not an npy file";
inline constexpr std::string_view kNpyVersion = "npy: version not supported";
inline constexpr std::string_view kNpyHeaderTooLarge = "npy: header too large";
inline constexpr std::string_view kNpyHeaderMalformed = "npy: header malformed";
inline constexpr std::string_view kNpyDtype = "npy: dtype not uint8";
inline constexpr std::string_view kNpyFortranOrder = "npy: fortran order";
inline constexpr std::string_view kNpyShape = "npy: shape is not frames x height x width x 3";
inline constexpr std::string_view kNpyOverLimit = "npy: over the size limit";
inline constexpr std::string_view kNpyTruncated = "npy: truncated";
inline constexpr std::string_view kNpyTrailingData = "npy: data after the array";
inline constexpr std::string_view kNpzNotAZip = "npz: not a zip";
inline constexpr std::string_view kNpzNoFrames = "npz: no frames array";
inline constexpr std::string_view kScreenRefsNoFrames = "screen refs: no frames for the region";
inline constexpr std::string_view kScreenRefsUnknownRegion = "screen refs: frames for a region the pack does not have";
inline constexpr std::string_view kScreenRefsAmbiguous = "screen refs: two frames files for one region";
inline constexpr std::string_view kScreenRefsShortened = "screen refs: sequence shortened";
inline constexpr std::string_view kScreenRefsOverLimit = "screen refs: over the limit, dropped";
inline constexpr std::string_view kMdaaExecutableNotRun = "mdaa executable not run";
inline constexpr std::string_view kProgramNotRun = "program not run";
inline constexpr std::string_view kAudioNotPlayed = "audio not played";
inline constexpr std::string_view kSpecRefused = "spec refused";
inline constexpr std::string_view kSourceNotFound = "source not found";
inline constexpr std::string_view kSourceNotRegular = "source not a regular file";
inline constexpr std::string_view kSourceOverLimit = "source over the size limit";
inline constexpr std::string_view kNotAnImage = "not an image";
inline constexpr std::string_view kImagesFewer = "images: fewer than 2";
inline constexpr std::string_view kImagesSizesDiffer = "images: sizes differ";
inline constexpr std::string_view kImagesTooMany = "images: too many";
inline constexpr std::string_view kZipNoImages = "zip: no images";
inline constexpr std::string_view kTooManyFrames = "too many frames";
inline constexpr std::string_view kOutputOverBudget = "output over the budget";
inline constexpr std::string_view kNotEnoughDisk = "not enough disk space";
inline constexpr std::string_view kHelperFailed = "helper failed";
inline constexpr std::string_view kTimeBudget = "time budget exceeded";
inline constexpr std::string_view kImagesIgnored = "images: N files ignored";
inline constexpr std::string_view kNotSandboxed = "import: not sandboxed";
inline constexpr std::string_view kCancelled = "cancelled";
inline constexpr std::string_view kSandboxUnavailable = "sandbox unavailable";
inline constexpr std::string_view kNotAVideo = "not a video";
inline constexpr std::string_view kVideoContainerNotAllowed = "video: container not allowed";
inline constexpr std::string_view kVideoNoStream = "video: no video stream";
inline constexpr std::string_view kVideoDecoderMissing = "video: decoder not available";
inline constexpr std::string_view kVideoTooLarge = "video: picture over the size limit";
inline constexpr std::string_view kVideoTimeout = "video: time limit reached";
// Informational, on a result (source.json and ImportState); N stands for the number.
inline constexpr std::string_view kZipEntriesIgnored = "zip: N entries ignored";
inline constexpr std::string_view kScaledDown = "import: images scaled to one size";
inline constexpr std::string_view kVideoOpaque = "import: opaque video (no alpha)";
inline constexpr std::string_view kVideoScaledDown = "import: video scaled down";
inline constexpr std::string_view kVideoFpsLowered = "import: frame rate lowered";

struct Name {
    std::string_view text;
    std::string_view meaning;
};

inline constexpr auto kNames = std::to_array<Name>({
    {kSheetFrameIsZero, "the frame width or height of a sprite sheet is zero or negative"},
    {kSheetFrameLargerThanSheet, "a frame does not fit once, after the padding, in the sheet"},
    {kSheetTooFewCells, "counting from the start frame the sheet holds fewer frames than asked"},
    {kSheetTooManyFrames, "more frames than the frame cache decodes"},
    {kSheetPaddingOutside, "the padding or spacing is negative, or puts the first frame outside the sheet"},
    {kSheetTooLarge, "the sheet image is over 16384 pixels a side or 100 megapixels"},
    {kSheetBadStart, "the start frame is below 1 or past the last cell of the sheet"},
    {kZipNotAZip, "the file is not a ZIP this reader accepts (no end record, a comment trick, ZIP64, several disks, damaged directory)"},
    {kZipEntryRefused, "a member is refused: its name, method, size, ratio or position breaks a rule (the detail says which)"},
    {kZipOverTheLimit, "the members together are over the size or count limit"},
    {kMdaaLineRefused, "a line of actions.mddo is not an action this reader accepts; the line is skipped and the others kept"},
    {kMdaaTableRefused, "actions.mddo as a whole is over its limits (size, lines)"},
    {kMdaaNotAZip, "the .mdaa file is not a ZIP this reader accepts (the detail is the ZIP reader's)"},
    {kMdaaNoMedia, "the package holds no animation Murasaki can use"},
    {kMdaaPathOutside, "an actions.mddo line names a file that is not a member of the package (an absolute path of the author's machine, a .. path, or a file that is not there); the line is dropped"},
    {kMdaaMediaRefused, "a media member did not read back as an animation (header, frame count or size); the member is skipped"},
    {kMdaaTriggersPack, "informational: the package is a triggers pack (pack.json); its animations were imported and its timelines are kept in the record (made on request, disabled)"},
    {kMdaaPackRefused, "a part of pack.json or meta.json is not usable (not an object, over a limit, an unknown action); that part is dropped and the rest kept"},
    {kMdaaSoundNotFollowed, "informational: a sound action of a triggers pack names a file by the author's own path; only the file name is kept and nothing is read or played"},
    {kMdaaFramesNotRead, "informational: the pack carries captured frames (.npz) that could not be used (see the screen refs notes); nothing of them is kept"},
    {kNpyNotNpy, "a captured-frames array does not start with the .npy magic"},
    {kNpyVersion, "the .npy format version is not 1.0, 2.0 or 3.0"},
    {kNpyHeaderTooLarge, "the .npy header is over 64 KiB"},
    {kNpyHeaderMalformed, "the .npy header is not the fixed dictionary of descr, fortran_order and shape (the detail says what is wrong)"},
    {kNpyDtype, "the array is not unsigned 8-bit (|u1 or <u1); object arrays and every other type are refused and never loaded"},
    {kNpyFortranOrder, "the array is stored in Fortran order"},
    {kNpyShape, "the array is not 4-dimensional [frames, height, width, 3], or a dimension is zero"},
    {kNpyOverLimit, "the array has more than 1024 frames, a side over 4096, or more than 256 MiB of data; refused before any data is read"},
    {kNpyTruncated, "the array holds fewer bytes than its shape needs"},
    {kNpyTrailingData, "the array is followed by bytes its shape does not account for"},
    {kNpzNotAZip, "a frames file (.npz) is not a ZIP this reader accepts"},
    {kNpzNoFrames, "a frames file (.npz) holds no array named frames"},
    {kScreenRefsNoFrames, "the pack has a screen region but no frames file for it"},
    {kScreenRefsUnknownRegion, "the pack carries frames for a region number the pack does not list; they are ignored"},
    {kScreenRefsAmbiguous, "the pack has both frames/region<i>.npz and frames/region_<i>.npz for one region; neither is used, because there is no telling which is the right one"},
    {kScreenRefsShortened, "informational: the clip ends before the sequence does, so fewer reference frames were kept than seq_len"},
    {kScreenRefsOverLimit, "informational: more regions or reference frames than the import keeps (16 regions, 64 frames each, 256 in all); the rest are dropped"},
    {kMdaaExecutableNotRun, "informational: the package carries core.exe; it was hashed and dropped, never extracted or started"},
    {kProgramNotRun, "informational: an archive member is a program (.exe, .dll, .so, .sh, ...); it was ignored and never touched"},
    {kAudioNotPlayed, "informational: the package carries sound files; they are not played yet"},
    {kSpecRefused, "the job description is not valid JSON of the expected shape, or a value is out of range (the detail says which)"},
    {kSourceNotFound, "a source path does not exist"},
    {kSourceNotRegular, "a source is a directory (where a file is needed), a device, a pipe or a socket"},
    {kSourceOverLimit, "a source file is over the file size limit"},
    {kNotAnImage, "a source file is not an image this build reads, or its header claims a size over the limits"},
    {kImagesFewer, "an animation needs at least two images"},
    {kImagesSizesDiffer, "the images differ in size and the size policy is first"},
    {kImagesTooMany, "more images than the frame cache decodes"},
    {kZipNoImages, "the archive holds no image the importer can use"},
    {kTooManyFrames, "the result would have more frames than the frame cache decodes"},
    {kOutputOverBudget, "the converted file would pass the output limit"},
    {kNotEnoughDisk, "less is free where the file would go than the output could need (frames x canvas as raw RGBA, at most the output limit)"},
    {kNotAVideo, "a source file is not a video this build reads (the detail is libav's reason), or it is damaged before its first frame"},
    {kVideoContainerNotAllowed, "the file is a container the importer does not take: only MP4/MOV, Matroska/WebM, AVI, GIF and APNG are read (the detail names it)"},
    {kVideoNoStream, "the file holds no video stream (cover art does not count), or no frame falls inside the trim"},
    {kVideoDecoderMissing, "this build has no decoder for the video's codec"},
    {kVideoTooLarge, "the video's picture is over the size limit (16384 pixels a side or 35 megapixels, which is 8K), claimed or decoded"},
    {kVideoTimeout, "opening, probing and decoding the video took longer than the time limit setting (default 10 minutes)"},
    {kHelperFailed, "the helper process ended without a result"},
    {kTimeBudget, "the import ran longer than its time budget (checked between frames) and was stopped cleanly; nothing was kept"},
    {kNotSandboxed, "informational: this import ran without the sandbox (importRequireSandbox is off and Landlock or murasaki-sandbox was not there)"},
    {kCancelled, "the user cancelled the import; nothing was kept"},
    {kSandboxUnavailable, "the sandbox is required and not available on this system"},
    {kImagesIgnored, "informational: the folder held N files that are not images (a Thumbs.db, a README); they were skipped"},
    {kZipEntriesIgnored, "informational: the archive held N entries that are not images; they were skipped and counted"},
    {kScaledDown, "informational: the images were scaled to the first image's size"},
    {kVideoOpaque, "informational: the video has no alpha channel, so every pixel is opaque (the colour key can still cut a background)"},
    {kVideoScaledDown, "informational: the video was larger than the longest-side setting and was scaled down"},
    {kVideoFpsLowered, "informational: frames were dropped to keep to the frame rate setting"},
});

} // namespace murasaki::import
