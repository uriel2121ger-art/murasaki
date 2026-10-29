#pragma once

// Streaming decoder for the sound objects of a scene: mp3 (dr_mp3), FLAC (dr_flac), WAV (dr_wav) and Ogg Vorbis (stb_vorbis), all
// vendored, picked by what the file starts with, not by its name. The whole file stays in memory compressed; samples are produced
// on demand as interleaved stereo float at the output rate, so a track of minutes does not become a hundred MB of PCM. A mono file
// is duplicated to both channels, another sample rate is converted with linear interpolation (the music of wallpapers; not a studio
// resampler).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

// The largest sound file that is opened (it stays in memory compressed while it plays).
constexpr size_t kMaxSoundFileBytes = 160u << 20;

class SoundStream
{
public:
    // Null with `error` set when the bytes are not a sound the decoders understand.
    static std::unique_ptr<SoundStream> open(std::vector<uint8_t> bytes, uint32_t outputRate, std::string *error = nullptr);
    ~SoundStream();
    SoundStream(const SoundStream &) = delete;
    SoundStream &operator=(const SoundStream &) = delete;

    // Writes up to `frames` stereo frames (2 floats each) and returns how many; fewer than asked only at the end.
    size_t read(float *stereo, size_t frames);
    // Back to the start (looping).
    void rewind();
    uint32_t sourceRate() const;
    // "mp3", "flac", "wav" or "vorbis".
    const char *format() const;

private:
    struct Impl;
    explicit SoundStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace murasaki::scene
