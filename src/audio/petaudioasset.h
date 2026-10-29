#pragma once

// The normalised sound a pet plays (docs/pets/audio.md): a WAV file of PCM 16-bit, stereo, 48 kHz, named by the SHA-256 of the source it came from.
// This is the only format the player reads, so it is parsed strictly and here: a canonical 44-byte header whose sizes agree with the file's.

#include "petmixer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::audio {

inline constexpr std::size_t kWavHeaderBytes = 44;

// The path of an asset in `directory`; empty when `id` is not 64 lowercase hex characters (so an id can never name another place).
std::string assetPath(const std::string &directory, const std::string &id);
bool isAssetId(std::string_view id);

// A mapped, verified asset. Reading is by page cache: the real-time thread only reads mapped memory, and prefetch() asks the kernel for the
// next stretch from another thread.
class MappedClip final : public Clip
{
public:
    ~MappedClip() override;
    const std::int16_t *data() const override { return m_samples; }
    std::size_t frames() const override { return m_frames; }
    // Asks the kernel to read `frames` frames from `from` (non-real-time thread).
    void prefetch(std::size_t from, std::size_t frames) const;

    // Null with `error` set to a name of petaudiolimits.h (alone) when the file is missing or is not a normalised asset.
    static std::shared_ptr<MappedClip> open(const std::string &path, std::string *error);

private:
    MappedClip() = default;
    void *m_map = nullptr;
    std::size_t m_mapBytes = 0;
    const std::int16_t *m_samples = nullptr;
    std::size_t m_frames = 0;
};

// Checks the bytes of a header against a file of `fileBytes`; returns the number of frames, or -1.
long long verifyWavHeader(const std::uint8_t *header, std::size_t fileBytes);
// The header of an asset of `frames` frames.
std::vector<std::uint8_t> wavHeader(std::size_t frames);

// Writes an asset as it is decoded: a `.part` file, renamed to `path` by finish(). Nothing is left behind if it is destroyed first.
class WavWriter
{
public:
    explicit WavWriter(std::string path);
    ~WavWriter();
    WavWriter(const WavWriter &) = delete;
    WavWriter &operator=(const WavWriter &) = delete;
    bool ok() const { return m_fd >= 0 && !m_failed; }
    // Interleaved stereo frames. False (and the writer is failed) on a write error or when the asset would pass kMaxPetAudioAssetBytes.
    bool append(const std::int16_t *stereo, std::size_t frames);
    std::size_t frames() const { return m_frames; }
    bool finish();

private:
    std::string m_path;
    int m_fd = -1;
    std::size_t m_frames = 0;
    bool m_failed = false;
    bool m_done = false;
};

// Removes the assets of `directory` that `keep` does not list and that are older than `minAgeSeconds` (and leftover `.part` files older than that),
// with their `.json`. Returns the number of assets removed. Only files named <64 hex>.wav / .json / .wav.part are touched.
int collectAssets(const std::string &directory, const std::vector<std::string> &keep, long long minAgeSeconds);

} // namespace murasaki::audio
