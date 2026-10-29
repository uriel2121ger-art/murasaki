#include "pausepolicy.h"

#include "appclass.h"

#include <QHash>
#include <QRegularExpression>

namespace murasaki {

QString batteryModeToString(BatteryMode mode)
{
    switch (mode) {
    case BatteryMode::Pause:
        return QStringLiteral("pause");
    case BatteryMode::LowFps:
        return QStringLiteral("lowfps");
    case BatteryMode::Off:
        break;
    }
    return QStringLiteral("off");
}

BatteryMode batteryModeFromString(const QString &text)
{
    if (text == u"pause")
        return BatteryMode::Pause;
    if (text == u"lowfps")
        return BatteryMode::LowFps;
    return BatteryMode::Off;
}

QString pauseReasonToString(PauseReason reason)
{
    switch (reason) {
    case PauseReason::Game:
        return QStringLiteral("game");
    case PauseReason::Battery:
        return QStringLiteral("battery");
    case PauseReason::App:
        return QStringLiteral("app");
    case PauseReason::None:
        break;
    }
    return {};
}

bool appMatches(const QString &rule, const QString &resourceClass)
{
    if (rule.isEmpty() || resourceClass.isEmpty())
        return false;
    // Compiled once per rule: this runs on every state report. fromWildcard anchors the pattern (the whole class).
    static QHash<QString, QRegularExpression> compiled;
    auto it = compiled.find(rule);
    if (it == compiled.end()) {
        if (compiled.size() > 256) // rules are few; this only guards against a pathological stream of them
            compiled.clear();
        it = compiled.insert(rule, QRegularExpression::fromWildcard(rule, Qt::CaseInsensitive,
                                                                    QRegularExpression::NonPathWildcardConversion));
    }
    return it->match(resourceClass).hasMatch() || sameAppClass(rule, resourceClass);
}

PauseVerdict evaluatePauseRules(const PauseRules &rules, const PauseFacts &facts)
{
    PauseVerdict verdict;
    if (rules.pauseOnGame && facts.gameRunning) {
        verdict.pauseAll = true;
        verdict.reason = PauseReason::Game;
        verdict.coveredOnly = facts.windowsKnown;
        return verdict;
    }
    if (facts.onBattery && rules.battery == BatteryMode::Pause) {
        verdict.pauseAll = true;
        verdict.reason = PauseReason::Battery;
        return verdict;
    }
    for (const QString &rule : rules.apps) {
        bool matches = appMatches(rule, facts.activeApp);
        for (const QString &fullscreen : facts.fullscreenApps)
            matches = matches || appMatches(rule, fullscreen);
        if (matches) {
            verdict.pauseAll = true;
            verdict.reason = PauseReason::App;
            verdict.detail = rule;
            return verdict;
        }
    }
    if (facts.onBattery && rules.battery == BatteryMode::LowFps)
        verdict.fpsCap = kBatteryFpsCap;
    return verdict;
}

} // namespace murasaki
