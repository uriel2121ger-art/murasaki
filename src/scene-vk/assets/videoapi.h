#pragma once

// The interface between the engine and the video plugin (videoplugin.cpp): plain C functions and an opaque handle, so the plugin can be
// built and loaded on its own. `murasaki_video_api()` is the plugin's one exported symbol.
//
// Two ways to get the pictures out: `frameAt` hands RGBA the plugin made on the CPU; `exportFrame` hands the decoder's own GPU surface
// as a DRM PRIME descriptor (a dma-buf), which the engine imports into Vulkan and converts on the GPU, so no picture is read back.
// Version 2 added `exportFrame` and the open flags; version 3 `openFile`, a stream read from a file region instead of held in memory;
// version 4 `seek`, `setLoop`, `position` and `ended` (a script steers the video: SceneScript's IVideoTexture).

#include <cstddef>
#include <cstdint>

inline constexpr uint32_t kMurasakiVideoApiVersion = 4;

// `open` flags
inline constexpr uint32_t kMurasakiVideoHardware = 1; // try VAAPI first
inline constexpr uint32_t kMurasakiVideoZeroCopy = 2; // keep the decoder's surfaces for `exportFrame` instead of converting them to RGBA

// A decoded picture as the dma-buf of its surface. The descriptors belong to the caller (it closes them, whatever it does with the rest).
struct MurasakiVideoExport {
    int32_t status;            // 1 a new picture, 0 nothing new since the last call, -1 the export is not possible (use `frameAt`)
    uint32_t fourcc;           // DRM format of the surface: 'NV12', 'P010'
    uint64_t modifier;         // DRM format modifier of the buffer
    uint32_t surfaceWidth;     // of the buffer (coded size, often padded up)
    uint32_t surfaceHeight;
    uint32_t pictureWidth;     // of the picture inside it
    uint32_t pictureHeight;
    uint32_t planes;           // 1 to 4; every plane is in the one object `fd`
    int32_t fd;                // the buffer
    uint64_t size;             // of the buffer
    uint32_t offsets[4];       // of each plane, in bytes from the start of the buffer
    uint32_t pitches[4];
    uint32_t colorspace;       // 0 BT.601, 1 BT.709, 2 BT.2020
    uint32_t fullRange;        // 1: the full 0 to 255 range, 0: the video range
};

struct MurasakiVideoApi {
    uint32_t version;
    // `bytes` stay valid until `close`. Null, with the reason in `error`, when the stream cannot be played. `flags`: kMurasakiVideo*.
    void *(*open)(const uint8_t *bytes, size_t size, const char *renderNode, uint32_t flags, char *error, size_t errorSize);
    void (*close)(void *decoder);
    uint32_t (*width)(void *decoder);
    uint32_t (*height)(void *decoder);
    double (*duration)(void *decoder);
    int (*hardware)(void *decoder);
    void (*setMaxFps)(void *decoder, double fps);
    const uint8_t *(*frameAt)(void *decoder, double seconds);
    // The picture to show at `seconds`, as a dma-buf; only for a decoder opened with kMurasakiVideoZeroCopy. When the status is -1 the
    // picture is still delivered by the next `frameAt`, converted on the spot.
    void (*exportFrame)(void *decoder, double seconds, MurasakiVideoExport *out);
    // Like `open`, for a stream of `size` bytes at `offset` in the file `path` (a package entry): the plugin reads it on demand with its own
    // descriptor, so a stream of hundreds of megabytes costs no memory but the pages the decoder is reading.
    void *(*openFile)(const char *path, uint64_t offset, uint64_t size, const char *renderNode, uint32_t flags, char *error, size_t errorSize);
    // Continues from `seconds` of the stream: the pictures that follow carry stream time (the clock given to `frameAt` should then restart at
    // `seconds`), the first one shown is the one at or just before it. Asynchronous: the pictures of before arrive no more.
    void (*seek)(void *decoder, double seconds);
    // Whether the stream starts over at its end (the default) or stops there, waiting for a `seek` or `setLoop(true)`.
    void (*setLoop)(void *decoder, int loop);
    // Where in the stream the picture shown is (the seek target until a picture of there is shown). Stream time, never beyond one run.
    double (*position)(void *decoder);
    // 1 when a stream that does not loop has delivered its last picture and the picture on show is that one (or it is the last waiting).
    int (*ended)(void *decoder);
};

extern "C" const MurasakiVideoApi *murasaki_video_api();
