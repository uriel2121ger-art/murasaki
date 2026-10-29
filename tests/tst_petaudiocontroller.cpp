// When the pets make their sounds (src/overlay/petaudiocontroller.h): every mode, the offset both ways, hot edits, pause and hide, the global mute and
// volume, the player's failures. The player is a script: the lines the controller sends are read back.

#include "petaudiocontroller.h"

#include <QJsonArray>
#include <QSet>
#include <QTest>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

constexpr qint64 kMs = 1'000'000;

class FakeLink final : public PetAudioLink
{
public:
    bool start() override
    {
        ++starts;
        if (!canStart)
            return false;
        up = true;
        return true;
    }
    bool running() const override { return up; }
    void send(const QString &text) override { lines << text; }
    void stop() override { up = false; }
    void die(int code, bool crashed)
    {
        up = false;
        emit exited(code, crashed);
    }
    void say(const QString &text) { emit line(text); }

    QStringList lines;
    bool up = false;
    bool canStart = true;
    int starts = 0;
    // The lines that start with `word`.
    QStringList with(const QString &word) const
    {
        QStringList out;
        for (const QString &l : lines)
            if (l.startsWith(word + u' ') || l == word)
                out << l;
        return out;
    }
    void clear() { lines.clear(); }
};

QString id(char c) { return QString(64, QLatin1Char(c)); }

PetAudio sound(PetAudioMode mode, int volume = 75, int offset = 0, char asset = 'a')
{
    PetAudio a;
    a.asset = id(asset);
    a.mode = mode;
    a.volume = volume;
    a.offsetMs = offset;
    return a;
}

PetAudioController::Pet pet(const PetAudio &audio, bool clickThrough = false, bool shown = true, bool paused = false)
{
    PetAudioController::Pet p;
    p.audio = audio;
    p.clickThrough = clickThrough;
    p.shown = shown;
    p.paused = paused;
    p.output = u"DP-1"_s;
    p.frames = 10;
    return p;
}

} // namespace

class TstPetAudioController : public QObject
{
    Q_OBJECT

private:
    qint64 m_now = 5'000 * kMs;
    PetAudioController::Clock clock() { return [this] { return m_now; }; }

    // Fields of a play line: play PET GEN ASSET START GAIN LOOP AT
    static QStringList fields(const QString &line) { return line.split(u' '); }

private slots:
    void aPetWithoutASoundCostsNothing()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p1"_s, pet(PetAudio{}));
        c.setPet(u"p2"_s, pet(sound(PetAudioMode::None)));
        PetAudio noAsset;
        noAsset.mode = PetAudioMode::Once;
        c.setPet(u"p3"_s, pet(noAsset));
        c.onWrap(u"p1"_s, m_now, 0);
        c.onClick(u"p1"_s);
        QCOMPARE(link.starts, 0);
        QVERIFY(link.lines.isEmpty());
        QVERIFY(!c.playerRunning());
        QCOMPARE(c.status(u"p1"_s).value("state").toString(), QString("off"));
        QCOMPARE(c.status(u"nobody"_s), QJsonObject());
    }

    void onceStartsWhenThePetBecomesActiveAndAgainAfterBeingHidden()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once), false, false)); // not shown yet
        QCOMPARE(link.starts, 0);
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once)));
        QCOMPARE(link.starts, 1);
        const QStringList play = link.with("play");
        QCOMPARE(play.size(), 1);
        const QStringList f = fields(play[0]);
        QCOMPARE(f[3], id('a'));
        QCOMPARE(f[4], QString("0"));      // from the start
        QCOMPARE(f[5], QString("750"));    // 75 %
        QCOMPARE(f[6], QString("0"));      // not looped
        QCOMPARE(f[7].toLongLong(), 0LL);  // "now" has no time of its own: a player that took a while to come up still plays it
        QVERIFY(!link.with("master").isEmpty());
        // a plain edit (the position, the opacity) is not a new activation
        link.clear();
        auto edited = pet(sound(PetAudioMode::Once));
        edited.output = u"DP-1"_s;
        c.setPet(u"p"_s, edited);
        QVERIFY(link.with("play").isEmpty());
        // hidden, then shown: again
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once), false, false));
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once)));
        QCOMPARE(link.with("play").size(), 1);
        // paused, then resumed: again
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once), false, true, true));
        QVERIFY(link.with("play").isEmpty());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once)));
        QCOMPARE(link.with("play").size(), 1);
    }

    void continuousLoopsPausesWithThePetAndStopsWhenHidden()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous)));
        QCOMPARE(fields(link.with("play")[0])[6], QString("1"));
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous), false, true, true)); // paused
        QCOMPARE(link.with("pause").size(), 1);
        QVERIFY(link.with("stop").isEmpty());
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous)));
        QCOMPARE(link.with("resume").size(), 1); // where it was
        QVERIFY(link.with("play").isEmpty());
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous), false, false)); // hidden
        QCOMPARE(link.with("stop").size(), 1);
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous)));
        QCOMPARE(link.with("play").size(), 1); // from the start again
    }

    void animationLoopStartsOnTheWrapOnlyAndTheOffsetMovesIt()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, 120)));
        QVERIFY(link.with("play").isEmpty()); // waits for the wrap
        const qint64 wrap = m_now - 8 * kMs;  // the timer fired 8 ms after it
        c.onWrap(u"p"_s, wrap, 3);
        QStringList f = fields(link.with("play")[0]);
        QCOMPARE(f[4], QString("0"));
        QCOMPARE(f[7].toLongLong(), wrap + 120 * kMs); // delayed by the offset
        // a negative offset starts into the file at the wrap itself
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, -250)));
        QCOMPARE(link.with("stop").size(), 1); // the old sound goes with the old settings
        c.onWrap(u"p"_s, wrap, 4);
        f = fields(link.with("play").last());
        QCOMPARE(f[4], QString::number(250 * 48));
        QCOMPARE(f[7].toLongLong(), wrap);
        // a wrap that is long past is not played out of time
        link.clear();
        c.onWrap(u"p"_s, m_now - 400 * kMs, 5);
        QVERIFY(link.with("play").isEmpty());
        // a paused or hidden pet ignores the wraps
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, -250), false, true, true));
        c.onWrap(u"p"_s, m_now, 6);
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, -250), false, false));
        c.onWrap(u"p"_s, m_now, 7);
        QVERIFY(link.with("play").isEmpty());
        // other modes ignore wraps too
        c.setPet(u"q"_s, pet(sound(PetAudioMode::Click)));
        c.onWrap(u"q"_s, m_now, 1);
        QVERIFY(link.with("play").isEmpty());
    }

    void anAnnouncedWrapIsScheduledAheadAndCanBeCancelledOrRedoneAfterAnEdit()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, 100)));
        const qint64 wrap = m_now + 120 * kMs; // announced 120 ms ahead
        c.onWrap(u"p"_s, wrap, 1);
        QStringList f = fields(link.with("play").last());
        QCOMPARE(f[7].toLongLong(), wrap + 100 * kMs); // the start is the wrap plus the offset, sent now
        // the timing changed: the wrap is not where it was said to be
        link.clear();
        c.onWrapCancelled(u"p"_s);
        QCOMPARE(link.lines, QStringList{u"cancel 0"_s});
        // and the new time is announced
        link.clear();
        c.onWrap(u"p"_s, wrap + 30 * kMs, 1);
        QCOMPARE(fields(link.with("play").last())[7].toLongLong(), wrap + 130 * kMs);
        // once the wrap has come there is nothing left to cancel
        m_now = wrap + 50 * kMs;
        link.clear();
        c.onWrapCancelled(u"p"_s);
        QVERIFY(link.lines.isEmpty());
        // an edit of the settings kills the scheduled start with the old sound; the controller schedules the same wrap again
        m_now = 10'000 * kMs;
        const qint64 next = m_now + 80 * kMs;
        c.onWrap(u"p"_s, next, 2);
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, -50, 'b')));
        QVERIFY(link.with("stop").size() == 1);
        f = fields(link.with("play").last());
        QCOMPARE(f[3], id('b'));
        QCOMPARE(f[4], QString::number(50 * 48));
        QCOMPARE(f[7].toLongLong(), next);
        // pausing the pet kills it too
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, -50, 'b'), false, true, true));
        QCOMPARE(link.with("stop").size(), 1);
    }

    void clickPlaysOnlyOnAClickablePetThatIsShown()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click, 75, 40)));
        c.onClick(u"p"_s);
        QCOMPARE(fields(link.with("play")[0])[7].toLongLong(), m_now + 40 * kMs);
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click), true)); // click-through: no click can reach it, and the controller checks again
        c.onClick(u"p"_s);
        QVERIFY(link.with("play").isEmpty());
        QVERIFY(c.status(u"p"_s).value("degraded").toArray().contains(QString::fromUtf8(kAudioClickNeedsClickable.data())));
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click), false, true, true));
        c.onClick(u"p"_s);
        QVERIFY(link.with("play").isEmpty()); // paused
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click), false, false));
        c.onClick(u"p"_s);
        QVERIFY(link.with("play").isEmpty()); // hidden
        c.setPet(u"nobody"_s, pet(PetAudio{}));
        c.onClick(u"unknown"_s); // no crash
    }

    void editingTheSoundStopsTheOldOneAndBumpsTheGeneration()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'a')));
        const unsigned first = fields(link.with("play")[0])[2].toUInt();
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'b')));
        QCOMPARE(link.lines.size(), 2); // stop, then the new sound
        QVERIFY(link.lines[0].startsWith("stop"));
        QStringList f = fields(link.lines[1]);
        QCOMPARE(f[3], id('b'));
        QVERIFY(f[2].toUInt() > first);
        // an answer about the old generation is not believed
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'b')));
        link.say(QString("started 0 %1").arg(first));
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("idle"));
        link.say(QString("started 0 %1").arg(f[2]));
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("playing"));
        // only the volume changes: no restart, a gain
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 20, 0, 'b')));
        QCOMPARE(link.lines.size(), 1);
        QCOMPARE(link.lines[0], QString("gain 0 200"));
        link.say(QString("ended 0 %1").arg(f[2]));
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("idle"));
        // the mode back to none stops it
        link.clear();
        c.setPet(u"p"_s, pet(sound(PetAudioMode::None, 20, 0, 'b')));
        QVERIFY(link.with("play").isEmpty());
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("off"));
    }

    void theMasterAndTheFullscreenMute()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 80)));
        QCOMPARE(link.with("master").last(), QString("master 1000"));
        link.clear();
        c.setVolume(40);
        QCOMPARE(link.lines, QStringList{u"master 400"_s});
        link.clear();
        c.setMuted(true);
        QCOMPARE(link.lines, QStringList{u"master 0"_s});
        c.setMuted(false);
        link.clear();
        // a fullscreen window on the pet's output silences that pet only
        c.setPet(u"q"_s, [] { auto p = pet(sound(PetAudioMode::Continuous, 60, 0, 'c')); p.output = u"HDMI-1"_s; return p; }());
        link.clear();
        c.setFullscreenOutputs({u"DP-1"_s});
        QCOMPARE(link.lines, QStringList{u"gain 0 0"_s});
        c.setFullscreenOutputs({});
        QVERIFY(link.lines.contains(u"gain 0 800"_s));
        link.clear();
        c.setMuteWhenFullscreen(false);
        c.setFullscreenOutputs({u"DP-1"_s});
        QVERIFY(link.with("gain").isEmpty()); // the rule is off
    }

    void thePlayerThatCannotStartOrHasNoAudioIsReportedNotRetriedAtOnce()
    {
        FakeLink link;
        link.canStart = false;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once)));
        QCOMPARE(link.starts, 1);
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("unavailable"));
        QVERIFY(c.status(u"p"_s).value("degraded").toArray().contains(QString::fromUtf8(kAudioUnavailable.data())));
        c.onClick(u"p"_s); // not a click mode: nothing
        c.setPet(u"q"_s, pet(sound(PetAudioMode::Click)));
        c.onClick(u"q"_s);
        QCOMPARE(link.starts, 1); // not tried again within 30 s
        m_now += 31'000 * kMs;
        link.canStart = true;
        c.onClick(u"q"_s);
        QCOMPARE(link.starts, 2);
        QVERIFY(c.playerRunning());
        // the player that starts and says there is no audio system (exit code 3)
        FakeLink l2;
        PetAudioController d(&l2, clock());
        d.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous)));
        l2.die(3, false);
        QCOMPARE(d.status(u"p"_s).value("state").toString(), QString("unavailable"));
    }

    void aPlayerThatKeepsCrashingIsGivenUpOnUntilTold()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click)));
        for (int i = 0; i < 2; ++i) {
            c.onClick(u"p"_s);
            link.die(139, true);
        }
        c.onClick(u"p"_s);
        QCOMPARE(link.starts, 3);
        link.die(139, true);
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("failed"));
        c.onClick(u"p"_s);
        QCOMPARE(link.starts, 3); // given up
        c.resetFailures();
        c.onClick(u"p"_s);
        QCOMPARE(link.starts, 4);
        // a player that left on its own (idle, exit 0) is not a crash
        link.die(0, false);
        for (int i = 0; i < 5; ++i) {
            c.onClick(u"p"_s);
            link.die(0, false);
        }
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("idle"));
    }

    void aPlayerThatEndedWithoutBeingAskedTakesItsLongSoundsBackAtOnce()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous)));
        c.setPet(u"q"_s, pet(sound(PetAudioMode::Once, 75, 0, 'b'), false, true, true)); // paused: not re-armed
        link.clear();
        link.die(4, false); // PipeWire went away: the player left by itself
        QCOMPARE(link.with("play").size(), 1);
        QCOMPARE(fields(link.with("play")[0])[6], QString("1"));
        QCOMPARE(link.starts, 2);
        // a player that left idle (exit 0) is not started again by this
        link.clear();
        link.die(0, false);
        QVERIFY(link.with("play").isEmpty());
    }

    void aPlayerGivenUpOnIsTriedAgainAfterHalfAMinute()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Click)));
        for (int i = 0; i < 3; ++i) {
            c.onClick(u"p"_s);
            link.die(139, true);
        }
        QCOMPARE(c.status(u"p"_s).value("state").toString(), QString("failed"));
        c.onClick(u"p"_s);
        QCOMPARE(link.starts, 3);
        m_now += 31'000 * kMs;
        c.onClick(u"p"_s);
        QCOMPARE(link.starts, 4);
        QVERIFY(c.playerRunning());
    }

    void aRestartedPlayerGetsTheLongSoundsAgain()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'a')));
        c.setPet(u"q"_s, pet(sound(PetAudioMode::Click, 75, 0, 'b')));
        link.clear();
        link.die(139, true);
        c.onClick(u"q"_s); // starts the player again
        QCOMPARE(link.with("play").size(), 2); // the click, and p's continuous sound again
        bool loops = false;
        for (const QString &l : link.with("play"))
            loops = loops || fields(l)[6] == u"1"_s;
        QVERIFY(loops);
        QVERIFY(!link.with("master").isEmpty());
    }

    void thePlayersAnswersSayWhatFailed()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::Once)));
        const QString gen = fields(link.with("play")[0])[2];
        link.say(QString("failed 0 %1 audio asset missing").arg(gen));
        QVERIFY(c.status(u"p"_s).value("degraded").toArray().contains(QString::fromUtf8(kAudioAssetMissing.data())));
        // a still image never wraps
        auto still = pet(sound(PetAudioMode::AnimationLoop));
        still.frames = 1;
        c.setPet(u"s"_s, still);
        QVERIFY(c.status(u"s"_s).value("degraded").toArray().contains(QString::fromUtf8(kAudioNoLoop.data())));
    }

    void thePetsOwnSlotsAreSmallDistinctAndReused()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        for (int i = 0; i < 5; ++i)
            c.setPet(QString("p%1").arg(i), pet(sound(PetAudioMode::Continuous, 75, 0, char('a' + i))));
        QSet<QString> used;
        for (const QString &l : link.with("play"))
            used.insert(fields(l)[1]);
        QCOMPARE(used.size(), 5);
        c.removePet(u"p2"_s);
        QCOMPARE(link.with("stop").size(), 1);
        link.clear();
        c.setPet(u"fresh"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'z')));
        QCOMPARE(fields(link.with("play")[0])[1], QString("2")); // the freed slot, with a generation nobody used before
    }

    void silencingThePetsLeavesAContinuousSoundAlone()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"loop"_s, pet(sound(PetAudioMode::Continuous, 75, 0, 'a')));
        c.setPet(u"click"_s, pet(sound(PetAudioMode::Click, 75, 0, 'b'), false, true));
        QVERIFY(c.playNow(u"click"_s)); // a one-shot is sounding
        link.clear();
        c.stopOneShots();
        QCOMPARE(link.with("stop").size(), 1); // only the click pet's
        QVERIFY(fields(link.with("stop")[0]).size() >= 2);
        // the continuous one still stands: a later stopAll (the test button's) does stop it
        link.clear();
        c.stopAll();
        QCOMPARE(link.with("stop").size(), 1); // (the click pet's voice is already off): the continuous one was still sounding
    }

    void theTestButtonDoesNotLoseTheWrapThatIsAboutToCome()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::AnimationLoop, 75, 0)));
        const qint64 wrap = m_now + 80 * kMs;
        c.onWrap(u"p"_s, wrap, 1); // announced ahead
        link.clear();
        QVERIFY(c.playNow(u"p"_s));
        const QStringList plays = link.with("play");
        QCOMPARE(plays.size(), 2); // the sound of the button, then the wrap again (a start that is now discards the scheduled ones)
        QCOMPARE(fields(plays[0])[7].toLongLong(), 0LL);
        QCOMPARE(fields(plays[1])[7].toLongLong(), wrap);
        // a wrap that has passed is not sent again
        m_now += 200 * kMs;
        link.clear();
        QVERIFY(c.playNow(u"p"_s));
        QCOMPARE(link.with("play").size(), 1);
    }

    void theTestButtonPlaysEvenWithTheModeNoneAndStopAllSilencesEverything()
    {
        FakeLink link;
        PetAudioController c(&link, clock());
        c.setPet(u"p"_s, pet(sound(PetAudioMode::None, 50, 300)));
        QVERIFY(c.playNow(u"p"_s));
        const QStringList f = fields(link.with("play")[0]);
        QCOMPARE(f[5], QString("500"));
        QCOMPARE(f[6], QString("0"));
        QCOMPARE(f[7].toLongLong(), 0LL); // the offset is for the mode, not for the test
        QVERIFY(!c.playNow(u"nobody"_s));
        PetAudio none;
        c.setPet(u"q"_s, pet(none));
        QVERIFY(!c.playNow(u"q"_s)); // nothing chosen
        link.clear();
        c.stopAll();
        QCOMPARE(link.with("stop").size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstPetAudioController)
#include "tst_petaudiocontroller.moc"
