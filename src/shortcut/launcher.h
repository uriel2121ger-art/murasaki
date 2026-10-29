#pragma once

// Starting the application a user chose for a shortcut. The scene's text never reaches a command: the host passes the real path
// (or id) of a desktop entry it found itself. In order of preference: `gio launch <path>` (GIO applies Exec, field codes, DBus
// activation, Flatpak and TryExec), `gtk-launch <id>`, and as a last resort the entry's own Exec line, read as the Desktop Entry
// Specification says (quoting and escapes), with the field codes dropped and no shell. When none of them is safe nothing starts.
// docs/design/specs/2026-10-21-user-shortcuts-design.md

#include "shortcut/desktopentry.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace murasaki::shortcut {

struct LaunchCommand {
    QString program; // an absolute path
    QStringList arguments;
    QString how;     // "gio", "gtk-launch" or "exec": for the log
};

// Where programs are looked for (a name in PATH to an absolute path, or an empty string when there is none).
using FindProgram = std::function<QString(const QString &name)>;
FindProgram systemFindProgram();

// The Exec line as an argument vector: split by the specification's rules (double quotes, escapes inside them), the field codes
// %f %F %u %U %i %c %k (and the deprecated %d %D %n %N %v %m) dropped, %% a percent sign. Nullopt (refused) for: an unterminated
// quote, an unknown % code, a reserved character outside quotes, an empty result, more than 64 arguments or 4096 bytes.
std::optional<QStringList> parseExec(const QString &exec);

// How to start `entry`, or nullopt when there is no safe way (and the reason in `why`, for the log).
std::optional<LaunchCommand> planLaunch(const DesktopEntry &entry, const FindProgram &find, QString *why = nullptr);

// Starts a command detached from this process (new session, no stdio, a cleaned environment). False when it could not start.
bool startDetached(const LaunchCommand &command);

// Plans and starts through `start` (injected so tests record the command instead of running it).
bool launch(const DesktopEntry &entry, const FindProgram &find, const std::function<bool(const LaunchCommand &)> &start, QString *why = nullptr);

} // namespace murasaki::shortcut
