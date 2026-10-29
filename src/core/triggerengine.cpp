#include "triggerengine.h"

#include "automation.h"
#include "constants.h"

#include <QJsonArray>
#include <QTime>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr qint64 kSecondMs = 1000;
constexpr qint64 kCauseWindowMs = kTriggerChainWindowMs; // a fact trigger that fires this soon after another fire is taken as a consequence of it (a loop of period under 2 s is a chain)
constexpr int kMaxSustainedPerMinute = 30; // and a fact trigger never fires more than this in a minute, whatever the period

bool isExternal(TriggerKind kind)
{
    switch (kind) {
    case TriggerKind::AppFocus:
    case TriggerKind::AppOpen:
    case TriggerKind::Fullscreen:
    case TriggerKind::Power:
    case TriggerKind::Session:
    case TriggerKind::Game:
    case TriggerKind::Output:
        return false; // the facts can be changed by what a pet's action does (a profile switch opens a window of a pet's own)
    default:
        return true;
    }
}

// Whether the facts needed by the trigger's source exist on this session.
bool availableFor(const TriggerSource &s, const AutomationFacts &f)
{
    switch (s.kind) {
    case TriggerKind::AppFocus:
    case TriggerKind::AppOpen:
    case TriggerKind::Fullscreen:
    case TriggerKind::Power:
    case TriggerKind::Game:
        return f.known;
    case TriggerKind::Idle:
        return f.idleKnown;
    case TriggerKind::Output:
        return f.outputsKnown;
    case TriggerKind::Screen:
        return false;
    default:
        return true;
    }
}

// The condition an edge source watches; only meaningful when availableFor() is true.
bool conditionOf(const TriggerSource &s, const AutomationFacts &f)
{
    switch (s.kind) {
    case TriggerKind::AppFocus:
        return matchesApplication(s.app, f.activeApp);
    case TriggerKind::AppOpen:
        return std::any_of(f.openApps.begin(), f.openApps.end(), [&](const QString &a) { return matchesApplication(s.app, a); });
    case TriggerKind::Fullscreen:
        for (auto it = f.fullscreen.constBegin(); it != f.fullscreen.constEnd(); ++it)
            if ((s.output.isEmpty() || it.key() == s.output) && (s.app.isEmpty() || matchesApplication(s.app, it.value())))
                return true;
        return false;
    case TriggerKind::Power:
        return s.state == u"battery" ? f.onBattery : !f.onBattery;
    case TriggerKind::Session:
        return s.state == u"locked" ? f.locked : !f.locked;
    case TriggerKind::Game:
        return s.state == u"started" ? f.gameRunning : !f.gameRunning;
    default:
        return false;
    }
}

bool isEdgeKind(TriggerKind kind)
{
    return !isExternal(kind) && kind != TriggerKind::Output;
}

} // namespace

void AutomationFacts::setLogindIdle(bool known, bool value)
{
    logindIdleKnown = known;
    logindIdle = known && value;
    resolveIdle();
}

void AutomationFacts::setSurfaceIdle(bool known, bool value, qint64 afterMs)
{
    surfaceIdleKnown = known;
    surfaceIdle = known && value;
    surfaceIdleAfterMs = known ? std::clamp<qint64>(afterMs, 0, kMaxIdleAfterMs) : 0;
    surfaceIdleSeen = surfaceIdleSeen || known;
    resolveIdle();
}

void AutomationFacts::resolveIdle()
{
    idleFromSurface = surfaceIdleSeen;
    idleKnown = surfaceIdleSeen ? surfaceIdleKnown : logindIdleKnown;
    idle = surfaceIdleSeen ? surfaceIdle : logindIdle;
    idleAfterMs = surfaceIdleSeen ? surfaceIdleAfterMs : 0;
}

AutomationFacts factsFromSessionJson(const QJsonObject &json, const AutomationFacts &base)
{
    AutomationFacts f = base;
    f.known = json.value(u"known"_s).toBool(false);
    f.activeApp = json.value(u"activeApp"_s).toString();
    f.openApps.clear();
    for (const QJsonValue &v : json.value(u"openApps"_s).toArray())
        if (v.isString() && f.openApps.size() < 64)
            f.openApps.append(v.toString());
    f.fullscreen.clear();
    const QJsonObject outputs = json.value(u"outputs"_s).toObject();
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it)
        if (it->toObject().value(u"fullscreen"_s).toBool())
            f.fullscreen.insert(it.key(), it->toObject().value(u"app"_s).toString());
    f.onBattery = json.value(u"onBattery"_s).toBool(false);
    f.gameRunning = json.value(u"gameRunning"_s).toBool(false);
    f.setSurfaceIdle(json.value(u"idleKnown"_s).toBool(false), json.value(u"idle"_s).toBool(false), qint64(json.value(u"idleAfterMs"_s).toDouble(0)));
    return f;
}

QString fireOutcomeName(FireOutcome outcome)
{
    switch (outcome) {
    case FireOutcome::Ran:
        return u"ran"_s;
    case FireOutcome::Cooldown:
        return u"cooldown"_s;
    case FireOutcome::RateLimited:
        return u"rate limited"_s;
    case FireOutcome::ChainTooDeep:
        return u"chain too deep"_s;
    case FireOutcome::Disabled:
        return u"disabled"_s;
    case FireOutcome::SourceUnavailable:
        return u"source unavailable"_s;
    case FireOutcome::ShortcutUnbound:
        return u"shortcut unbound"_s;
    }
    return {};
}

void TriggerEngine::releaseHeld(Entry &e, qint64 nowMs)
{
    if (!e.held)
        return;
    e.held = false;
    if (const auto inverse = inverseAction(e.trigger.action)) {
        Fired fired;
        fired.triggerId = e.trigger.id;
        fired.action = *inverse;
        fired.release = true;
        fired.atMs = nowMs;
        m_fires.append(fired);
    }
}

void TriggerEngine::setAutomationEnabled(bool enabled)
{
    if (m_enabled && !enabled)
        for (const QString &id : std::as_const(m_order))
            releaseHeld(m_entries[id], m_lastMs);
    m_enabled = enabled;
}

void TriggerEngine::setTriggers(const QList<Trigger> &triggers, qint64 nowMs, const QDateTime &nowLocal)
{
    m_lastMs = nowMs;
    // What a hold was holding is undone when its trigger goes away, is disabled, or changes what it watches or does: the new entry would not know to release it.
    for (const QString &id : std::as_const(m_order)) {
        Entry &old = m_entries[id];
        if (!old.held)
            continue;
        const auto now = std::find_if(triggers.cbegin(), triggers.cend(), [&](const Trigger &t) { return t.id == id; });
        if (now == triggers.cend() || !now->enabled || now->source != old.trigger.source || now->action != old.trigger.action)
            releaseHeld(old, nowMs);
    }
    QHash<QString, Entry> next;
    QStringList order;
    for (const Trigger &t : triggers) {
        Entry e;
        if (const auto old = m_entries.constFind(t.id); old != m_entries.constEnd() && old->trigger.source == t.source) {
            e = *old; // the same source: its edges and its cooldown stay
        } else {
            e.timeCursor = nowLocal;
            e.intervalFromMs = nowMs;
            if (m_haveFacts && isEdgeKind(t.source.kind) && availableFor(t.source, m_facts)) {
                e.initialized = true;
                e.last = conditionOf(t.source, m_facts);
            }
            if (t.source.kind == TriggerKind::Output) {
                e.initialized = m_haveFacts && m_facts.outputsKnown;
                e.lastOutputs = QSet<QString>(m_facts.outputs.begin(), m_facts.outputs.end());
            }
            if (t.source.kind == TriggerKind::Idle && m_facts.idleKnown) {
                e.idleSource = m_facts.idleFromSurface ? 1 : 0;
                if (m_facts.idle) {
                    e.idleSinceMs = nowMs - m_facts.idleAfterMs;
                    e.idleFired = true; // already idle when it was made: not "became idle"
                }
            }
        }
        e.trigger = t;
        next.insert(t.id, e);
        order.append(t.id);
    }
    m_entries = next;
    m_order = order;
}

EngineNeeds TriggerEngine::needs() const
{
    EngineNeeds n;
    if (!m_enabled)
        return n;
    for (const QString &id : m_order) {
        const Entry &e = m_entries[id];
        if (!e.trigger.enabled)
            continue;
        const TriggerSource &s = e.trigger.source;
        switch (s.kind) {
        case TriggerKind::AppFocus:
        case TriggerKind::AppOpen:
        case TriggerKind::Fullscreen:
            n.windows = true;
            break;
        case TriggerKind::Power:
            n.power = true;
            break;
        case TriggerKind::Game:
            n.game = true;
            break;
        case TriggerKind::Session:
            n.session = true;
            break;
        case TriggerKind::Idle:
            n.idle = true;
            n.timer = s.state != u"back"; // back waits for the end of the idle, not for a moment
            break;
        case TriggerKind::Output:
            n.outputs = true;
            break;
        case TriggerKind::Time:
        case TriggerKind::Interval:
            n.timer = true;
            break;
        case TriggerKind::SignalFile:
            n.signalFiles = true;
            break;
        case TriggerKind::Shortcut:
            if (!n.shortcutIds.contains(s.id))
                n.shortcutIds.append(s.id);
            break;
        case TriggerKind::DBus: {
            const EngineNeeds::DBusRule rule{s.interface, s.member, s.senderNamespace};
            if (!n.dbusRules.contains(rule))
                n.dbusRules.append(rule);
            break;
        }
        case TriggerKind::Screen:
            n.screen = true;
            break;
        case TriggerKind::Fire:
            break;
        }
    }
    return n;
}

void TriggerEngine::record(Entry &e, FireOutcome outcome, qint64 nowMs)
{
    e.outcome = outcome;
    e.hasOutcome = true;
    m_log.append({e.trigger.id, nowMs, outcome});
    while (m_log.size() > kMaxTriggerLog)
        m_log.removeFirst();
}

bool TriggerEngine::tryFire(Entry &e, qint64 nowMs, bool external, bool release)
{
    if (!release) {
        if (e.lastFireMs >= 0 && nowMs - e.lastFireMs < e.trigger.cooldownMs && nowMs >= e.lastFireMs) {
            record(e, FireOutcome::Cooldown, nowMs);
            return false;
        }
        while (!m_recentAll.isEmpty() && nowMs - m_recentAll.first() >= kSecondMs)
            m_recentAll.removeFirst();
        while (!e.recent.isEmpty() && nowMs - e.recent.first() >= kSecondMs)
            e.recent.removeFirst();
        if (m_recentAll.size() >= kMaxFiresPerSecond || e.recent.size() >= kMaxFiresPerSecondPerTrigger) {
            e.rateLimited = true;
            record(e, FireOutcome::RateLimited, nowMs);
            return false;
        }
        while (!m_hops.isEmpty() && nowMs - m_hops.first().atMs > kTriggerChainWindowMs)
            m_hops.removeFirst();
        while (!e.minute.isEmpty() && nowMs - e.minute.first() >= 60'000)
            e.minute.removeFirst();
        if (!external && e.minute.size() >= kMaxSustainedPerMinute) {
            record(e, FireOutcome::ChainTooDeep, nowMs);
            return false;
        }
        int depth = 1;
        if (!external)
            for (const Hop &h : std::as_const(m_hops))
                if (nowMs - h.atMs <= kCauseWindowMs)
                    depth = std::max(depth, h.depth + 1);
        if (depth > kMaxTriggerChain) {
            record(e, FireOutcome::ChainTooDeep, nowMs);
            return false;
        }
        m_hops.append({nowMs, depth});
        m_recentAll.append(nowMs);
        e.recent.append(nowMs);
        if (!external)
            e.minute.append(nowMs);
        e.lastFireMs = nowMs;
    }
    Fired fired;
    fired.triggerId = e.trigger.id;
    fired.action = e.trigger.action;
    fired.release = release;
    fired.atMs = nowMs;
    if (release) {
        const auto inverse = inverseAction(e.trigger.action);
        if (!inverse)
            return false;
        fired.action = *inverse;
    }
    m_fires.append(fired);
    record(e, FireOutcome::Ran, nowMs);
    return true;
}

void TriggerEngine::setFacts(const AutomationFacts &facts, qint64 nowMs)
{
    m_facts = facts;
    m_haveFacts = true;
    for (const QString &id : m_order) {
        Entry &e = m_entries[id];
        const TriggerSource &s = e.trigger.source;
        if (s.kind == TriggerKind::Idle) {
            if (!facts.idleKnown)
                continue;
            const int source = facts.idleFromSurface ? 1 : 0;
            if (source != e.idleSource) {
                // the first answer, or the surface taking over from logind: a starting point, no edge (an idle already there is not "became idle")
                e.idleSource = source;
                if (!facts.idle) {
                    e.idleSinceMs = -1;
                    e.idleFired = false;
                } else if (e.idleSinceMs < 0) {
                    e.idleSinceMs = nowMs - facts.idleAfterMs;
                    e.idleFired = true;
                } // (idle for both: the period goes on)
                continue;
            }
            if (facts.idle && e.idleSinceMs < 0) {
                e.idleSinceMs = nowMs - facts.idleAfterMs; // (the facts arrive that long after the last input)
                e.idleFired = false;
            } else if (!facts.idle) {
                // back: the idle that ends here lasted long enough, and a nap could have fired for it
                if (s.state == u"back" && e.idleSinceMs >= 0 && !e.idleFired && nowMs - e.idleSinceMs >= qint64(s.minutes) * 60'000) {
                    if (eligible(e))
                        tryFire(e, nowMs, true);
                    else
                        record(e, FireOutcome::Disabled, nowMs);
                }
                e.idleSinceMs = -1;
                e.idleFired = false;
            }
            continue;
        }
        if (s.kind == TriggerKind::Output) {
            if (!facts.outputsKnown) {
                e.initialized = false;
                continue;
            }
            const QSet<QString> now(facts.outputs.begin(), facts.outputs.end());
            if (e.initialized && eligible(e)) {
                const bool added = s.state == u"added";
                const QSet<QString> changed = added ? QSet<QString>(now).subtract(e.lastOutputs) : QSet<QString>(e.lastOutputs).subtract(now);
                for (const QString &name : changed) {
                    if (!s.name.isEmpty() && name != s.name)
                        continue;
                    tryFire(e, nowMs, false);
                }
            }
            e.lastOutputs = now;
            e.initialized = true;
            continue;
        }
        if (!isEdgeKind(s.kind))
            continue;
        if (!availableFor(s, facts)) {
            e.initialized = false; // the facts went away: when they come back they only set the starting point
            continue;
        }
        const bool value = conditionOf(s, facts);
        if (!e.initialized) {
            e.initialized = true;
            e.last = value;
            continue;
        }
        const bool fires = isEdgeKind(s.kind) && (s.kind == TriggerKind::AppFocus || s.kind == TriggerKind::AppOpen || s.kind == TriggerKind::Fullscreen) && s.onLeave
                               ? (e.last && !value)
                               : (!e.last && value);
        e.last = value;
        if (!fires)
            continue;
        if (!eligible(e)) {
            record(e, FireOutcome::Disabled, nowMs);
            continue;
        }
        tryFire(e, nowMs, false);
    }
}

QDateTime TriggerEngine::nextTimeTarget(const Entry &e, const QDateTime &after) const
{
    const TriggerSource &s = e.trigger.source;
    const QTime at = QTime::fromString(s.at, u"HH:mm"_s);
    if (!at.isValid())
        return {};
    for (int d = 0; d <= 8; ++d) {
        const QDate date = after.date().addDays(d);
        const QDateTime target(date, at);
        if (!(target > after))
            continue;
        const int day = date.dayOfWeek() % 7; // 0 Sunday .. 6 Saturday
        if (s.days.isEmpty() || s.days.contains(day))
            return target;
    }
    return {};
}

void TriggerEngine::tick(qint64 nowMs, const QDateTime &nowLocal)
{
    for (const QString &id : m_order) {
        Entry &e = m_entries[id];
        const TriggerSource &s = e.trigger.source;
        if (s.kind == TriggerKind::Time) {
            if (e.timeCursor > nowLocal)
                e.timeCursor = nowLocal; // the clock was set back
            for (int step = 0; step < 8; ++step) {
                const QDateTime target = nextTimeTarget(e, e.timeCursor);
                if (!target.isValid() || target > nowLocal)
                    break;
                e.timeCursor = target;
                if (target.msecsTo(nowLocal) > kMissedTimeToleranceMs)
                    continue; // due while the machine slept: skipped
                if (eligible(e))
                    tryFire(e, nowMs, true);
                else
                    record(e, FireOutcome::Disabled, nowMs);
            }
        } else if (s.kind == TriggerKind::Interval) {
            const qint64 due = e.intervalFromMs + qint64(s.minutes) * 60'000;
            if (nowMs < e.intervalFromMs)
                e.intervalFromMs = nowMs;
            else if (nowMs >= due) {
                const bool late = nowMs - due > kMissedTimeToleranceMs;
                e.intervalFromMs = nowMs;
                if (!late) {
                    if (eligible(e))
                        tryFire(e, nowMs, true);
                    else
                        record(e, FireOutcome::Disabled, nowMs);
                }
            }
        } else if (s.kind == TriggerKind::Idle && s.state != u"back") {
            if (e.idleSinceMs >= 0 && !e.idleFired && nowMs - e.idleSinceMs >= qint64(s.minutes) * 60'000) {
                e.idleFired = true;
                if (eligible(e))
                    tryFire(e, nowMs, true);
                else
                    record(e, FireOutcome::Disabled, nowMs);
            }
        }
    }
}

qint64 TriggerEngine::nextWakeMs(qint64 nowMs, const QDateTime &nowLocal) const
{
    qint64 next = -1;
    const auto offer = [&](qint64 ms) {
        ms = std::max<qint64>(0, ms);
        if (next < 0 || ms < next)
            next = ms;
    };
    for (const QString &id : m_order) {
        const Entry &e = m_entries[id];
        if (!eligible(e))
            continue;
        const TriggerSource &s = e.trigger.source;
        if (s.kind == TriggerKind::Time) {
            const QDateTime target = nextTimeTarget(e, e.timeCursor > nowLocal ? nowLocal : e.timeCursor);
            if (target.isValid())
                offer(nowLocal.msecsTo(target));
        } else if (s.kind == TriggerKind::Interval) {
            offer(e.intervalFromMs + qint64(s.minutes) * 60'000 - nowMs);
        } else if (s.kind == TriggerKind::Idle && s.state != u"back" && e.idleSinceMs >= 0 && !e.idleFired) {
            offer(e.idleSinceMs + qint64(s.minutes) * 60'000 - nowMs);
        }
    }
    return next;
}

int TriggerEngine::fire(const QString &name, qint64 nowMs)
{
    int count = 0;
    for (const QString &id : m_order) {
        Entry &e = m_entries[id];
        if (e.trigger.source.kind != TriggerKind::Fire || e.trigger.source.name != name)
            continue;
        ++count;
        if (eligible(e))
            tryFire(e, nowMs, true);
        else
            record(e, FireOutcome::Disabled, nowMs);
    }
    return count;
}

int TriggerEngine::signalFile(const QString &name, qint64 nowMs)
{
    int count = 0;
    for (const QString &id : m_order) {
        Entry &e = m_entries[id];
        if (e.trigger.source.kind != TriggerKind::SignalFile || e.trigger.source.name != name)
            continue;
        ++count;
        if (eligible(e))
            tryFire(e, nowMs, true);
        else
            record(e, FireOutcome::Disabled, nowMs);
    }
    return count;
}

int TriggerEngine::dbusSignal(const QString &interface, const QString &member, const QStringList &senderNames, qint64 nowMs)
{
    int count = 0;
    for (const QString &id : m_order) {
        Entry &e = m_entries[id];
        const TriggerSource &s = e.trigger.source;
        if (s.kind != TriggerKind::DBus || s.interface != interface || s.member != member)
            continue;
        if (!s.senderNamespace.isEmpty()) {
            const bool inside = std::any_of(senderNames.begin(), senderNames.end(), [&](const QString &n) { return n == s.senderNamespace || n.startsWith(s.senderNamespace + u'.'); });
            if (!inside)
                continue;
        }
        ++count;
        if (eligible(e))
            tryFire(e, nowMs, true);
        else
            record(e, FireOutcome::Disabled, nowMs);
    }
    return count;
}

int TriggerEngine::shortcut(const QString &id, bool pressed, qint64 nowMs, bool bound)
{
    int count = 0;
    for (const QString &key : m_order) {
        Entry &e = m_entries[key];
        if (e.trigger.source.kind != TriggerKind::Shortcut || e.trigger.source.id != id)
            continue;
        ++count;
        if (!eligible(e)) {
            if (pressed)
                record(e, FireOutcome::Disabled, nowMs);
            continue;
        }
        if (pressed) {
            if (!bound)
                record(e, FireOutcome::ShortcutUnbound, nowMs);
            else
                e.held = tryFire(e, nowMs, true) && e.trigger.hold; // held: the release undoes it
        } else if (e.trigger.hold && e.held) {
            e.held = false;
            tryFire(e, nowMs, true, true);
        }
    }
    return count;
}

bool TriggerEngine::screenActive(const QString &triggerId, bool active, qint64 nowMs)
{
    const auto it = m_entries.find(triggerId);
    if (it == m_entries.end() || it->trigger.source.kind != TriggerKind::Screen)
        return false;
    Entry &e = *it;
    m_lastMs = nowMs;
    if (!active) {
        // The end undoes what the activation did, whatever has become of the trigger since (disabled, the switch off): the undo is not a new fire.
        if (e.held) {
            e.held = false;
            tryFire(e, nowMs, true, true);
        }
        return true;
    }
    if (!eligible(e)) {
        record(e, FireOutcome::Disabled, nowMs);
        return true;
    }
    // Not external: a screen picture that comes back because of what the last action did is a loop like any other, so the chain depth and the sustained cap apply (the plan, 4.7).
    e.held = tryFire(e, nowMs, false);
    return true;
}

bool TriggerEngine::test(const QString &triggerId, qint64 nowMs)
{
    const auto it = m_entries.find(triggerId);
    if (it == m_entries.end())
        return false;
    if (!eligible(*it)) {
        record(*it, FireOutcome::Disabled, nowMs);
        return false;
    }
    return tryFire(*it, nowMs, true);
}

QList<Fired> TriggerEngine::takeFires()
{
    return std::exchange(m_fires, {});
}

TriggerState TriggerEngine::state(const QString &triggerId) const
{
    TriggerState s;
    const auto it = m_entries.constFind(triggerId);
    if (it == m_entries.constEnd())
        return s;
    s.enabled = eligible(*it);
    s.available = availableFor(it->trigger.source, m_facts) || !m_haveFacts;
    s.rateLimited = it->rateLimited;
    s.lastFireMs = it->lastFireMs;
    s.lastOutcome = it->outcome;
    s.hasOutcome = it->hasOutcome;
    return s;
}

QStringList TriggerEngine::enabledIds() const
{
    QStringList ids;
    for (const QString &id : m_order)
        if (eligible(m_entries[id]))
            ids.append(id);
    return ids;
}

} // namespace murasaki
