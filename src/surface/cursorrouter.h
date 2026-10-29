#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <optional>

namespace murasaki {

// An output with its place in the compositor's global logical coordinate space (the space of KWin's cursor
// position: scaled pixels, not device pixels).
struct CursorScreen {
    QString name;
    QRectF geometry;
};

// Where a global position falls: the output and the position inside it, both in logical pixels.
struct CursorHit {
    QString output;
    QPointF local;
    QSizeF size;
};

// The output under `global`, or nothing when it is outside every output. An output owns its left and top edge
// but not the right and bottom one, so a shared edge belongs to exactly one output.
std::optional<CursorHit> mapCursor(const QList<CursorScreen> &screens, const QPointF &global);

// What the policy server tells the host of an output.
struct CursorMessage {
    enum class Kind { Move, Left };
    Kind kind = Kind::Move;
    QString output;
    QPointF local;  // Move only
    QSizeF size;    // Move only: the output's logical size, so the host can scale to its render size
    bool operator==(const CursorMessage &other) const = default;
};

// Turns the stream of global cursor positions into at most `minIntervalMs`-spaced messages for the hosts: a Move
// for the output the cursor is on and a Left for the output it came from. A position that arrives too early is
// held back (only the latest one) and released by takePending(), so the end of a movement is never lost.
class CursorRouter
{
public:
    explicit CursorRouter(int minIntervalMs = 30) : m_minIntervalMs(minIntervalMs) {}

    QList<CursorMessage> update(const QList<CursorScreen> &screens, const QPointF &global, qint64 nowMs);
    // Milliseconds until a held-back position may go out; -1 when none is held back.
    int pendingDelayMs(qint64 nowMs) const;
    QList<CursorMessage> takePending(const QList<CursorScreen> &screens, qint64 nowMs);
    // Forgets the cursor's output (the hosts are gone, the script was unloaded): the next position is a fresh start.
    void reset();

private:
    QList<CursorMessage> route(const QList<CursorScreen> &screens, const QPointF &global, qint64 nowMs);

    int m_minIntervalMs;
    std::optional<qint64> m_lastSentMs;
    std::optional<QPointF> m_pending;
    QString m_current; // the output the cursor was last on; empty: none
};

} // namespace murasaki
