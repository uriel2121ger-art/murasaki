#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki::autostart {

// The systemd user unit that shows the wallpaper (installed from data/murasaki-surface.service.in).
inline const QString kSurfaceUnit = QStringLiteral("murasaki-surface.service");

// The fixed `systemctl` argument lists for that unit (no shell is involved, nothing user-controlled in them).
QStringList queryEnabledArguments(); // is-enabled --quiet
QStringList changeEnabledArguments(bool enabled); // enable|disable --now
QStringList startArguments(); // start
// The verb of changeEnabledArguments(), for messages.
QString changeEnabledVerb(bool enabled);

// Whether the systemd user unit that shows the wallpaper starts with the session
// (`systemctl --user is-enabled`). Synchronous, with a short timeout; false if systemctl cannot be run.
bool isEnabled();

// Enables (and starts) or disables (and stops) the unit, i.e. `systemctl --user enable|disable --now`.
// systemctl's own output goes to this process's stdout/stderr, so a CLI user sees why it failed.
// On failure returns false and, if error is not null, stores a message in it; if couldNotRun is not null it
// tells whether systemctl could not be started or timed out (true) rather than exiting with an error (false).
bool setEnabled(bool enabled, QString *error = nullptr, bool *couldNotRun = nullptr);

// Runs `systemctl` with these arguments and returns its exit code, or -1 if it cannot be started or does not
// finish within timeoutMs in total (negative: no limit on the run itself; starting is still bounded).
// forwardOutput passes systemctl's stdout/stderr through to this process, otherwise they are discarded.
// Replaceable so tests never touch the real session.
using CommandRunner = std::function<int(const QStringList &arguments, int timeoutMs, bool forwardOutput)>;

// Installs a runner and returns the previous one; an empty runner restores the real systemctl.
// Not thread-safe: call it, isEnabled() and setEnabled() from the main thread only.
CommandRunner setCommandRunner(CommandRunner runner);

} // namespace murasaki::autostart
