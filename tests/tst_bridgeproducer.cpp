#include "bridge/producer.h"
#include "dmabufimport.h"
#include "eglcontext.h"
#include "bridgeclient.h"

#include <QTest>

#include <poll.h>

#include <cstdint>
#include <functional>
#include <vector>

using namespace murasaki;
using namespace murasaki::bridge;
using testegl::EglContext;
using testegl::haveRenderNode;

namespace {

using GlBindTexture = void (*)(unsigned, unsigned);
using GlTexSubImage2D = void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
using GlGetTexImage = void (*)(unsigned, int, unsigned, unsigned, void *);
using GlFinish = void (*)();
using GlGetIntegerv = void (*)(unsigned, int *);
using GlGen = void (*)(int, unsigned *);
using GlBindFramebuffer = void (*)(unsigned, unsigned);
using GlBindBuffer = void (*)(unsigned, unsigned);

constexpr unsigned kTexture2D = 0x0DE1;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kUnsignedByte = 0x1401;

struct GlBindings {
    int texture, draw, read, unpack, pack;
    bool operator==(const GlBindings &) const = default;
};

GlBindings queryBindings()
{
    static auto get = reinterpret_cast<GlGetIntegerv>(DmabufImporter::procAddress("glGetIntegerv"));
    GlBindings b{};
    get(0x8069, &b.texture); // GL_TEXTURE_BINDING_2D
    get(0x8CA6, &b.draw);    // GL_DRAW_FRAMEBUFFER_BINDING
    get(0x8CAA, &b.read);    // GL_READ_FRAMEBUFFER_BINDING
    get(0x88EF, &b.unpack);  // GL_PIXEL_UNPACK_BUFFER_BINDING
    get(0x88ED, &b.pack);    // GL_PIXEL_PACK_BUFFER_BINDING
    return b;
}

template <class F>
F proc(const char *name)
{
    return reinterpret_cast<F>(DmabufImporter::procAddress(name));
}

std::vector<uint8_t> pattern(int w, int h, int seed = 0)
{
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / (w - 1) + seed);
            p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<uint8_t>((x ^ y) * 7);
            p[3] = 255;
        }
    }
    return px;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 3000)
{
    for (int waited = 0; waited <= timeoutMs; waited += 10) {
        if (condition())
            return true;
        QTest::qWait(10);
    }
    return condition();
}

// Raw consumer end: the next message of `type`, skipping others. The socket is blocking, so it is only read once
// poll says a message is there: a message that never comes is a failure after `timeoutMs`, not a hang.
std::optional<Message> expect(Channel &ch, MsgType type, int timeoutMs = 2000)
{
    for (int waited = 0; waited <= timeoutMs; waited += 5) {
        pollfd p{ch.fd(), POLLIN, 0};
        if (::poll(&p, 1, 5) <= 0)
            continue;
        ReceiveResult r = ch.receive();
        if (r.status == ChannelStatus::Ok) {
            if (r.message.type == static_cast<uint32_t>(type))
                return std::move(r.message);
            continue;
        }
        if (r.status != ChannelStatus::WouldBlock)
            return std::nullopt;
    }
    return std::nullopt;
}

template <class T>
T decoded(const Message &m)
{
    T out{};
    decode(m.payload, out);
    return out;
}

// A producer on a surfaceless context, with the consumer end of the socket.
struct Rig {
    EglContext egl;
    Channel producerEnd;
    Channel consumerEnd;
    std::unique_ptr<Producer> producer;
    bool ok = false;

    // Returns false (the caller skips) when no context is available.
    bool setup()
    {
        if (!haveRenderNode() || !egl.create())
            return false;
        auto pair = makeSocketPair();
        if (!pair)
            return false;
        producerEnd = Channel(std::move(pair->first));
        consumerEnd = Channel(std::move(pair->second));
        producer = std::make_unique<Producer>(producerEnd, egl.display);
        ok = producer->valid();
        return ok;
    }
};

void upload(unsigned texture, int w, int h, const std::vector<uint8_t> &px)
{
    static auto bind = proc<GlBindTexture>("glBindTexture");
    static auto sub = proc<GlTexSubImage2D>("glTexSubImage2D");
    bind(kTexture2D, texture);
    sub(kTexture2D, 0, 0, 0, w, h, kRgba, kUnsignedByte, px.data());
    bind(kTexture2D, 0);
}

} // namespace

class TstBridgeProducer : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { qunsetenv("MURASAKI_BRIDGE_FORCE_SHM"); }

    void invalidWithoutContext()
    {
        Channel ch;
        Producer producer(ch, nullptr);
        QVERIFY(!producer.valid());
        QVERIFY(!producer.addViewport("x", 16, 16).has_value());
        QVERIFY(!producer.beginFrame(0).has_value());
    }

    void dmabufRoundTrip()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        constexpr int w = 97, h = 53;
        auto id = rig.producer->addViewport("t", w, h);
        QVERIFY(id.has_value());
        if (rig.producer->transport(*id) != Transport::Dmabuf)
            QSKIP("dma-buf export is not available");
        QVERIFY(rig.producer->start());

        BridgeClient client(UniqueFd(rig.consumerEnd.takeFd()));
        auto queue = client.frames();
        QVERIFY(waitUntil([&] { return client.isReady() && queue->buffers(); }));
        auto set = queue->buffers();
        QCOMPARE(set->kind, BridgeBuffers::Kind::Dmabuf);
        QCOMPARE(set->count, 3u);
        QCOMPARE(set->width, uint32_t{w});

        const auto source = pattern(w, h);
        auto target = rig.producer->beginFrame(*id);
        QVERIFY(target.has_value());
        upload(target->texture, w, h, source);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);

        std::optional<BridgeFrame> frame;
        QVERIFY(waitUntil([&] { frame = queue->takeFrame(); return frame.has_value(); }));
        QCOMPARE(frame->buffer, target->buffer);
        QVERIFY(!frame->flipY);

        DmabufImporter importer;
        std::string error;
        QVERIFY2(importer.init(&error), error.c_str());
        DmabufDescription d;
        d.width = set->width;
        d.height = set->height;
        d.fourcc = set->fourcc;
        d.modifier = set->modifier;
        d.planeCount = set->planes;
        for (uint32_t p = 0; p < set->planes; ++p)
            d.planes[p] = {set->dmabufFd(frame->buffer, p), set->offset[frame->buffer][p], set->stride[frame->buffer][p]};
        auto imported = importer.import(d, &error);
        QVERIFY2(imported.has_value(), error.c_str());
        static auto bind = proc<GlBindTexture>("glBindTexture");
        static auto getTex = proc<GlGetTexImage>("glGetTexImage");
        bind(kTexture2D, imported->texture);
        std::vector<uint8_t> readback(source.size(), 0xAA);
        getTex(kTexture2D, 0, kRgba, kUnsignedByte, readback.data());
        bind(kTexture2D, 0);
        QVERIFY2(readback == source, "dma-buf round trip is not pixel exact");
        importer.destroy(*imported);

        // Release echoes the generation and frees the buffer.
        QCOMPARE(rig.producer->heldCount(*id), 1u);
        queue->release(*frame);
        QVERIFY(waitUntil([&] { rig.producer->service(); return rig.producer->heldCount(*id) == 0; }));
    }

    void linearRoundTrip()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        constexpr int w = 64, h = 40;
        auto id = rig.producer->addViewport("t", w, h);
        QVERIFY(id.has_value());
        if (rig.producer->transport(*id) != Transport::Dmabuf)
            QSKIP("dma-buf export is not available");
        if (!rig.producer->canLinear())
            QSKIP("no GBM render node for linear buffers");
        QVERIFY(rig.producer->start());

        BridgeClient client(UniqueFd(rig.consumerEnd.takeFd()));
        auto queue = client.frames();
        QVERIFY(waitUntil([&] { return queue->buffers() != nullptr; }));
        const uint32_t firstGeneration = queue->buffers()->generation;

        // What the consumer does when the first import fails.
        queue->reportImportFailure(firstGeneration);
        QVERIFY(waitUntil([&] {
            rig.producer->service();
            auto set = queue->buffers();
            return set && set->generation != firstGeneration;
        }));
        if (rig.producer->transport(*id) != Transport::DmabufLinear)
            QSKIP("the driver cannot allocate linear GBM buffers");
        auto set = queue->buffers();
        QCOMPARE(set->kind, BridgeBuffers::Kind::Dmabuf);
        QCOMPARE(set->modifier, uint64_t{0});

        const auto source = pattern(w, h, 5);
        auto target = rig.producer->beginFrame(*id);
        QVERIFY(target.has_value());
        upload(target->texture, w, h, source);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        std::optional<BridgeFrame> frame;
        QVERIFY(waitUntil([&] { frame = queue->takeFrame(); return frame.has_value(); }));

        DmabufImporter importer;
        std::string error;
        QVERIFY2(importer.init(&error), error.c_str());
        DmabufDescription d;
        d.width = set->width;
        d.height = set->height;
        d.fourcc = set->fourcc;
        d.modifier = set->modifier;
        d.planeCount = set->planes;
        for (uint32_t p = 0; p < set->planes; ++p)
            d.planes[p] = {set->dmabufFd(frame->buffer, p), set->offset[frame->buffer][p], set->stride[frame->buffer][p]};
        auto imported = importer.import(d, &error);
        QVERIFY2(imported.has_value(), error.c_str());
        static auto bind = proc<GlBindTexture>("glBindTexture");
        static auto getTex = proc<GlGetTexImage>("glGetTexImage");
        bind(kTexture2D, imported->texture);
        std::vector<uint8_t> readback(source.size(), 0xAA);
        getTex(kTexture2D, 0, kRgba, kUnsignedByte, readback.data());
        bind(kTexture2D, 0);
        QVERIFY2(readback == source, "linear round trip is not pixel exact");
        importer.destroy(*imported);
    }

    void shmRoundTripForced()
    {
        qputenv("MURASAKI_BRIDGE_FORCE_SHM", "1");
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        constexpr int w = 33, h = 21;
        auto id = rig.producer->addViewport("t", w, h);
        QVERIFY(id.has_value());
        QCOMPARE(rig.producer->transport(*id), Transport::Shm);
        QVERIFY(rig.producer->start());

        BridgeClient client(UniqueFd(rig.consumerEnd.takeFd()));
        auto queue = client.frames();
        QVERIFY(waitUntil([&] { return queue->buffers() != nullptr; }));
        auto set = queue->buffers();
        QCOMPARE(set->kind, BridgeBuffers::Kind::Shm);
        QCOMPARE(set->count, 3u);

        const auto source = pattern(w, h, 9);
        auto target = rig.producer->beginFrame(*id);
        QVERIFY(target.has_value());
        upload(target->texture, w, h, source);
        QCOMPARE(rig.producer->endFrame(*id, true), FrameResult::Sent);
        std::optional<BridgeFrame> frame;
        QVERIFY(waitUntil([&] { frame = queue->takeFrame(); return frame.has_value(); }));
        QVERIFY(frame->flipY);
        const auto bytes = set->ring->frame(frame->buffer);
        QCOMPARE(bytes.size(), source.size());
        QVERIFY2(std::equal(bytes.begin(), bytes.end(), source.begin()), "SHM round trip is not pixel exact");
    }

    void requestShmSwitchesTransport()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 32, 24);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        QVERIFY(expect(rig.consumerEnd, MsgType::Hello));
        QVERIFY(expect(rig.consumerEnd, MsgType::Viewport));
        auto first = expect(rig.consumerEnd, rig.producer->transport(*id) == Transport::Shm ? MsgType::ShmBuffers
                                                                                            : MsgType::DmabufBuffers);
        QVERIFY(first);
        if (rig.producer->transport(*id) == Transport::Shm)
            QSKIP("dma-buf export is not available: the viewport is on shared memory already");
        QVERIFY(rig.consumerEnd.send(msg::RequestShm{*id}) == ChannelStatus::Ok);
        QVERIFY(waitUntil([&] { rig.producer->service(); return rig.producer->transport(*id) == Transport::Shm; }));
        auto shm = expect(rig.consumerEnd, MsgType::ShmBuffers);
        QVERIFY(shm);
        const auto m = decoded<msg::ShmBuffers>(*shm);
        QVERIFY(m.generation != 0);
        QCOMPARE(m.count, 3u);
        QCOMPARE(m.generation, rig.producer->generation(*id));
        QCOMPARE(shm->fds.size(), size_t{1});
    }

    void generationsOnResize()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 64, 48);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        const bool shm = rig.producer->transport(*id) == Transport::Shm;
        const MsgType kind = shm ? MsgType::ShmBuffers : MsgType::DmabufBuffers;
        QVERIFY(expect(rig.consumerEnd, MsgType::Hello));
        QVERIFY(expect(rig.consumerEnd, MsgType::Viewport));
        auto first = expect(rig.consumerEnd, kind);
        QVERIFY(first);
        const uint32_t gen1 = shm ? decoded<msg::ShmBuffers>(*first).generation
                                  : decoded<msg::DmabufBuffers>(*first).generation;

        auto t = rig.producer->beginFrame(*id);
        QVERIFY(t);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        auto frame1 = expect(rig.consumerEnd, MsgType::Frame);
        QVERIFY(frame1);
        QCOMPARE(decoded<msg::Frame>(*frame1).generation, gen1);
        QCOMPARE(rig.producer->heldCount(*id), 1u);

        // Same size: nothing changes.
        QVERIFY(rig.producer->resize(*id, 64, 48));
        QCOMPARE(rig.producer->generation(*id), gen1);

        QVERIFY(rig.producer->resize(*id, 128, 96));
        const uint32_t gen2 = rig.producer->generation(*id);
        QVERIFY(gen2 != gen1);
        QCOMPARE(rig.producer->heldCount(*id), 0u); // a new set starts with every buffer free
        auto second = expect(rig.consumerEnd, kind);
        QVERIFY(second);
        if (shm) {
            const auto m = decoded<msg::ShmBuffers>(*second);
            QCOMPARE(m.generation, gen2);
            QCOMPARE(m.width, 128u);
            QCOMPARE(m.height, 96u);
        } else {
            const auto m = decoded<msg::DmabufBuffers>(*second);
            QCOMPARE(m.generation, gen2);
            QCOMPARE(m.width, 128u);
            QCOMPARE(m.height, 96u);
            QCOMPARE(m.count, 3u);
        }
        t = rig.producer->beginFrame(*id);
        QVERIFY(t);
        QCOMPARE(t->width, 128u);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        auto frame2 = expect(rig.consumerEnd, MsgType::Frame);
        QVERIFY(frame2);
        QCOMPARE(decoded<msg::Frame>(*frame2).generation, gen2);
    }

    void staleReleaseIsIgnored()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 40, 30);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        const uint32_t gen1 = rig.producer->generation(*id);
        QVERIFY(rig.producer->resize(*id, 80, 60));
        const uint32_t gen2 = rig.producer->generation(*id);
        auto t = rig.producer->beginFrame(*id);
        QVERIFY(t);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        QCOMPARE(rig.producer->heldCount(*id), 1u);

        QVERIFY(rig.consumerEnd.send(msg::Release{*id, t->buffer, gen1}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->heldCount(*id), 1u); // stale generation: ignored

        QVERIFY(rig.consumerEnd.send(msg::Release{*id, t->buffer, gen2}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->heldCount(*id), 0u);
    }

    void allHeldMeansNoBuffer()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 32, 32);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        const uint32_t gen = rig.producer->generation(*id);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Skipped); // no beginFrame
        std::vector<uint32_t> used;
        for (int i = 0; i < 3; ++i) {
            auto t = rig.producer->beginFrame(*id);
            QVERIFY(t);
            used.push_back(t->buffer);
            QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        }
        QCOMPARE(rig.producer->heldCount(*id), 3u);
        QVERIFY(!rig.producer->beginFrame(*id).has_value());

        QVERIFY(rig.consumerEnd.send(msg::Release{*id, used[1], gen}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        auto t = rig.producer->beginFrame(*id);
        QVERIFY(t);
        QCOMPARE(t->buffer, used[1]);
        // beginFrame is idempotent until endFrame.
        QCOMPARE(rig.producer->beginFrame(*id)->buffer, used[1]);
    }

    void badReleaseFailsTheConnection()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 16, 16);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        QVERIFY(rig.consumerEnd.send(msg::Release{*id, 7, rig.producer->generation(*id)}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Failed);
        QVERIFY(!rig.producer->lastError().empty());
        QVERIFY(expect(rig.consumerEnd, MsgType::Error));
        QVERIFY(!rig.producer->beginFrame(*id).has_value());
    }

    void controlMessagesReachTheHandler()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 16, 16);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        std::vector<ControlMessage> seen;
        rig.producer->setControlHandler([&](const ControlMessage &m) { seen.push_back(m); });

        QVERIFY(rig.consumerEnd.send(msg::Pause{1}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Pointer{*id, 3, 4, 1}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Resize{*id, 32, 24}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Volume{64}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Scaling{2}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Fps{30}) == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.sendProperties(R"({"a":1})") == ChannelStatus::Ok);
        QVERIFY(rig.consumerEnd.send(msg::Quit{0}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(seen.size(), size_t{8});
        QCOMPARE(std::get<msg::Pause>(seen[0]).paused, 1u);
        QCOMPARE(std::get<msg::Pointer>(seen[1]).x, 3);
        QCOMPARE(std::get<msg::Resize>(seen[2]).width, 32u);
        QCOMPARE(std::get<msg::Volume>(seen[3]).volume, 64u);
        QCOMPARE(std::get<msg::Scaling>(seen[4]).mode, 2u);
        QCOMPARE(std::get<msg::Fps>(seen[5]).fps, 30u);
        QCOMPARE(std::get<PropertiesJson>(seen[6]).json, std::string(R"({"a":1})"));
        QVERIFY(std::holds_alternative<msg::Quit>(seen[7]));
    }

    void glStateIsPreserved()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto gen = proc<GlGen>("glGenTextures");
        auto genFb = proc<GlGen>("glGenFramebuffers");
        auto genBuf = proc<GlGen>("glGenBuffers");
        auto bindTex = proc<GlBindTexture>("glBindTexture");
        auto bindFb = proc<GlBindFramebuffer>("glBindFramebuffer");
        auto bindBuf = proc<GlBindBuffer>("glBindBuffer");
        unsigned tex = 0, fbs[2] = {0, 0}, bufs[2] = {0, 0};
        gen(1, &tex);
        genFb(2, fbs);
        genBuf(2, bufs);
        bindTex(kTexture2D, tex);
        bindFb(0x8CA9, fbs[0]); // draw
        bindFb(0x8CA8, fbs[1]); // read
        bindBuf(0x88EC, bufs[0]); // unpack
        bindBuf(0x88EB, bufs[1]); // pack
        const GlBindings before = queryBindings();
        QVERIFY(before.texture != 0 && before.draw != before.read && before.unpack != 0 && before.pack != 0);

        auto id = rig.producer->addViewport("t", 48, 32);
        QVERIFY(id.has_value());
        QCOMPARE(queryBindings(), before);
        QVERIFY(rig.producer->start());
        QVERIFY(rig.producer->resize(*id, 96, 64));
        QCOMPARE(queryBindings(), before);
        QVERIFY(rig.consumerEnd.send(msg::RequestShm{*id}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->transport(*id), Transport::Shm);
        QCOMPARE(queryBindings(), before);
        // The SHM read-back path restores its own state as well.
        QVERIFY(rig.producer->beginFrame(*id).has_value());
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        QCOMPARE(queryBindings(), before);
        if (rig.producer->canLinear()) {
            QVERIFY(rig.consumerEnd.send(msg::RequestLinear{*id}) == ChannelStatus::Ok);
            QCOMPARE(rig.producer->service(), ServiceResult::Ok);
            QCOMPARE(queryBindings(), before);
        }
    }

    void targetsChangedIsReported()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 32, 32);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        QVERIFY(rig.producer->consumeTargetsChanged()); // the initial allocation
        QVERIFY(!rig.producer->consumeTargetsChanged());
        QVERIFY(rig.producer->resize(*id, 32, 32)); // same size: no new set
        QVERIFY(!rig.producer->consumeTargetsChanged());
        const bool wasShm = rig.producer->transport(*id) == Transport::Shm; // then the request changes nothing
        QVERIFY(rig.consumerEnd.send(msg::RequestShm{*id}) == ChannelStatus::Ok);
        rig.producer->service();
        QCOMPARE(rig.producer->consumeTargetsChanged(), !wasShm && rig.producer->transport(*id) == Transport::Shm);
        QVERIFY(rig.producer->resize(*id, 40, 40));
        QVERIFY(rig.producer->consumeTargetsChanged());
    }

    void linearWithoutGbmFallsBackToShm()
    {
        qputenv("MURASAKI_BRIDGE_NO_GBM", "1");
        Rig rig;
        const bool ok = rig.setup();
        qunsetenv("MURASAKI_BRIDGE_NO_GBM");
        if (!ok)
            QSKIP("no render node or surfaceless EGL context");
        QVERIFY(!rig.producer->canLinear());
        auto id = rig.producer->addViewport("t", 32, 24);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        QVERIFY(rig.consumerEnd.send(msg::RequestLinear{*id}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->transport(*id), Transport::Shm);
        QVERIFY(expect(rig.consumerEnd, MsgType::ShmBuffers));
        // Asking again does not thrash the buffer set.
        const uint32_t gen = rig.producer->generation(*id);
        QVERIFY(rig.consumerEnd.send(msg::RequestLinear{*id}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->generation(*id), gen);
    }

    void staleReleaseAfterTransportSwitchIsIgnored()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 32, 24);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start());
        if (rig.producer->transport(*id) == Transport::Shm)
            QSKIP("dma-buf export is not available, no switch to test");
        const uint32_t gen1 = rig.producer->generation(*id);
        QVERIFY(rig.producer->beginFrame(*id));
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        QVERIFY(rig.consumerEnd.send(msg::RequestShm{*id}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->transport(*id), Transport::Shm);
        QVERIFY(rig.producer->generation(*id) != gen1);
        auto t = rig.producer->beginFrame(*id);
        QVERIFY(t);
        QCOMPARE(rig.producer->endFrame(*id, false), FrameResult::Sent);
        QCOMPARE(rig.producer->heldCount(*id), 1u);
        QVERIFY(rig.consumerEnd.send(msg::Release{*id, t->buffer, gen1}) == ChannelStatus::Ok);
        QCOMPARE(rig.producer->service(), ServiceResult::Ok);
        QCOMPARE(rig.producer->heldCount(*id), 1u);
    }

    void flipYIsForwarded()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        auto id = rig.producer->addViewport("t", 32, 24);
        QVERIFY(id.has_value());
        QVERIFY(rig.producer->start()); // default transport: dma-buf where the driver can export
        for (bool flip : {true, false}) {
            QVERIFY(rig.producer->beginFrame(*id));
            QCOMPARE(rig.producer->endFrame(*id, flip), FrameResult::Sent);
            auto frame = expect(rig.consumerEnd, MsgType::Frame);
            QVERIFY(frame);
            QCOMPARE(decoded<msg::Frame>(*frame).flipY, flip ? 1u : 0u);
        }
    }

    void peerCloseIsReported()
    {
        Rig rig;
        if (!rig.setup())
            QSKIP("no render node or surfaceless EGL context");
        QVERIFY(rig.producer->addViewport("t", 16, 16).has_value());
        QVERIFY(rig.producer->start());
        rig.consumerEnd = Channel();
        QCOMPARE(rig.producer->service(), ServiceResult::PeerClosed);
        QVERIFY(!rig.producer->beginFrame(0).has_value());
    }
};

QTEST_GUILESS_MAIN(TstBridgeProducer)
#include "tst_bridgeproducer.moc"
