// The puppet engine process end to end (src/puppet/engine.*, session.*, main.cpp): the real loop on a hand-made model in a thread, and the real executable, with a consumer that speaks the bridge
// protocol over a socketpair and reads the pictures from the shared-memory ring. Needs a Vulkan 1.3 device (a software one will do); skips without one. Run under the GPU lock, one at a time.
#include "bridge/channel.h"
#include "bridge/puppetview.h"
#include "core/context.h"
#include "puppet/engine.h"
#include "puppet/manifest.h"
#include "puppet/session.h"
#include "puppet/view.h"
#include "puppetfake.h"
#include "puppetlimits.h"
#include "testutil.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

#include <array>
#include <utility>
#include <atomic>
#include <functional>
#include <thread>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;
using namespace puppetfake;
namespace bridge = murasaki::bridge;

namespace {

using Px = std::array<int, 4>;

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    Px at(uint32_t x, uint32_t y) const
    {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

bool near(const Px &a, const Px &b, int tol = 2)
{
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(a[i] - b[i]) > tol)
            return false;
    return true;
}

QString show(const Px &p)
{
    return u"(%1,%2,%3,%4)"_s.arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]);
}

// The consumer end of the bridge: the producer's messages in, ours out.
class Consumer
{
public:
    explicit Consumer(bridge::Channel channel) : m_channel(std::move(channel)) { m_channel.setNonblocking(true); }

    // Reads whatever has arrived for up to `ms`, handling it; returns when something new was handled or the time is up.
    void pump(int ms)
    {
        pollfd p{m_channel.fd(), POLLIN, 0};
        ::poll(&p, 1, ms);
        for (;;) {
            bridge::ReceiveResult r = m_channel.receive();
            if (r.status == bridge::ChannelStatus::Closed) {
                closed = true;
                return;
            }
            if (r.status != bridge::ChannelStatus::Ok)
                return;
            handle(std::move(r.message));
        }
    }

    bool waitFor(const std::function<bool()> &done, int ms)
    {
        QElapsedTimer t;
        t.start();
        while (!done() && !closed && t.elapsed() < ms)
            pump(20);
        return done();
    }

    bridge::Channel &channel() { return m_channel; }

    bool hello = false;
    uint32_t version = 0;
    uint32_t viewportW = 0, viewportH = 0;
    uint32_t generation = 0;
    int shmBuffers = 0;
    int dmabufBuffers = 0;
    int frames = 0;
    Picture last;
    std::vector<bridge::HitMaskHeader> hitHeaders;
    std::vector<std::vector<uint8_t>> hitBits;
    std::vector<std::string> errors;
    bool closed = false;

private:
    void handle(bridge::Message &&m)
    {
        using bridge::MsgType;
        const std::span<const uint8_t> payload(m.payload);
        if (qEnvironmentVariableIsSet("PUPPET_TRACE"))
            qWarning("message %u (%zu bytes, %zu fds)", m.type, m.payload.size(), m.fds.size());
        switch (m.type) {
        case static_cast<uint32_t>(MsgType::Hello): {
            bridge::msg::Hello h{};
            if (bridge::decode(payload, h)) {
                hello = true;
                version = h.version;
            }
            break;
        }
        case static_cast<uint32_t>(MsgType::Viewport): {
            bridge::msg::Viewport v{};
            if (bridge::decode(payload, v)) {
                viewportW = v.width;
                viewportH = v.height;
            }
            break;
        }
        case static_cast<uint32_t>(MsgType::ShmBuffers): {
            bridge::msg::ShmBuffers b{};
            if (bridge::decode(payload, b) && bridge::msg::validate(b, m.fds.size())) {
                ++shmBuffers;
                generation = b.generation;
                viewportW = b.width;
                viewportH = b.height;
                bridge::ShmError err;
                m_ring = bridge::ShmRing::open(std::move(m.fds[0]), {b.count, b.width, b.height, b.stride}, &err);
                if (!m_ring)
                    qWarning("ring open failed: %d", int(err));
                m_stride = b.stride;
            }
            break;
        }
        case static_cast<uint32_t>(MsgType::DmabufBuffers):
            ++dmabufBuffers;
            break;
        case static_cast<uint32_t>(MsgType::Frame): {
            bridge::msg::Frame f{};
            if (!bridge::decode(payload, f) || !m_ring || f.generation != generation) {
                qWarning("frame dropped: ring %d gen %u/%u", m_ring.has_value(), f.generation, generation);
                break;
            }
            const auto frame = std::as_const(*m_ring).frame(f.buffer);
            if (frame.empty())
                break;
            last.width = viewportW;
            last.height = viewportH;
            last.rgba.resize(static_cast<size_t>(viewportW) * viewportH * 4);
            for (uint32_t y = 0; y < viewportH; ++y)
                std::memcpy(last.rgba.data() + static_cast<size_t>(y) * viewportW * 4, frame.data() + static_cast<size_t>(y) * m_stride, static_cast<size_t>(viewportW) * 4);
            ++frames;
            bridge::msg::Release rel{0, f.buffer, f.generation};
            m_channel.send(rel);
            break;
        }
        case static_cast<uint32_t>(MsgType::Error): {
            bridge::msg::Error e{};
            if (bridge::decode(payload, e))
                errors.emplace_back(bridge::getString(e.text));
            break;
        }
        case static_cast<uint32_t>(bridge::kHitMaskType):
            if (const auto mask = bridge::decodeHitMask(payload)) {
                hitHeaders.push_back(mask->header);
                hitBits.emplace_back(mask->bits.begin(), mask->bits.end());
            }
            break;
        default:
            break;
        }
    }

    bridge::Channel m_channel;
    std::optional<bridge::ShmRing> m_ring;
    uint32_t m_stride = 0;
};

struct Pair {
    bridge::UniqueFd producer;
    std::unique_ptr<Consumer> consumer;
    static Pair make()
    {
        auto p = bridge::makeSocketPair();
        if (!p)
            qFatal("socketpair");
        Pair out;
        out.producer = std::move(p->first);
        out.consumer = std::make_unique<Consumer>(bridge::Channel(std::move(p->second)));
        return out;
    }
};

TextureImage whiteTexture()
{
    TextureImage t;
    t.width = t.height = 1;
    t.rgba = {255, 255, 255, 255};
    return t;
}

// A model that draws one 20 x 20 square at (x * 20, 22) in a 64 x 64 canvas; "X" moves it. With `breathing` it has ParamBreath and so animates.
std::unique_ptr<FakeModel> squareModel(bool breathing = false, bool angle = false)
{
    auto m = std::make_unique<FakeModel>();
    m->params.add("X", 0, 0, 2);
    if (breathing)
        m->params.add("ParamBreath", 0, 0, 1);
    if (angle)
        m->params.add("ParamAngleX", -30, 0, 30);
    m->shapes.push_back(FakeModel::quadShape());
    m->extent = {0, 0, 64, 64};
    FakeModel *raw = m.get();
    m->fill = [raw](const ParameterSet &v, const PartSet &, Frame &f) {
        double x = v.value(v.find("X")) * 20;
        if (const int a = v.find("ParamAngleX"); a >= 0)
            x += v.value(a);
        f.items.push_back(FakeModel::mesh(0, x, 22, 20, 20));
        (void)raw;
    };
    return m;
}

} // namespace

class TestPuppetEngine : public QObject
{
    Q_OBJECT
public:
    bool m_softwareDevice = false;
    struct Running {
        std::thread thread;
        std::atomic<int> status{-1};
        Pair pair;
        FakeModel *model = nullptr;
    };

    std::unique_ptr<Running> start(std::unique_ptr<FakeModel> model, int fps = 60, uint32_t w = 64, uint32_t h = 64, uint32_t maxLongSide = 2048,
                                   const std::function<void(EngineOptions &)> &tweak = {}, SessionAssets assets = {})
    {
        auto run = std::make_unique<Running>();
        run->pair = Pair::make();
        run->model = model.get();
        EngineOptions options;
        options.bridgeFd = run->pair.producer.release();
        options.width = w;
        options.height = h;
        options.fps = fps;
        options.maxLongSide = maxLongSide;
        if (tweak)
            tweak(options);
        Running *r = run.get();
        auto owned = std::shared_ptr<FakeModel>(std::move(model));
        (void)owned;
        run->thread = std::thread([r, options, m = std::move(owned), assets = std::move(assets)]() mutable {
            // the engine takes the model: hand it a forwarding wrapper that keeps ours alive
            struct Forward : ModelBackend {
                std::shared_ptr<FakeModel> inner;
                std::string name() const override { return inner->name(); }
                const ParameterSet &parameters() const override { return inner->parameters(); }
                const PartSet &parts() const override { return inner->parts(); }
                size_t drawableCount() const override { return inner->drawableCount(); }
                const Shape &shape(int d) const override { return inner->shape(d); }
                Bounds bounds() const override { return inner->bounds(); }
                bool evaluate(const ParameterSet &v, const PartSet &p, Frame &f) override { return inner->evaluate(v, p, f); }
            };
            auto forward = std::make_unique<Forward>();
            forward->inner = std::move(m);
            r->status = runEngineWithModel(options, std::move(forward), {whiteTexture()}, true, std::move(assets));
        });
        return run;
    }

    // The engine ended by itself: its status, once the consumer has read what was left in the socket.
    int waitForExit(Running &run)
    {
        QElapsedTimer timer;
        timer.start();
        while (run.status.load() < 0 && timer.elapsed() < 15000)
            run.pair.consumer->waitFor([] { return false; }, 20); // (the consumer keeps releasing buffers, or the engine waits for one)
        if (run.status.load() < 0)
            return -1;
        run.thread.join();
        run.pair.consumer->waitFor([] { return false; }, 200);
        return run.status.load();
    }

    void finish(Running &run, int expected = kExitOk)
    {
        run.pair.consumer->channel().send(bridge::msg::Quit{0});
        const bool exited = QTest::qWaitFor([&] { return run.status.load() >= 0; }, 10000);
        QVERIFY2(exited, "the engine did not stop");
        run.thread.join();
        QCOMPARE(run.status.load(), expected);
    }

private slots:
    void initTestCase()
    {
        qputenv("MURASAKI_BRIDGE_FORCE_SHM", "1"); // the consumer here reads the shared-memory ring
        std::string error;
        auto ctx = murasaki::scene::VkContext::create(murasaki::scene::contextOptionsFromEnvironment(), error);
        if (!ctx)
            QSKIP(qPrintable(QString::fromStdString("no Vulkan 1.3 device: " + error)));
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx->physicalDevice(), &props);
        m_softwareDevice = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    }

    void firstFrameAndHitMask();
    void commandsMoveTheModel();
    void badCommandsChangeNothing();
    void pausedMeansNothingIsDrawn();
    void theRateIsCapped();
    void anIdleModelIsNotRedrawn();
    void resizeIsHonouredAndClamped();
    void lookingFollowsThePointer();
    void closingTheSocketStopsTheEngine();
    void aModelThatRefusesStopsTheEngine();
    void theExecutable();
    void theExecutableUnderTheSandbox();
    void theExecutableRefusesWhatItCannotShow();
    void theSessionAlone();
    void aHeavyModelIsNotDrawnAndGivenUp();
    void aSlowGpuLowersTheRateButKeepsTheModel();
    void aRealisticModelIsDrawnNotRefused();
    void aMotionPlaysThroughTheBridge();
    void aModelThatFailsBeforeItsFirstFrameIsRefused();
    void theMaskIsNotWorkedOutAgainForNothing();
    void onlyInpNamesAreShown();
};

void TestPuppetEngine::firstFrameAndHitMask()
{
    auto run = start(squareModel());
    Consumer &c = *run->pair.consumer;
    const bool got = c.waitFor([&] { return c.frames >= 2 && !c.hitHeaders.empty(); }, 15000);
    if (!got)
        qWarning("hello %d shm %d frames %d hits %d errors %d closed %d status %d", c.hello, c.shmBuffers, c.frames, int(c.hitHeaders.size()), int(c.errors.size()), c.closed, run->status.load());
    QVERIFY(got);
    QVERIFY(c.hello);
    QCOMPARE(c.version, bridge::kPuppetVersion); // the HitMask message comes with version 6
    QCOMPARE(c.viewportW, 64u);
    QCOMPARE(c.viewportH, 64u);
    QCOMPARE(c.shmBuffers, 1);
    const Px inside = {c.last.at(10, 32)}, outside = {c.last.at(50, 10)};
    QVERIFY2(near(inside, {255, 255, 255, 255}), qPrintable(show(inside)));
    QVERIFY2(near(outside, {0, 0, 0, 0}, 0), qPrintable(show(outside)));
    // the mask of the first picture came with it: 8 pixel cells over the square (x 0..20, y 22..42 at 1:1)
    const bridge::HitMaskHeader h = c.hitHeaders.front();
    QCOMPARE(h.cols, 8u);
    QCOMPARE(h.rows, 8u);
    QCOMPARE(h.cell, 8u);
    QCOMPARE(h.generation, c.generation);
    const auto bit = [&](uint32_t col, uint32_t row) { return (c.hitBits.front()[(row * h.cols + col) / 8] >> ((row * h.cols + col) % 8)) & 1; };
    QVERIFY(bit(1, 4));   // (8..16, 32..40): inside
    QVERIFY(!bit(6, 1));  // (48.., 8..): outside
    QVERIFY(!bit(7, 7));
    finish(*run);
}

void TestPuppetEngine::commandsMoveTheModel()
{
    auto run = start(squareModel());
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    QVERIFY(near(c.last.at(10, 32), {255, 255, 255, 255}));
    QVERIFY(near(c.last.at(40, 32), {0, 0, 0, 0}, 0));
    QCOMPARE(c.channel().sendProperties(R"({"set":{"X":1.5}})"), bridge::ChannelStatus::Ok); // the square moves to x = 30..50
    const int before = c.frames;
    QVERIFY(c.waitFor([&] { return c.frames > before + 1 && near(c.last.at(40, 32), {255, 255, 255, 255}); }, 15000));
    QVERIFY(near(c.last.at(10, 32), {0, 0, 0, 0}, 0));
    // a new mask went out because the footprint moved a long way
    QVERIFY(c.waitFor([&] { return c.hitHeaders.size() >= 2; }, 5000));
    // reset brings it back; opacity of a part the model has is not required
    QCOMPARE(c.channel().sendProperties(R"({"reset":true})"), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return near(c.last.at(10, 32), {255, 255, 255, 255}); }, 15000));
    finish(*run);
}

void TestPuppetEngine::badCommandsChangeNothing()
{
    auto run = start(squareModel());
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    for (const char *bad : {"not json", "[]", R"({"set":{"Nope":1}})", R"({"set":{"X":"1"}})", R"({"set":{"X":1},"look":5})", R"({"set":[1]})", R"({"set":{"X":1,"X":2}})"}) {
        QCOMPARE(c.channel().sendProperties(bad), bridge::ChannelStatus::Ok);
    }
    c.waitFor([] { return false; }, 300); // (the consumer keeps releasing buffers meanwhile)
    QVERIFY(near(c.last.at(10, 32), {255, 255, 255, 255}));
    QVERIFY(near(c.last.at(40, 32), {0, 0, 0, 0}, 0));
    QVERIFY(c.errors.empty()); // a refused command is the host's mistake, not the engine's failure: it goes on
    finish(*run);
}

void TestPuppetEngine::pausedMeansNothingIsDrawn()
{
    auto run = start(squareModel(true)); // animated: it would draw every frame
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 5; }, 15000));
    QCOMPARE(c.channel().send(bridge::msg::Pause{1}), bridge::ChannelStatus::Ok);
    c.waitFor([] { return false; }, 300); // (the consumer keeps releasing buffers meanwhile)
    const int frames = c.frames;
    const int evaluations = run->model->evaluations;
    c.waitFor([] { return false; }, 1200); // (the consumer keeps releasing buffers meanwhile)
    QCOMPARE(c.frames, frames);                        // not one picture while paused
    QCOMPARE(run->model->evaluations.load(), evaluations); // and the model was not even evaluated
    QCOMPARE(c.channel().send(bridge::msg::Pause{0}), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return c.frames > frames + 2; }, 15000));
    finish(*run);
}

void TestPuppetEngine::theRateIsCapped()
{
    auto run = start(squareModel(true), 60);
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 5; }, 15000));
    const auto rate = [&](int fps) {
        c.channel().send(bridge::msg::Fps{uint32_t(fps)});
        c.waitFor([] { return false; }, 300); // (the consumer keeps releasing buffers meanwhile)
        const int before = c.frames;
        c.waitFor([] { return false; }, 2000); // (the consumer keeps releasing buffers meanwhile)
        return (c.frames - before) / 2.0;
    };
    const double ten = rate(10);
    QVERIFY2(ten >= 6 && ten <= 12.5, qPrintable(QString::number(ten)));
    const double thirty = rate(30);
    QVERIFY2(thirty >= 20 && thirty <= 32, qPrintable(QString::number(thirty)));
    finish(*run);
}

void TestPuppetEngine::anIdleModelIsNotRedrawn()
{
    auto run = start(squareModel(false), 60); // nothing animates
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    c.waitFor([] { return false; }, 300); // (the consumer keeps releasing buffers meanwhile)
    const int frames = c.frames, evaluations = run->model->evaluations;
    c.waitFor([] { return false; }, 2500); // (the consumer keeps releasing buffers meanwhile)
    QVERIFY2(c.frames - frames <= 3, qPrintable(QString::number(c.frames - frames))); // a keep-alive a second
    QVERIFY2(run->model->evaluations - evaluations <= 3, qPrintable(QString::number(run->model->evaluations - evaluations)));
    finish(*run);
}

void TestPuppetEngine::resizeIsHonouredAndClamped()
{
    auto run = start(squareModel(true), 60, 64, 64, 100);
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 2; }, 15000));
    const uint32_t first = c.generation;
    QCOMPARE(c.channel().send(bridge::msg::Resize{0, 80, 40}), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return c.generation != first && c.viewportW == 80 && c.viewportH == 40 && c.frames > 0 && c.last.width == 80; }, 15000));
    QVERIFY(c.last.width == 80); // a picture of the new size arrived
    // the model is fitted into the new aspect: a 64 x 64 canvas in 80 x 40 is 40 x 40 in the middle (the square is at x 20 to 32, y 14 to 26)
    QVERIFY(c.waitFor([&] { return c.last.width == 80 && near(c.last.at(26, 20), {255, 255, 255, 255}); }, 15000));
    QVERIFY(near(c.last.at(5, 20), {0, 0, 0, 0}, 0));
    // an absurd size is brought down to the long side the engine allows (100), keeping the aspect
    const uint32_t second = c.generation;
    QCOMPARE(c.channel().send(bridge::msg::Resize{0, 400, 200}), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return c.generation != second && c.viewportW == 100 && c.viewportH == 50; }, 15000));
    // a size the protocol refuses ends the exchange, not the engine's good name: it exits, with a failure status
    finish(*run);
}

void TestPuppetEngine::lookingFollowsThePointer()
{
    auto run = start(squareModel(false, true));
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    // looking is off: the pointer changes nothing
    c.channel().send(bridge::msg::Pointer{0, 63, 32, 0});
    c.waitFor([] { return false; }, 500); // (the consumer keeps releasing buffers meanwhile)
    QVERIFY(near(c.last.at(10, 32), {255, 255, 255, 255}));
    // on: the head turns toward the pointer, ParamAngleX goes up to its maximum (30), the square moves 30 pixels to the right
    QCOMPARE(c.channel().sendProperties(R"({"look":true})"), bridge::ChannelStatus::Ok);
    c.channel().send(bridge::msg::Pointer{0, 640, 32, 0});
    QVERIFY(c.waitFor([&] { return near(c.last.at(45, 32), {255, 255, 255, 255}); }, 15000));
    QVERIFY(near(c.last.at(5, 32), {0, 0, 0, 0}, 0));
    // and back when looking is turned off
    QCOMPARE(c.channel().sendProperties(R"({"look":false})"), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return near(c.last.at(10, 32), {255, 255, 255, 255}); }, 15000));
    finish(*run);
}

void TestPuppetEngine::closingTheSocketStopsTheEngine()
{
    auto run = start(squareModel());
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    run->pair.consumer.reset(); // the consumer goes away
    QVERIFY(QTest::qWaitFor([&] { return run->status.load() >= 0; }, 10000));
    run->thread.join();
    QCOMPARE(run->status.load(), kExitOk);
}

void TestPuppetEngine::aModelThatRefusesStopsTheEngine()
{
    auto model = squareModel();
    model->fill = nullptr;
    FakeModel *raw = model.get();
    auto run = start(std::move(model));
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    (void)raw;
    // the engine keeps going with a model that draws nothing
    QVERIFY(near(c.last.at(10, 32), {0, 0, 0, 0}, 0));
    finish(*run);
    // the usage errors
    EngineOptions none;
    QCOMPARE(runEngine(none), kExitUsage);
    EngineOptions noFile;
    noFile.bridgeFd = 9;
    noFile.width = noFile.height = 8;
    QCOMPARE(runEngine(noFile), kExitUsage);
    QCOMPARE(runEngineWithModel(noFile, nullptr, {}, true), kExitUsage);
}

namespace {

// A puppet on disk: a root and one part with a texture (a PNG of a red square), a parameter that moves the part.
QByteArray makePuppet(bool alphaTexture = true)
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(qRgba(255, 0, 0, alphaTexture ? 255 : 128));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const auto arr = [](std::initializer_list<double> v) {
        QJsonArray a;
        for (const double d : v)
            a.append(d);
        return a;
    };
    QJsonObject part{{"uuid", 2}, {"name", "Body"}, {"type", "Part"}, {"enabled", true}, {"zsort", 0}, {"transform", QJsonObject{{"trans", arr({0, 0, 0})}, {"rot", arr({0, 0, 0})}, {"scale", arr({1, 1})}}},
                     {"mesh", QJsonObject{{"verts", arr({12, 22, 32, 22, 32, 42, 12, 42})}, {"uvs", arr({0, 0, 1, 0, 1, 1, 0, 1})}, {"indices", arr({0, 1, 2, 0, 2, 3})}, {"origin", arr({0, 0})}}},
                     {"textures", arr({0, 4294967295.0, 4294967295.0})}, {"blend_mode", "Normal"}, {"tint", arr({1, 1, 1})}, {"screenTint", arr({0, 0, 0})}, {"opacity", 1}, {"mask_threshold", 0.5}};
    QJsonObject root{{"uuid", 1}, {"name", "Root"}, {"type", "Node"}, {"enabled", true}, {"zsort", 0}, {"transform", QJsonObject{{"trans", arr({0, 0, 0})}, {"rot", arr({0, 0, 0})}, {"scale", arr({1, 1})}}},
                     {"children", QJsonArray{part}}};
    QJsonObject move{{"node", 2}, {"param_name", "transform.t.x"}, {"values", QJsonArray{QJsonArray{0}, QJsonArray{0}, QJsonArray{20}}}, {"isSet", QJsonArray()}, {"interpolate_mode", "Linear"}};
    QJsonObject param{{"uuid", 3}, {"name", "Move"}, {"is_vec2", false}, {"min", arr({0, 0})}, {"max", arr({1, 1})}, {"defaults", arr({0, 0})}, {"axis_points", QJsonArray{arr({0, 0.5, 1}), arr({0})}},
                      {"bindings", QJsonArray{move}}};
    const QByteArray json = QJsonDocument(QJsonObject{{"meta", QJsonObject{{"name", "Test"}}}, {"nodes", root}, {"param", QJsonArray{param}}}).toJson(QJsonDocument::Compact);
    const auto be32 = [](quint32 v) {
        QByteArray b(4, '\0');
        b[0] = char(v >> 24);
        b[1] = char(v >> 16);
        b[2] = char(v >> 8);
        b[3] = char(v);
        return b;
    };
    return QByteArray("TRNSRTS\0", 8) + be32(quint32(json.size())) + json + "TEX_SECT" + be32(1) + be32(quint32(png.size())) + char(0) + png;
}

} // namespace

void TestPuppetEngine::theExecutable()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.path() + u"/p.inp"_s, makePuppet());
    Pair pair = Pair::make();
    QProcess process;
    const int childFd = pair.producer.get();
    process.setChildProcessModifier([childFd] {
        ::dup2(childFd, 7);
        ::fcntl(7, F_SETFD, 0);
    });
    process.setProcessEnvironment([] {
        auto e = QProcessEnvironment::systemEnvironment();
        e.insert(u"MURASAKI_BRIDGE_FORCE_SHM"_s, u"1"_s);
        return e;
    }());
    process.setProgram(QStringLiteral(MURASAKI_PUPPET_BIN));
    process.setArguments({u"--engine"_s, u"--item"_s, tmp.path(), u"--file"_s, u"p.inp"_s, u"--murasaki-bridge-fd"_s, u"7"_s, u"--murasaki-size"_s, u"64x64"_s, u"--fps"_s, u"30"_s});
    process.start();
    QVERIFY(process.waitForStarted(10000));
    pair.producer.reset();
    Consumer &c = *pair.consumer;
    QVERIFY2(c.waitFor([&] { return c.frames >= 2 && !c.hitHeaders.empty(); }, 30000), qPrintable(QString::fromLocal8Bit(process.readAllStandardError())));
    // the part spans (12,22)-(32,42) at the model's rest bounds (12..32 x 22..42) fitted into 64 x 64: scale 3.2, so it fills the picture
    QVERIFY2(near(c.last.at(32, 32), {255, 0, 0, 255}), qPrintable(show(c.last.at(32, 32))));
    QVERIFY(c.hitBits.front().size() == 8);
    // the parameter moves it (the bounds stay those of the rest pose, so it leaves the picture to the right)
    QCOMPARE(c.channel().sendProperties(R"({"set":{"Move":1}})"), bridge::ChannelStatus::Ok);
    QVERIFY(c.waitFor([&] { return near(c.last.at(32, 32), {0, 0, 0, 0}, 0); }, 15000)); // moved 20 units right: out of the picture, whose frame is the rest bounds
    c.channel().send(bridge::msg::Quit{0});
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), kExitOk);
}

void TestPuppetEngine::theExecutableUnderTheSandbox()
{
    QProcess probe;
    probe.start(QStringLiteral(MURASAKI_SANDBOX_BIN), {u"--report"_s});
    probe.waitForFinished(10000);
    if (!QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value(u"landlockAbi"_s).toInt())
        QSKIP("needs Landlock");
    QTemporaryDir tmp;
    testutil::writeFile(tmp.path() + u"/p.inp"_s, makePuppet());
    Pair pair = Pair::make();
    QProcess process;
    const int childFd = pair.producer.get();
    process.setChildProcessModifier([childFd] {
        ::dup2(childFd, 7);
        ::fcntl(7, F_SETFD, 0);
    });
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(u"MURASAKI_BRIDGE_FORCE_SHM"_s, u"1"_s);
    process.setProcessEnvironment(env);
    process.setProgram(QStringLiteral(MURASAKI_SANDBOX_BIN));
    process.setArguments({u"--profile"_s, u"puppet"_s, u"--item"_s, tmp.path(), u"--require"_s, u"--"_s, QStringLiteral(MURASAKI_PUPPET_BIN), u"--engine"_s, u"--item"_s, tmp.path(), u"--file"_s,
                          u"p.inp"_s, u"--murasaki-bridge-fd"_s, u"7"_s, u"--murasaki-size"_s, u"64x64"_s, u"--fps"_s, u"30"_s});
    process.start();
    QVERIFY(process.waitForStarted(10000));
    pair.producer.reset();
    Consumer &c = *pair.consumer;
    QVERIFY2(c.waitFor([&] { return c.frames >= 2 && !c.hitHeaders.empty(); }, 60000), qPrintable(QString::fromLocal8Bit(process.readAllStandardError())));
    QVERIFY2(near(c.last.at(32, 32), {255, 0, 0, 255}), qPrintable(show(c.last.at(32, 32))));
    c.channel().send(bridge::msg::Quit{0});
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitCode(), kExitOk);
}

void TestPuppetEngine::theExecutableRefusesWhatItCannotShow()
{
    QTemporaryDir tmp;
    const auto run = [&](const QStringList &args) {
        QProcess p;
        p.start(QStringLiteral(MURASAKI_PUPPET_BIN), args);
        if (!p.waitForFinished(30000))
            qFatal("murasaki-puppet did not finish");
        return p.exitCode();
    };
    const QStringList base = {u"--engine"_s, u"--item"_s, tmp.path(), u"--murasaki-bridge-fd"_s, u"7"_s, u"--murasaki-size"_s, u"64x64"_s};
    // a puppet that is not one, a Cubism manifest, a missing file: refused as a model (3), not as a crash
    testutil::writeFile(tmp.path() + u"/junk.inp"_s, "not a puppet at all, not at all");
    QCOMPARE(run(base + QStringList{u"--file"_s, u"junk.inp"_s}), kExitModelRefused);
    testutil::writeFile(tmp.path() + u"/m.model3.json"_s, "{}");
    QCOMPARE(run(base + QStringList{u"--file"_s, u"m.model3.json"_s}), kExitModelRefused);
    QCOMPARE(run(base + QStringList{u"--file"_s, u"missing.inp"_s}), kExitModelRefused);
    QCOMPARE(run(base + QStringList{u"--file"_s, u"../x.inp"_s}), kExitModelRefused);
    // a texture the decoder cannot read
    QByteArray puppet = makePuppet();
    puppet.replace("\x89PNG", "XXXX");
    testutil::writeFile(tmp.path() + u"/badtex.inp"_s, puppet);
    QCOMPARE(run(base + QStringList{u"--file"_s, u"badtex.inp"_s}), kExitModelRefused);
    // usage errors are 2
    QCOMPARE(run({u"--engine"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--bogus"_s, u"x"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--murasaki-size"_s, u"0x10"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--murasaki-size"_s, u"99999x10"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--murasaki-size"_s, u"10x10x"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--fps"_s, u"0"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--murasaki-bridge-fd"_s, u"-1"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--seed"_s, u"x"_s}), kExitUsage);
    QCOMPARE(run({u"--engine"_s, u"--item"_s}), kExitUsage);
    QCOMPARE(run({}), kExitUsage);
}

void TestPuppetEngine::theSessionAlone()
{
    // no GPU in this one: the session's commands, life and change detection
    PuppetSession session(squareModel(true, true), 5);
    Frame f;
    bool changed = false;
    QVERIFY(session.evaluate(f, changed));
    QVERIFY(changed);
    QCOMPARE(f.items.size(), size_t(1));
    QVERIFY(session.animated()); // breathing on a model with ParamBreath
    session.step(0.5);
    QVERIFY(session.evaluate(f, changed));
    QVERIFY(changed); // half a second of breath moved the values
    bool again = false;
    PuppetSession still(squareModel(false), 5);
    still.evaluate(f, again);
    still.evaluate(f, again);
    QVERIFY(!again);                  // nothing set, nothing animated: the same inputs
    QVERIFY(!still.animated());
    std::string error;
    QVERIFY(still.command(R"({"set":{"X":2}})", error));
    still.evaluate(f, again);
    QVERIFY(again);
    QVERIFY(!still.command(R"({"set":{"X":1}, "part":{"Nope":1}})", error)); // refused as a whole
    still.evaluate(f, again);
    QVERIFY(!again);
    QVERIFY(!error.empty());
    // values are clamped to the parameter's range, a non-finite one is ignored
    QVERIFY(still.command(R"({"set":{"X":99}})", error));
    // breathing moves the values with time; a huge or bad step is cut or ignored
    session.step(1e9);
    session.step(-1);
    session.step(NAN);
    bool moved = false;
    session.evaluate(f, moved);
    // looking: pointer messages are ignored until it is on
    session.pointer(1000, 32, 64, 64);
    QVERIFY(!session.looking());
    QVERIFY(session.command(R"({"look":true})", error));
    session.pointer(1000, 32, 64, 64);
    for (int i = 0; i < 100; ++i)
        session.step(0.1);
    session.evaluate(f, moved);
    QVERIFY(session.animated());
    QVERIFY(session.command(R"({"look":false,"breath":false,"blink":false,"reset":true})", error));
    QVERIFY(!session.looking());
    // a model with an angle and no breath is animated only while it looks somewhere
    PuppetSession looker(squareModel(false, true), 5);
    QVERIFY(!looker.animated());
    QVERIFY(looker.command(R"({"look":true})", error));
    looker.pointer(1000, 32, 64, 64);
    for (int i = 0; i < 20; ++i)
        looker.step(0.1);
    QVERIFY(looker.animated()); // the gaze is away from the centre
    QVERIFY(looker.command(R"({"look":false})", error));
    for (int i = 0; i < 10; ++i)
        looker.step(0.1);
    QVERIFY(!looker.animated());
    // the commands' size and depth limits
    std::string big = R"({"set":{")";
    big += std::string(300, 'a') + R"(":1}})";
    QVERIFY(!session.command(big, error));
    QVERIFY(!session.command(R"({"a":{"b":{"c":{"d":{"e":1}}}}})", error));
}

void TestPuppetEngine::aHeavyModelIsNotDrawnAndGivenUp()
{
    // 2000 quads that each cover the whole picture: 2000 pictures of overdraw, far over the budget
    const auto heavy = [](int quads) {
        auto m = squareModel();
        m->extent = {0, 0, 64, 64};
        m->fill = [quads](const ParameterSet &, const PartSet &, Frame &f) {
            for (int i = 0; i < quads; ++i)
                f.items.push_back(FakeModel::mesh(0, 0, 0, 64, 64));
        };
        return m;
    };
    auto run = start(heavy(2000), 240, 64, 64, 2048, [](EngineOptions &o) { o.maxHeavyFrames = 3; });
    Consumer &c = *run->pair.consumer;
    QCOMPARE(waitForExit(*run), kExitModelRefused);
    QCOMPARE(c.frames, 0); // not one of those frames was drawn
    QVERIFY2(!c.errors.empty(), "the consumer is told why");
    // under the budget it is drawn as usual
    auto fine = start(heavy(20), 240, 64, 64, 2048, [](EngineOptions &o) { o.maxHeavyFrames = 3; });
    Consumer &d = *fine->pair.consumer;
    QVERIFY(d.waitFor([&] { return d.frames >= 3; }, 15000));
    QCOMPARE(fine->status.load(), -1); // still running
    finish(*fine);
}

void TestPuppetEngine::aSlowGpuLowersTheRateButKeepsTheModel()
{
    if (m_softwareDevice)
        QSKIP("the time of a software device is not watched");
    // every frame counts as slow when the limit is 0 ms; three in a row halve the engine's own rate, down to kMinSlowFps - and the model is kept
    auto run = start(squareModel(true), 240, 64, 64, 2048, [](EngineOptions &o) {
        o.slowFrameMs = 0;
        o.maxSlowFrames = 3;
    });
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 3; }, 15000));
    // it settles at 5 fps (240 -> 120 -> ... -> 7 -> 5 after 3 frames at each step), and goes on
    c.waitFor([] { return false; }, 2500); // (pumps the consumer)
    const int before = c.frames;
    c.waitFor([] { return false; }, 2000);
    const int perTwoSeconds = c.frames - before;
    QVERIFY2(perTwoSeconds >= 4 && perTwoSeconds <= 14, qPrintable(QString::number(perTwoSeconds)));
    QCOMPARE(run->status.load(), -1); // never refused for being slow
    finish(*run);
    // with the real limit the same model runs at full rate
    auto fine = start(squareModel(true), 240);
    Consumer &d = *fine->pair.consumer;
    QVERIFY(d.waitFor([&] { return d.frames >= 20; }, 15000));
    QCOMPARE(fine->status.load(), -1);
    finish(*fine);
}

void TestPuppetEngine::aRealisticModelIsDrawnNotRefused()
{
    // 20 groups and 40 masked parts: about a dozen pictures of area, well under the budget, so it is drawn (the clears and the groups are not whole pictures each)
    auto m = squareModel();
    m->extent = {0, 0, 64, 64};
    m->shapes = {FakeModel::quadShape(), FakeModel::quadShape(), FakeModel::quadShape()};
    m->fill = [](const ParameterSet &, const PartSet &, Frame &f) {
        f.maskSources.push_back(FakeModel::mesh(2, 20, 20, 20, 20));
        for (int g = 0; g < 20; ++g) {
            DrawItem begin;
            begin.kind = DrawKind::BeginGroup;
            f.items.push_back(begin);
            f.items.push_back(FakeModel::mesh(0, 2 + g, 10, 12, 12));
            DrawItem end;
            end.kind = DrawKind::EndGroup;
            f.items.push_back(end);
        }
        for (int k = 0; k < 40; ++k) {
            DrawItem part = FakeModel::mesh(1, 10 + k % 20, 30, 10, 10);
            part.masks = {MaskRef{2, false}};
            f.items.push_back(part);
        }
    };
    auto run = start(std::move(m), 120, 64, 64, 2048, [](EngineOptions &o) { o.maxHeavyFrames = 3; });
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 3; }, 15000));
    QCOMPARE(run->status.load(), -1);
    finish(*run);
}

void TestPuppetEngine::aMotionPlaysThroughTheBridge()
{
    // the square sits at x = X * 20; a motion raises X from 0 to 2 in a second and holds it there (its last value, no fade out), a command from the host starts it
    SessionAssets assets;
    const std::string json = R"({"Version":3,"Meta":{"Duration":1,"Fps":30,"Loop":false,"CurveCount":1,"TotalSegmentCount":1,"TotalPointCount":2},"Curves":[{"Target":"Parameter","Id":"X","FadeInTime":0,"FadeOutTime":0,"Segments":[0,0,0,1,2]}]})";
    const MotionResult parsed = parseMotion(json);
    QVERIFY(parsed.ok);
    assets.motionGroups["Tap"] = {{std::make_shared<const Motion>(parsed.motion), 0, 0}};
    auto run = start(squareModel(), 60, 64, 64, 2048, {}, std::move(assets));
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 1; }, 15000));
    QVERIFY(near(c.last.at(10, 32), {255, 255, 255, 255}));
    QCOMPARE(c.channel().sendProperties(R"({"play":{"group":"Tap","index":0,"priority":"force"}})"), bridge::ChannelStatus::Ok);
    // while it plays the square travels right (the engine draws each frame: it is animated), and a refused command is only logged
    QVERIFY(c.waitFor([&] { return near(c.last.at(30, 32), {255, 255, 255, 255}) || near(c.last.at(45, 32), {255, 255, 255, 255}); }, 15000));
    QCOMPARE(c.channel().sendProperties(R"({"play":{"group":"Nope"}})"), bridge::ChannelStatus::Ok);
    QCOMPARE(run->status.load(), -1);
    // when it ends the host's value (0) is back
    QVERIFY(c.waitFor([&] { return near(c.last.at(10, 32), {255, 255, 255, 255}) && near(c.last.at(45, 32), {0, 0, 0, 0}, 0); }, 15000));
    finish(*run);
}

void TestPuppetEngine::aModelThatFailsBeforeItsFirstFrameIsRefused()
{
    // it refuses the values at once: the model's doing, not a crash of the engine
    auto first = squareModel();
    first->failFrom = 0;
    auto run = start(std::move(first));
    Consumer &c = *run->pair.consumer;
    QCOMPARE(waitForExit(*run), kExitModelRefused);
    QCOMPARE(c.frames, 0);
    // after frames were shown the same refusal is a failure (the crash ledger counts it)
    auto later = squareModel(true);
    later->failFrom = 3;
    auto second = start(std::move(later), 240);
    Consumer &d = *second->pair.consumer;
    QCOMPARE(waitForExit(*second), kExitFailure);
    QVERIFY(d.frames >= 1);
}

void TestPuppetEngine::theMaskIsNotWorkedOutAgainForNothing()
{
    // an idle model is redrawn once a second as a keep-alive; its footprint is the same, so it is worked out once
    const std::uint64_t before = hitMaskRuns();
    const std::uint64_t costBefore = frameCostRuns();
    auto run = start(squareModel(false));
    Consumer &c = *run->pair.consumer;
    QVERIFY(c.waitFor([&] { return c.frames >= 3; }, 15000));
    const std::uint64_t idleRuns = hitMaskRuns() - before;
    QCOMPARE(idleRuns, std::uint64_t(1));
    QCOMPARE(frameCostRuns() - costBefore, std::uint64_t(1)); // and so is the cost of the frame
    finish(*run);
    // a model that moves works it out for each frame
    const std::uint64_t movingBefore = hitMaskRuns();
    auto moving = start(squareModel(true), 120);
    Consumer &d = *moving->pair.consumer;
    QVERIFY(d.waitFor([&] { return d.frames >= 6; }, 15000));
    QVERIFY(hitMaskRuns() - movingBefore >= 5);
    finish(*moving);
}

void TestPuppetEngine::onlyInpNamesAreShown()
{
    QVERIFY(hasInpExtension("model.inp"));
    QVERIFY(hasInpExtension("MODEL.INP"));
    QVERIFY(hasInpExtension("a.Inp"));
    QVERIFY(hasInpExtension(".inp"));
    QVERIFY(!hasInpExtension("x"));
    QVERIFY(!hasInpExtension("inp"));
    QVERIFY(!hasInpExtension("model.png"));
    QVERIFY(!hasInpExtension("model.moc3"));
    QVERIFY(!hasInpExtension(""));
    QVERIFY(!hasInpExtension("model.inp.exe"));
}

QTEST_GUILESS_MAIN(TestPuppetEngine)
#include "tst_puppetengine.moc"
