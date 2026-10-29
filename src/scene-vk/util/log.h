#pragma once

// Minimal logging to stderr. The engine is a plain process: its output goes to a log file (bridge-dump, the bridge
// host) and is never parsed, so there are no levels beyond the prefix.

#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace murasaki::scene {

template <class... Args>
inline void logInfo(std::format_string<Args...> fmt, Args &&...args)
{
    const std::string text = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "murasaki-scene-vk: %s\n", text.c_str());
}

template <class... Args>
inline void logWarn(std::format_string<Args...> fmt, Args &&...args)
{
    const std::string text = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "murasaki-scene-vk: warning: %s\n", text.c_str());
}

template <class... Args>
inline void logError(std::format_string<Args...> fmt, Args &&...args)
{
    const std::string text = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "murasaki-scene-vk: error: %s\n", text.c_str());
}

} // namespace murasaki::scene
