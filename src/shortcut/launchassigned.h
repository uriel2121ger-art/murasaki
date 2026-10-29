#pragma once

// The last step of a shortcut launch, in the daemon: the application the user assigned (a desktop id) is resolved against the installed
// entries now, judged by the global policy (the user's switch, the minute's count, the same application's gap) and started. Nothing in
// it comes from a scene: the id is what the user stored.

#include "shortcut/desktopentry.h"
#include "shortcut/launcher.h"
#include "shortcut/shortcutpolicy.h"

#include <QString>

#include <functional>

namespace murasaki::shortcut {

struct LaunchOutcome {
    bool started = false;
    QString reason; // why not, for the log
};

LaunchOutcome launchAssigned(const QString &desktopId, bool switchOn, GlobalLaunchPolicy &policy, qint64 nowMs, const QStringList &roots,
                             const DesktopEnvironment &env, const FindProgram &find, const std::function<bool(const LaunchCommand &)> &start);

} // namespace murasaki::shortcut
