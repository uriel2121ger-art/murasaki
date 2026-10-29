// murasaki-scene-vk media: the state the host sends, the colours of a cover and the sealed memfd that carries it.

#include "bridge/mediaview.h"
#include "media/mediastate.h"

#include <QTest>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace murasaki::scene;
using namespace murasaki::bridge;

class TstSceneVkMedia : public QObject
{
    Q_OBJECT

private slots:
    void aStateIsReadAndBounded()
    {
        const auto s = parseMediaState(R"({"enabled":true,"state":"playing","title":"Song\tOne\u0007","artist":"A","albumTitle":"B",
            "albumArtist":"C","genres":["x","y"],"position":12.5,"rate":2,"duration":200,"unknown":1})");
        QVERIFY(s.has_value());
        QVERIFY(s->enabled);
        QCOMPARE(s->playback, MediaPlayback::Playing);
        QCOMPARE(s->title, std::string("Song One")); // the tab is a space, the bell is gone
        QCOMPARE(s->genres.size(), size_t{2});
        QCOMPARE(s->position, 12.5);
        QCOMPARE(s->rate, 2.0);
        QCOMPARE(s->duration, 200.0);

        const auto big = parseMediaState(R"({"enabled":true,"title":")" + std::string(1000, 'a') + R"(","position":1e12,"rate":-4,"duration":"x",
            "genres":["1","2","3","4","5","6","7","8","9","10"]})");
        QVERIFY(big.has_value());
        QCOMPARE(big->title.size(), kMaxMediaText);
        QCOMPARE(big->position, 86400.0);
        QCOMPARE(big->rate, 0.0);
        QCOMPARE(big->duration, 0.0); // not a number: the default
        QCOMPARE(big->genres.size(), kMaxMediaGenres);
        // a cut never leaves half a character: 255 ASCII bytes then a 2-byte letter would straddle the limit
        const auto multibyte = parseMediaState(R"({"enabled":true,"title":")" + std::string(255, 'a') + R"(é"})");
        QCOMPARE(multibyte->title.size(), size_t{255});

        QVERIFY(!parseMediaState("[1]"));
        QVERIFY(!parseMediaState("not json"));
        const auto off = parseMediaState(R"({"enabled":false,"title":"left over"})");
        QVERIFY(off && !off->enabled && off->title.empty()); // without a player nothing else counts
    }

    void thePositionMovesOnlyWhilePlayingAndStopsAtTheEnd()
    {
        MediaState s;
        s.position = 10;
        s.rate = 2;
        s.duration = 30;
        s.playback = MediaPlayback::Paused;
        QCOMPARE(extrapolatedPosition(s, 5.0), 10.0);
        s.playback = MediaPlayback::Playing;
        QCOMPARE(extrapolatedPosition(s, 5.0), 20.0);
        QCOMPARE(extrapolatedPosition(s, 50.0), 30.0);
        s.duration = 0;
        QCOMPARE(extrapolatedPosition(s, 50.0), 110.0);
    }

    void coloursOfACoverAreDeterministic()
    {
        // 100 x 100: 60 % red, 30 % blue, 10 % transparent green
        std::vector<uint8_t> px;
        for (int i = 0; i < 10000; ++i) {
            if (i % 10 < 6)
                px.insert(px.end(), {250, 10, 10, 255});
            else if (i % 10 < 9)
                px.insert(px.end(), {10, 10, 250, 255});
            else
                px.insert(px.end(), {10, 250, 10, 0});
        }
        const MediaColors c = mediaColors(100, 100, px.data());
        QVERIFY(c.primary[0] > 0.9f && c.primary[2] < 0.1f);
        QVERIFY(c.secondary[2] > 0.9f && c.secondary[0] < 0.1f);
        QCOMPARE(c.tertiary, c.primary); // the green is not opaque: no third colour, so the primary again
        // red luma 0.21 is dark: white text; the farthest luma from it is the white text itself
        QCOMPARE(c.text, (std::array<float, 3>{1, 1, 1}));
        QCOMPARE(c.highContrast, c.text);
        const MediaColors again = mediaColors(100, 100, px.data());
        QCOMPARE(again.primary, c.primary);
        // a light picture gets black text; nothing opaque gives grey
        const std::vector<uint8_t> white(16 * 4, 255);
        QCOMPARE(mediaColors(4, 4, white.data()).text, (std::array<float, 3>{0, 0, 0}));
        const std::vector<uint8_t> clear(16 * 4, 0);
        QCOMPARE(mediaColors(4, 4, clear.data()).primary, (std::array<float, 3>{0.5f, 0.5f, 0.5f}));
        QCOMPARE(mediaColors(0, 0, nullptr).primary, (std::array<float, 3>{0.5f, 0.5f, 0.5f}));
    }

    void aSealedMemfdCarriesTheCover()
    {
        std::vector<uint8_t> px(4 * 3 * 4);
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = static_cast<uint8_t>(i);
        UniqueFd fd = sealedPixelsFd(px);
        QVERIFY(fd.valid());
        const int seals = ::fcntl(fd.get(), F_GET_SEALS);
        QVERIFY((seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
        QCOMPARE(::write(fd.get(), "x", 1), ssize_t{-1}); // sealed against writes
        UniqueFd dup(::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0));
        const auto back = readSealedPixels(std::move(dup), 4, 3);
        QVERIFY(back.has_value());
        QCOMPARE(*back, px);
        // the size must match what was declared
        UniqueFd dup2(::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0));
        QVERIFY(!readSealedPixels(std::move(dup2), 4, 4));
        UniqueFd dup3(::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0));
        QVERIFY(!readSealedPixels(std::move(dup3), kMaxThumbnailSide + 1, 1));
    }

    void anUnsealedOrWrongFileIsRefused()
    {
        // a memfd nobody sealed: the sender could still change it while the engine reads
        UniqueFd open(::memfd_create("t", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        QVERIFY(open.valid());
        const std::vector<uint8_t> px(16, 7);
        QCOMPARE(::write(open.get(), px.data(), px.size()), ssize_t(px.size()));
        QVERIFY(!readSealedPixels(std::move(open), 2, 2));
        // a pipe is not a regular file
        int p[2];
        QCOMPARE(::pipe(p), 0);
        UniqueFd r(p[0]), w(p[1]);
        QVERIFY(!readSealedPixels(std::move(r), 1, 1));
        QVERIFY(!readSealedPixels(UniqueFd(), 1, 1));
    }

    void theThumbnailMessageIsValidatedByItsFds()
    {
        using msg::MediaThumbnail;
        QVERIFY(msg::validate(MediaThumbnail{0, 0, 1, 0}, 0));
        QVERIFY(!msg::validate(MediaThumbnail{0, 0, 1, 0}, 1));
        QVERIFY(msg::validate(MediaThumbnail{512, 512, 1, 0}, 1));
        QVERIFY(!msg::validate(MediaThumbnail{512, 512, 1, 0}, 0));
        QVERIFY(!msg::validate(MediaThumbnail{512, 512, 1, 0}, 2));
        QVERIFY(!msg::validate(MediaThumbnail{513, 4, 1, 0}, 1));
        QVERIFY(!msg::validate(MediaThumbnail{4, 0, 1, 0}, 1));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkMedia)
#include "tst_scenevk_media.moc"
