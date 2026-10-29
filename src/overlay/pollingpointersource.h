#pragma once

#include "pointersource.h"

#include <QTimer>

#include <functional>
#include <optional>

namespace murasaki {

// A global source that asks for the cursor on a timer, only while wanted: X11, where QCursor::pos() is reliable (the window code passes
// it as `provider`; on Wayland it is not, docs/pets/pointer-facts.md F6). Reports the first position at once and then only changes.
class PollingPointerSource : public PointerSource
{
    Q_OBJECT

public:
    using Provider = std::function<std::optional<QPointF>()>;
    static constexpr int kDefaultIntervalMs = 32; // Anima's own tick

    explicit PollingPointerSource(Provider provider, int intervalMs = kDefaultIntervalMs, QObject *parent = nullptr);

    Level level() const override { return Level::Global; }
    void wanted(bool on) override;
    bool isRunning() const { return m_timer.isActive(); }
    // One poll now (the timer calls this; tests too).
    void pollOnce();

private:
    Provider m_provider;
    QTimer m_timer;
    std::optional<QPointF> m_last;
};

} // namespace murasaki
