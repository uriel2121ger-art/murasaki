#pragma once

// The rules of user shortcuts that need no bus, no files and no process: what a property key and a display name may look like, and
// when a wallpaper's request to start an application is allowed. Everything is a pure function or driven by a clock passed in, so
// it is tested without a session. The scene is untrusted: it can ask for a property key and nothing else.
// docs/design/specs/2026-10-21-user-shortcuts-design.md

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <deque>

namespace murasaki::shortcut {

inline constexpr int kMaxKeyBytes = 63;          // a property key as the bridge carries it (name[64], NUL terminated)
inline constexpr int kMaxDisplayNameBytes = 64;  // the application's name as the engine sees it
inline constexpr int kMaxSlots = 64;             // shortcut slots of one scene

// A property key a scene may name: 1 to 63 bytes of UTF-8 with no NUL or control characters.
bool validKey(const QByteArray &key);

// An application's name for the engine and for lists: control characters, bidirectional controls and zero-width marks removed,
// runs of white space made one space, trimmed, cut at kMaxDisplayNameBytes of UTF-8 on a character boundary.
QString cleanDisplayName(const QString &name);

// How often applications may be started.
struct LaunchLimits {
    qint64 clickWindowMs = 750;     // a real button press must have reached the engine this recently
    qint64 gapMs = 2000;            // between two launches of one wallpaper
    qint64 sameAppGapMs = 5000;     // the same application again
    int perMinute = 6;              // over all wallpapers (one LaunchLimiter)
};

// What the host knows when a request arrives.
struct LaunchRequest {
    qint64 nowMs = 0;       // a monotonic clock
    bool allowed = true;    // the user's switch (shortcutsLaunch)
    bool knownKey = false;  // the key is a usershortcut property of this wallpaper
    QString assignedId;     // the desktop id the user assigned to it now; empty: nothing (or not installed any more)
    bool visible = true;    // the wallpaper is shown (not paused or hidden)
};

enum class Verdict { Allow, SwitchOff, UnknownKey, Unassigned, NoRecentClick, TooSoon, SameAppTooSoon, RateLimit, NotVisible };
const char *verdictName(Verdict verdict);

// The launches of all wallpapers over the last minute.
class LaunchLimiter
{
public:
    explicit LaunchLimiter(int perMinute = LaunchLimits{}.perMinute) : m_perMinute(perMinute) {}
    bool allows(qint64 nowMs);
    void record(qint64 nowMs);

private:
    void trim(qint64 nowMs);
    int m_perMinute;
    std::deque<qint64> m_times;
};

// What the daemon applies on top of what the surface checked (a click, the wallpaper shown): the user's switch, a global count over the minute
// and the same application not twice within a few seconds. One per daemon.
class GlobalLaunchPolicy
{
public:
    explicit GlobalLaunchPolicy(LaunchLimits limits = {}) : m_limits(limits), m_limiter(limits.perMinute) {}
    // Allow records the launch.
    Verdict decide(qint64 nowMs, bool switchOn, const QString &desktopId);

private:
    LaunchLimits m_limits;
    LaunchLimiter m_limiter;
    QString m_lastId;
    qint64 m_lastIdAt = 0;
    bool m_any = false;
};

// The state of one wallpaper: its clicks and its last launches.
class LaunchGate
{
public:
    explicit LaunchGate(LaunchLimiter *limiter, LaunchLimits limits = {}) : m_limiter(limiter), m_limits(limits) {}

    // A real button press was delivered to the wallpaper's engine at `nowMs`.
    void noteClick(qint64 nowMs) { m_lastClick = nowMs; m_clicked = true; }
    // Judges a request; on Allow the launch is recorded (the caller then starts the application).
    Verdict decide(const LaunchRequest &request);

private:
    LaunchLimiter *m_limiter;
    LaunchLimits m_limits;
    qint64 m_lastClick = 0;
    bool m_clicked = false;
    qint64 m_lastLaunch = 0;
    bool m_launched = false;
    QString m_lastId;
    qint64 m_lastIdAt = 0;
};

} // namespace murasaki::shortcut
