#pragma once

// SHA-256, for cache keys: a collision-resistant key means a cache file can never answer for different input.
// Straight from FIPS 180-4.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace murasaki::scene {

class Sha256
{
public:
    Sha256() { reset(); }
    void reset();
    void update(const void *data, size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }
    // Finishes and returns the digest as 64 lower-case hex characters. The object must be reset() to be reused.
    std::string hex();

private:
    void block(const uint8_t *chunk);

    std::array<uint32_t, 8> m_state{};
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffered = 0;
    uint64_t m_total = 0;
};

inline std::string sha256Hex(std::string_view text)
{
    Sha256 h;
    h.update(text);
    return h.hex();
}

} // namespace murasaki::scene
