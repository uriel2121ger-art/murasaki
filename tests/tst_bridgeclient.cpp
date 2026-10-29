#include "bridgeclient.h"
#include "bridge/mediaview.h"
#include "bridge/shortcutview.h"
#include "bridge/spanview.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace murasaki;
using namespace murasaki::bridge;

namespace {

constexpr uint32_t kViewport = 3;

// The engine side of the socketpair, driven synchronously from the test thread.
struct FakeProducer {
    Channel channel;
    std::optional<ShmRing> ring;
    uint32_t generation = 0;

    void hello(uint32_t version = kBridgeVersion)
    {
        send(msg::Hello{version, 4242});
    }
    void viewport(uint32_t id = kViewport, uint32_t w = 64, uint32_t h = 32)
    {
        msg::Viewport vp{};
        vp.id = id;
        vp.width = w;
        vp.height = h;
        setString(vp.name, "DP-1");
        send(vp);
    }
    // A fresh ring whose frame i is filled with the byte (gen * 16 + i).
    void shmBuffers(uint32_t gen, uint32_t w = 64, uint32_t h = 32, uint32_t stride = 0, uint32_t count = 3)
    {
        generation = gen;
        stride = stride ? stride : w * 4;
        auto r = ShmRing::create({count, w, h, stride});
        QVERIFY(r.has_value());
        for (uint32_t i = 0; i < count; ++i) {
            auto f = r->frame(i);
            std::fill(f.begin(), f.end(), static_cast<uint8_t>(gen * 16 + i));
        }
        msg::ShmBuffers m{kViewport, count, w, h, stride, kFormatRgba8, gen};
        const int fd = r->fd();
        QCOMPARE(channel.send(m, std::span<const int>(&fd, 1)), ChannelStatus::Ok);
        ring = std::move(r);
    }
    void dmabufBuffers(uint32_t gen, uint32_t count = 3)
    {
        generation = gen;
        msg::DmabufBuffers m{};
        m.viewport = kViewport;
        m.count = count;
        m.planes = 1;
        m.fourcc = 0x34324241; // AB24
        m.width = 64;
        m.height = 32;
        m.generation = gen;
        m.modifier = 0;
        for (auto &plane : m.plane)
            plane[0] = {0, 256};
        // Any three fds do for the client, which only keeps them; real ones come from memfd.
        std::vector<UniqueFd> keep;
        std::vector<int> fds;
        for (uint32_t i = 0; i < count; ++i) {
            keep.emplace_back(::memfd_create("fake", MFD_CLOEXEC));
            fds.push_back(keep.back().get());
        }
        QCOMPARE(channel.send(m, fds), ChannelStatus::Ok);
    }
    void frame(uint32_t buffer, uint32_t gen, bool flip = false, uint64_t seq = 1)
    {
        send(msg::Frame{kViewport, buffer, seq, flip ? 1u : 0u, gen});
    }
    template <class T>
    void send(const T &m)
    {
        QCOMPARE(channel.send(m), ChannelStatus::Ok);
    }
    void sendRaw(MsgType type, std::span<const uint8_t> payload)
    {
        QCOMPARE(channel.send(type, payload), ChannelStatus::Ok);
    }

    // Next message from the client, or empty type 0 when none is waiting.
    Message next()
    {
        ReceiveResult r = channel.receive();
        return r.status == ChannelStatus::Ok ? std::move(r.message) : Message{};
    }
};

struct Rig {
    FakeProducer producer;
    std::unique_ptr<BridgeClient> client;
    std::unique_ptr<QSignalSpy> lost, ready, frames;

    Rig()
    {
        auto sp = makeSocketPair();
        if (!sp)
            qFatal("socketpair");
        producer.channel = Channel(std::move(sp->first));
        producer.channel.setNonblocking(true);
        client = std::make_unique<BridgeClient>(std::move(sp->second));
        lost = std::make_unique<QSignalSpy>(client.get(), &BridgeClient::lost);
        ready = std::make_unique<QSignalSpy>(client.get(), &BridgeClient::ready);
        frames = std::make_unique<QSignalSpy>(client.get(), &BridgeClient::frameAvailable);
    }
};

template <class T>
T decodeAs(const Message &m)
{
    T out{};
    if (!decode(m.payload, out))
        qFatal("decode");
    return out;
}

} // namespace

class TstBridgeClient : public QObject
{
    Q_OBJECT

    // Waits for a message of `type` from the client; fails the test if something else arrives first.
    static bool expectMessage(Rig &rig, MsgType type, Message *out)
    {
        for (int i = 0; i < 200; ++i) {
            QCoreApplication::processEvents();
            Message m = rig.producer.next();
            if (m.type != 0) {
                *out = std::move(m);
                return m.type == 0 ? false : out->type == static_cast<uint32_t>(type);
            }
            QTest::qWait(5);
        }
        return false;
    }
    static bool nothingPending(Rig &rig)
    {
        QTest::qWait(30);
        return rig.producer.next().type == 0;
    }

private slots:
    void mediaMessagesGoOnlyToAProducerThatAnnouncedThem()
    {
        {
            Rig rig; // the base protocol: nothing is sent
            rig.producer.hello(kBridgeVersion);
            rig.producer.viewport();
            QTRY_COMPARE(rig.ready->count(), 1);
            QVERIFY(!rig.client->supportsMedia());
            rig.client->sendMedia(R"({"enabled":false})");
            rig.client->sendMediaThumbnail(0, 0, {}, 1);
            QVERIFY(nothingPending(rig));
        }
        Rig rig;
        rig.producer.hello(kMediaVersion);
        rig.producer.viewport();
        QTRY_COMPARE(rig.ready->count(), 1);
        QVERIFY(rig.client->supportsMedia());
        rig.client->sendMedia(R"({"enabled":true,"title":"a"})");
        Message m;
        QVERIFY(expectMessage(rig, kMediaType, &m));
        const auto json = decodeProperties(m.payload);
        QVERIFY(json.has_value());
        QCOMPARE(QByteArray(json->data(), static_cast<qsizetype>(json->size())), QByteArray(R"({"enabled":true,"title":"a"})"));
        QVERIFY(m.fds.empty());
        // a cover travels as a sealed memfd of exactly width * height * 4 bytes
        QByteArray px(2 * 3 * 4, '\x7f');
        rig.client->sendMediaThumbnail(2, 3, px, 9);
        QVERIFY(expectMessage(rig, kMediaThumbnailType, &m));
        const auto header = decodeAs<msg::MediaThumbnail>(m);
        QCOMPARE(header.width, 2u);
        QCOMPARE(header.height, 3u);
        QCOMPARE(header.generation, 9u);
        QVERIFY(msg::validate(header, m.fds.size()));
        const auto back = readSealedPixels(std::move(m.fds[0]), 2, 3);
        QVERIFY(back.has_value());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(back->data()), static_cast<qsizetype>(back->size())), px);
        // none: no fd
        rig.client->sendMediaThumbnail(0, 0, {}, 10);
        QVERIFY(expectMessage(rig, kMediaThumbnailType, &m));
        QVERIFY(m.fds.empty());
        QCOMPARE(decodeAs<msg::MediaThumbnail>(m).width, 0u);
        // a wrong size or an over-large cover is not sent
        rig.client->sendMediaThumbnail(2, 3, QByteArray(5, 'x'), 11);
        rig.client->sendMediaThumbnail(kMaxThumbnailSide + 1, 1, QByteArray((kMaxThumbnailSide + 1) * 4, 'x'), 12);
        rig.client->sendMedia(QByteArray(kMaxMediaJson + 1, 'x'));
        QVERIFY(nothingPending(rig));
    }

    void shortcutMessagesGoOnlyToAProducerThatAnnouncedVersionFive()
    {
        {
            Rig rig; // version 4 (media): no shortcut messages
            rig.producer.hello(kMediaVersion);
            rig.producer.viewport();
            QTRY_COMPARE(rig.ready->count(), 1);
            QVERIFY(rig.client->supportsMedia());
            QVERIFY(!rig.client->supportsShortcut());
            rig.client->sendShortcutIcon("launcher1", 0, 0, {}, 1);
            QVERIFY(nothingPending(rig));
        }
        Rig rig;
        rig.producer.hello(kShortcutVersion);
        rig.producer.viewport();
        QTRY_COMPARE(rig.ready->count(), 1);
        QVERIFY(rig.client->supportsShortcut());
        QVERIFY(rig.client->supportsMedia()); // version 5 includes 4's messages
        // an icon travels as a sealed memfd of exactly width * height * 4 bytes, with the property key
        QByteArray px(2 * 3 * 4, '\x55');
        rig.client->sendShortcutIcon("launcher1", 2, 3, px, 7);
        Message m;
        QVERIFY(expectMessage(rig, kShortcutIconType, &m));
        const auto header = decodeAs<msg::ShortcutIcon>(m);
        QCOMPARE(header.width, 2u);
        QCOMPARE(header.height, 3u);
        QCOMPARE(header.generation, 7u);
        QCOMPARE(QByteArray(header.name), QByteArray("launcher1"));
        QVERIFY(msg::validate(header, m.fds.size()));
        const auto back = readSealedPixels(std::move(m.fds[0]), 2, 3);
        QVERIFY(back.has_value());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(back->data()), static_cast<qsizetype>(back->size())), px);
        // cleared: no fd
        rig.client->sendShortcutIcon("launcher1", 0, 0, {}, 8);
        QVERIFY(expectMessage(rig, kShortcutIconType, &m));
        QVERIFY(m.fds.empty());
        QCOMPARE(decodeAs<msg::ShortcutIcon>(m).width, 0u);
        // a wrong size, an over-large icon, a name that is empty or too long are not sent
        rig.client->sendShortcutIcon("launcher1", 2, 3, QByteArray(5, 'x'), 9);
        rig.client->sendShortcutIcon("launcher1", kMaxShortcutIconSide + 1, 1, QByteArray((kMaxShortcutIconSide + 1) * 4, 'x'), 10);
        rig.client->sendShortcutIcon("", 0, 0, {}, 11);
        rig.client->sendShortcutIcon(QByteArray(64, 'k'), 0, 0, {}, 12);
        rig.client->sendShortcutIcon("bad\x01name", 0, 0, {}, 13);
        QVERIFY(nothingPending(rig));
    }

    void aLaunchRequestFromTheEngineBecomesASignalOnlyWhenWellFormed()
    {
        Rig rig;
        rig.producer.hello(kShortcutVersion);
        rig.producer.viewport();
        QTRY_COMPARE(rig.ready->count(), 1);
        QSignalSpy launches(rig.client.get(), &BridgeClient::shortcutLaunchRequested);
        msg::ShortcutLaunch launch{};
        launch.serial = 1;
        msg::setShortcutName(launch, "launcher7");
        rig.producer.send(launch);
        QTRY_COMPARE(launches.count(), 1);
        QCOMPARE(launches.takeFirst().at(0).toByteArray(), QByteArray("launcher7"));
        // malformed ones are dropped without ending the connection: a control character, an empty name, a reserved field
        msg::ShortcutLaunch bad{};
        msg::setShortcutName(bad, "a\x02");
        rig.producer.send(bad);
        msg::ShortcutLaunch empty{};
        rig.producer.send(empty);
        msg::ShortcutLaunch reserved = launch;
        reserved.reserved = 5;
        rig.producer.send(reserved);
        QTest::qWait(50);
        QCOMPARE(launches.count(), 0);
        QVERIFY(rig.client->isOpen());
        // and a producer that announced less never gets one honoured
        Rig old;
        old.producer.hello(kMediaVersion);
        old.producer.viewport();
        QTRY_COMPARE(old.ready->count(), 1);
        QSignalSpy none(old.client.get(), &BridgeClient::shortcutLaunchRequested);
        old.producer.send(launch);
        QTest::qWait(50);
        QCOMPARE(none.count(), 0);
    }

    void aPuppetEngineHello6IsAcceptedAndANewerOneIsNot()
    {
        Rig six;
        six.producer.hello(kPuppetVersion);
        six.producer.viewport();
        QTRY_COMPARE(six.ready->count(), 1);
        QVERIFY(six.client->supportsPuppet());
        QVERIFY(six.client->supportsShortcut());
        QCOMPARE(six.client->peerVersion(), kPuppetVersion);
        Rig five;
        five.producer.hello(kShortcutVersion);
        five.producer.viewport();
        QTRY_COMPARE(five.ready->count(), 1);
        QVERIFY(!five.client->supportsPuppet());
        // version 7 does not exist yet: a protocol violation, like any other number outside the range
        Rig seven;
        QSignalSpy lost(seven.client.get(), &BridgeClient::lost);
        seven.producer.hello(kPuppetVersion + 1);
        QTRY_COMPARE(lost.count(), 1);
        QVERIFY(lost.takeFirst().at(0).toString().contains(QStringLiteral("bridge version 7")));
        QCOMPARE(seven.ready->count(), 0);
    }

    void aHitMaskIsPassedOnOnlyForTheCurrentBuffersOfAPuppetEngine()
    {
        const auto payload = [](uint32_t cols, uint32_t rows, uint32_t cell, uint32_t generation, uint8_t fill = 0x0F) {
            std::vector<uint8_t> bits(hitMaskBytes(cols, rows), 0);
            if (!bits.empty() && cols * rows % 8 == 0)
                std::fill(bits.begin(), bits.end(), fill);
            return encodeHitMask({cols, rows, cell, generation}, bits);
        };
        Rig rig;
        rig.producer.hello(kPuppetVersion);
        rig.producer.viewport(kViewport, 64, 32);
        rig.producer.shmBuffers(5, 64, 32);
        QTRY_COMPARE(rig.ready->count(), 1);
        QSignalSpy masks(rig.client.get(), &BridgeClient::hitMaskReceived);
        // 64 x 32 in 8 pixel cells: 8 x 4 = 32 bits = 4 bytes
        auto good = payload(8, 4, 8, 5);
        QVERIFY(good);
        rig.producer.sendRaw(kHitMaskType, *good);
        QTRY_COMPARE(masks.count(), 1);
        QCOMPARE(masks.at(0).at(0).toUInt(), 8u);
        QCOMPARE(masks.at(0).at(1).toUInt(), 4u);
        QCOMPARE(masks.at(0).at(2).toUInt(), 8u);
        QCOMPARE(masks.at(0).at(3).toByteArray().size(), 4);
        masks.clear();
        // another generation, a grid that is not the buffers' (cells bigger than the picture's, wrong rows), a malformed payload: dropped, the connection stays
        auto staleGeneration = payload(8, 4, 8, 4);
        rig.producer.sendRaw(kHitMaskType, *staleGeneration);
        auto wrongGrid = payload(4, 2, 16, 5);   // (cols 4 x 16 = 64 and rows 2 x 16 = 32 are right for cell 16: this one passes)
        auto badGrid = payload(8, 8, 8, 5);      // rows would be 4
        rig.producer.sendRaw(kHitMaskType, *badGrid);
        rig.producer.sendRaw(kHitMaskType, std::vector<uint8_t>(7, 1));
        QTest::qWait(80);
        QCOMPARE(masks.count(), 0);
        QVERIFY(rig.client->isOpen());
        rig.producer.sendRaw(kHitMaskType, *wrongGrid);
        QTRY_COMPARE(masks.count(), 1);
        // a producer that announced less than 6 never gets one honoured
        Rig old;
        old.producer.hello(kShortcutVersion);
        old.producer.viewport(kViewport, 64, 32);
        old.producer.shmBuffers(5, 64, 32);
        QTRY_COMPARE(old.ready->count(), 1);
        QSignalSpy none(old.client.get(), &BridgeClient::hitMaskReceived);
        old.producer.sendRaw(kHitMaskType, *good);
        QTest::qWait(80);
        QCOMPARE(none.count(), 0);
        QVERIFY(old.client->isOpen());
        // and before any buffers exist there is nothing to match
        Rig early;
        early.producer.hello(kPuppetVersion);
        early.producer.viewport(kViewport, 64, 32);
        QTRY_COMPARE(early.ready->count(), 1);
        QSignalSpy before(early.client.get(), &BridgeClient::hitMaskReceived);
        early.producer.sendRaw(kHitMaskType, *good);
        QTest::qWait(80);
        QCOMPARE(before.count(), 0);
    }

    void shmFlowDeliversPixelsAndReleasesWithGeneration()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport(kViewport, 64, 32);
        rig.producer.shmBuffers(5);
        rig.producer.frame(1, 5, true, 77);
        QTRY_COMPARE(rig.frames->count(), 1);
        QCOMPARE(rig.ready->count(), 1);
        QCOMPARE(rig.client->viewportSize(), QSize(64, 32));
        QCOMPARE(*rig.client->viewportId(), kViewport);

        auto frame = rig.client->frames()->takeFrame();
        QVERIFY(frame.has_value());
        QCOMPARE(frame->buffer, 1u);
        QCOMPARE(frame->generation(), 5u);
        QCOMPARE(frame->seq, uint64_t{77});
        QVERIFY(frame->flipY);
        const auto pixels = frame->buffers->ring->frame(frame->buffer);
        QCOMPARE(pixels.size(), size_t{64 * 32 * 4});
        QVERIFY(std::all_of(pixels.begin(), pixels.end(), [](uint8_t b) { return b == 5 * 16 + 1; }));
        QVERIFY(!rig.client->frames()->takeFrame().has_value());

        // The render thread releases; the GUI thread puts it on the wire with the frame's generation.
        rig.client->frames()->release(*frame);
        Message m;
        QVERIFY(expectMessage(rig, MsgType::Release, &m));
        const auto rel = decodeAs<msg::Release>(m);
        QCOMPARE(rel.viewport, kViewport);
        QCOMPARE(rel.buffer, 1u);
        QCOMPARE(rel.generation, 5u);
        QCOMPARE(rig.lost->count(), 0);
    }

    void frameReplacedBeforeBeingTakenIsReleasedImmediately()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(2);
        rig.producer.frame(0, 2, false, 1);
        rig.producer.frame(2, 2, false, 2);
        QTRY_COMPARE(rig.frames->count(), 2);
        Message m;
        QVERIFY(expectMessage(rig, MsgType::Release, &m));
        const auto rel = decodeAs<msg::Release>(m);
        QCOMPARE(rel.buffer, 0u); // the older, never shown one
        QCOMPARE(rel.generation, 2u);
        auto frame = rig.client->frames()->takeFrame();
        QCOMPARE(frame->buffer, 2u);
        QVERIFY(nothingPending(rig));
    }

    void staleGenerationFramesAreDropped()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(1);
        rig.producer.frame(0, 1);
        QTRY_COMPARE(rig.frames->count(), 1);
        QVERIFY(rig.client->frames()->takeFrame().has_value());

        rig.producer.shmBuffers(2); // resize: new ring
        rig.producer.frame(1, 1);   // late frame of the old ring
        rig.producer.frame(7, 2);   // not even a valid index... but for generation 2 this is out of range
        QTRY_COMPARE(rig.lost->count(), 1); // an out-of-range index of the current set is a violation
        QVERIFY(rig.lost->first().first().toString().contains(QStringLiteral("out of range")));
    }

    void staleFrameAloneIsSilentlyIgnored()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(1);
        rig.producer.shmBuffers(2);
        rig.producer.frame(1, 1); // old generation
        rig.producer.frame(0, 3); // generation that was never announced
        QTest::qWait(50);
        QCOMPARE(rig.frames->count(), 0);
        QCOMPARE(rig.lost->count(), 0);
        QVERIFY(nothingPending(rig)); // nothing released either: the producer ignores old-ring releases
        rig.producer.frame(2, 2);
        QTRY_COMPARE(rig.frames->count(), 1);
        QCOMPARE(rig.client->frames()->takeFrame()->generation(), 2u);
    }

    void newBufferSetDropsPendingFrame()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(1);
        rig.producer.frame(0, 1);
        QTRY_COMPARE(rig.frames->count(), 1);
        rig.producer.shmBuffers(2);
        QTRY_VERIFY(rig.client->frames()->buffers() && rig.client->frames()->buffers()->generation == 2);
        QVERIFY(!rig.client->frames()->takeFrame().has_value());
    }

    void generationMustAdvance()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(4);
        rig.producer.shmBuffers(4);
        QTRY_COMPARE(rig.lost->count(), 1);
        QVERIFY(rig.lost->first().first().toString().contains(QStringLiteral("generation")));
    }

    void importFailureRequestsLinearThenShmOnce()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.dmabufBuffers(1);
        rig.producer.frame(0, 1);
        QTRY_COMPARE(rig.frames->count(), 1);
        auto frame = rig.client->frames()->takeFrame();
        QVERIFY(frame.has_value());
        QCOMPARE(frame->buffers->kind, BridgeBuffers::Kind::Dmabuf);
        QCOMPARE(frame->buffers->planes, 1u);
        QCOMPARE(frame->buffers->stride[2][0], 256u);
        QVERIFY(frame->buffers->dmabufFd(2, 0) >= 0);

        auto *queue = rig.client->frames().get();
        queue->reportImportFailure(1);
        queue->reportImportFailure(1); // same set again: no second request
        Message m;
        QVERIFY(expectMessage(rig, MsgType::RequestLinear, &m));
        QCOMPARE(decodeAs<msg::RequestLinear>(m).viewport, kViewport);
        QVERIFY(nothingPending(rig));

        // The producer re-exports linear (generation 2) and that fails too.
        rig.producer.dmabufBuffers(2);
        QTRY_VERIFY(rig.client->frames()->buffers()->generation == 2);
        queue->reportImportFailure(1); // stale report
        queue->reportImportFailure(2);
        QVERIFY(expectMessage(rig, MsgType::RequestShm, &m));
        QCOMPARE(decodeAs<msg::RequestShm>(m).viewport, kViewport);

        // Shared memory arrives: no more requests, and the pixels are readable.
        rig.producer.shmBuffers(3);
        rig.producer.frame(0, 3);
        QTRY_COMPARE(rig.frames->count(), 2);
        queue->reportImportFailure(3);
        QVERIFY(nothingPending(rig));
        QCOMPARE(rig.lost->count(), 0);
    }

    void spanViewReachesAProducerThatKnowsIt()
    {
        Rig rig;
        rig.producer.hello(kSpanViewVersion);
        rig.producer.viewport(kViewport, 100, 50);
        QTRY_COMPARE(rig.ready->count(), 1);
        QVERIFY(rig.client->supportsSpan());
        QVERIFY(rig.client->sendSpanView(true, 3840, 1080, 1920, 0, 1920, 1080));
        Message m;
        QVERIFY(expectMessage(rig, kSpanViewType, &m));
        const auto view = decodeAs<msg::SpanView>(m);
        QCOMPARE(view.enabled, 1u);
        QCOMPARE(msg::spanLogical(view.canvasW), 3840.0f);
        QCOMPARE(msg::spanLogical(view.x), 1920.0f);
        QVERIFY(rig.client->sendSpanView(false, 0, 0, 0, 0, 0, 0));
        QVERIFY(expectMessage(rig, kSpanViewType, &m));
        QCOMPARE(decodeAs<msg::SpanView>(m).enabled, 0u);
    }

    void anOlderProducerIsAcceptedButGetsNoSpanView()
    {
        Rig rig;
        rig.producer.hello(kBridgeVersion);
        rig.producer.viewport(kViewport, 100, 50);
        QTRY_COMPARE(rig.ready->count(), 1);
        QCOMPARE(rig.lost->count(), 0);
        QVERIFY(!rig.client->supportsSpan());
        QCOMPARE(rig.client->peerVersion(), kBridgeVersion);
        QVERIFY(!rig.client->sendSpanView(true, 3840, 1080, 1920, 0, 1920, 1080));
        rig.client->sendFps(24); // other messages still flow
        Message m;
        QVERIFY(expectMessage(rig, MsgType::Fps, &m)); // and nothing else was sent before it
        // Turning span off for a peer that never had it is a quiet success
        QVERIFY(rig.client->sendSpanView(false, 0, 0, 0, 0, 0, 0));
    }

    void controlMessagesReachTheProducer()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport(kViewport, 100, 50);
        QTRY_COMPARE(rig.ready->count(), 1);
        QTRY_VERIFY(rig.client->viewportId().has_value());

        rig.client->sendPause(true);
        rig.client->sendVolume(500);
        rig.client->sendScaling(2);
        rig.client->sendFps(24);
        rig.client->sendResize(1920, 1080);
        rig.client->sendPointer(10, -20, 0xff);
        rig.client->sendProperties(QJsonObject{{QStringLiteral("speed"), 1.5}, {QStringLiteral("on"), true}});
        rig.client->sendQuit();

        Message m;
        QVERIFY(expectMessage(rig, MsgType::Pause, &m));
        QCOMPARE(decodeAs<msg::Pause>(m).paused, 1u);
        QVERIFY(expectMessage(rig, MsgType::Volume, &m));
        QCOMPARE(decodeAs<msg::Volume>(m).volume, 128u); // clamped
        QVERIFY(expectMessage(rig, MsgType::Scaling, &m));
        QCOMPARE(decodeAs<msg::Scaling>(m).mode, 2u);
        QVERIFY(expectMessage(rig, MsgType::Fps, &m));
        QCOMPARE(decodeAs<msg::Fps>(m).fps, 24u);
        QVERIFY(expectMessage(rig, MsgType::Resize, &m));
        const auto resize = decodeAs<msg::Resize>(m);
        QCOMPARE(resize.viewport, kViewport);
        QCOMPARE(resize.width, 1920u);
        QCOMPARE(resize.height, 1080u);
        QVERIFY(expectMessage(rig, MsgType::Pointer, &m));
        const auto pointer = decodeAs<msg::Pointer>(m);
        QCOMPARE(pointer.x, 10);
        QCOMPARE(pointer.y, -20);
        QCOMPARE(pointer.buttons, 7u);
        QVERIFY(msg::validate(pointer, 0));
        QVERIFY(expectMessage(rig, MsgType::Properties, &m));
        const auto json = decodeProperties(m.payload);
        QVERIFY(json.has_value());
        const QJsonObject obj = QJsonDocument::fromJson(QByteArray(json->data(), static_cast<qsizetype>(json->size()))).object();
        QCOMPARE(obj[QStringLiteral("speed")].toDouble(), 1.5);
        QCOMPARE(obj[QStringLiteral("on")].toBool(), true);
        QVERIFY(expectMessage(rig, MsgType::Quit, &m));
    }

    void twoBuffersAreEnough()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(1, 64, 32, 0, 2);
        rig.producer.frame(1, 1);
        QTRY_COMPARE(rig.frames->count(), 1);
        QCOMPARE(rig.lost->count(), 0);
    }

    void resizeBeforeViewportIsSentOnceAnnounced()
    {
        Rig rig;
        rig.client->sendResize(1280, 720); // nothing is known about the engine yet
        QVERIFY(nothingPending(rig));
        rig.producer.hello();
        rig.producer.viewport(kViewport, 640, 360);
        Message m;
        QVERIFY(expectMessage(rig, MsgType::Resize, &m));
        const auto resize = decodeAs<msg::Resize>(m);
        QCOMPARE(resize.viewport, kViewport);
        QCOMPARE(resize.width, 1280u);
        QCOMPARE(resize.height, 720u);
    }

    void buffersMayFollowAResizeAnswer()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport(kViewport, 64, 32);
        rig.producer.shmBuffers(1, 64, 32, 0, 2);
        QTRY_VERIFY(rig.client->viewportId().has_value());
        // The engine answers a Resize with a new buffer set of the requested size, no new Viewport.
        rig.client->sendResize(1280, 720);
        rig.producer.shmBuffers(2, 1280, 720, 0, 2);
        rig.producer.frame(0, 2);
        QTRY_COMPARE(rig.frames->count(), 1);
        QCOMPARE(rig.lost->count(), 0);
        // And shrinking back (a stale larger ceiling only loosens the check).
        rig.client->sendResize(640, 360);
        rig.producer.shmBuffers(3, 640, 360, 0, 2);
        rig.producer.frame(0, 3, false, 2);
        QTRY_COMPARE(rig.frames->count(), 2);
        QCOMPARE(rig.lost->count(), 0);
    }

    void resizeToTheAnnouncedSizeIsNotSent()
    {
        Rig rig;
        rig.client->sendResize(640, 360);
        rig.producer.hello();
        rig.producer.viewport(kViewport, 640, 360);
        QTRY_VERIFY(rig.client->viewportId().has_value());
        QVERIFY(nothingPending(rig));
    }

    void importSuccessRestartsTheFailureStaging()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.dmabufBuffers(1);
        QTRY_VERIFY(rig.client->frames()->buffers());
        auto *queue = rig.client->frames().get();
        queue->reportImportFailure(1);
        Message m;
        QVERIFY(expectMessage(rig, MsgType::RequestLinear, &m));
        rig.producer.dmabufBuffers(2);
        QTRY_VERIFY(rig.client->frames()->buffers()->generation == 2);
        queue->reportImportSuccess(2); // the linear export worked
        // Later (say after a resize) a new set fails again: negotiation starts from RequestLinear, not RequestShm.
        rig.producer.dmabufBuffers(3);
        QTRY_VERIFY(rig.client->frames()->buffers()->generation == 3);
        queue->reportImportFailure(3);
        QVERIFY(expectMessage(rig, MsgType::RequestLinear, &m));
    }

    void switchingBufferKindRestartsTheFailureStaging()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.dmabufBuffers(1);
        QTRY_VERIFY(rig.client->frames()->buffers());
        auto *queue = rig.client->frames().get();
        queue->reportImportFailure(1);
        Message m;
        QVERIFY(expectMessage(rig, MsgType::RequestLinear, &m));
        rig.producer.shmBuffers(2);
        QTRY_VERIFY(rig.client->frames()->buffers()->generation == 2);
        rig.producer.dmabufBuffers(3);
        QTRY_VERIFY(rig.client->frames()->buffers()->generation == 3);
        queue->reportImportFailure(3);
        QVERIFY(expectMessage(rig, MsgType::RequestLinear, &m));
    }

    void unknownMessagesAreIgnored()
    {
        Rig rig;
        rig.producer.hello();
        const uint8_t junk[3] = {1, 2, 3};
        rig.producer.sendRaw(static_cast<MsgType>(999), junk);
        rig.producer.sendRaw(MsgType::Pause, std::span<const uint8_t>(junk, 3)); // consumer-direction type
        rig.producer.viewport();
        QTRY_VERIFY(rig.client->viewportId().has_value());
        QCOMPARE(rig.lost->count(), 0);
    }

    void protocolViolationsEndTheConnection_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("frame before hello") << "frame-first";
        QTest::newRow("version mismatch") << "version";
        QTest::newRow("version below the oldest") << "tooold";
        QTest::newRow("duplicate hello") << "double-hello";
        QTest::newRow("buffers for unknown viewport") << "unknown-viewport";
        QTest::newRow("unsealed ring") << "unsealed";
        QTest::newRow("bad geometry") << "geometry";
        QTest::newRow("truncated payload") << "truncated";
        QTest::newRow("hostile frame flag") << "flip";
        QTest::newRow("oversize datagram") << "oversize";
        QTest::newRow("unexpected fd on frame") << "fd-on-frame";
        QTest::newRow("zero viewport size") << "zero-viewport";
        QTest::newRow("single shm buffer") << "one-shm";
        QTest::newRow("single dmabuf buffer") << "one-dmabuf";
        QTest::newRow("too many viewports") << "many-viewports";
        QTest::newRow("buffers far larger than the viewport") << "huge-buffers";
    }

    void protocolViolationsEndTheConnection()
    {
        QFETCH(QString, what);
        Rig rig;
        FakeProducer &p = rig.producer;
        if (what == QLatin1String("frame-first")) {
            p.frame(0, 1);
        } else if (what == QLatin1String("tooold")) {
            p.hello(kBridgeVersion - 1);
        } else if (what == QLatin1String("version")) {
            p.hello(kPuppetVersion + 1);
        } else if (what == QLatin1String("double-hello")) {
            p.hello();
            p.hello();
        } else if (what == QLatin1String("unknown-viewport")) {
            p.hello();
            p.shmBuffers(1); // viewport never announced
        } else if (what == QLatin1String("unsealed")) {
            p.hello();
            p.viewport();
            UniqueFd fd(::memfd_create("unsealed", MFD_CLOEXEC | MFD_ALLOW_SEALING));
            QVERIFY(::ftruncate(fd.get(), 3 * 64 * 32 * 4) == 0);
            msg::ShmBuffers m{kViewport, 3, 64, 32, 256, kFormatRgba8, 1};
            const int raw = fd.get();
            QCOMPARE(p.channel.send(m, std::span<const int>(&raw, 1)), ChannelStatus::Ok);
        } else if (what == QLatin1String("geometry")) {
            p.hello();
            p.viewport();
            msg::ShmBuffers m{kViewport, 3, 64, 32, 10 /* stride < width * 4 */, kFormatRgba8, 1};
            UniqueFd fd(::memfd_create("x", MFD_CLOEXEC));
            const int raw = fd.get();
            QCOMPARE(p.channel.send(m, std::span<const int>(&raw, 1)), ChannelStatus::Ok);
        } else if (what == QLatin1String("truncated")) {
            p.hello();
            const uint8_t two[2] = {0, 0};
            p.sendRaw(MsgType::Viewport, two);
        } else if (what == QLatin1String("flip")) {
            p.hello();
            p.viewport();
            p.shmBuffers(1);
            msg::Frame f{kViewport, 0, 1, 7 /* flipY must be 0 or 1 */, 1};
            p.send(f);
        } else if (what == QLatin1String("oversize")) {
            p.hello();
            std::vector<uint8_t> big(kMaxPayload + 1);
            QCOMPARE(p.channel.send(MsgType::Error, big), ChannelStatus::TooLarge); // refused on our side...
            // ...so send it raw: a datagram the client must reject.
            std::vector<uint8_t> raw(sizeof(MsgHeader) + big.size());
            MsgHeader h{static_cast<uint32_t>(MsgType::Error), static_cast<uint32_t>(big.size())};
            std::memcpy(raw.data(), &h, sizeof(h));
            QVERIFY(::send(p.channel.fd(), raw.data(), raw.size(), MSG_NOSIGNAL) >= 0);
        } else if (what == QLatin1String("fd-on-frame")) {
            p.hello();
            p.viewport();
            p.shmBuffers(1);
            UniqueFd fd(::memfd_create("x", MFD_CLOEXEC));
            const int raw = fd.get();
            QCOMPARE(p.channel.send(msg::Frame{kViewport, 0, 1, 0, 1}, std::span<const int>(&raw, 1)),
                     ChannelStatus::Ok);
        } else if (what == QLatin1String("one-shm")) {
            p.hello();
            p.viewport();
            p.shmBuffers(1, 64, 32, 0, 1);
        } else if (what == QLatin1String("one-dmabuf")) {
            p.hello();
            p.viewport();
            p.dmabufBuffers(1, 1);
        } else if (what == QLatin1String("many-viewports")) {
            p.hello();
            for (uint32_t id = 1; id <= 9; ++id)
                p.viewport(id, 64, 32);
        } else if (what == QLatin1String("huge-buffers")) {
            p.hello();
            p.viewport(kViewport, 64, 32);
            p.shmBuffers(1, 129, 32, 0, 2); // more than twice the announced width
        } else if (what == QLatin1String("zero-viewport")) {
            p.hello();
            p.viewport(kViewport, 0, 0);
        }
        QTRY_COMPARE(rig.lost->count(), 1);
        QVERIFY(!rig.client->isOpen());
        // Inert afterwards: nothing is sent, and the reason is reported exactly once.
        rig.client->sendPause(true);
        QTest::qWait(30);
        QCOMPARE(rig.lost->count(), 1);
    }

    void peerCloseReportsEngineError()
    {
        Rig rig;
        rig.producer.hello();
        msg::Error e{};
        e.code = 3;
        setString(e.text, "scene failed to load");
        rig.producer.send(e);
        rig.producer.channel = Channel(); // engine exits
        QTRY_COMPARE(rig.lost->count(), 1);
        const QString reason = rig.lost->first().first().toString();
        QVERIFY2(reason.contains(QStringLiteral("scene failed to load")), qPrintable(reason));
        QCOMPARE(rig.client->lastEngineError(), QStringLiteral("scene failed to load"));
    }

    void framesBeforeCloseAreStillDelivered()
    {
        Rig rig;
        rig.producer.hello();
        rig.producer.viewport();
        rig.producer.shmBuffers(1);
        rig.producer.frame(0, 1);
        rig.producer.channel = Channel();
        QTRY_COMPARE(rig.lost->count(), 1);
        QCOMPARE(rig.frames->count(), 1);
        QVERIFY(rig.client->frames()->takeFrame().has_value());
    }

    void releaseAfterCloseIsHarmless()
    {
        auto rig = std::make_unique<Rig>();
        rig->producer.hello();
        rig->producer.viewport();
        rig->producer.shmBuffers(1);
        rig->producer.frame(0, 1);
        QTRY_COMPARE(rig->frames->count(), 1);
        auto queue = rig->client->frames();
        auto frame = queue->takeFrame();
        rig->client.reset(); // the item is gone; the render thread still holds the queue
        queue->release(*frame);
        queue->reportImportFailure(1);
        QVERIFY(queue->takeRequests().empty());
    }
};

QTEST_GUILESS_MAIN(TstBridgeClient)
#include "tst_bridgeclient.moc"
