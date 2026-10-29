#pragma once

// Playing the MP4 stream a video texture carries (TexFlags::Video): libavformat reads it from memory, libavcodec decodes it in
// software on a thread of its own, libswscale turns the frames into RGBA at no more than 1920 x 1080. The decoder keeps a few frames
// ahead of the clock it is asked for and then waits, so a scene that is not being drawn costs nothing; at the end of the stream
// it starts over.
//
// The stream comes from a Workshop item, so it is treated as hostile: it is read only from the bytes handed over (no file, no
// network), only as MP4 and only with a short list of decoders, with the probing and the picture size bounded.
// Decoding is done by the GPU (VAAPI, on the render node of the device the engine draws with) when the machine can and by the CPU
// when it cannot: the same stream plays either way. It never decodes more pictures than the scene shows (`setMaxFps`).
// Without libav in the build `videoSupported()` is false and `open` refuses.

#include "assets/videoapi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

bool videoSupported();

inline constexpr uint32_t kMaxVideoWidth = 1920;
inline constexpr uint32_t kMaxVideoHeight = 1080;

class VideoDecoder
{
public:
    // Opens the stream, decodes its first frame and starts the decoding thread. Null, with `error`, when the build has no libav or
    // the stream cannot be played.
    struct Options {
        std::string renderNode;  // "/dev/dri/renderD128": the device VAAPI decodes on; empty for libva's own choice
        bool hardware = true;    // try VAAPI first (a failure of any step falls back to the CPU)
        bool zeroCopy = false;   // keep the GPU surfaces for exportFrame(); frameAt() still works (it converts on the spot)
    };
    static std::unique_ptr<VideoDecoder> open(std::vector<uint8_t> bytes, std::string &error, const Options &options);
    static std::unique_ptr<VideoDecoder> open(std::vector<uint8_t> bytes, std::string &error);
    // The same for a stream of `size` bytes at `offset` in `path` (a package entry): read where it lies, as the decoder needs it, never held.
    static std::unique_ptr<VideoDecoder> openFile(const std::string &path, uint64_t offset, uint64_t size, std::string &error, const Options &options);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    uint32_t width() const;  // of the pictures it hands out (even, at most kMaxVideoWidth)
    uint32_t height() const;
    // Seconds of one run of the stream, 0 when it does not say.
    double duration() const;
    // Whether the GPU decodes it.
    bool hardware() const;
    // The most pictures a second the scene draws: the decoder converts no more than that (0: all). Safe to call from any thread.
    void setMaxFps(double fps);

    // The picture to show at `seconds` of the scene's clock (the stream loops): width() x height() x 4 bytes of RGBA, valid until
    // the next call. Null when that is the picture the last call gave.
    const uint8_t *frameAt(double seconds);

    // zeroCopy decoders: the picture to show at `seconds` as the dma-buf of its surface (see MurasakiVideoExport). Status 0: nothing new;
    // -1: not possible (and never will be: frameAt() delivers the pictures from then on). The descriptor in a status 1 is the caller's.
    MurasakiVideoExport exportFrame(double seconds);

    // Steering for a script: continue from `seconds` of the stream (the clock handed to frameAt/exportFrame should restart at `seconds`);
    // start over at the end or stop there; where in the stream the picture on show is; whether a stream that does not loop is spent.
    void seek(double seconds);
    void setLoop(bool loop);
    double position() const;
    bool ended() const;

private:
    struct Impl;
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace murasaki::scene
