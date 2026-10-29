#include "puppet/params.h"

#include "puppetlimits.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

bool finite(double v)
{
    return std::isfinite(v);
}

} // namespace

const Parameter &ParameterSet::at(size_t index) const
{
    static const Parameter none{{}, 0, 0, 0, 0};
    return index < m_items.size() ? m_items[index] : none;
}

const std::string &PartSet::id(size_t index) const
{
    static const std::string none;
    return index < m_ids.size() ? m_ids[index] : none;
}

bool ParameterSet::add(std::string id, double min, double def, double max)
{
    if (id.empty() || static_cast<std::int64_t>(id.size()) > kMaxIdBytes || static_cast<std::int64_t>(m_items.size()) >= kMaxParameters)
        return false;
    if (!finite(min) || !finite(def) || !finite(max) || min > max || def < min || def > max)
        return false;
    if (find(id) >= 0)
        return false;
    m_items.push_back({std::move(id), min, def, max, def});
    return true;
}

int ParameterSet::find(std::string_view id) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == id)
            return static_cast<int>(i);
    return -1;
}

bool ParameterSet::set(int index, double value)
{
    if (index < 0 || static_cast<size_t>(index) >= m_items.size())
        return false;
    if (!finite(value))
        return true; // ignored
    Parameter &p = m_items[static_cast<size_t>(index)];
    p.value = std::clamp(value, p.min, p.max);
    return true;
}

bool ParameterSet::setById(std::string_view id, double value)
{
    const int index = find(id);
    return index >= 0 && set(index, value);
}

void ParameterSet::resetToDefaults()
{
    for (Parameter &p : m_items)
        p.value = p.def;
}

std::vector<double> ParameterSet::snapshot() const
{
    std::vector<double> out;
    out.reserve(m_items.size());
    for (const Parameter &p : m_items)
        out.push_back(p.value);
    return out;
}

bool ParameterSet::restore(const std::vector<double> &values)
{
    if (values.size() != m_items.size())
        return false;
    for (size_t i = 0; i < values.size(); ++i)
        set(static_cast<int>(i), values[i]);
    return true;
}

bool PartSet::add(std::string id)
{
    if (id.empty() || static_cast<std::int64_t>(id.size()) > kMaxIdBytes || static_cast<std::int64_t>(m_ids.size()) >= kMaxParts || find(id) >= 0)
        return false;
    m_ids.push_back(std::move(id));
    m_opacity.push_back(1.0);
    return true;
}

int PartSet::find(std::string_view id) const
{
    for (size_t i = 0; i < m_ids.size(); ++i)
        if (m_ids[i] == id)
            return static_cast<int>(i);
    return -1;
}

bool PartSet::set(int index, double opacity)
{
    if (index < 0 || static_cast<size_t>(index) >= m_ids.size())
        return false;
    if (finite(opacity))
        m_opacity[static_cast<size_t>(index)] = std::clamp(opacity, 0.0, 1.0);
    return true;
}

void PartSet::resetToOpaque()
{
    std::fill(m_opacity.begin(), m_opacity.end(), 1.0);
}

void Breath::update(double dt)
{
    if (!finite(dt) || dt < 0)
        return;
    m_time = std::fmod(m_time + dt, m_config.period > 0 ? m_config.period : 1.0);
}

bool Breath::apply(ParameterSet &parameters) const
{
    const int index = parameters.find("ParamBreath");
    if (index < 0)
        return false;
    const Parameter &p = parameters.at(static_cast<size_t>(index));
    const double period = m_config.period > 0 ? m_config.period : 1.0;
    parameters.set(index, p.min + (p.max - p.min) * (0.5 + 0.5 * std::sin(kTwoPi * m_time / period)));
    return true;
}

AutoBlink::AutoBlink(std::uint64_t seed, BlinkConfig config) : m_config(config), m_rng(seed)
{
    startWait();
}

void AutoBlink::startWait()
{
    m_phase = Phase::Waiting;
    m_left = m_config.minInterval + (m_config.maxInterval - m_config.minInterval) * m_rng.uniform();
}

void AutoBlink::update(double dt)
{
    if (!finite(dt) || dt <= 0)
        return;
    // consume the time phase by phase, so the result does not depend on how it is cut into steps
    int guard = 0;
    while (dt > 0 && guard++ < 64) {
        if (dt < m_left) {
            m_left -= dt;
            return;
        }
        dt -= m_left;
        switch (m_phase) {
        case Phase::Waiting: m_phase = Phase::Closing; m_left = m_config.closeTime; break;
        case Phase::Closing: m_phase = Phase::Closed; m_left = m_config.holdTime; break;
        case Phase::Closed: m_phase = Phase::Opening; m_left = m_config.openTime; break;
        case Phase::Opening: startWait(); break;
        }
    }
}

double AutoBlink::openness() const
{
    const auto fraction = [](double left, double total) { return total > 0 ? std::clamp(left / total, 0.0, 1.0) : 0.0; };
    switch (m_phase) {
    case Phase::Waiting: return 1.0;
    case Phase::Closing: return fraction(m_left, m_config.closeTime);          // 1 -> 0
    case Phase::Closed: return 0.0;
    case Phase::Opening: return 1.0 - fraction(m_left, m_config.openTime);     // 0 -> 1
    }
    return 1.0;
}

int AutoBlink::apply(ParameterSet &parameters, const std::vector<std::string> &eyeIds) const
{
    int touched = 0;
    const double open = openness();
    for (const std::string &id : eyeIds) {
        const int index = parameters.find(id);
        if (index < 0)
            continue;
        parameters.set(index, parameters.value(index) * open);
        ++touched;
    }
    return touched;
}

} // namespace murasaki::puppet
