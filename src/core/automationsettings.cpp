#include "automationsettings.h"

#include <QJsonArray>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

QStringList AutomationSettings::keys()
{
    return {u"automationEnabled"_s, u"fireByTimelineName"_s, u"triggerCooldownMs"_s, u"demoCardsDismissed"_s,
            u"screenTriggersEnabled"_s, u"screenTicksPerSecond"_s, u"screenRemember"_s, u"screenWatchAtLogin"_s, u"screenIgnorePets"_s};
}

AutomationSettings::Result AutomationSettings::apply(const QString &key, const QJsonValue &v, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Result::Error;
    };
    if (key == u"automationEnabled") {
        if (!v.isBool())
            return fail(key + u" must be a boolean"_s);
        enabled = v.toBool();
    } else if (key == u"fireByTimelineName") {
        if (!v.isBool())
            return fail(key + u" must be a boolean"_s);
        fireByTimelineName = v.toBool();
    } else if (key == u"triggerCooldownMs") {
        const double d = v.toDouble();
        if (!v.isDouble() || d != std::floor(d) || d < kMinTriggerCooldownMs || d > kMaxTriggerCooldownMs)
            return fail(key + u" must be a whole number between %1 and %2"_s.arg(kMinTriggerCooldownMs).arg(kMaxTriggerCooldownMs));
        triggerCooldownMs = int(d);
    } else if (key == u"screenTriggersEnabled" || key == u"screenRemember" || key == u"screenWatchAtLogin" || key == u"screenIgnorePets") {
        if (!v.isBool())
            return fail(key + u" must be a boolean"_s);
        (key == u"screenTriggersEnabled" ? screenTriggersEnabled : key == u"screenRemember" ? screenRemember : key == u"screenWatchAtLogin" ? screenWatchAtLogin : screenIgnorePets) = v.toBool();
    } else if (key == u"screenTicksPerSecond") {
        const double d = v.toDouble();
        if (!v.isDouble() || d != std::floor(d) || d < 1 || d > 16)
            return fail(key + u" must be a whole number between 1 and 16"_s);
        screenTicksPerSecond = int(d);
    } else if (key == u"demoCardsDismissed") {
        if (!v.isArray())
            return fail(key + u" must be an array of page names"_s);
        QStringList pages;
        for (const QJsonValue &page : v.toArray()) {
            const QString name = page.toString();
            if (!page.isString() || (name != u"timelines" && name != u"triggers" && name != u"profiles" && name != u"screen"))
                return fail(key + u" holds timelines, triggers, profiles or screen"_s);
            if (!pages.contains(name))
                pages.append(name);
        }
        demoCardsDismissed = pages;
    } else {
        return Result::NotMine;
    }
    return Result::Ok;
}

void AutomationSettings::write(QJsonObject &o) const
{
    o.insert(u"automationEnabled"_s, enabled);
    o.insert(u"fireByTimelineName"_s, fireByTimelineName);
    o.insert(u"triggerCooldownMs"_s, triggerCooldownMs);
    o.insert(u"screenTriggersEnabled"_s, screenTriggersEnabled);
    o.insert(u"screenTicksPerSecond"_s, screenTicksPerSecond);
    o.insert(u"screenRemember"_s, screenRemember);
    o.insert(u"screenWatchAtLogin"_s, screenWatchAtLogin);
    o.insert(u"screenIgnorePets"_s, screenIgnorePets);
    o.insert(u"demoCardsDismissed"_s, QJsonArray::fromStringList(demoCardsDismissed));
}

} // namespace murasaki
