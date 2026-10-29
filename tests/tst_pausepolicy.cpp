#include "pausepolicy.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPausePolicy : public QObject
{
    Q_OBJECT

private slots:
    void nothingHappensWithoutFacts();
    void aRunningGamePausesByDefault();
    void gamePauseCanBeSwitchedOff();
    void aGamePausesOnlyTheCoveredOutputsWhenTheyAreKnown();
    void batteryModes();
    void aFocusedOrFullscreenAppPauses();
    void appRulesMatchWholeClassesCaseInsensitively();
    void theFirstRuleThatAppliesNamesTheReason();
    void lowFpsOnlyCapsWhenNothingPauses();
    void stringConversions();
};

void TestPausePolicy::nothingHappensWithoutFacts()
{
    PauseRules rules;
    rules.battery = BatteryMode::Pause;
    rules.apps = {u"firefox"_s};
    const PauseVerdict verdict = evaluatePauseRules(rules, {});
    QVERIFY(!verdict.pauseAll);
    QCOMPARE(verdict.reason, PauseReason::None);
    QCOMPARE(verdict.fpsCap, 0);
    QVERIFY(!evaluatePauseRules({}, {}).pauseAll);
}

void TestPausePolicy::aRunningGamePausesByDefault()
{
    PauseFacts facts;
    facts.gameRunning = true;
    const PauseVerdict verdict = evaluatePauseRules({}, facts);
    QVERIFY(verdict.pauseAll);
    QCOMPARE(verdict.reason, PauseReason::Game);
}

void TestPausePolicy::aGamePausesOnlyTheCoveredOutputsWhenTheyAreKnown()
{
    PauseFacts facts;
    facts.gameRunning = true;
    QVERIFY(!evaluatePauseRules({}, facts).coveredOnly); // nobody says where windows are: everything pauses
    facts.windowsKnown = true;
    const PauseVerdict verdict = evaluatePauseRules({}, facts);
    QVERIFY(verdict.pauseAll);
    QCOMPARE(verdict.reason, PauseReason::Game);
    QVERIFY(verdict.coveredOnly);
    // the battery still stops everything
    facts.gameRunning = false;
    facts.onBattery = true;
    PauseRules rules;
    rules.battery = BatteryMode::Pause;
    QVERIFY(!evaluatePauseRules(rules, facts).coveredOnly);
}

void TestPausePolicy::gamePauseCanBeSwitchedOff()
{
    PauseRules rules;
    rules.pauseOnGame = false;
    PauseFacts facts;
    facts.gameRunning = true;
    QVERIFY(!evaluatePauseRules(rules, facts).pauseAll);
}

void TestPausePolicy::batteryModes()
{
    PauseFacts facts;
    facts.onBattery = true;
    PauseRules rules;
    QVERIFY(!evaluatePauseRules(rules, facts).pauseAll); // off by default
    QCOMPARE(evaluatePauseRules(rules, facts).fpsCap, 0);

    rules.battery = BatteryMode::Pause;
    const PauseVerdict paused = evaluatePauseRules(rules, facts);
    QVERIFY(paused.pauseAll);
    QCOMPARE(paused.reason, PauseReason::Battery);

    rules.battery = BatteryMode::LowFps;
    const PauseVerdict capped = evaluatePauseRules(rules, facts);
    QVERIFY(!capped.pauseAll);
    QCOMPARE(capped.fpsCap, 15);

    facts.onBattery = false; // plugged in: nothing
    QVERIFY(!evaluatePauseRules(rules, facts).pauseAll);
    QCOMPARE(evaluatePauseRules(rules, facts).fpsCap, 0);
}

void TestPausePolicy::aFocusedOrFullscreenAppPauses()
{
    PauseRules rules;
    rules.apps = {u"obs"_s, u"steam_app_*"_s};
    PauseFacts facts;
    facts.activeApp = u"obs"_s;
    PauseVerdict verdict = evaluatePauseRules(rules, facts);
    QVERIFY(verdict.pauseAll);
    QCOMPARE(verdict.reason, PauseReason::App);
    QCOMPARE(verdict.detail, u"obs"_s);

    facts.activeApp = u"konsole"_s;
    QVERIFY(!evaluatePauseRules(rules, facts).pauseAll);
    facts.fullscreenApps = {u"mpv"_s, u"steam_app_620"_s}; // one of them is fullscreen somewhere
    verdict = evaluatePauseRules(rules, facts);
    QVERIFY(verdict.pauseAll);
    QCOMPARE(verdict.detail, u"steam_app_*"_s);
}

void TestPausePolicy::appRulesMatchWholeClassesCaseInsensitively()
{
    QVERIFY(appMatches(u"firefox"_s, u"Firefox"_s));
    QVERIFY(appMatches(u"FIREFOX"_s, u"firefox"_s));
    QVERIFY(!appMatches(u"fire"_s, u"firefox"_s)); // not a prefix match
    QVERIFY(!appMatches(u"fox"_s, u"firefox"_s));
    QVERIFY(appMatches(u"steam_app_*"_s, u"steam_app_1245620"_s));
    QVERIFY(!appMatches(u"steam_app_*"_s, u"steam"_s));
    QVERIFY(appMatches(u"*.exe"_s, u"game.exe"_s));
    QVERIFY(appMatches(u"vlc?"_s, u"vlc2"_s));
    QVERIFY(!appMatches(u"vlc?"_s, u"vlc"_s));
    // regex characters in a rule are plain characters
    QVERIFY(appMatches(u"a.b"_s, u"a.b"_s));
    QVERIFY(!appMatches(u"a.b"_s, u"axb"_s));
    QVERIFY(appMatches(u"(x)+"_s, u"(x)+"_s));
    QVERIFY(appMatches(u"vlc"_s, u"vlc"_s)); // the cached pattern answers the same twice
    QVERIFY(appMatches(u"dolphin"_s, u"org.kde.dolphin"_s)); // a bare name against a reverse-DNS app_id
    QVERIFY(appMatches(u"org.kde.dolphin"_s, u"Dolphin"_s));
    QVERIFY(appMatches(u"firefox.desktop"_s, u"firefox"_s));
    QVERIFY(!appMatches(u"org.kde.dolphin"_s, u"org.gnome.dolphin"_s));
    QVERIFY(!appMatches(QString(), u"x"_s));
    QVERIFY(!appMatches(u"x"_s, QString())); // no focused window matches nothing, not even "*"
    QVERIFY(!appMatches(u"*"_s, QString()));
}

void TestPausePolicy::theFirstRuleThatAppliesNamesTheReason()
{
    PauseRules rules;
    rules.battery = BatteryMode::Pause;
    rules.apps = {u"obs"_s};
    PauseFacts facts;
    facts.gameRunning = true;
    facts.onBattery = true;
    facts.activeApp = u"obs"_s;
    QCOMPARE(evaluatePauseRules(rules, facts).reason, PauseReason::Game);
    facts.gameRunning = false;
    QCOMPARE(evaluatePauseRules(rules, facts).reason, PauseReason::Battery);
    facts.onBattery = false;
    QCOMPARE(evaluatePauseRules(rules, facts).reason, PauseReason::App);
}

void TestPausePolicy::lowFpsOnlyCapsWhenNothingPauses()
{
    PauseRules rules;
    rules.battery = BatteryMode::LowFps;
    PauseFacts facts;
    facts.onBattery = true;
    facts.gameRunning = true;
    const PauseVerdict verdict = evaluatePauseRules(rules, facts);
    QVERIFY(verdict.pauseAll);
    QCOMPARE(verdict.fpsCap, 0); // paused anyway
}

void TestPausePolicy::stringConversions()
{
    for (const BatteryMode mode : {BatteryMode::Off, BatteryMode::Pause, BatteryMode::LowFps})
        QCOMPARE(batteryModeFromString(batteryModeToString(mode)), mode);
    QCOMPARE(batteryModeFromString(u"bogus"_s), BatteryMode::Off);
    QCOMPARE(pauseReasonToString(PauseReason::None), QString());
    QCOMPARE(pauseReasonToString(PauseReason::Game), u"game"_s);
    QCOMPARE(pauseReasonToString(PauseReason::Battery), u"battery"_s);
    QCOMPARE(pauseReasonToString(PauseReason::App), u"app"_s);
}

QTEST_APPLESS_MAIN(TestPausePolicy)
#include "tst_pausepolicy.moc"
