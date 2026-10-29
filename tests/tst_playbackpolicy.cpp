#include "playbackpolicy.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPlaybackPolicy : public QObject
{
    Q_OBJECT

private slots:
    void pausesCoveredOutputs();
    void respectsMute();
    void coveredRuleCanBeDisabled();
    void oneAudioOwnerPerItem();
    void ownershipMovesWhenOwnerPauses();
    void emptyOutputsAreSilent();
    void spectrumGoesToRunningListeningWebPagesOnly();
    void captureNeededIffSomeOutputWantsIt();
};

void TestPlaybackPolicy::pausesCoveredOutputs()
{
    const auto d = decidePlayback({{u"DP-1"_s, u"we:1"_s, false, true}});
    QCOMPARE(d.value(u"DP-1"_s).paused, true);
    QCOMPARE(d.value(u"DP-1"_s).audible, false);
}

void TestPlaybackPolicy::respectsMute()
{
    const auto d = decidePlayback({{u"DP-1"_s, u"we:1"_s, true, false}});
    QCOMPARE(d.value(u"DP-1"_s).paused, false);
    QCOMPARE(d.value(u"DP-1"_s).audible, false);
}

void TestPlaybackPolicy::coveredRuleCanBeDisabled()
{
    const auto d = decidePlayback({{u"DP-1"_s, u"we:1"_s, false, true}, {u"DP-2"_s, u"we:2"_s, true, true}}, false);
    QCOMPARE(d.value(u"DP-1"_s).paused, false);
    QCOMPARE(d.value(u"DP-2"_s).paused, false);
    // Covered outputs keep their audio rules: unmuted ones still sound (once per item), muted ones stay silent.
    QCOMPARE(d.value(u"DP-1"_s).audible, true);
    QCOMPARE(d.value(u"DP-2"_s).audible, false);
    QCOMPARE(decidePlayback({{u"DP-1"_s, u"we:1"_s, false, true}}, true).value(u"DP-1"_s).paused, true);
}

void TestPlaybackPolicy::oneAudioOwnerPerItem()
{
    const auto d = decidePlayback({
        {u"HDMI-A-1"_s, u"we:1"_s, false, false},
        {u"DP-1"_s, u"we:1"_s, false, false},
        {u"DP-2"_s, u"we:2"_s, false, false},
    });
    QCOMPARE(d.value(u"DP-1"_s).audible, true);       // first connector name owns we:1
    QCOMPARE(d.value(u"HDMI-A-1"_s).audible, false);
    QCOMPARE(d.value(u"DP-2"_s).audible, true);       // a different item has its own owner
}

void TestPlaybackPolicy::ownershipMovesWhenOwnerPauses()
{
    const auto d = decidePlayback({
        {u"DP-1"_s, u"we:1"_s, false, true},
        {u"HDMI-A-1"_s, u"we:1"_s, false, false},
    });
    QCOMPARE(d.value(u"DP-1"_s).audible, false);
    QCOMPARE(d.value(u"HDMI-A-1"_s).audible, true);
}

void TestPlaybackPolicy::emptyOutputsAreSilent()
{
    const auto d = decidePlayback({{u"DP-1"_s, QString(), false, false}});
    QCOMPARE(d.value(u"DP-1"_s).audible, false);
    QCOMPARE(d.value(u"DP-1"_s).paused, false);
}

void TestPlaybackPolicy::spectrumGoesToRunningListeningWebPagesOnly()
{
    QVERIFY(outputWantsSpectrum(u"web"_s, false, true));
    QVERIFY(!outputWantsSpectrum(u"web"_s, true, true));   // covered or paused
    QVERIFY(!outputWantsSpectrum(u"web"_s, false, false)); // no listener
    QVERIFY(!outputWantsSpectrum(u"video"_s, false, true));
    QVERIFY(!outputWantsSpectrum(u"preview"_s, false, true));
    QVERIFY(!outputWantsSpectrum(QString(), false, true));
}

void TestPlaybackPolicy::captureNeededIffSomeOutputWantsIt()
{
    QVERIFY(!anyWantsSpectrum({}));
    QVERIFY(!anyWantsSpectrum({{u"web"_s, true, true}, {u"video"_s, false, true}, {u"web"_s, false, false}}));
    QVERIFY(anyWantsSpectrum({{u"web"_s, true, true}, {u"web"_s, false, true}}));
}

QTEST_GUILESS_MAIN(TestPlaybackPolicy)
#include "tst_playbackpolicy.moc"
