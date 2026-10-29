// The trigger engine with scripted events and a fake clock: edges, cooldowns, rate limits, the loop guard, missed times, and what the engine says it needs.

#include "automation.h"
#include "triggerengine.h"

#include <QJsonArray>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

Trigger trigger(const QString &id, const TriggerSource &source, bool enabled = true)
{
    Trigger t;
    t.id = id;
    t.name = id;
    t.enabled = enabled;
    t.source = source;
    t.action.type = TriggerActionType::HideAll;
    return t;
}

TriggerSource app(TriggerKind kind, const QString &pattern, bool leave = false)
{
    TriggerSource s;
    s.kind = kind;
    s.app = pattern;
    s.onLeave = leave;
    return s;
}

TriggerSource named(TriggerKind kind, const QString &name)
{
    TriggerSource s;
    s.kind = kind;
    s.name = name;
    return s;
}

TriggerSource state(TriggerKind kind, const QString &state)
{
    TriggerSource s;
    s.kind = kind;
    s.state = state;
    return s;
}

AutomationFacts facts(const QString &active = {}, const QStringList &open = {})
{
    AutomationFacts f;
    f.known = true;
    f.activeApp = active;
    f.openApps = open;
    return f;
}

const QDateTime kNoon = QDateTime(QDate(2026, 10, 22), QTime(12, 0)); // a Thursday

QStringList ids(const QList<Fired> &fires)
{
    QStringList out;
    for (const Fired &f : fires)
        out.append(f.triggerId);
    return out;
}

}

class TstTriggerEngine : public QObject
{
    Q_OBJECT

private slots:
    void aFocusTriggerFiresOnTheEdgeNotWhileTrue()
    {
        TriggerEngine e;
        e.setTriggers({trigger(u"a"_s, app(TriggerKind::AppFocus, u"firefox"_s))}, 0, kNoon);
        e.setFacts(facts(u"firefox"_s), 0); // the first facts are only the starting point
        QVERIFY(e.takeFires().isEmpty());
        e.setFacts(facts(u"konsole"_s), 1000);
        e.setFacts(facts(u"firefox"_s), 2000);
        QCOMPARE(ids(e.takeFires()), QStringList{u"a"_s});
        e.setFacts(facts(u"Firefox"_s, {u"x"_s}), 3000); // still true: no repeat
        e.setFacts(facts(u"firefox"_s, {u"y"_s}), 4000);
        QVERIFY(e.takeFires().isEmpty());
    }
    void leaveFiresWhenItStops()
    {
        TriggerEngine e;
        e.setTriggers({trigger(u"a"_s, app(TriggerKind::AppFocus, u"fire*"_s, true))}, 0, kNoon);
        e.setFacts(facts(u"firefox"_s), 0);
        e.setFacts(facts(u"konsole"_s), 1000);
        QCOMPARE(ids(e.takeFires()), QStringList{u"a"_s});
        e.setFacts(facts(u"firefox"_s), 2000);
        QVERIFY(e.takeFires().isEmpty());
    }
    void openAndCloseFollowTheSetOfClasses()
    {
        TriggerEngine e;
        e.setTriggers({trigger(u"open"_s, app(TriggerKind::AppOpen, u"steam"_s)), trigger(u"close"_s, app(TriggerKind::AppOpen, u"steam"_s, true))}, 0, kNoon);
        e.setFacts(facts({}, {u"konsole"_s}), 0);
        e.setFacts(facts({}, {u"konsole"_s, u"steam"_s}), 1000);
        QCOMPARE(ids(e.takeFires()), QStringList{u"open"_s});
        e.setFacts(facts({}, {u"konsole"_s}), 2000);
        QCOMPARE(ids(e.takeFires()), QStringList{u"close"_s});
    }
    void fullscreenCanBeFilteredByApplicationAndOutput()
    {
        TriggerSource s = app(TriggerKind::Fullscreen, u"mpv"_s);
        s.output = u"DP-1"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"fs"_s, s), trigger(u"any"_s, app(TriggerKind::Fullscreen, QString()))}, 0, kNoon);
        e.setFacts(facts(), 0);
        AutomationFacts f = facts();
        f.fullscreen.insert(u"HDMI-A-1"_s, u"mpv"_s);
        e.setFacts(f, 1000);
        QCOMPARE(ids(e.takeFires()), QStringList{u"any"_s}); // the wrong output
        f.fullscreen.clear();
        f.fullscreen.insert(u"DP-1"_s, u"mpv"_s);
        e.setFacts(facts(), 2000);
        e.takeFires();
        e.setFacts(f, 3000);
        QCOMPARE(ids(e.takeFires()).size(), 2);
    }
    void powerSessionGameAndOutputsAreEdges()
    {
        TriggerEngine e;
        e.setTriggers({trigger(u"bat"_s, state(TriggerKind::Power, u"battery"_s)), trigger(u"ac"_s, state(TriggerKind::Power, u"ac"_s)), trigger(u"lock"_s, state(TriggerKind::Session, u"locked"_s)),
                       trigger(u"unlock"_s, state(TriggerKind::Session, u"unlocked"_s)), trigger(u"gs"_s, state(TriggerKind::Game, u"started"_s)), trigger(u"ge"_s, state(TriggerKind::Game, u"ended"_s)),
                       trigger(u"plug"_s, state(TriggerKind::Output, u"added"_s)), trigger(u"unplug"_s, state(TriggerKind::Output, u"removed"_s))},
                      0, kNoon);
        AutomationFacts f = facts();
        f.outputsKnown = true;
        f.outputs = {u"DP-1"_s};
        e.setFacts(f, 0);
        QVERIFY(e.takeFires().isEmpty());
        f.onBattery = true;
        f.locked = true;
        f.gameRunning = true;
        f.outputs = {u"DP-1"_s, u"HDMI-A-1"_s};
        e.setFacts(f, 1000);
        QStringList fired = ids(e.takeFires());
        fired.sort();
        QCOMPARE(fired, (QStringList{u"bat"_s, u"gs"_s, u"lock"_s, u"plug"_s}));
        f.onBattery = false;
        f.locked = false;
        f.gameRunning = false;
        f.outputs = {u"DP-1"_s};
        e.setFacts(f, 5000);
        fired = ids(e.takeFires());
        fired.sort();
        QCOMPARE(fired, (QStringList{u"ac"_s, u"ge"_s, u"unlock"_s, u"unplug"_s}));
    }
    void anUnknownSessionFiresNothingAndSaysSo()
    {
        TriggerEngine e;
        e.setTriggers({trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s)), trigger(u"l"_s, state(TriggerKind::Session, u"locked"_s))}, 0, kNoon);
        AutomationFacts none; // no monitor
        e.setFacts(none, 0);
        QVERIFY(!e.state(u"a"_s).available);
        QVERIFY(e.state(u"l"_s).available); // the lock comes from the session manager
        e.setFacts(facts(u"x"_s), 1000);   // the monitor appears with the app already focused: the starting point, not a fire
        QVERIFY(e.takeFires().isEmpty());
        QVERIFY(e.state(u"a"_s).available);
    }
    void aTimeTriggerFiresOnceOnItsDayAndSkipsWhatWasMissed()
    {
        TriggerSource s;
        s.kind = TriggerKind::Time;
        s.at = u"22:00"_s;
        s.days = {4}; // Thursday
        TriggerEngine e;
        e.setTriggers({trigger(u"t"_s, s)}, 0, kNoon);
        QCOMPARE(e.nextWakeMs(0, kNoon), qint64(10 * 3600 * 1000));
        e.tick(1000, kNoon.addSecs(3600));
        QVERIFY(e.takeFires().isEmpty());
        e.tick(2000, QDateTime(kNoon.date(), QTime(22, 0, 1)));
        QCOMPARE(ids(e.takeFires()), QStringList{u"t"_s});
        e.tick(3000, QDateTime(kNoon.date(), QTime(22, 0, 30))); // handled: not again
        QVERIFY(e.takeFires().isEmpty());
        // not Friday
        e.tick(4000, QDateTime(kNoon.date().addDays(1), QTime(22, 0, 1)));
        QVERIFY(e.takeFires().isEmpty());
        // next Thursday, but the machine woke 3 hours late: skipped
        e.tick(5000, QDateTime(kNoon.date().addDays(7), QTime(23, 30)));
        QVERIFY(e.takeFires().isEmpty());
        // and a time that was missed by under 2 minutes fires
        TriggerEngine late;
        late.setTriggers({trigger(u"t"_s, s)}, 0, kNoon);
        late.tick(1000, QDateTime(kNoon.date(), QTime(22, 1, 30)));
        QCOMPARE(ids(late.takeFires()), QStringList{u"t"_s});
    }
    void everyDayWhenNoDaysAreGivenAndTheClockSetBackIsHandled()
    {
        TriggerSource s;
        s.kind = TriggerKind::Time;
        s.at = u"09:00"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"t"_s, s)}, 0, kNoon);
        e.tick(1000, QDateTime(kNoon.date().addDays(1), QTime(9, 0, 5)));
        QCOMPARE(e.takeFires().size(), 1);
        e.tick(2000, kNoon); // the clock is set back a day
        e.tick(3000, QDateTime(kNoon.date().addDays(1), QTime(9, 0, 5)));
        QCOMPARE(e.takeFires().size(), 1);
    }
    void anIntervalFiresEveryNMinutesAndSkipsALongSleep()
    {
        TriggerSource s;
        s.kind = TriggerKind::Interval;
        s.minutes = 5;
        TriggerEngine e;
        e.setTriggers({trigger(u"i"_s, s)}, 0, kNoon);
        QCOMPARE(e.nextWakeMs(0, kNoon), qint64(300'000));
        e.tick(299'999, kNoon);
        QVERIFY(e.takeFires().isEmpty());
        e.tick(300'000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
        e.tick(600'000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
        e.tick(600'000 + 300'000 + 10 * 60'000, kNoon); // slept 10 minutes past: skipped
        QVERIFY(e.takeFires().isEmpty());
        QCOMPARE(e.nextWakeMs(600'000 + 300'000 + 10 * 60'000, kNoon), qint64(300'000)); // and counted again from there
    }
    void idleFiresOnceWhenTheIdleHasLastedLongEnough()
    {
        TriggerSource s;
        s.kind = TriggerKind::Idle;
        s.minutes = 2;
        TriggerEngine e;
        e.setTriggers({trigger(u"idle"_s, s)}, 0, kNoon);
        AutomationFacts f = facts();
        f.idleKnown = true;
        e.setFacts(f, 0);
        f.idle = true;
        e.setFacts(f, 10'000);
        QCOMPARE(e.nextWakeMs(10'000, kNoon), qint64(120'000));
        e.tick(129'999, kNoon);
        QVERIFY(e.takeFires().isEmpty());
        e.tick(130'000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
        e.tick(500'000, kNoon);
        QVERIFY(e.takeFires().isEmpty()); // once per idle period
        f.idle = false;
        e.setFacts(f, 600'000);
        f.idle = true;
        e.setFacts(f, 700'000);
        e.tick(700'000 + 120'000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
    }
    void idleBackFiresOnceWhenALongEnoughIdleEnds()
    {
        TriggerSource s;
        s.kind = TriggerKind::Idle;
        s.minutes = 2;
        s.state = u"back"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"back"_s, s)}, 0, kNoon);
        AutomationFacts f = facts();
        f.idleKnown = true;
        e.setFacts(f, 0);
        QCOMPARE(e.nextWakeMs(0, kNoon), qint64(-1)); // no timer: it waits for the end of an idle period
        f.idle = true;
        e.setFacts(f, 10'000);
        e.tick(500'000, kNoon);
        QVERIFY(e.takeFires().isEmpty()); // nothing while the idle lasts
        QCOMPARE(e.nextWakeMs(500'000, kNoon), qint64(-1));
        f.idle = false;
        e.setFacts(f, 600'000);
        QCOMPARE(e.takeFires().size(), 1);
        e.setFacts(f, 610'000);
        QVERIFY(e.takeFires().isEmpty()); // once per period
        // a short one (1 minute 59 s) does not count, one of exactly 2 minutes does
        f.idle = true;
        e.setFacts(f, 700'000);
        f.idle = false;
        e.setFacts(f, 700'000 + 119'999);
        QVERIFY(e.takeFires().isEmpty());
        f.idle = true;
        e.setFacts(f, 900'000);
        f.idle = false;
        e.setFacts(f, 900'000 + 120'000);
        QCOMPARE(e.takeFires().size(), 1);
    }
    void idleBackPairsWithTheNapsThatFired()
    {
        TriggerSource away;
        away.kind = TriggerKind::Idle;
        away.minutes = 2;
        TriggerSource back = away;
        back.state = u"back"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"away"_s, away), trigger(u"back"_s, back)}, 0, kNoon);
        AutomationFacts f = facts();
        f.idleKnown = true;
        e.setFacts(f, 0);
        f.idle = true;
        e.setFacts(f, 1'000);
        e.tick(1'000 + 120'000, kNoon);
        const QList<Fired> napped = e.takeFires();
        QCOMPARE(napped.size(), 1);
        QCOMPARE(napped.at(0).triggerId, u"away"_s);
        f.idle = false;
        e.setFacts(f, 1'000 + 130'000);
        const QList<Fired> woke = e.takeFires();
        QCOMPARE(woke.size(), 1);
        QCOMPARE(woke.at(0).triggerId, u"back"_s);
    }
    void idleBackIgnoresAnIdleThatWasThereBeforeTheEngine()
    {
        TriggerSource s;
        s.kind = TriggerKind::Idle;
        s.minutes = 1;
        s.state = u"back"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"back"_s, s)}, 0, kNoon);
        AutomationFacts f = facts();
        f.idleKnown = true;
        f.idle = true;
        e.setFacts(f, 0); // idle from the start: no nap fired, so there is nothing to undo
        f.idle = false;
        e.setFacts(f, 600'000);
        QVERIFY(e.takeFires().isEmpty());
        f.idle = true; // a later period counts
        e.setFacts(f, 700'000);
        f.idle = false;
        e.setFacts(f, 800'000);
        QCOMPARE(e.takeFires().size(), 1);
    }
    void idleReportedByTheSurfaceCountsFromTheLastInput()
    {
        // the surface says "idle" `idleAfterMs` after the last input: the period began then, not now
        TriggerSource away;
        away.kind = TriggerKind::Idle;
        away.minutes = 2;
        TriggerSource back = away;
        back.state = u"back"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"away"_s, away), trigger(u"back"_s, back)}, 0, kNoon);
        AutomationFacts f = facts();
        f.idleKnown = true;
        e.setFacts(f, 0);
        f.idle = true;
        f.idleAfterMs = 60'000;
        e.setFacts(f, 1'000'000);
        QCOMPARE(e.nextWakeMs(1'000'000, kNoon), qint64(60'000)); // 2 minutes after the last input, 1 minute after the notification
        e.tick(1'059'999, kNoon);
        QVERIFY(e.takeFires().isEmpty());
        e.tick(1'060'000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
        f.idle = false;
        e.setFacts(f, 1'070'000);
        QCOMPARE(e.takeFires().size(), 1);
        // 1 minute idle on the surface's clock is 2 minutes at the user's: the back of a 2 minute nap that never fired because it ended at once
        f.idle = true;
        e.setFacts(f, 2'000'000);
        f.idle = false;
        e.setFacts(f, 2'000'000 + 59'999); // 119.999 s since the last input: too short
        QVERIFY(e.takeFires().isEmpty());
    }
    void fireSignalFileAndDBusNameTheirTriggers()
    {
        TriggerSource d;
        d.kind = TriggerKind::DBus;
        d.interface = u"org.example.I"_s;
        d.member = u"Changed"_s;
        d.senderNamespace = u"org.example"_s;
        TriggerEngine e;
        e.setTriggers({trigger(u"f"_s, named(TriggerKind::Fire, u"sleepy"_s)), trigger(u"s"_s, named(TriggerKind::SignalFile, u"sleepy"_s)), trigger(u"d"_s, d), trigger(u"off"_s, named(TriggerKind::Fire, u"sleepy"_s), false)}, 0, kNoon);
        QCOMPARE(e.fire(u"sleepy"_s, 0), 2); // two triggers have the name; one of them is disabled
        QCOMPARE(ids(e.takeFires()), QStringList{u"f"_s});
        QCOMPARE(e.state(u"off"_s).lastOutcome, FireOutcome::Disabled);
        QCOMPARE(e.fire(u"nobody"_s, 0), 0);
        QCOMPARE(e.signalFile(u"sleepy"_s, 1000), 1);
        QCOMPARE(ids(e.takeFires()), QStringList{u"s"_s});
        QCOMPARE(e.dbusSignal(u"org.example.I"_s, u"Changed"_s, {u"org.example.App"_s}, 2000), 1);
        QCOMPARE(ids(e.takeFires()), QStringList{u"d"_s});
        QCOMPARE(e.dbusSignal(u"org.example.I"_s, u"Changed"_s, {u"org.evil.App"_s}, 3000), 0); // another sender
        QCOMPARE(e.dbusSignal(u"org.example.I"_s, u"Other"_s, {u"org.example.App"_s}, 4000), 0);
        QCOMPARE(e.dbusSignal(u"org.example.I"_s, u"Changed"_s, {}, 5000), 0); // a sender whose names are unknown never matches a namespace
        QVERIFY(e.takeFires().isEmpty());
    }
    void aScreenTriggerIsAHoldTheMatcherDrives()
    {
        TriggerSource s;
        s.kind = TriggerKind::Screen;
        TriggerEngine e;
        // nothing needs the screen until an enabled screen trigger exists
        e.setTriggers({trigger(u"s"_s, s, false)}, 0, kNoon);
        QVERIFY(!e.needs().screen);
        QVERIFY(!e.needs().any());
        e.setTriggers({trigger(u"s"_s, s)}, 0, kNoon);
        QVERIFY(e.needs().screen);
        QVERIFY(e.needs().any());
        // the activation runs the action, the end runs its inverse (hide-all <-> show-all)
        QVERIFY(e.screenActive(u"s"_s, true, 1000));
        QList<Fired> fired = e.takeFires();
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].action.type, TriggerActionType::HideAll);
        QVERIFY(!fired[0].release);
        QVERIFY(e.screenActive(u"s"_s, false, 3000));
        fired = e.takeFires();
        QCOMPARE(fired.size(), 1);
        QVERIFY(fired[0].release);
        QCOMPARE(fired[0].action.type, TriggerActionType::ShowAll);
        // an end without an activation does nothing; neither does a second one
        QVERIFY(e.screenActive(u"s"_s, false, 4000));
        QVERIFY(e.takeFires().isEmpty());
        // not a screen trigger, or no such trigger
        e.setTriggers({trigger(u"f"_s, named(TriggerKind::Fire, u"x"_s))}, 0, kNoon);
        QVERIFY(!e.screenActive(u"f"_s, true, 5000));
        QVERIFY(!e.screenActive(u"nope"_s, true, 5000));
        QVERIFY(e.takeFires().isEmpty());
    }
    void whatAScreenTriggerHoldsIsUndoneWhateverBecomesOfIt()
    {
        TriggerSource s;
        s.kind = TriggerKind::Screen;
        const auto held = [&](TriggerEngine &e) {
            e.setTriggers({trigger(u"s"_s, s)}, 0, kNoon);
            e.screenActive(u"s"_s, true, 1000);
            QCOMPARE(e.takeFires().size(), 1);
        };
        const auto released = [](TriggerEngine &e) {
            const QList<Fired> fired = e.takeFires();
            QCOMPARE(fired.size(), 1);
            QVERIFY(fired[0].release);
            QCOMPARE(fired[0].action.type, TriggerActionType::ShowAll);
        };
        { // disabled while held
            TriggerEngine e;
            held(e);
            e.setTriggers({trigger(u"s"_s, s, false)}, 2000, kNoon);
            released(e);
            e.screenActive(u"s"_s, false, 3000); // the matcher's own end comes after: nothing twice
            QVERIFY(e.takeFires().isEmpty());
        }
        { // removed while held
            TriggerEngine e;
            held(e);
            e.setTriggers({}, 2000, kNoon);
            released(e);
        }
        { // the source edited while held
            TriggerEngine e;
            held(e);
            TriggerSource edited = s;
            edited.screen = QJsonObject{{u"regions"_s, QJsonArray{QJsonObject{{u"x"_s, 5}}}}};
            e.setTriggers({trigger(u"s"_s, edited)}, 2000, kNoon);
            released(e);
        }
        { // the action edited while held: the old action is undone
            TriggerEngine e;
            held(e);
            Trigger changed = trigger(u"s"_s, s);
            changed.action.type = TriggerActionType::Mute;
            changed.action.mute = u"on"_s;
            e.setTriggers({changed}, 2000, kNoon);
            released(e);
        }
        { // the same trigger kept as it is stays held
            TriggerEngine e;
            held(e);
            e.setTriggers({trigger(u"s"_s, s)}, 2000, kNoon);
            QVERIFY(e.takeFires().isEmpty());
            e.screenActive(u"s"_s, false, 3000);
            released(e);
        }
        { // the master switch (and the rescue's pause) goes off while held; the end that follows is not a second undo
            TriggerEngine e;
            held(e);
            e.setAutomationEnabled(false);
            released(e);
            e.screenActive(u"s"_s, false, 3000);
            QVERIFY(e.takeFires().isEmpty());
        }
        { // the end of a hold whose trigger was disabled is still undone, if nothing else did it
            TriggerEngine e;
            held(e);
            e.setAutomationEnabled(false);
            e.takeFires();
            e.setAutomationEnabled(true);
            e.screenActive(u"s"_s, true, 4000); // a stale activation after the switch is back is a new one
            QCOMPARE(e.takeFires().size(), 1);
        }
    }
    void aScreenTriggerThatFlapsIsCutByTheLoopGuard()
    {
        TriggerSource s;
        s.kind = TriggerKind::Screen;
        Trigger t = trigger(u"s"_s, s);
        t.cooldownMs = 100;
        TriggerEngine e;
        e.setTriggers({t}, 0, kNoon);
        int ran = 0;
        for (int i = 0; i < 60; ++i) { // the picture comes back every 1.5 s because of what the action did
            e.screenActive(u"s"_s, true, 1000 + i * 1500);
            ran += e.takeFires().size();
            e.screenActive(u"s"_s, false, 1500 + i * 1500);
            e.takeFires();
        }
        QVERIFY2(ran < 60, qPrintable(QString::number(ran)));
        QVERIFY(ran >= 25); // a minute at that period fires 30 at most; the guard cut the rest
        bool cut = false;
        for (const TriggerLogEntry &entry : e.log())
            cut = cut || entry.outcome == FireOutcome::ChainTooDeep;
        QVERIFY(cut);
    }
    void aScreenTriggerObeysTheSwitchesTheCooldownAndTheLimits()
    {
        TriggerSource s;
        s.kind = TriggerKind::Screen;
        Trigger t = trigger(u"s"_s, s);
        t.cooldownMs = 1000;
        TriggerEngine e;
        e.setTriggers({t}, 0, kNoon);
        e.setAutomationEnabled(false); // the master switch (and the rescue's pause)
        QVERIFY(e.screenActive(u"s"_s, true, 1000));
        QVERIFY(e.takeFires().isEmpty());
        QCOMPARE(e.state(u"s"_s).lastOutcome, FireOutcome::Disabled);
        e.setAutomationEnabled(true);
        e.screenActive(u"s"_s, true, 2000);
        QCOMPARE(e.takeFires().size(), 1);
        e.screenActive(u"s"_s, false, 2100);
        QCOMPARE(e.takeFires().size(), 1);
        e.screenActive(u"s"_s, true, 2500); // inside the trigger's own cooldown
        QVERIFY(e.takeFires().isEmpty());
        QCOMPARE(e.state(u"s"_s).lastOutcome, FireOutcome::Cooldown);
        e.screenActive(u"s"_s, false, 2600); // (it never ran, so there is nothing to release)
        QVERIFY(e.takeFires().isEmpty());
        e.screenActive(u"s"_s, true, 4000);
        QCOMPARE(e.takeFires().size(), 1);
        // a disabled trigger is told no: nothing runs (what it held is undone once, when it is disabled)
        Trigger off = t;
        off.enabled = false;
        e.setTriggers({off}, 5000, kNoon);
        QCOMPARE(e.takeFires().size(), 1);
        e.screenActive(u"s"_s, true, 6000);
        QVERIFY(e.takeFires().isEmpty());
    }
    void aHoldShortcutRunsItsInverseOnRelease()
    {
        TriggerSource s;
        s.kind = TriggerKind::Shortcut;
        s.id = u"peek"_s;
        Trigger t = trigger(u"k"_s, s);
        t.hold = true;
        Trigger press = trigger(u"p"_s, s);
        press.action.type = TriggerActionType::ShowAll;
        TriggerEngine e;
        e.setTriggers({t, press}, 0, kNoon);
        QCOMPARE(e.shortcut(u"peek"_s, true, 0), 2);
        QList<Fired> fired = e.takeFires();
        QCOMPARE(fired.size(), 2);
        QVERIFY(!fired[0].release);
        e.shortcut(u"peek"_s, false, 1000);
        fired = e.takeFires();
        QCOMPARE(fired.size(), 1); // only the hold one has a release
        QVERIFY(fired[0].release);
        QCOMPARE(fired[0].action.type, TriggerActionType::ShowAll); // the inverse of hide-all
        e.shortcut(u"peek"_s, false, 2000); // a second release does nothing
        QVERIFY(e.takeFires().isEmpty());
        // a key the desktop holds nothing for
        e.shortcut(u"peek"_s, true, 5000, false);
        QVERIFY(e.takeFires().isEmpty());
        QCOMPARE(e.state(u"k"_s).lastOutcome, FireOutcome::ShortcutUnbound);
    }
    void theCooldownBoundaryIsExact()
    {
        Trigger t = trigger(u"f"_s, named(TriggerKind::Fire, u"x"_s));
        t.cooldownMs = 500;
        TriggerEngine e;
        e.setTriggers({t}, 0, kNoon);
        e.fire(u"x"_s, 1000);
        e.fire(u"x"_s, 1499);
        QCOMPARE(e.state(u"f"_s).lastOutcome, FireOutcome::Cooldown);
        e.fire(u"x"_s, 1500);
        QCOMPARE(e.takeFires().size(), 2);
        QCOMPARE(e.state(u"f"_s).lastOutcome, FireOutcome::Ran);
    }
    void aStormIsHeldByTheRateLimits()
    {
        QList<Trigger> triggers;
        for (int i = 0; i < 5; ++i) {
            Trigger t = trigger(u"t%1"_s.arg(i), named(TriggerKind::Fire, u"storm"_s));
            t.cooldownMs = kMinTriggerCooldownMs;
            triggers.append(t);
        }
        TriggerEngine e;
        e.setTriggers(triggers, 0, kNoon);
        int fired = 0;
        for (int i = 0; i < 10000; ++i) { // ten thousand fires in one second
            e.fire(u"storm"_s, i / 10);
            fired += e.takeFires().size();
        }
        QVERIFY2(fired <= kMaxFiresPerSecond * 2, qPrintable(QString::number(fired)));
        QVERIFY(fired >= kMaxFiresPerSecond);
        QVERIFY(e.state(u"t0"_s).rateLimited || e.state(u"t4"_s).rateLimited);
        QVERIFY(e.log().size() <= kMaxTriggerLog);
        // a second later it works again
        e.fire(u"storm"_s, 20'000);
        QVERIFY(!e.takeFires().isEmpty());
    }
    void aLoopOfTriggersThatCauseEachOthersEventsIsCut()
    {
        // A fires when "x" is focused and its action (in the world of the test) focuses "y"; B fires on "y" and focuses "x" again, 150 ms later each time.
        Trigger a = trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s));
        Trigger b = trigger(u"b"_s, app(TriggerKind::AppFocus, u"y"_s));
        a.cooldownMs = b.cooldownMs = kMinTriggerCooldownMs;
        TriggerEngine e;
        e.setTriggers({a, b}, 0, kNoon);
        e.setFacts(facts(u"z"_s), 0);
        int total = 0;
        qint64 now = 1000;
        e.setFacts(facts(u"x"_s), now);
        for (int hop = 0; hop < 12; ++hop) {
            const QList<Fired> fired = e.takeFires();
            total += fired.size();
            if (fired.isEmpty())
                break;
            now += 150;
            e.setFacts(facts(fired[0].triggerId == u"a" ? u"y"_s : u"x"_s), now);
        }
        QCOMPARE(total, kMaxTriggerChain);
        const auto state = e.state(u"a"_s).lastOutcome == FireOutcome::ChainTooDeep || e.state(u"b"_s).lastOutcome == FireOutcome::ChainTooDeep;
        QVERIFY(state);
        // after the window the chain starts over
        e.setFacts(facts(u"z"_s), now + 5000);
        e.setFacts(facts(u"x"_s), now + 5010);
        QCOMPARE(e.takeFires().size(), 1);
    }
    void aSlowFeedbackLoopIsCutToo()
    {
        // A and B cause each other's events with a period of 1.2 s each way: the old half-second window never saw it as a chain.
        Trigger a = trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s));
        Trigger b = trigger(u"b"_s, app(TriggerKind::AppFocus, u"y"_s));
        a.cooldownMs = b.cooldownMs = kMinTriggerCooldownMs;
        TriggerEngine e;
        e.setTriggers({a, b}, 0, kNoon);
        e.setFacts(facts(u"z"_s), 0);
        qint64 now = 1000;
        e.setFacts(facts(u"x"_s), now);
        int total = 0;
        for (int hop = 0; hop < 20; ++hop) {
            const QList<Fired> fired = e.takeFires();
            total += fired.size();
            if (fired.isEmpty())
                break;
            now += 1200;
            e.setFacts(facts(fired[0].triggerId == u"a" ? u"y"_s : u"x"_s), now);
        }
        QCOMPARE(total, kMaxTriggerChain);
    }
    void aFactTriggerIsCappedPerMinuteWhateverItsPeriod()
    {
        // fires every 2.1 s (outside the chain window) 28 times a minute: allowed; the cap is for what the window can not see
        Trigger a = trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s));
        a.cooldownMs = kMinTriggerCooldownMs;
        TriggerEngine e;
        e.setTriggers({a}, 0, kNoon);
        e.setFacts(facts(u"z"_s), 0);
        qint64 now = 0;
        int total = 0;
        for (int i = 0; i < 40; ++i) {
            now += 2100;
            e.setFacts(facts(u"x"_s), now);
            total += e.takeFires().size();
            now += 100;
            e.setFacts(facts(u"z"_s), now);
        }
        QCOMPARE(total, 40); // 40 in 88 s is under 30 in any minute
        // a burst that is not a chain (spread over more than the window each) but passes the cap
        TriggerEngine f;
        f.setTriggers({a}, 0, kNoon);
        f.setFacts(facts(u"z"_s), 0);
        now = 0;
        total = 0;
        for (int i = 0; i < 100; ++i) {
            now += 1900; // each fire is inside the window of the one before: chain, cut
            f.setFacts(facts(u"x"_s), now);
            total += f.takeFires().size();
            now += 50;
            f.setFacts(facts(u"z"_s), now);
        }
        QVERIFY(total < 100);
    }
    void nothingIsNeededWithoutAnEnabledTrigger()
    {
        TriggerEngine e;
        QVERIFY(!e.needs().any());
        e.setTriggers({trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s), false)}, 0, kNoon);
        QVERIFY(!e.needs().any());
        QCOMPARE(e.nextWakeMs(0, kNoon), qint64(-1));
        TriggerSource shortcut;
        shortcut.kind = TriggerKind::Shortcut;
        shortcut.id = u"k"_s;
        TriggerSource time;
        time.kind = TriggerKind::Time;
        time.at = u"10:00"_s;
        TriggerSource dbus;
        dbus.kind = TriggerKind::DBus;
        dbus.interface = u"a.B"_s;
        dbus.member = u"C"_s;
        e.setTriggers({trigger(u"a"_s, app(TriggerKind::AppOpen, u"x"_s)), trigger(u"s"_s, shortcut), trigger(u"t"_s, time), trigger(u"f"_s, named(TriggerKind::SignalFile, u"n"_s)), trigger(u"d"_s, dbus),
                       trigger(u"p"_s, state(TriggerKind::Power, u"ac"_s))},
                      0, kNoon);
        const EngineNeeds n = e.needs();
        QVERIFY(n.windows && n.timer && n.signalFiles && n.power);
        QVERIFY(!n.game && !n.idle && !n.outputs && !n.session);
        QCOMPARE(n.shortcutIds, QStringList{u"k"_s});
        QCOMPARE(n.dbusRules.size(), 1);
        e.setAutomationEnabled(false); // the master switch
        QVERIFY(!e.needs().any());
        QVERIFY(e.enabledIds().isEmpty());
        e.setFacts(facts(u"x"_s, {}), 0);
        e.setFacts(facts(u"x"_s, {u"x"_s}), 10'000);
        QVERIFY(e.takeFires().isEmpty());
    }
    void theLogKeepsTheLastFiftyAndTheStateTheLastOutcome()
    {
        Trigger t = trigger(u"f"_s, named(TriggerKind::Fire, u"x"_s));
        t.cooldownMs = kMinTriggerCooldownMs;
        TriggerEngine e;
        e.setTriggers({t}, 0, kNoon);
        for (int i = 0; i < 80; ++i)
            e.fire(u"x"_s, i * 200);
        QCOMPARE(e.log().size(), kMaxTriggerLog);
        QCOMPARE(e.state(u"f"_s).lastFireMs, qint64(79 * 200));
        QVERIFY(e.state(u"f"_s).hasOutcome);
    }
    void changingTheRulesKeepsTheEdgesOfWhatStays()
    {
        Trigger a = trigger(u"a"_s, app(TriggerKind::AppFocus, u"x"_s));
        TriggerEngine e;
        e.setTriggers({a}, 0, kNoon);
        e.setFacts(facts(u"x"_s), 0);
        a.name = u"renamed"_s;
        e.setTriggers({a, trigger(u"b"_s, app(TriggerKind::AppFocus, u"x"_s))}, 10, kNoon); // b is new and starts from the facts as they are
        e.setFacts(facts(u"x"_s, {u"q"_s}), 20);
        QVERIFY(e.takeFires().isEmpty());
        e.setFacts(facts(u"y"_s), 30);
        e.setFacts(facts(u"x"_s), 40);
        QCOMPARE(e.takeFires().size(), 2);
    }
    void factsComeFromTheSurfaceJson()
    {
        const QJsonObject json = QJsonDocument::fromJson(R"({"known":true,"activeApp":"firefox","openApps":["firefox","konsole"],"outputs":{"DP-1":{"fullscreen":true,"app":"mpv","occluded":true},
            "HDMI-A-1":{"fullscreen":false,"app":"","occluded":false}},"anyFullscreen":true,"gameRunning":true,"onBattery":true})").object();
        AutomationFacts base;
        base.locked = true;
        base.outputs = {u"DP-1"_s};
        const AutomationFacts f = factsFromSessionJson(json, base);
        QVERIFY(f.known && f.locked && f.onBattery && f.gameRunning);
        QCOMPARE(f.activeApp, u"firefox"_s);
        QCOMPARE(f.openApps.size(), 2);
        QCOMPARE(f.fullscreen.size(), 1);
        QCOMPARE(f.fullscreen.value(u"DP-1"_s), u"mpv"_s);
        QCOMPARE(f.outputs, base.outputs);
        QVERIFY(!factsFromSessionJson(QJsonObject{{u"outputs"_s, QJsonObject()}}, base).known); // a missing "known" is unknown
    }
    void theSurfaceIdleWinsAndLogindIsTheFallback()
    {
        const QJsonObject without = QJsonDocument::fromJson(R"({"known":true,"outputs":{}})").object();
        const QJsonObject idle = QJsonDocument::fromJson(R"({"known":true,"outputs":{},"idleKnown":true,"idle":true,"idleAfterMs":60000})").object();
        const QJsonObject awake = QJsonDocument::fromJson(R"({"known":true,"outputs":{},"idleKnown":true,"idle":false,"idleAfterMs":60000})").object();
        // logind knows, the surface does not: logind's answer stays
        AutomationFacts base;
        base.setLogindIdle(true, true);
        AutomationFacts f = factsFromSessionJson(without, base);
        QVERIFY(f.idleKnown && f.idle);
        QCOMPARE(f.idleAfterMs, qint64(0));
        // the surface knows: it wins over logind, either way, and says how long ago the input was
        f = factsFromSessionJson(awake, base);
        QVERIFY(f.idleKnown && !f.idle);
        f = factsFromSessionJson(idle, AutomationFacts());
        QVERIFY(f.idleKnown && f.idle);
        QCOMPARE(f.idleAfterMs, qint64(60'000));
        // logind changes while the surface knows: ignored
        f.setLogindIdle(true, false);
        QVERIFY(f.idleKnown && f.idle && f.idleAfterMs == 60'000);
        // the surface stops knowing (it restarted): unknown until it answers again, not logind's answer (on Plasma it would end every nap)
        f = factsFromSessionJson(without, f);
        QVERIFY(!f.idleKnown);
        f = factsFromSessionJson(idle, f);
        QVERIFY(f.idleKnown && f.idle && f.idleFromSurface);
        // neither knows
        QVERIFY(!factsFromSessionJson(without, AutomationFacts()).idleKnown);
        // a surface that is not a monitor (known: false) can still know the idle
        const QJsonObject onlyIdle = QJsonDocument::fromJson(R"({"known":false,"outputs":{},"idleKnown":true,"idle":true,"idleAfterMs":60000})").object();
        QVERIFY(factsFromSessionJson(onlyIdle, AutomationFacts()).idleKnown);
        // the bounds of the offset
        const QJsonObject wild = QJsonDocument::fromJson(R"({"idleKnown":true,"idle":true,"idleAfterMs":99999999999})").object();
        QVERIFY(factsFromSessionJson(wild, AutomationFacts()).idleAfterMs <= 3'600'000);
    }
};

QTEST_GUILESS_MAIN(TstTriggerEngine)
#include "tst_triggerengine.moc"
