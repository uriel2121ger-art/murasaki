#pragma once

#include <QObject>

namespace murasaki {

// Whether the user is away, as the compositor says it (docs/pets/session-facts.md, "idle"): known only when the compositor has a way to tell. The surface publishes it in
// SessionFacts() and the daemon's idle triggers prefer it to logind's IdleHint, which Plasma never sets.
class IdleSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() {}
    bool known() const { return m_known; }
    bool idle() const { return m_known && m_idle; }
    // How long after the last input an idle is reported; what the daemon takes off the moment it hears of it.
    virtual int afterMs() const { return 0; }

    // For implementations (and the tests that stand in for one).
    void setIdle(bool known, bool idle)
    {
        if (known == m_known && idle == m_idle)
            return;
        m_known = known;
        m_idle = idle;
        emit changed();
    }

signals:
    void changed();

private:
    bool m_known = false;
    bool m_idle = false;
};

} // namespace murasaki
