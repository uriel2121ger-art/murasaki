// murasaki-scene-vk audio: the mp3 stream, the spectrum, the sound player and the scene's audio with a fake backend.
// Nothing here plays sound or touches the system's audio.

#include "audio/soundstream.h"
#include "audio/sceneaudio.h"
#include "audio/soundplayer.h"
#include "audio/spectrum.h"
#include "audiofixtures.h"

#include <QTest>

#include <algorithm>
#include <QElapsedTimer>
#include <atomic>
#include <cmath>
#include <thread>

using namespace murasaki::scene;

namespace {

std::vector<float> sine(double hz, size_t frames, float amplitude, int channel = -1)
{
    std::vector<float> out(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float v = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / 48000.0));
        if (channel != 1) {
            out[i * 2] = v;
        }
        if (channel != 0) {
            out[i * 2 + 1] = v;
        }
    }
    return out;
}

size_t loudest(const std::array<float, 64> &bands)
{
    return static_cast<size_t>(std::max_element(bands.begin(), bands.end()) - bands.begin());
}

// A backend that is driven by hand.
class FakeBackend : public AudioBackend
{
public:
    bool startPlayback(Pull pull, std::string *) override
    {
        m_pull = std::move(pull);
        ++playbackStarts;
        return true;
    }
    bool startCapture(Push push, std::string *) override
    {
        m_push = std::move(push);
        ++captureStarts;
        return true;
    }
    void stopPlayback() override { m_pull = nullptr; }
    void stopCapture() override
    {
        m_push = nullptr;
        ++captureStops;
    }
    void setPlaybackActive(bool active) override { activity.push_back(active); }
    std::vector<bool> activity;
    Pull m_pull;
    Push m_push;
    int playbackStarts = 0, captureStarts = 0, captureStops = 0;
};

// Which band of 64 holds `hz` (the same log axis the analyzer uses).
size_t bandOf(double hz)
{
    return static_cast<size_t>(std::floor(std::log(hz / 30.0) / std::log(16000.0 / 30.0) * 64.0));
}

// The peak of each 10 ms of what the player produces over `seconds` of real time (it decodes in step with the clock).
std::vector<float> record(SoundPlayer &player, double seconds)
{
    std::vector<float> peaks;
    std::vector<float> out(480 * 2);
    QElapsedTimer timer;
    timer.start();
    size_t blocks = 0;
    while (timer.elapsed() < seconds * 1000.0) {
        // one 10 ms block per 10 ms of the clock
        while (static_cast<double>(blocks) * 10.0 <= static_cast<double>(timer.elapsed())) {
            player.pull(out.data(), 480);
            float peak = 0.0f;
            for (float v : out)
                peak = std::max(peak, std::fabs(v));
            peaks.push_back(peak);
            ++blocks;
        }
        QTest::qWait(2);
    }
    return peaks;
}


} // namespace

class TstSceneVkAudio : public QObject
{
    Q_OBJECT

private slots:
    void fftFindsTheFrequencyOfASine()
    {
        const size_t n = 1024;
        std::vector<float> re(n), im(n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            re[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 64.0 * static_cast<double>(i) / static_cast<double>(n)));
        fft(re.data(), im.data(), n);
        size_t best = 0;
        float bestMagnitude = 0.0f;
        for (size_t i = 0; i < n / 2; ++i) {
            const float m = std::hypot(re[i], im[i]);
            if (m > bestMagnitude) {
                bestMagnitude = m;
                best = i;
            }
        }
        QCOMPARE(best, size_t{64});
        QVERIFY(std::fabs(bestMagnitude - static_cast<float>(n) / 2.0f) < 1.0f);
    }

    void mp3DecodesAndConvertsToStereoAtTheOutputRate()
    {
        std::string error;
        auto stream = SoundStream::open(audiofix::sineMp3(), 48000, &error);
        QVERIFY2(stream, error.c_str());
        QCOMPARE(stream->sourceRate(), 22050u);
        std::vector<float> all;
        std::vector<float> chunk(1000 * 2);
        for (;;) {
            const size_t n = stream->read(chunk.data(), 1000);
            all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * 2));
            if (n < 1000)
                break;
        }
        const double seconds = static_cast<double>(all.size() / 2) / 48000.0;
        QVERIFY2(seconds > 0.35 && seconds < 0.5, qPrintable(QString::number(seconds)));
        // mono is duplicated
        for (size_t i = 0; i < 200; ++i)
            QCOMPARE(all[i * 2], all[i * 2 + 1]);
        // and the pitch survives the conversion: 440 Hz lands in the band of 440 Hz
        SpectrumAnalyzer analyzer;
        analyzer.push(all.data() + 2000, 2048);
        const AudioSpectrum &s = analyzer.update(1.0);
        QVERIFY2(std::abs(static_cast<int>(loudest(s.left64)) - static_cast<int>(bandOf(440.0))) <= 1, qPrintable(QString::number(loudest(s.left64))));
        QVERIFY(!SoundStream::open({1, 2, 3, 4}, 48000, &error));
    }

    void everyFormatIsPickedByWhatTheFileStartsWith()
    {
        struct Case { std::vector<uint8_t> bytes; const char *format; uint32_t rate; int channels; };
        const Case cases[] = {{audiofix::sineMp3(), "mp3", 22050, 1},
                              {audiofix::sineWav(8000, 0.1, 1), "wav", 8000, 1},
                              {audiofix::sineWav(44100, 0.1, 2), "wav", 44100, 2},
                              {audiofix::sineFlac(), "flac", 8000, 1},
                              {audiofix::sineOgg(), "vorbis", 8000, 1}};
        for (const Case &c : cases) {
            std::string error;
            auto stream = SoundStream::open(c.bytes, 48000, &error);
            QVERIFY2(stream, (std::string(c.format) + ": " + error).c_str());
            QCOMPARE(QString::fromLatin1(stream->format()), QString::fromLatin1(c.format));
            QCOMPARE(stream->sourceRate(), c.rate);
            // about 0.1 s (0.4 s for the mp3) of audible sound at the output rate, mono duplicated, then the end
            std::vector<float> all, chunk(1000 * 2);
            for (;;) {
                const size_t n = stream->read(chunk.data(), 1000);
                all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * 2));
                if (n < 1000)
                    break;
            }
            const double seconds = static_cast<double>(all.size() / 2) / 48000.0;
            const double expected = std::string(c.format) == "mp3" ? 0.4 : 0.1;
            QVERIFY2(seconds > expected * 0.8 && seconds < expected * 1.3, qPrintable(QString("%1: %2 s").arg(c.format).arg(seconds)));
            float peak = 0.0f;
            for (float v : all)
                peak = std::max(peak, std::fabs(v));
            QVERIFY2(peak > 0.1f && peak <= 1.0f, qPrintable(QString("%1: peak %2").arg(c.format).arg(static_cast<double>(peak))));
            if (c.channels == 1) {
                for (size_t i = 0; i < 100; ++i)
                    QCOMPARE(all[i * 2], all[i * 2 + 1]);
            }
            // rewinding starts the same sound again
            stream->rewind();
            std::vector<float> again(2 * 200);
            QCOMPARE(stream->read(again.data(), 200), size_t{200});
            QCOMPARE(std::vector<float>(all.begin(), all.begin() + 400), again);
        }
        // the bytes decide, not a name: a WAV that is not one is refused as a sound, and a truncated or damaged FLAC or Ogg never hangs
        std::string error;
        std::vector<uint8_t> riff = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        QVERIFY(!SoundStream::open(riff, 48000, &error));
        for (const auto &whole : {audiofix::sineFlac(), audiofix::sineOgg(), audiofix::sineWav()}) {
            for (size_t cut = 0; cut < whole.size(); cut += 37) {
                auto stream = SoundStream::open(std::vector<uint8_t>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut)), 48000);
                std::vector<float> out(2 * 512);
                for (int i = 0; stream && i < 40 && stream->read(out.data(), 512) > 0; ++i) {
                }
            }
            std::vector<uint8_t> damaged = whole;
            for (size_t i = 60; i < damaged.size(); i += 53)
                damaged[i] ^= 0x5a;
            auto stream = SoundStream::open(damaged, 48000);
            std::vector<float> out(2 * 512);
            for (int i = 0; stream && i < 40 && stream->read(out.data(), 512) > 0; ++i) {
            }
        }
    }

    void aLazyPlaylistHoldsOneFileAtATimeAndSkipsTheOnesThatDoNotOpen()
    {
        // 300 files, one of them junk: each is asked for when its turn comes, and loop plays them in order
        std::atomic<int> asked{0};
        std::vector<int> order;
        std::mutex mutex;
        std::vector<SoundTrack> tracks(1);
        tracks[0].name = "lazy";
        for (int i = 0; i < 300; ++i) {
            tracks[0].sources.push_back([i, &asked, &order, &mutex]() -> std::optional<std::vector<uint8_t>> {
                ++asked;
                std::lock_guard lock(mutex);
                order.push_back(i);
                if (i == 1)
                    return std::vector<uint8_t>{1, 2, 3, 4}; // not a sound
                if (i == 2)
                    return std::nullopt;                      // cannot be read
                return audiofix::sineWav(8000, 0.05, 1);
            });
        }
        tracks[0].loop = true;
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QCOMPARE(player.trackCount(), size_t{1});
        QCOMPARE(asked.load(), 1); // only the first, to know that it plays
        player.setMasterVolume(1.0f);
        record(player, 1.0);
        QVERIFY(asked.load() < 300); // the 300 files are not all read at once (0.05 s each: about twenty in a second)
        std::lock_guard lock(mutex);
        QVERIFY(order.size() >= 4);
        QCOMPARE(order[0], 0);
        QCOMPARE(order[1], 1);
        QCOMPARE(order[2], 2);
        QCOMPARE(order[3], 3); // the junk and the unreadable are passed over
    }

    void aPlaylistOfOnlyBadSourcesIsSilentAndReported()
    {
        std::vector<SoundTrack> tracks(1);
        tracks[0].name = "bad";
        for (int i = 0; i < 3; ++i)
            tracks[0].sources.push_back([]() -> std::optional<std::vector<uint8_t>> { return std::nullopt; });
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QCOMPARE(player.trackCount(), size_t{0});
        QCOMPARE(errors.size(), size_t{3});
    }

    void aSingleTrackPlaysOnceAndStops()
    {
        std::vector<SoundTrack> tracks(1);
        tracks[0].name = "once";
        tracks[0].mp3 = audiofix::sineMp3(); // 0.4 s
        tracks[0].loop = false;
        SoundPlayer player(std::move(tracks));
        player.setMasterVolume(1.0f);
        const std::vector<float> peaks = record(player, 1.5);
        size_t last = 0;
        for (size_t i = 0; i < peaks.size(); ++i)
            if (peaks[i] > 0.01f)
                last = i;
        QVERIFY2(last > 20 && last < 70, qPrintable(QString::number(last))); // about 0.4 s of sound, then nothing for the other second
    }

    void hostileMp3sAreRefusedWithoutHanging()
    {
        std::string error;
        // over the size limit: refused before the decoder looks at it
        QVERIFY(!SoundStream::open(std::vector<uint8_t>(kMaxSoundFileBytes + 1, 0xFF), 48000, &error));
        QVERIFY(error.find("MiB") != std::string::npos);
        // noise, sync words with nothing behind them, a truncated header
        QVERIFY(!SoundStream::open({}, 48000, &error));
        std::vector<uint8_t> noise(100000);
        uint32_t x = 12345;
        for (auto &b : noise) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(x >> 24);
        }
        auto stream = SoundStream::open(noise, 48000, &error);
        if (stream) { // when the decoder finds a frame in the noise, reading and rewinding must still end
            std::vector<float> out(2048);
            for (int i = 0; i < 100; ++i) {
                if (stream->read(out.data(), 1024) == 0)
                    stream->rewind();
            }
        }
        // a player given such tracks keeps running and reports each one
        std::vector<SoundTrack> tracks(3);
        tracks[0].mp3 = noise;
        tracks[1].mp3 = std::vector<uint8_t>(kMaxSoundFileBytes + 1, 0xFF);
        tracks[2].mp3 = {};
        for (auto &t : tracks)
            t.loop = true;
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QVERIFY(errors.size() >= 2);
    }

    void anMp3LoopsByRewinding()
    {
        auto stream = SoundStream::open(audiofix::sineMp3(), 48000);
        QVERIFY(stream);
        std::vector<float> first(2 * 500), again(2 * 500), sink(2 * 1000);
        QCOMPARE(stream->read(first.data(), 500), size_t{500});
        while (stream->read(sink.data(), 1000) == 1000) {
        }
        stream->rewind();
        QCOMPARE(stream->read(again.data(), 500), size_t{500});
        QCOMPARE(first, again);
    }

    void theSpectrumPutsAToneInItsBandAndOnItsChannel()
    {
        SpectrumAnalyzer analyzer;
        const auto tone = sine(1000.0, 4096, 1.0f, 0); // full scale, left only
        analyzer.push(tone.data(), 4096);
        const AudioSpectrum &s = analyzer.update(1.0);
        QVERIFY2(std::abs(static_cast<int>(loudest(s.left64)) - static_cast<int>(bandOf(1000.0))) <= 1, qPrintable(QString::number(loudest(s.left64))));
        QVERIFY2(*std::max_element(s.left64.begin(), s.left64.end()) > 0.9f, "a full scale sine reads near 1");
        QVERIFY(*std::max_element(s.right64.begin(), s.right64.end()) < 0.05f);
        // 16 and 32 band versions agree on where the tone is (the band index scales)
        const size_t b16 = static_cast<size_t>(std::max_element(s.left16.begin(), s.left16.end()) - s.left16.begin());
        QVERIFY(std::abs(static_cast<int>(b16) - static_cast<int>(bandOf(1000.0) / 4)) <= 1);
        // quieter by 20 dB reads 20 / 60 lower
        SpectrumAnalyzer quiet;
        const auto soft = sine(1000.0, 4096, 0.1f);
        quiet.push(soft.data(), 4096);
        const AudioSpectrum &q = quiet.update(1.0);
        const float level = *std::max_element(q.left64.begin(), q.left64.end());
        QVERIFY2(std::fabs(level - (1.0f - 20.0f / 60.0f)) < 0.12f, qPrintable(QString::number(level)));
    }

    void theSpectrumFallsAfterTheSoundStops()
    {
        SpectrumAnalyzer analyzer;
        const auto tone = sine(500.0, 4096, 1.0f);
        analyzer.push(tone.data(), 4096);
        const float loud = *std::max_element(analyzer.update(1.0).left16.begin(), analyzer.current().left16.end());
        const std::vector<float> silence(4096 * 2, 0.0f);
        analyzer.push(silence.data(), 4096);
        const float after = *std::max_element(analyzer.update(0.15).left16.begin(), analyzer.current().left16.end());
        QVERIFY(loud > 0.9f);
        QVERIFY2(after < loud * 0.5f && after > 0.0f, qPrintable(QString::number(after))); // one time constant: it fades, not at once
        analyzer.update(5.0);
        QVERIFY(*std::max_element(analyzer.current().left16.begin(), analyzer.current().left16.end()) < 0.01f);
    }

    void thePlayerLoopsMixesAndObeysVolumeAndPause()
    {
        std::vector<SoundTrack> tracks;
        tracks.push_back({"a", audiofix::sineMp3(), 0.5f, true});
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QVERIFY(errors.empty());
        const auto energy = [&](size_t frames) {
            std::vector<float> out(frames * 2);
            player.pull(out.data(), frames);
            float peak = 0.0f;
            for (float v : out)
                peak = std::max(peak, std::fabs(v));
            return peak;
        };
        // it starts silent and decodes nothing until a volume arrives: no blast at launch, no work for a muted scene
        QTest::qWait(100);
        QCOMPARE(energy(512), 0.0f);
        QCOMPARE(player.decodedFrames(), size_t{0});
        player.setMasterVolume(1.0f);
        // let the decoder fill the ring, then read for longer than the clip lasts (0.4 s): it loops
        QTest::qWait(100);
        float peak = 0.0f;
        for (int i = 0; i < 40; ++i) {
            peak = std::max(peak, energy(1024));
            QTest::qWait(15);
        }
        QVERIFY2(peak > 0.045f && peak < 0.08f, qPrintable(QString::number(peak))); // the clip peaks at 0.125 (ffmpeg's sine); at volume 0.5: 0.0625
        // the master volume
        player.setMasterVolume(0.0f);
        float quiet = 1.0f;
        for (int i = 0; i < 40; ++i) {
            quiet = energy(1024);
            QTest::qWait(10);
        }
        QVERIFY2(quiet < 0.01f, qPrintable(QString::number(quiet)));
        // at volume 0 the decoder rests: what was decoded stays
        QTest::qWait(300);
        const size_t resting = player.decodedFrames();
        QTest::qWait(300);
        QCOMPARE(player.decodedFrames(), resting);
        player.setMasterVolume(1.0f);
        QTest::qWait(300);
        QVERIFY(player.decodedFrames() > resting);
        player.setPaused(true);
        QCOMPARE(energy(512), 0.0f);
        QTest::qWait(300);
        const size_t paused = player.decodedFrames();
        QTest::qWait(300);
        QCOMPARE(player.decodedFrames(), paused); // paused: no decoding either
        // a track that cannot be decoded is reported and skipped
        std::vector<SoundTrack> bad;
        bad.push_back({"junk", {1, 2, 3}, 1.0f, true});
        std::vector<std::string> badErrors;
        SoundPlayer broken(std::move(bad), &badErrors);
        QCOMPARE(broken.trackCount(), size_t{0});
        QCOMPARE(badErrors.size(), size_t{1});
    }

    void aRandomTrackPlaysThenRestsForTheDelayAndPlaysAgain()
    {
        std::vector<SoundTrack> tracks(1);
        tracks[0].name = "r";
        tracks[0].mp3 = audiofix::sineMp3(); // 0.4 s
        tracks[0].volume = 1.0f;
        tracks[0].random = true;
        tracks[0].minDelay = 0.5f;
        tracks[0].maxDelay = 0.5f;
        tracks[0].seed = 7;
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QVERIFY(errors.empty());
        player.setMasterVolume(1.0f);
        const std::vector<float> peaks = record(player, 3.7);
        // active runs of blocks, and the rests between them
        std::vector<std::pair<size_t, size_t>> runs;
        for (size_t i = 0; i < peaks.size(); ++i) {
            if (peaks[i] > 0.01f) {
                if (runs.empty() || i > runs.back().second + 1)
                    runs.push_back({i, i});
                else
                    runs.back().second = i;
            }
        }
        // a play is about 0.4 s, a rest about 0.5 s: in 3.7 s that is four plays (the last may be cut)
        QVERIFY2(runs.size() >= 3 && runs.size() <= 5, qPrintable(QString::number(runs.size())));
        for (size_t i = 1; i < runs.size(); ++i) {
            const double rest = static_cast<double>(runs[i].first - runs[i - 1].second - 1) * 10.0;
            QVERIFY2(rest >= 380.0 && rest <= 700.0, qPrintable(QString::number(rest)));
        }
        QVERIFY(runs.front().first < 40); // it starts at once
    }

    void aRandomTrackPicksAmongTheFilesAndSkipsOnesThatDoNotOpen()
    {
        std::vector<SoundTrack> tracks(1);
        tracks[0].mp3 = audiofix::sineMp3();
        tracks[0].more = {std::vector<uint8_t>(500, 0x41), audiofix::sineMp3()}; // junk in the middle of the list
        tracks[0].random = true;
        tracks[0].minDelay = 0.0f;
        tracks[0].maxDelay = 0.1f;
        tracks[0].seed = 3;
        std::vector<std::string> errors;
        SoundPlayer player(std::move(tracks), &errors);
        QCOMPARE(errors.size(), size_t{1}); // the junk is reported, the two good files play
        QCOMPARE(player.trackCount(), size_t{1});
        player.setMasterVolume(1.0f);
        const std::vector<float> peaks = record(player, 1.5);
        int active = 0;
        for (float p : peaks)
            active += p > 0.01f;
        QVERIFY2(active > 60, qPrintable(QString::number(active))); // mostly playing: rests of at most 0.1 s
    }

    void aLoopPlaylistPlaysEveryFileInOrderWithoutGaps()
    {
        std::vector<SoundTrack> tracks(1);
        tracks[0].mp3 = audiofix::sineMp3();
        auto half = audiofix::sineMp3();
        tracks[0].more = {half, half};
        tracks[0].loop = true;
        SoundPlayer player(std::move(tracks), nullptr);
        player.setMasterVolume(1.0f);
        const std::vector<float> peaks = record(player, 2.0);
        size_t quiet = 0;
        for (size_t i = 20; i < peaks.size(); ++i) // after the start
            quiet += peaks[i] < 0.005f;
        QVERIFY2(quiet < 6, qPrintable(QString::number(quiet))); // a file ends and the next one is already there
    }

    void thePlaybackStreamIsCorkedWhileNobodyCanHearIt()
    {
        auto backend = std::make_unique<FakeBackend>();
        FakeBackend *b = backend.get();
        std::vector<SoundTrack> tracks;
        tracks.push_back({"a", audiofix::sineMp3(), 1.0f, true});
        SceneAudio audio(std::move(tracks), false, std::move(backend));
        QVERIFY(audio.playing());
        QCOMPARE(b->activity.back(), false); // starts corked: the volume is 0 until the first message
        audio.setVolume(64);
        QCOMPARE(b->activity.back(), true);
        audio.setPaused(true);
        QCOMPARE(b->activity.back(), false);
        audio.setPaused(false);
        QCOMPARE(b->activity.back(), true);
        audio.setVolume(0); // mute, or a covered output
        QCOMPARE(b->activity.back(), false);
        audio.setVolume(128);
        QCOMPARE(b->activity.back(), true);
    }

    void theSpectrumHandOffNeedsNoLockAndKeepsTheLatestWindow()
    {
        SpectrumAnalyzer analyzer;
        // a producer thread pushes while the reader updates: nothing hangs or tears (a value is always a real level)
        std::atomic<bool> stop{false};
        std::thread producer([&] {
            std::vector<float> chunk(480 * 2);
            double phase = 0.0;
            while (!stop) {
                for (size_t i = 0; i < 480; ++i, phase += 2.0 * M_PI * 1000.0 / 48000.0)
                    chunk[i * 2] = chunk[i * 2 + 1] = static_cast<float>(0.5 * std::sin(phase));
                analyzer.push(chunk.data(), 480);
            }
        });
        float last = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const auto &s = analyzer.update(0.02);
            for (float v : s.left64)
                QVERIFY(v >= 0.0f && v <= 1.0f && std::isfinite(v));
            last = *std::max_element(s.left64.begin(), s.left64.end());
            QTest::qWait(1);
        }
        stop = true;
        producer.join();
        QVERIFY(last > 0.5f);
        // reset forgets what was pushed before it
        analyzer.reset();
        analyzer.update(5.0);
        QVERIFY(*std::max_element(analyzer.current().left64.begin(), analyzer.current().left64.end()) < 0.01f);
    }

    void sceneAudioStartsCaptureOnlyWhenTheSpectrumIsWanted()
    {
        auto backendA = std::make_unique<FakeBackend>();
        FakeBackend *a = backendA.get();
        {
            std::vector<SoundTrack> tracks;
            tracks.push_back({"a", audiofix::sineMp3(), 1.0f, true});
            SceneAudio audio(std::move(tracks), false, std::move(backendA));
            QCOMPARE(a->playbackStarts, 1);
            QCOMPARE(a->captureStarts, 0); // nothing reads the spectrum: no capture
            QVERIFY(audio.playing());
        }
        auto backendB = std::make_unique<FakeBackend>();
        FakeBackend *b = backendB.get();
        SceneAudio audio({}, true, std::move(backendB));
        QCOMPARE(b->playbackStarts, 0); // no sound objects: no playback stream
        QCOMPARE(b->captureStarts, 1);
        // the capture callback feeds the spectrum
        const auto tone = sine(2000.0, 4096, 1.0f);
        b->m_push(tone.data(), 4096);
        QVERIFY(*std::max_element(audio.spectrum(1.0).left16.begin(), audio.spectrum(0.0).left16.end()) > 0.5f);
        // pausing closes the capture and clears what was heard; resuming opens it again
        audio.setPaused(true);
        QCOMPARE(b->captureStops, 1);
        QVERIFY(!b->m_push);
        QVERIFY(*std::max_element(audio.spectrum(0.0).left16.begin(), audio.spectrum(0.0).left16.end()) == 0.0f);
        audio.setPaused(false);
        QCOMPARE(b->captureStarts, 2);
    }

    void withoutABackendThereIsSilenceNotAFailure()
    {
        std::vector<SoundTrack> tracks;
        tracks.push_back({"a", audiofix::sineMp3(), 1.0f, true});
        SceneAudio audio(std::move(tracks), true, nullptr);
        QVERIFY(!audio.playing());
        QCOMPARE(*std::max_element(audio.spectrum(1.0).left16.begin(), audio.spectrum(0.0).left16.end()), 0.0f);
        audio.setVolume(64);
        audio.setPaused(true);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkAudio)
#include "tst_scenevk_audio.moc"
