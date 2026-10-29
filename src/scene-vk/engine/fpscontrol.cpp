#include "engine/fpscontrol.h"

#include <algorithm>
#include <numeric>

namespace murasaki::scene {

void StepBackOff::tick()
{
    ++m_sinceUp;
    ++m_sinceDown;
    // stable for long (sixteen of the ordinary waits since the last fall): the falls are forgotten
    if (m_hold > 1 && m_sinceDown >= 16ul * m_upFrames) {
        m_hold = 1;
    }
}

void StepBackOff::steppedDown(unsigned upFrames)
{
    if (m_sinceUp < 2ul * upFrames) {
        m_hold = std::min(m_hold * 2, kMaxHold);
    }
    m_sinceDown = 0;
    m_upFrames = upFrames;
}

FpsController::FpsController(int requestedFps, Config config) : m_config(std::move(config))
{
    setRequested(requestedFps);
}

void FpsController::setRequested(int fps)
{
    m_requested = std::max(1, fps);
    rebuild();
}

void FpsController::rebuild()
{
    m_ladder.clear();
    m_ladder.push_back(m_requested);
    std::vector<int> steps = m_config.ladder;
    std::sort(steps.begin(), steps.end(), std::greater<int>());
    for (const int step : steps) {
        if (step < m_ladder.back() && step > 0) {
            m_ladder.push_back(step);
        }
    }
    m_level = 0;
    m_samples.clear();
    m_backOff = {}; // a new request starts afresh
}

namespace {
double mean(const std::deque<double> &samples, size_t last)
{
    const size_t n = std::min(last, samples.size());
    return std::accumulate(samples.end() - static_cast<std::ptrdiff_t>(n), samples.end(), 0.0) / static_cast<double>(std::max<size_t>(1, n));
}

// The median of the last `last` samples (the upper one of an even count)
double median(const std::deque<double> &samples, size_t last)
{
    const size_t n = std::min(last, samples.size());
    std::vector<double> window(samples.end() - static_cast<std::ptrdiff_t>(n), samples.end());
    if (window.empty()) {
        return 0.0;
    }
    const auto middle = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
    std::nth_element(window.begin(), middle, window.end());
    return *middle;
}
} // namespace

std::optional<int> FpsController::update(double gpuMs)
{
    if (!(gpuMs >= 0.0)) {
        return std::nullopt;
    }
    m_samples.push_back(gpuMs);
    while (m_samples.size() > std::max(m_config.downFrames, m_config.upFrames)) {
        m_samples.pop_front();
    }
    m_backOff.tick();
    const double interval = 1000.0 / static_cast<double>(current());
    if (const double cost = m_samples.size() >= m_config.downFrames ? median(m_samples, m_config.downFrames) : 0.0;
        m_level + 1 < m_ladder.size() && cost > m_config.downLoad * interval) {
        // straight to the highest rate this GPU time fits (the last one when none does)
        do {
            ++m_level;
        } while (m_level + 1 < m_ladder.size() && cost > m_config.downLoad * 1000.0 / static_cast<double>(m_ladder[m_level]));
        m_samples.clear();
        m_backOff.steppedDown(m_config.upFrames);
        return current();
    }
    if (m_level > 0 && m_samples.size() >= m_config.upFrames && m_backOff.waited(m_config.upFrames)) {
        // the load the rate one step up would have for this GPU time
        const double upInterval = 1000.0 / static_cast<double>(m_ladder[m_level - 1]);
        if (mean(m_samples, m_config.upFrames) < m_config.upLoad * upInterval) {
            --m_level;
            m_samples.clear();
            m_backOff.steppedUp();
            return current();
        }
    }
    return std::nullopt;
}

ScaleController::ScaleController(double ceiling, Config config) : m_config(std::move(config))
{
    if (m_config.steps.empty()) {
        m_config.steps = {1.0};
    }
    setCeiling(ceiling);
}

void ScaleController::setCeiling(double ceiling)
{
    m_ceiling = ceiling > 0.0 ? ceiling : 1.0;
    m_level = 0;
    m_samples.clear();
    m_backOff = {};
}

std::optional<double> ScaleController::update(double load, bool atRequestedRate)
{
    if (!enabled() || !(load >= 0.0)) {
        return std::nullopt;
    }
    m_samples.push_back(load);
    while (m_samples.size() > std::max(m_config.downFrames, m_config.upFrames)) {
        m_samples.pop_front();
    }
    m_backOff.tick();
    if (const double load = m_samples.size() >= m_config.downFrames ? median(m_samples, m_config.downFrames) : 0.0;
        !atFloor() && load > m_config.budget) {
        // straight to the largest step whose predicted load (the cost follows the pixel count) fits; the floor when none does
        const double now = m_config.steps[m_level];
        do {
            ++m_level;
        } while (!atFloor() && load * (m_config.steps[m_level] * m_config.steps[m_level]) / (now * now) > m_config.budget);
        m_samples.clear();
        m_backOff.steppedDown(m_config.upFrames);
        return current();
    }
    if (m_level > 0 && atRequestedRate && m_samples.size() >= m_config.upFrames && m_backOff.waited(m_config.upFrames)) {
        const double now = m_config.steps[m_level];
        const double up = m_config.steps[m_level - 1];
        const double predicted = mean(m_samples, m_config.upFrames) * (up * up) / (now * now);
        if (predicted < m_config.budget * m_config.upMargin) {
            --m_level;
            m_samples.clear();
            m_backOff.steppedUp();
            return current();
        }
    }
    return std::nullopt;
}

} // namespace murasaki::scene
