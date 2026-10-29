#pragma once

// Where the cursor position of a "global" pointer level comes from (a KWin script, Hyprland's IPC, an X11 poll). The
// interaction asks for it only while a pet needs it: with no hide, fade or dodge on, nothing runs.
//
// Backends (docs/pets/behaviours.md, "Pointer sources"): KWinPointerSource, HyprlandPointerSource, PollingPointerSource (X11) and
// LocalPointerSource (everything else); pointercapability.h picks one per session.

#include <QObject>
#include <QPointF>
#include <QString>

#include <functional>
#include <memory>

namespace murasaki {

class PointerSource : public QObject
{
    Q_OBJECT

public:
    enum class Level { Global, Local };

    using QObject::QObject;

    // Global: moved() carries the cursor in the compositor's global logical coordinates (scaled pixels, the space of QScreen::geometry;
    // `mapCursor()` of cursorrouter.h turns it into an output and a position in it), at most ~30 times a second, whether or not the
    // cursor is over a pet. Local: the source knows nothing (enter, leave and motion on the pet's own surface are all there is).
    virtual Level level() const = 0;
    // Start (true) or stop (false) reporting; the last call wins. Stopping releases whatever the backend runs (a KWin script, a timer).
    virtual void wanted(bool on) = 0;

signals:
    void moved(QPointF global);
    // A global backend could not start or lost its compositor (KWin refused the script, Hyprland's socket is gone): the owner should
    // fall back to a LocalPointerSource for the session. `reason` is for the log.
    void unavailable(QString reason);
};

// A source driven by hand, for tests and for the nested checks.
class ManualPointerSource : public PointerSource
{
    Q_OBJECT

public:
    explicit ManualPointerSource(Level level = Level::Global, QObject *parent = nullptr)
        : PointerSource(parent)
        , m_level(level)
    {
    }

    Level level() const override { return m_level; }
    void wanted(bool on) override
    {
        m_wanted = on;
        ++m_wantedCalls;
    }

    bool isWanted() const { return m_wanted; }
    int wantedCalls() const { return m_wantedCalls; }
    // Reports a position only while wanted, like a real source.
    void moveTo(QPointF p)
    {
        if (m_wanted)
            emit moved(p);
    }

private:
    Level m_level;
    bool m_wanted = false;
    int m_wantedCalls = 0;
};

// The level where the compositor tells a pet nothing but what happens on its own surface (sway, labwc, river, wayfire, niri, COSMIC,
// GNOME until its extension reports the pointer, or any session where the global source could not start).
class LocalPointerSource : public PointerSource
{
    Q_OBJECT

public:
    using PointerSource::PointerSource;

    Level level() const override { return Level::Local; }
    void wanted(bool on) override { m_wanted = on; }
    bool isWanted() const { return m_wanted; }

private:
    bool m_wanted = false;
};

// A source that exists only once something asks for it: the backend (a D-Bus object and a call to KWin, a socket, a timer) is made by the factory
// on the first wanted(true), so that a session whose pets never hide, fade or dodge never touches the compositor. Reports the level it was told.
class LazyPointerSource : public PointerSource
{
    Q_OBJECT

public:
    using Factory = std::function<std::unique_ptr<PointerSource>()>;

    LazyPointerSource(Level level, Factory factory, QObject *parent = nullptr)
        : PointerSource(parent)
        , m_level(level)
        , m_factory(std::move(factory))
    {
    }

    Level level() const override { return m_level; }
    void wanted(bool on) override
    {
        if (on && !m_real && m_factory) {
            m_real = m_factory();
            if (m_real) {
                m_real->setParent(this);
                connect(m_real.get(), &PointerSource::moved, this, &PointerSource::moved);
                connect(m_real.get(), &PointerSource::unavailable, this, &PointerSource::unavailable);
            }
        }
        if (m_real)
            m_real->wanted(on);
    }
    // Whether the backend was made.
    bool created() const { return m_real != nullptr; }

private:
    Level m_level;
    Factory m_factory;
    std::unique_ptr<PointerSource> m_real;
};

} // namespace murasaki
