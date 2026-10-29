// The pets' mixer (src/audio/petmixer.h): scheduled starts to the sample, offsets, loops, ramps, replacement, pause, limiter, limits.

#include "petmixer.h"

#include <QFile>
#include <QSet>
#include <QTest>

#include <cmath>

using namespace murasaki::audio;

namespace {

constexpr std::int64_t kSecondNs = 1'000'000'000;
constexpr std::size_t kBuffer = 480; // 10 ms

// A clip of `frames` frames of the constant `value` on both channels.
std::shared_ptr<const Clip> constant(std::size_t frames, std::int16_t value)
{
    return std::make_shared<VectorClip>(std::vector<std::int16_t>(frames * 2, value));
}
// A ramp 0, 1, 2, ... (frame index) so a position can be read back from the output.
std::shared_ptr<const Clip> counting(std::size_t frames)
{
    std::vector<std::int16_t> s(frames * 2);
    for (std::size_t i = 0; i < frames; ++i)
        s[2 * i] = s[2 * i + 1] = std::int16_t(i);
    return std::make_shared<VectorClip>(std::move(s));
}

struct Rig {
    PetMixer mixer;
    std::int64_t now = 1'000'000; // monotonic ns of the next buffer
    std::vector<float> all;       // left channel of everything rendered

    bool run(std::size_t frames = kBuffer)
    {
        std::vector<float> out(frames * 2);
        const bool audible = mixer.render(out.data(), frames, now);
        for (std::size_t i = 0; i < frames; ++i)
            all.push_back(out[2 * i]);
        now += std::int64_t(frames) * kSecondNs / kRate;
        return audible;
    }
    void runFor(std::size_t frames)
    {
        for (std::size_t done = 0; done < frames; done += kBuffer)
            run();
    }
    // The frame index (in `all`) of the first sample whose magnitude passes `level`, or -1.
    int firstAbove(float level) const
    {
        for (std::size_t i = 0; i < all.size(); ++i)
            if (std::fabs(all[i]) > level)
                return int(i);
        return -1;
    }
};

PlayCommand cmd(int pet, std::shared_ptr<const Clip> clip, unsigned generation = 1)
{
    PlayCommand c;
    c.pet = pet;
    c.clip = std::move(clip);
    c.generation = generation;
    return c;
}

} // namespace

class TstPetMixer : public QObject
{
    Q_OBJECT

private slots:
    void everyAudioNameIsInTheDocumentAndTheDocumentHasNoOthers()
    {
        QFile doc(QStringLiteral(PETS_AUDIO_DOC));
        QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/audio.md is missing");
        const QString text = QString::fromUtf8(doc.readAll());
        const int start = text.indexOf(QStringLiteral("## Names"));
        const int end = text.indexOf(QStringLiteral("## What is verified"));
        QVERIFY(start > 0 && end > start);
        const QString section = text.mid(start, end - start);
        QSet<QString> seen;
        for (const auto &n : murasaki::kPetAudioNames) {
            const QString name = QString::fromUtf8(n.text.data(), qsizetype(n.text.size()));
            QVERIFY2(section.contains(QStringLiteral("| `") + name + QStringLiteral("` | ") + QString::fromUtf8(n.meaning.data(), qsizetype(n.meaning.size())) + QStringLiteral(" |")),
                     qPrintable(name));
            QVERIFY2(!n.meaning.empty(), qPrintable(name));
            seen.insert(name);
        }
        QCOMPARE(seen.size(), qsizetype(murasaki::kPetAudioNames.size())); // unique
        int rows = 0;
        for (const QString &line : section.split(QLatin1Char('\n')))
            rows += line.startsWith(QStringLiteral("| `"));
        QCOMPARE(rows, int(murasaki::kPetAudioNames.size()));
    }
    void silenceIsExactAndSaysSo()
    {
        Rig r;
        QVERIFY(!r.run());
        for (float f : r.all)
            QCOMPARE(f, 0.0f);
        QCOMPARE(r.mixer.voices(), 0);
    }
    void aVoiceStartsAtOnceAndEndsOnItsOwn()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(1000, 16384)));
        QVERIFY(r.run());
        QCOMPARE(r.mixer.voices(), 1);
        r.runFor(2000);
        QCOMPARE(r.mixer.voices(), 0);
        QVERIFY(!r.run()); // silent again, may be corked
        const auto events = r.mixer.takeEvents();
        QCOMPARE(events.size(), size_t(2));
        QCOMPARE(int(events[0].kind), int(Event::Kind::Started));
        QCOMPARE(int(events[1].kind), int(Event::Kind::Ended));
        // 16384/32768 = 0.5 once the 5 ms ramp is done
        QVERIFY(std::fabs(r.all[kRampFrames + 10] - 0.5f) < 1e-4f);
    }
    void aScheduledStartLandsOnTheSample()
    {
        Rig r;
        PlayCommand c = cmd(1, constant(48000, 32767));
        c.atNs = r.now + 3 * kSecondNs / 1000 + 5000; // 3 ms + 5 us into the buffer: sample 144 (+0.24)
        r.mixer.play(c);
        r.run();
        const int first = r.firstAbove(0.0f);
        QVERIFY2(first >= 144 && first <= 146, qPrintable(QString::number(first)));
        QVERIFY(r.all[std::size_t(first) - 1] == 0.0f);
    }
    void aStartInALaterBufferWaitsForIt()
    {
        Rig r;
        PlayCommand c = cmd(1, constant(48000, 32767));
        c.atNs = r.now + 25 * kSecondNs / 1000; // 25 ms: the third buffer, sample 240 of it
        r.mixer.play(c);
        QVERIFY(r.run()); // pending counts as audible: the stream stays open
        r.run();
        QCOMPARE(r.firstAbove(0.0f), -1);
        r.run();
        const int first = r.firstAbove(0.0f);
        QVERIFY2(first >= int(2 * kBuffer + 239) && first <= int(2 * kBuffer + 241), qPrintable(QString::number(first)));
    }
    void aStartTooLateIsDroppedNotPlayedOutOfTime()
    {
        Rig r;
        r.now = 10 * kSecondNs;
        PlayCommand c = cmd(1, constant(48000, 1000));
        c.atNs = r.now - 200'000'000;
        r.mixer.play(c);
        QVERIFY(!r.run());
        const auto events = r.mixer.takeEvents();
        QCOMPARE(events.size(), size_t(1));
        QCOMPARE(int(events[0].kind), int(Event::Kind::Dropped));
        // a little late is played at once
        c.atNs = r.now - 20'000'000;
        r.mixer.play(c);
        QVERIFY(r.run());
    }
    void aNegativeOffsetStartsIntoTheFile()
    {
        Rig r;
        PlayCommand c = cmd(1, counting(4000));
        c.startFrame = 1000;
        r.mixer.play(c);
        r.run();
        // the first audible sample is clip frame 1000 + (frames spent in the ramp's silence = 0): the output at index n is sample (1000+n) * ramp
        const std::size_t n = kRampFrames; // ramp done: full gain
        QVERIFY(std::fabs(r.all[n] - float(1000 + n) / 32768.0f) < 1e-5f);
        // an offset past the end plays nothing
        PlayCommand past = cmd(2, counting(100));
        past.startFrame = 100;
        r.mixer.play(past);
        QVERIFY(!r.run() || r.mixer.voices() == 1);
        const auto events = r.mixer.takeEvents();
        bool dropped = false;
        for (const Event &e : events)
            dropped = dropped || (e.kind == Event::Kind::Dropped && e.pet == 2);
        QVERIFY(dropped);
    }
    void aLoopHasNoSeam()
    {
        Rig r;
        PlayCommand c = cmd(1, counting(100));
        c.loop = true;
        r.mixer.play(c);
        r.runFor(2000);
        QCOMPARE(r.mixer.voices(), 1); // never ends
        // after the ramp, the output is the frame index modulo 100
        for (std::size_t i = kRampFrames + 5; i < 1500; ++i)
            QVERIFY(std::fabs(r.all[i] - float(i % 100) / 32768.0f) < 1e-5f);
    }
    void replacingAVoiceFadesTheOldOneAndNeverClicks()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(48000, 16384), 1));
        r.runFor(2000);
        r.mixer.play(cmd(1, constant(48000, -16384), 2));
        r.run(100);
        QCOMPARE(r.mixer.voices(), 2); // the old one is still fading
        r.run();
        QCOMPARE(r.mixer.voices(), 1);
        // no sample-to-sample step bigger than the ramp allows around the change (0.5 + 0.5 over 240 frames)
        float worst = 0;
        for (std::size_t i = 1; i < r.all.size(); ++i)
            worst = std::max(worst, std::fabs(r.all[i] - r.all[i - 1]));
        QVERIFY2(worst < 0.01f, qPrintable(QString::number(worst)));
    }
    void aScheduledReplacementLeavesTheOldSoundPlayingUntilItBegins()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(480000, 16384), 1));
        r.runFor(2000);
        PlayCommand next = cmd(1, constant(480000, -16384), 2);
        next.atNs = r.now + 50 * kSecondNs / 1000; // 50 ms ahead
        r.mixer.play(next);
        const std::size_t before = r.all.size();
        r.runFor(960); // 20 ms: the old sound is still all there
        QCOMPARE(r.mixer.voices(), 2); // the playing one and the scheduled one
        QVERIFY(std::fabs(r.all.back() - 0.5f) < 1e-4f);
        r.runFor(2400); // past the start: the new one, the old one gone
        QVERIFY(r.all.back() < -0.4f);
        QCOMPARE(r.mixer.voices(), 1);
        // the switch happened at the scheduled sample (50 ms = 2400 frames after `before - 2000 ...`), without a jump bigger than the ramp allows
        float worst = 0;
        for (std::size_t i = before; i < r.all.size(); ++i)
            worst = std::max(worst, std::fabs(r.all[i] - r.all[i - 1]));
        QVERIFY(worst < 0.01f);
    }
    void aStartScheduledBeforeTheLastOneBeganDoesNotOvertakeIt()
    {
        // The wrap after next is announced when the next one is seen, which can be a few milliseconds before the next one's own time.
        Rig r;
        PlayCommand a = cmd(1, constant(480000, 8192), 1), b = cmd(1, constant(480000, 16384), 1);
        a.atNs = r.now + 50 * kSecondNs / 1000;
        b.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(a);
        r.mixer.play(b);
        r.runFor(4 * 480);                 // 40 ms: nothing yet
        QVERIFY(r.firstAbove(0.0f) == -1);
        r.runFor(6 * 480);                 // to 100 ms: the first has begun
        QVERIFY(std::fabs(r.all.back() - 0.25f) < 1e-3f);
        r.runFor(2 * 480);                 // past 100 ms: the second replaced it
        QVERIFY(std::fabs(r.all.back() - 0.5f) < 1e-3f);
        QCOMPARE(r.mixer.voices(), 1);
    }
    void aPetCanHaveTwoScheduledStartsWaitingAndNoMore()
    {
        Rig r;
        for (int i = 1; i <= 3; ++i) {
            PlayCommand c = cmd(1, constant(48000, 8192), 1);
            c.atNs = r.now + i * 100 * kSecondNs / 1000;
            r.mixer.play(c);
        }
        PlayCommand other = cmd(2, constant(48000, 8192), 1); // the cap is the pet's own
        other.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(other);
        r.run();
        int dropped = 0;
        for (const Event &e : r.mixer.takeEvents())
            dropped += e.kind == Event::Kind::Dropped ? 1 : 0;
        QCOMPARE(dropped, 1);
        r.mixer.cancelPending(1);
        r.run();
        PlayCommand again = cmd(1, constant(48000, 8192), 2); // after a cancel there is room again
        again.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(again);
        r.run();
        for (const Event &e : r.mixer.takeEvents())
            QVERIFY(e.kind != Event::Kind::Dropped);
    }
    void aCancelDropsTheScheduledStartAndNotThePlayingSound()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(480000, 16384), 1));
        r.runFor(1000);
        PlayCommand next = cmd(1, constant(480000, -16384), 2);
        next.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(next);
        r.run();
        r.mixer.cancelPending(1);
        r.runFor(9600); // well past the time it would have begun
        QVERIFY(r.all.back() > 0.4f);
        QCOMPARE(r.mixer.voices(), 1);
        // a second schedule replaces the first
        PlayCommand a = cmd(1, constant(480000, -16384), 3), b = cmd(1, constant(480000, 8192), 4);
        a.atNs = r.now + 100 * kSecondNs / 1000;
        b.atNs = r.now + 120 * kSecondNs / 1000;
        r.mixer.play(a);
        r.mixer.play(b);
        r.runFor(9600);
        QVERIFY(std::fabs(r.all.back() - 0.25f) < 1e-3f);
        // stop kills the scheduled one too
        PlayCommand c = cmd(1, constant(480000, -16384), 5);
        c.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(c);
        r.mixer.stop(1);
        r.runFor(9600);
        QCOMPARE(r.mixer.voices(), 0);
    }
    void pauseAndGainReachBothTheOldAndTheScheduledSound()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(480000, 16384), 1));
        r.runFor(1000);
        PlayCommand next = cmd(1, constant(480000, 16384), 2);
        next.atNs = r.now + 200 * kSecondNs / 1000;
        r.mixer.play(next);
        r.mixer.setGain(1, 0.5f);
        r.runFor(2400);
        QVERIFY(std::fabs(r.all.back() - 0.25f) < 1e-3f);
        r.mixer.pause(1);
        r.runFor(960);
        QCOMPARE(r.all.back(), 0.0f);
    }
    void aStopOnAScheduledStartLeavesNoTraceAndAnImmediatePlayReplacesIt()
    {
        Rig r;
        PlayCommand later = cmd(1, constant(480000, 16384), 1);
        later.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(later);
        r.run(100);
        r.mixer.stop(1);
        r.runFor(9600);
        QCOMPARE(r.mixer.voices(), 0);
        for (const Event &e : r.mixer.takeEvents())
            QVERIFY(e.kind != Event::Kind::Started && e.kind != Event::Kind::Ended);
        QCOMPARE(r.firstAbove(0.0f), -1);
        // a sound played now is not stopped later by a start that was scheduled before it
        PlayCommand scheduled = cmd(2, constant(480000, -16384), 1);
        scheduled.atNs = r.now + 100 * kSecondNs / 1000;
        r.mixer.play(scheduled);
        r.run(100);
        r.mixer.play(cmd(2, constant(480000, 16384), 2));
        r.runFor(9600);
        QVERIFY(r.all.back() > 0.4f);
        QCOMPARE(r.mixer.voices(), 1);
    }
    void aPausedVoiceDoesNotKeepTheStreamOpen()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(480000, 16384), 1));
        r.run();
        QCOMPARE(r.mixer.active(), 1);
        r.mixer.pause(1);
        r.runFor(960);
        QCOMPARE(r.mixer.voices(), 1);   // still there, where it was
        QCOMPARE(r.mixer.active(), 0);   // but the stream may be corked
        r.mixer.resume(1);
        r.run();
        QCOMPARE(r.mixer.active(), 1);
        PlayCommand later = cmd(2, constant(480000, 16384), 1);
        later.atNs = r.now + 500 * kSecondNs / 1000;
        r.mixer.play(later);
        QCOMPARE(r.mixer.active(), 2); // counted from the moment it is posted, before the next render
    }
    void aCorkedStreamIsWokenByEveryCommandNotOnlyByAPlay()
    {
        // The audio thread runs only while the mixer says it is active(), as a corked PipeWire stream does: nothing renders otherwise.
        Rig r;
        const auto renderIfActive = [&](int buffers) {
            for (int i = 0; i < buffers; ++i)
                if (r.mixer.active() > 0)
                    r.run();
        };
        r.mixer.play(cmd(1, constant(480000, 16384), 1));
        renderIfActive(5);
        r.mixer.pause(1);
        renderIfActive(5); // the pause is applied, the voice fades out, and now nothing keeps the stream open
        QCOMPARE(r.mixer.active(), 0);
        QCOMPARE(r.mixer.voices(), 1);
        const std::size_t corked = r.all.size();
        renderIfActive(5);
        QCOMPARE(r.all.size(), corked); // (corked: not a buffer rendered)
        // resume: the stream must wake for it, and the sound comes back
        r.mixer.resume(1);
        QVERIFY(r.mixer.active() > 0);
        renderIfActive(5);
        QVERIFY(r.all.size() > corked);
        QVERIFY(r.all.back() > 0.4f);
        // stop on a paused voice: also wakes it, and the voice is gone afterwards
        r.mixer.pause(1);
        renderIfActive(5);
        QCOMPARE(r.mixer.active(), 0);
        r.mixer.stop(1);
        QVERIFY(r.mixer.active() > 0);
        renderIfActive(5);
        QCOMPARE(r.mixer.voices(), 0);
        QCOMPARE(r.mixer.active(), 0);
        // cancel and a gain reach a stream that is corked too (and it corks again once they are applied)
        r.mixer.cancelPending(1);
        r.mixer.setGain(1, 0.5f);
        r.mixer.setMaster(0.5f);
        QVERIFY(r.mixer.active() > 0);
        renderIfActive(2);
        QCOMPARE(r.mixer.active(), 0);
    }
    void anOlderGenerationIsDropped()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(48000, 16384), 5));
        r.run();
        r.mixer.play(cmd(1, constant(48000, -16384), 4));
        r.run();
        QCOMPARE(r.mixer.voices(), 1);
        QVERIFY(r.all.back() > 0.4f); // still the generation 5 sound
    }
    void stopRampsOutAndDoesNotAnnounceAnEnd()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(48000, 16384)));
        r.runFor(1000);
        r.mixer.takeEvents();
        r.mixer.stop(1);
        r.run();
        QCOMPARE(r.mixer.voices(), 0);
        QVERIFY(std::fabs(r.all.back()) < 1e-6f);
        for (const Event &e : r.mixer.takeEvents())
            QVERIFY(e.kind != Event::Kind::Ended);
        QVERIFY(!r.run());
    }
    void pauseKeepsThePositionAndResumeContinuesFromIt()
    {
        Rig r;
        PlayCommand c = cmd(1, counting(48000));
        r.mixer.play(c);
        r.runFor(960);
        r.mixer.pause(1);
        r.run();
        r.run(); // fully paused: silence
        std::vector<float> paused(r.all.end() - 100, r.all.end());
        for (float f : paused)
            QCOMPARE(f, 0.0f);
        QCOMPARE(r.mixer.voices(), 1);
        const std::size_t before = r.all.size();
        r.mixer.resume(1);
        r.run();
        // the sound continues from where the ramp-out stopped it: increasing sample values, not from 0
        QVERIFY(r.all[before + kRampFrames + 5] > 1000.0f / 32768.0f);
    }
    void volumeIsLinearAndALiveChangeIsRamped()
    {
        Rig r;
        PlayCommand c = cmd(1, constant(48000, 20000));
        c.gain = 0.25f;
        r.mixer.play(c);
        r.runFor(1000);
        QVERIFY(std::fabs(r.all[900] - 20000.0f / 32768.0f * 0.25f) < 1e-4f);
        r.mixer.setGain(1, 1.0f);
        r.run();
        float worst = 0;
        for (std::size_t i = r.all.size() - kBuffer + 1; i < r.all.size(); ++i)
            worst = std::max(worst, std::fabs(r.all[i] - r.all[i - 1]));
        QVERIFY(worst < 0.01f);
        QVERIFY(std::fabs(r.all.back() - 20000.0f / 32768.0f) < 1e-4f);
    }
    void theMasterScalesEverythingAndRamps()
    {
        Rig r;
        r.mixer.play(cmd(1, constant(48000, 16384)));
        r.mixer.setMaster(0.5f);
        r.runFor(2400);
        QVERIFY(std::fabs(r.all.back() - 0.25f) < 1e-4f);
        r.mixer.setMaster(0);
        r.run();
        QCOMPARE(r.all.back(), 0.0f);
        QCOMPARE(r.mixer.voices(), 1); // muted, not stopped: the sound is where it would be
    }
    void theLimiterKeepsSeveralLoudVoicesBelowFullScale()
    {
        Rig r;
        for (int pet = 0; pet < 8; ++pet)
            r.mixer.play(cmd(pet, constant(48000, 30000)));
        r.runFor(2400);
        float peak = 0;
        for (float f : r.all)
            peak = std::max(peak, std::fabs(f));
        QVERIFY(peak <= 1.0f);
        QVERIFY(peak > 0.95f);
        // and a quiet signal passes untouched
        Rig q;
        q.mixer.play(cmd(0, constant(48000, 3000)));
        q.runFor(2400);
        QVERIFY(std::fabs(q.all.back() - 3000.0f / 32768.0f) < 1e-6f);
    }
    void theSeventeenthVoiceIsDropped()
    {
        Rig r;
        for (int pet = 0; pet < 17; ++pet)
            r.mixer.play(cmd(pet, constant(48000, 1000)));
        r.run();
        QCOMPARE(r.mixer.voices(), 16);
        int dropped = 0;
        for (const Event &e : r.mixer.takeEvents())
            dropped += e.kind == Event::Kind::Dropped;
        QCOMPARE(dropped, 1);
    }
    void commandsAreBoundedAndBadPetsAreIgnored()
    {
        Rig r;
        for (int i = 0; i < 5000; ++i)
            r.mixer.stop(1);
        r.mixer.play(cmd(-1, constant(10, 1)));
        r.mixer.play(cmd(kMaxPets, constant(10, 1)));
        r.mixer.play(cmd(3, nullptr));
        r.run();
        QCOMPARE(r.mixer.voices(), 0);
    }
    void aLongRunWithCommandsStaysBounded()
    {
        // The vectors are reserved up front; a long run of buffers with commands in between must not grow them (checked by capacity).
        Rig r;
        PlayCommand c = cmd(1, constant(480000, 100));
        c.loop = true;
        r.mixer.play(c);
        for (int i = 0; i < 2000; ++i) {
            if (i % 7 == 0)
                r.mixer.setGain(1, (i % 14) ? 0.5f : 1.0f);
            r.run();
            r.mixer.takeEvents();
        }
        QCOMPARE(r.mixer.voices(), 1);
    }
};

QTEST_APPLESS_MAIN(TstPetMixer)
#include "tst_petmixer.moc"
