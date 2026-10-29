#pragma once

#include <QString>
#include <QStringList>

namespace murasaki {

// What to do on battery: nothing, pause every wallpaper, or cap the animated ones at kBatteryFpsCap frames a second.
enum class BatteryMode { Off, Pause, LowFps };

inline constexpr int kBatteryFpsCap = 15;

QString batteryModeToString(BatteryMode mode);
// An unknown string is Off.
BatteryMode batteryModeFromString(const QString &text);

// The user's rules (the daemon's settings pauseOnGame, batteryMode and pauseApps).
struct PauseRules {
    bool pauseOnGame = true;
    BatteryMode battery = BatteryMode::Off;
    // Resource classes of the windows ("steam_app_1245620", "firefox"): `*` and `?` match any characters. The
    // wallpaper pauses while a window of one of them has the focus, or any is fullscreen.
    QStringList apps;

    bool operator==(const PauseRules &other) const = default;
};

// What the session says right now (the GameMode and UPower watchers, the compositor monitor).
struct PauseFacts {
    bool gameRunning = false;   // GameMode has clients
    bool onBattery = false;     // UPower OnBattery
    QString activeApp;          // resource class of the window that has the focus, empty when unknown or none
    QStringList fullscreenApps; // resource classes of the fullscreen windows
    bool windowsKnown = false;  // the compositor says which outputs a window covers (OutputSession::occluded)
};

enum class PauseReason { None, Game, Battery, App };

QString pauseReasonToString(PauseReason reason); // "", "game", "battery", "app"

struct PauseVerdict {
    bool pauseAll = false;
    PauseReason reason = PauseReason::None; // why, when pauseAll
    // The pause holds only where a fullscreen or maximized window covers the output: a game on one screen leaves the wallpaper
    // of another one playing (a clock goes on). Set for a game when the compositor says which outputs are covered.
    bool coveredOnly = false;
    QString detail;                         // the rule that matched for App (as the user wrote it)
    int fpsCap = 0;                         // 0: no cap; kBatteryFpsCap on battery with LowFps (when not paused)

    bool operator==(const PauseVerdict &other) const = default;
};

// Every wallpaper pauses (and goes silent) while a rule says so; the first that applies names the reason:
// game, then battery, then application. A game pauses only the covered outputs when they are known. LowFps only caps when nothing pauses.
PauseVerdict evaluatePauseRules(const PauseRules &rules, const PauseFacts &facts);

// Whether a resource class matches a rule (case-insensitive, with `*` and `?`).
bool appMatches(const QString &rule, const QString &resourceClass);

} // namespace murasaki
