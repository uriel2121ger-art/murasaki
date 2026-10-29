#include "render/gpuprofiler.h"
#include "util/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace murasaki::scene {

bool GpuProfiler::detailedFromEnvironment()
{
    const char *value = std::getenv("MURASAKI_SCENE_VK_PROFILE");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

GpuProfiler::GpuProfiler(const VkContext &ctx, bool detailed) : m_ctx(ctx), m_detailed(detailed)
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &familyCount, families.data());
    const uint32_t bits = ctx.queueFamily() < familyCount ? families[ctx.queueFamily()].timestampValidBits : 0;
    if (bits == 0 || props.limits.timestampPeriod <= 0.0f) {
        return; // no timestamps on this queue: lastFrameMs stays negative
    }
    m_periodNs = props.limits.timestampPeriod;
    m_validMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kMaxStamps;
    if (vkCreateQueryPool(ctx.device(), &info, nullptr, &m_pool) != VK_SUCCESS) {
        m_pool = VK_NULL_HANDLE;
    }
}

GpuProfiler::~GpuProfiler()
{
    if (m_pool) {
        vkDestroyQueryPool(m_ctx.device(), m_pool, nullptr);
    }
}

void GpuProfiler::begin(VkCommandBuffer cb)
{
    if (!m_pool) {
        return;
    }
    vkCmdResetQueryPool(cb, m_pool, 0, kMaxStamps);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, 0);
    m_next = 1;
    m_labels.clear();
    m_open = true;
}

void GpuProfiler::mark(VkCommandBuffer cb, const std::string &label)
{
    if (!m_pool || !m_detailed || !m_open || m_next >= kMaxStamps - 1) {
        return;
    }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, m_next++);
    m_labels.push_back(label);
}

void GpuProfiler::end(VkCommandBuffer cb)
{
    if (!m_pool || !m_open) {
        return;
    }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, m_next++);
    m_labels.emplace_back("(end)");
}

void GpuProfiler::collect()
{
    if (!m_pool || !m_open) {
        return;
    }
    m_open = false;
    std::vector<uint64_t> stamps(m_next);
    if (vkGetQueryPoolResults(m_ctx.device(), m_pool, 0, m_next, stamps.size() * sizeof(uint64_t), stamps.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        return;
    }
    const auto ms = [&](uint64_t from, uint64_t to) {
        return static_cast<double>((to - from) & m_validMask) * static_cast<double>(m_periodNs) * 1e-6;
    };
    m_lastFrameMs = ms(stamps.front(), stamps.back());
    ++m_frames;
    if (!m_detailed) {
        return;
    }
    for (uint32_t i = 1; i < m_next; ++i) {
        const double t = ms(stamps[i - 1], stamps[i]);
        Stat &s = m_stats[m_labels[i - 1]];
        s.label = m_labels[i - 1];
        s.totalMs += t;
        s.maxMs = std::max(s.maxMs, t);
        ++s.calls;
    }
    if (m_frames % kReportFrames == 0) {
        report();
    }
}

std::vector<GpuProfiler::Stat> GpuProfiler::summary() const
{
    std::vector<Stat> out;
    for (const auto &[label, stat] : m_stats) {
        out.push_back(stat);
    }
    std::sort(out.begin(), out.end(), [](const Stat &a, const Stat &b) { return a.totalMs > b.totalMs; });
    return out;
}

// One block per report: "profile-begin frames=N", one tab separated "profile-pass" line per label, "profile-end".
void GpuProfiler::report() const
{
    logInfo("profile-begin frames={}", m_frames);
    const std::vector<Stat> stats = summary();
    for (size_t i = 0; i < stats.size() && i < 60; ++i) {
        const Stat &s = stats[i];
        logInfo("profile-pass\t{}\t{:.4f}\t{:.3f}\t{:.4f}", s.label, s.totalMs / static_cast<double>(m_frames),
                static_cast<double>(s.calls) / static_cast<double>(m_frames), s.maxMs);
    }
    logInfo("profile-end");
}

} // namespace murasaki::scene
