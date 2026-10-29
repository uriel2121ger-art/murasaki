#pragma once

// Asks the daemon to start the application the user chose for a shortcut (D-Bus LaunchShortcut). The surface process never starts
// programs: it passes on a request its own checks let through, and the daemon judges it again from its own data.

#include <QString>

namespace murasaki::shortcut {

// Fire and forget (the answer only goes to the log).
void requestLaunch(const QString &output, const QString &desktop, const QString &itemDir, const QString &key);

} // namespace murasaki::shortcut
