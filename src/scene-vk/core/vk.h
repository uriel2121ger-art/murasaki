#pragma once

// Vulkan entry point loader (volk) and memory allocator (VMA) in one include, plus the error type the engine
// throws for failures that cannot be handled locally. Nothing links against libvulkan: volk loads it at run time.

#include <volk.h>
#include <vk_mem_alloc.h>

#include <stdexcept>
#include <string>

namespace murasaki::scene {

class VkException : public std::runtime_error
{
public:
    VkException(const std::string &what, VkResult result)
        : std::runtime_error(what + ": VkResult " + std::to_string(static_cast<int>(result))), m_result(result)
    {
    }
    explicit VkException(const std::string &what) : std::runtime_error(what), m_result(VK_ERROR_UNKNOWN) {}
    VkResult result() const { return m_result; }

private:
    VkResult m_result;
};

inline void vkCheck(VkResult result, const char *what)
{
    if (result != VK_SUCCESS) {
        throw VkException(what, result);
    }
}

} // namespace murasaki::scene
