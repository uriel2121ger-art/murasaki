#include "cursorrouter.h"

#include <algorithm>

namespace murasaki {

std::optional<CursorHit> mapCursor(const QList<CursorScreen> &screens, const QPointF &global)
{
    for (const CursorScreen &screen : screens) {
        const QRectF &g = screen.geometry;
        if (g.width() <= 0 || g.height() <= 0)
            continue;
        if (global.x() >= g.left() && global.x() < g.left() + g.width() && global.y() >= g.top()
            && global.y() < g.top() + g.height())
            return CursorHit{screen.name, global - g.topLeft(), g.size()};
    }
    return std::nullopt;
}

QList<CursorMessage> CursorRouter::update(const QList<CursorScreen> &screens, const QPointF &global, qint64 nowMs)
{
    if (m_lastSentMs && nowMs - *m_lastSentMs < m_minIntervalMs) {
        m_pending = global;
        return {};
    }
    m_pending.reset();
    return route(screens, global, nowMs);
}

int CursorRouter::pendingDelayMs(qint64 nowMs) const
{
    if (!m_pending)
        return -1;
    return m_lastSentMs ? int(std::max<qint64>(0, m_minIntervalMs - (nowMs - *m_lastSentMs))) : 0;
}

QList<CursorMessage> CursorRouter::takePending(const QList<CursorScreen> &screens, qint64 nowMs)
{
    if (!m_pending || pendingDelayMs(nowMs) > 0)
        return {};
    const QPointF global = *m_pending;
    m_pending.reset();
    return route(screens, global, nowMs);
}

void CursorRouter::reset()
{
    m_lastSentMs.reset();
    m_pending.reset();
    m_current.clear();
}

QList<CursorMessage> CursorRouter::route(const QList<CursorScreen> &screens, const QPointF &global, qint64 nowMs)
{
    m_lastSentMs = nowMs;
    QList<CursorMessage> messages;
    const std::optional<CursorHit> hit = mapCursor(screens, global);
    const QString now = hit ? hit->output : QString();
    if (!m_current.isEmpty() && m_current != now)
        messages.append({CursorMessage::Kind::Left, m_current, {}, {}});
    if (hit)
        messages.append({CursorMessage::Kind::Move, hit->output, hit->local, hit->size});
    m_current = now;
    return messages;
}

} // namespace murasaki
