#include "pollingpointersource.h"

#include <cmath>

namespace murasaki {

PollingPointerSource::PollingPointerSource(Provider provider, int intervalMs, QObject *parent)
    : PointerSource(parent)
    , m_provider(std::move(provider))
{
    m_timer.setInterval(intervalMs < 1 ? kDefaultIntervalMs : intervalMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PollingPointerSource::pollOnce);
}

void PollingPointerSource::wanted(bool on)
{
    if (on == m_timer.isActive())
        return;
    if (on) {
        m_last.reset(); // the first poll reports where the cursor rests
        m_timer.start();
        pollOnce();
    } else {
        m_timer.stop();
    }
}

void PollingPointerSource::pollOnce()
{
    if (!m_provider)
        return;
    const std::optional<QPointF> position = m_provider();
    if (!position || !std::isfinite(position->x()) || !std::isfinite(position->y()))
        return;
    if (m_last && *m_last == *position)
        return;
    m_last = position;
    emit moved(*position);
}

} // namespace murasaki
