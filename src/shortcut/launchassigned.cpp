#include "shortcut/launchassigned.h"

namespace murasaki::shortcut {

LaunchOutcome launchAssigned(const QString &desktopId, bool switchOn, GlobalLaunchPolicy &policy, qint64 nowMs, const QStringList &roots,
                             const DesktopEnvironment &env, const FindProgram &find, const std::function<bool(const LaunchCommand &)> &start)
{
    // the entry first: an application that is gone must not use up the minute's count
    std::optional<DesktopEntry> entry;
    if (switchOn && !desktopId.isEmpty()) {
        entry = findApplication(roots, desktopId, env);
        if (!entry) {
            return {false, QStringLiteral("the application %1 is not installed (or not shown on this desktop)").arg(desktopId.left(64))};
        }
    }
    const Verdict verdict = policy.decide(nowMs, switchOn, desktopId);
    if (verdict != Verdict::Allow || !entry) {
        return {false, QString::fromLatin1(verdictName(verdict))};
    }
    QString why;
    if (!launch(*entry, find, start, &why)) {
        return {false, why};
    }
    return {true, {}};
}

} // namespace murasaki::shortcut
