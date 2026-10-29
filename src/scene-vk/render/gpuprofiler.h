#pragma once

// GPU time of a frame from timestamp queries. The total of the frame is always measured (two timestamps, nothing
// else); with MURASAKI_SCENE_VK_PROFILE=1 every labelled step of the frame is timed too and a report goes to the log
// every kReportFrames frames (tools/scene-vk-profile.py turns it into a table of the top costs).
//
// Usage per frame: begin(cb); mark(cb, "label") after each step; end(cb); then, once the GPU finished the command
// buffer, collect(). A step's time is the time between its mark and the previous one, so the barriers around a pass
// are part of it.

#include "core/context.h"

#include <map>
#include <string>
#include <vector>

namespace murasaki::scene {

class GpuProfiler
{
public:
    // `detailed`: time every mark (otherwise marks are ignored).
    GpuProfiler(const VkContext &ctx, bool detailed);
    ~GpuProfiler();
    GpuProfiler(const GpuProfiler &) = delete;
    GpuProfiler &operator=(const GpuProfiler &) = delete;

    static bool detailedFromEnvironment();

    bool valid() const { return m_pool != VK_NULL_HANDLE; }
    bool detailed() const { return m_detailed; }

    void begin(VkCommandBuffer cb);
    void mark(VkCommandBuffer cb, const std::string &label);
    void end(VkCommandBuffer cb);
    // Reads the timestamps of the frame that was recorded between begin() and end(); call after its fence signalled.
    void collect();

    // GPU milliseconds of the last collected frame; negative until there is one.
    double lastFrameMs() const { return m_lastFrameMs; }

    struct Stat {
        std::string label;
        double totalMs = 0.0;
        double maxMs = 0.0;
        uint64_t calls = 0;
    };
    // Accumulated per label since the start, the most expensive first.
    std::vector<Stat> summary() const;
    uint64_t frames() const { return m_frames; }

    static constexpr uint32_t kMaxStamps = 1024;
    static constexpr uint64_t kReportFrames = 150;

private:
    void report() const;

    const VkContext &m_ctx;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    bool m_detailed = false;
    float m_periodNs = 1.0f;
    uint64_t m_validMask = ~0ull;
    uint32_t m_next = 0;               // stamps written in the frame being recorded
    std::vector<std::string> m_labels; // label of stamp i (1-based: stamp 0 is the start)
    bool m_open = false;
    double m_lastFrameMs = -1.0;
    uint64_t m_frames = 0;
    std::map<std::string, Stat> m_stats;
};

} // namespace murasaki::scene
