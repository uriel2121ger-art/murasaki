#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace murasaki::audio {

// SHA-256 (FIPS 180-4), streaming. The asset id of a sound is the hex of the hash of its source file.
class Sha256
{
public:
    Sha256();
    void update(const void *data, std::size_t size);
    std::array<std::uint8_t, 32> finish();
    // 64 lowercase hex characters.
    static std::string hex(const std::array<std::uint8_t, 32> &digest);

private:
    void block(const std::uint8_t *p);
    std::uint32_t m_state[8];
    std::uint8_t m_buffer[64];
    std::size_t m_used = 0;
    std::uint64_t m_bytes = 0;
};

} // namespace murasaki::audio
