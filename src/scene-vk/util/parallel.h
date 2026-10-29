#pragma once

// A bounded parallel loop for the decoding work of loading a scene (several textures, several mips each). The calling thread
// always takes part; helper threads are started only while fewer than `kMaxHelpers` run in the process, so nested loops
// (textures, and the mips inside each) never multiply into dozens of threads.

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace murasaki::scene {

// min(4, half the cores, at least 1): the helper threads a load may use besides the calling one, process wide.
inline int maxHelperThreads()
{
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp<unsigned>(cores / 2, 1, 4)) - 1;
}

inline std::atomic<int> &activeHelpers()
{
    static std::atomic<int> count{0};
    return count;
}

// Calls fn(i) for every i in [0, count), on this thread and up to maxHelperThreads() helpers; returns when all are done.
inline void parallelFor(size_t count, const std::function<void(size_t)> &fn)
{
    if (count == 0) {
        return;
    }
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            fn(i);
        }
    };
    std::vector<std::thread> helpers;
    const int limit = maxHelperThreads();
    while (helpers.size() + 1 < count) {
        if (activeHelpers().fetch_add(1) >= limit) {
            activeHelpers().fetch_sub(1);
            break;
        }
        helpers.emplace_back([&] {
            work();
            activeHelpers().fetch_sub(1);
        });
    }
    work();
    for (std::thread &t : helpers) {
        t.join();
    }
}

} // namespace murasaki::scene
