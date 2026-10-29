// A short mutation smoke over the binary readers: synthetic fixtures are corrupted (flipped bytes, extreme 32-bit fields,
// truncation, spliced and dropped regions) and fed to the parsers. The pass criterion is "no crash and no sanitizer
// report", so this is only worth running under the asan preset. MURASAKI_MUTATE_SECONDS sets the time per reader (default
// 1); MURASAKI_MUTATE_SEED changes the sequence.

#include "animfixtures.h"
#include "animationinfo.h"
#include "assets/imagedecode.h"
#include "audio/soundstream.h"
#include "audiofixtures.h"
#include "assets/pkg.h"
#include "assets/tex.h"
#include "puppet/mdl.h"
#include "puppetfixtures.h"
#include "scenevkfixtures.h"
#include "testutil.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <random>

using namespace murasaki;

namespace {

using Blob = std::vector<uint8_t>;

class Mutator
{
public:
    explicit Mutator(uint64_t seed) : m_rng(seed) {}

    Blob mutate(const Blob &seed)
    {
        Blob b = seed;
        const int rounds = 1 + static_cast<int>(m_rng() % 4);
        for (int r = 0; r < rounds && !b.empty(); ++r) {
            switch (m_rng() % 6) {
            case 0: // flip a few bytes
                for (int i = 0, n = 1 + static_cast<int>(m_rng() % 4); i < n; ++i)
                    b[m_rng() % b.size()] ^= static_cast<uint8_t>(1 + m_rng() % 255);
                break;
            case 1: { // an extreme 32-bit field, little or big endian
                static constexpr uint32_t kExtreme[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x10000u, 0x7fffu};
                if (b.size() < 4)
                    break;
                uint32_t v = kExtreme[m_rng() % std::size(kExtreme)];
                if (m_rng() & 1)
                    v = __builtin_bswap32(v);
                std::memcpy(b.data() + m_rng() % (b.size() - 3), &v, 4);
                break;
            }
            case 2: // truncate
                b.resize(m_rng() % (b.size() + 1));
                break;
            case 3: { // splice a region somewhere else
                const size_t from = m_rng() % b.size(), len = std::min<size_t>(1 + m_rng() % 64, b.size() - from);
                const Blob piece(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(from + len));
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(m_rng() % (b.size() + 1)), piece.begin(), piece.end());
                break;
            }
            case 4: { // drop a region
                const size_t from = m_rng() % b.size(), len = std::min<size_t>(1 + m_rng() % 64, b.size() - from);
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(from + len));
                break;
            }
            default: { // a run of one byte
                const size_t from = m_rng() % b.size(), len = std::min<size_t>(1 + m_rng() % 16, b.size() - from);
                std::memset(b.data() + from, static_cast<int>(m_rng() % 256), len);
                break;
            }
            }
        }
        return b;
    }

private:
    std::mt19937_64 m_rng;
};

double secondsPerReader()
{
    bool ok = false;
    const double v = qEnvironmentVariable("MURASAKI_MUTATE_SECONDS").toDouble(&ok);
    return ok && v > 0 ? v : 1.0;
}

uint64_t seedBase()
{
    bool ok = false;
    const uint64_t v = qEnvironmentVariable("MURASAKI_MUTATE_SEED").toULongLong(&ok);
    return ok ? v : 1;
}

Blob toBlob(const QByteArray &a)
{
    return Blob(a.begin(), a.end());
}

// Runs `feed` on mutations of every seed until the time is up; returns how many inputs were tried.
template<typename Feed>
int run(const std::vector<Blob> &seeds, uint64_t salt, Feed feed)
{
    Mutator mutator(seedBase() * 0x9e3779b97f4a7c15ull + salt);
    QElapsedTimer timer;
    timer.start();
    int count = 0;
    const double budgetMs = secondsPerReader() * 1000.0;
    while (static_cast<double>(timer.elapsed()) < budgetMs) {
        for (const Blob &seed : seeds) {
            feed(mutator.mutate(seed));
            ++count;
        }
    }
    return count;
}

} // namespace

class TstHostileMutation : public QObject
{
    Q_OBJECT
private slots:
    void mdl()
    {
        using namespace puppetfix;
        const std::vector<Blob> seeds = {
            makeMdl({{"root", -1, 0, 0}, {"b", 0, 1, 1}}, {{1, "a", "loop", 30.0f, 2, {{{frame(0, 0), frame(5, 0), frame(9, 0)}}, {{frame(0, 0), frame(0, 0)}}}}}),
            makeMdl({{"root", -1, 0, 0}, {"b", 0, 1, 1}}, {}, true, "MDLV0021"),
        };
        const int n = run(seeds, 1, [](const Blob &b) {
            const auto puppet = scene::parsePuppet(b);
            if (puppet) {
                (void)scene::puppetBoneMatrices(*puppet, {{1, 1.0f, 1.0f}, {2, 0.5f, 2.0f}}, 3.7);
            }
        });
        QVERIFY(n > 0);
        qInfo("mdl: %d inputs", n);
    }

    void tex()
    {
        using namespace scenevkfix;
        const std::vector<Blob> seeds = {makeTex(simpleRgba(8, 8)), makeTex(simpleRgba(3, 5))};
        const int n = run(seeds, 2, [](const Blob &b) {
            (void)scene::parseTex(b);
            (void)scene::parseTex(b, scene::TexReadOptions{false});
        });
        QVERIFY(n > 0);
        qInfo("tex: %d inputs", n);
    }

    void pkg()
    {
        using namespace scenevkfix;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("scene.pkg"));
        const std::vector<Blob> seeds = {makePkg({{"scene.json", "{}"}, {"materials/a.json", "{\"x\":1}"}, {"textures/a.tex", std::string(300, 'x')}})};
        const int n = run(seeds, 3, [&](const Blob &b) {
            testutil::writeFile(path, QByteArray(reinterpret_cast<const char *>(b.data()), static_cast<qsizetype>(b.size())));
            if (const auto archive = scene::PkgArchive::open(path.toStdString())) {
                for (const auto &entry : archive->entries())
                    (void)archive->read(entry.name);
                (void)archive->find("SCENE.JSON");
            }
        });
        QVERIFY(n > 0);
        qInfo("pkg: %d inputs", n);
    }

    void animationInfo()
    {
        using namespace animfixtures;
        const std::vector<Blob> seeds = {toBlob(apng(40, 30, {{1, 60}, {1, 30}, {1, 10}})), toBlob(gif(40, 30, {5, 10, 20})),
                                         toBlob(webpAnimated(40, 30, {40, 50, 60})), toBlob(webpStillLossless(16, 16))};
        const int n = run(seeds, 4, [](const Blob &b) {
            QByteArray data(reinterpret_cast<const char *>(b.data()), static_cast<qsizetype>(b.size()));
            for (auto *reader : {&readApngInfo, &readGifInfo, &readWebpInfo}) {
                QBuffer buffer(&data);
                buffer.open(QIODevice::ReadOnly);
                (void)reader(&buffer);
            }
        });
        QVERIFY(n > 0);
        qInfo("animationinfo: %d inputs", n);
    }

    void soundStream()
    {
        const std::vector<Blob> seeds = {audiofix::sineMp3(), audiofix::sineWav(8000, 0.1, 1), audiofix::sineFlac(), audiofix::sineOgg()};
        const int n = run(seeds, 6, [](const Blob &b) {
            auto stream = scene::SoundStream::open(b, 48000);
            std::vector<float> out(2 * 512);
            for (int i = 0; stream && i < 30; ++i) {
                if (stream->read(out.data(), 512) == 0) {
                    stream->rewind();
                }
            }
        });
        QVERIFY(n > 0);
        qInfo("soundstream: %d inputs", n);
    }

    void imageDecode()
    {
        using namespace scenevkfix;
        const std::vector<Blob> seeds = {makePng(6, 4, rgbaPattern(6, 4))};
        const int n = run(seeds, 5, [](const Blob &b) {
            uint32_t w = 0, h = 0;
            (void)scene::imageSize(b, w, h);
            scene::DecodedImage out;
            std::string error;
            (void)scene::decodeImage(b, 4096, 16u << 20, out, error);
        });
        QVERIFY(n > 0);
        qInfo("imagedecode: %d inputs", n);
    }
};

QTEST_MAIN(TstHostileMutation)
#include "tst_hostile_mutation.moc"
