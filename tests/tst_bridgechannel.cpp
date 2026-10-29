#include "bridge/channel.h"
#include "bridge/spanview.h"
#include "testutil.h"

#include <QDir>
#include <QTemporaryFile>
#include <QTest>

#include <cmath>
#include <limits>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace murasaki::bridge;
using namespace murasaki::bridge::msg;

namespace {

int openFdCount()
{
    return static_cast<int>(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot).size());
}

struct Pair {
    Channel a;
    Channel b;
};

Pair makePair()
{
    auto sp = makeSocketPair();
    if (!sp) {
        qFatal("socketpair failed");
    }
    return {Channel(std::move(sp->first)), Channel(std::move(sp->second))};
}

// Sends an arbitrary datagram, bypassing Channel's own checks.
void sendRaw(int fd, const void *data, size_t size, const std::vector<int> &fds = {})
{
    iovec iov{const_cast<void *>(data), size};
    alignas(cmsghdr) char control[CMSG_SPACE(64 * sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (!fds.empty()) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
        std::memcpy(CMSG_DATA(cm), fds.data(), fds.size() * sizeof(int));
    }
    QCOMPARE_NE(::sendmsg(fd, &msg, MSG_NOSIGNAL), -1);
}

template <class T>
T roundTrip(Pair &p, const T &in)
{
    T out{};
    [&] {
        QCOMPARE(p.a.send(in), ChannelStatus::Ok);
        ReceiveResult r = p.b.receive();
        QCOMPARE(r.status, ChannelStatus::Ok);
        QCOMPARE(r.message.type, static_cast<uint32_t>(T::kType));
        QVERIFY(decode<T>(r.message.payload, out));
    }();
    return out;
}

} // namespace

class TestBridgeChannel : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsEveryMessage();
    void versionAndSizes();
    void stringsTruncateAndTerminate();
    void decodeRejectsShortPayload();
    void propertiesEncoding();
    void propertiesOverChannel();
    void fdsArePassedAndUsable();
    void fdsAreCloexecOnReceive();
    void oversizeAndTruncatedAreRejectedWithoutLeaks();
    void tooManyFdsRejectedWithoutLeaks();
    void sendRejectsOverCaps();
    void sendWouldBlockKeepsFds();
    void unknownTypePassesThrough();
    void zeroLengthDatagramIsClosed();
    void drainHonoursLimit();
    void validateRules();
    void decodeZeroesUnusedPlanes();
    void regularFileRejectedByRing();
    void nonblockingReceiveReportsWouldBlock();
    void closedPeerReportsClosed();
    void ringRoundTripsKnownPattern();
    void ringRejectsBadGeometry();
    void ringRejectsSizeAndSealMismatch();
};

void TestBridgeChannel::roundTripsEveryMessage()
{
    Pair p = makePair();

    Hello hello{kBridgeVersion, 4242};
    Hello h2 = roundTrip(p, hello);
    QCOMPARE(h2.version, kBridgeVersion);
    QCOMPARE(h2.pid, 4242u);

    Viewport vp{};
    vp.id = 3;
    vp.width = 1920;
    vp.height = 1080;
    setString(vp.name, "DP-1");
    Viewport v2 = roundTrip(p, vp);
    QCOMPARE(v2.id, 3u);
    QCOMPARE(v2.width, 1920u);
    QCOMPARE(v2.height, 1080u);
    QCOMPARE(QString::fromUtf8(getString(v2.name).data(), getString(v2.name).size()), QStringLiteral("DP-1"));

    DmabufBuffers db{};
    db.viewport = 1;
    db.count = 3;
    db.planes = 4;
    db.fourcc = 0x34325258;
    db.width = 64;
    db.height = 32;
    db.generation = 7;
    db.modifier = 0x0123456789abcdefull;
    for (uint32_t b = 0; b < 3; ++b) {
        for (uint32_t pl = 0; pl < 4; ++pl) {
            db.plane[b][pl] = {b * 100 + pl, b * 1000 + pl * 10 + 4};
        }
    }
    DmabufBuffers d2 = roundTrip(p, db);
    QVERIFY(validate(d2, 12));
    QCOMPARE(d2.modifier, db.modifier);
    QCOMPARE(d2.fourcc, db.fourcc);
    QCOMPARE(d2.generation, 7u);
    QCOMPARE(d2.plane[2][3].offset, 203u);
    QCOMPARE(d2.plane[2][3].stride, 2034u);

    ShmBuffers sb{2, 3, 640, 480, 2560, kFormatRgba8, 9};
    ShmBuffers s2 = roundTrip(p, sb);
    QVERIFY(validate(s2, 1));
    QCOMPARE(s2.stride, 2560u);
    QCOMPARE(s2.generation, 9u);

    Frame fr{1, 2, 0x1122334455667788ull, 1, 5};
    Frame f2 = roundTrip(p, fr);
    QCOMPARE(f2.seq, fr.seq);
    QCOMPARE(f2.buffer, 2u);
    QCOMPARE(f2.flipY, 1u);
    QCOMPARE(f2.generation, 5u);

    Error er{};
    er.code = 7;
    setString(er.text, "boom");
    Error e2 = roundTrip(p, er);
    QCOMPARE(e2.code, 7u);
    QCOMPARE(getString(e2.text), std::string_view("boom"));

    Release rel = roundTrip(p, Release{1, 2, 11});
    QCOMPARE(rel.buffer, 2u);
    QCOMPARE(rel.generation, 11u);
    QCOMPARE(roundTrip(p, RequestLinear{5}).viewport, 5u);
    QCOMPARE(roundTrip(p, RequestShm{6}).viewport, 6u);
    QCOMPARE(roundTrip(p, Pause{1}).paused, 1u);
    QCOMPARE(roundTrip(p, Volume{128}).volume, 128u);
    Pointer pt = roundTrip(p, Pointer{1, -5, 700, 3});
    QCOMPARE(pt.x, -5);
    QCOMPARE(pt.y, 700);
    QCOMPARE(pt.buttons, 3u);
    Resize rs = roundTrip(p, Resize{1, 800, 600});
    QCOMPARE(rs.width, 800u);
    QCOMPARE(rs.height, 600u);
    QCOMPARE(roundTrip(p, Scaling{static_cast<uint32_t>(ScalingMode::Stretch)}).mode, 2u);
    QCOMPARE(roundTrip(p, Fps{30}).fps, 30u);
    const SpanView sv = roundTrip(p, makeSpanView(true, 3840, 1080, 1920.5, 0, 1920, 1080));
    QCOMPARE(sv.enabled, 1u);
    QCOMPARE(spanLogical(sv.canvasW), 3840.0f);
    QCOMPARE(spanLogical(sv.x), 1920.5f);
    QCOMPARE(spanLogical(sv.h), 1080.0f);
    roundTrip(p, Quit{});
}

void TestBridgeChannel::versionAndSizes()
{
    QCOMPARE(kBridgeVersion, 2u); // the base protocol, shared by the producers
    QCOMPARE(kSpanViewVersion, 3u);
    QCOMPARE(static_cast<uint32_t>(kSpanViewType), 43u);
    QCOMPARE(sizeof(SpanView), size_t(28));
    QCOMPARE(sizeof(ShmBuffers), size_t(28));
    QCOMPARE(sizeof(DmabufBuffers), size_t(136));
    QCOMPARE(sizeof(Frame), size_t(24));
    QCOMPARE(sizeof(Release), size_t(12));
}

void TestBridgeChannel::stringsTruncateAndTerminate()
{
    char buf[8];
    std::memset(buf, 'x', sizeof(buf));
    setString(buf, "0123456789");
    QCOMPARE(buf[7], '\0');
    QCOMPARE(getString(buf), std::string_view("0123456"));
    setString(buf, "ab");
    QCOMPARE(getString(buf), std::string_view("ab"));
    QCOMPARE(buf[7], '\0');

    // A hostile peer sends a name without NUL: decode re-terminates it.
    Viewport vp{};
    std::memset(vp.name, 'A', sizeof(vp.name));
    Viewport out{};
    QVERIFY(decode<Viewport>(encode(vp), out));
    QCOMPARE(getString(out.name).size(), kViewportNameSize - 1);
    Error er{};
    std::memset(er.text, 'B', sizeof(er.text));
    Error eo{};
    QVERIFY(decode<Error>(encode(er), eo));
    QCOMPARE(getString(eo.text).size(), kErrorTextSize - 1);
}

void TestBridgeChannel::decodeRejectsShortPayload()
{
    auto bytes = encode(Resize{1, 2, 3});
    Resize out{};
    QVERIFY(decode<Resize>(bytes, out));
    QVERIFY(!decode<Resize>(std::span<const uint8_t>(bytes).first(sizeof(Resize) - 1), out));
    QVERIFY(!decode<Resize>(std::span<const uint8_t>(), out));
    bytes.push_back(0xff); // trailing bytes from a newer peer are ignored
    QVERIFY(decode<Resize>(bytes, out));
    QCOMPARE(out.height, 3u);
}

void TestBridgeChannel::propertiesEncoding()
{
    const auto empty = encodeProperties("");
    QVERIFY(empty);
    QCOMPARE(empty->size(), size_t(4));
    QCOMPARE(decodeProperties(*empty).value(), std::string_view(""));

    const std::string json = R"({"color":"1 0 0","speed":2})";
    const auto enc = encodeProperties(json);
    QVERIFY(enc);
    QCOMPARE(enc->size(), json.size() + 4);
    QCOMPARE(decodeProperties(*enc).value(), std::string_view(json));

    // exactly the limit passes, one more does not
    QVERIFY(encodeProperties(std::string(kMaxPropertiesJson, 'a')));
    QVERIFY(!encodeProperties(std::string(kMaxPropertiesJson + 1, 'a')));

    // bad payloads
    QVERIFY(!decodeProperties({}));
    std::vector<uint8_t> shortPrefix{1, 0};
    QVERIFY(!decodeProperties(shortPrefix));
    auto lying = *enc;
    lying.pop_back(); // declared length now exceeds the data
    QVERIFY(!decodeProperties(lying));
    auto trailing = *enc;
    trailing.push_back('x'); // trailing garbage
    QVERIFY(!decodeProperties(trailing));
    std::vector<uint8_t> huge(4 + 10, 'a');
    const uint32_t big = kMaxPropertiesJson + 1;
    std::memcpy(huge.data(), &big, 4);
    QVERIFY(!decodeProperties(huge));
}

void TestBridgeChannel::propertiesOverChannel()
{
    Pair p = makePair();
    const std::string json(kMaxPropertiesJson, 'j');
    QCOMPARE(p.a.sendProperties(json), ChannelStatus::Ok);
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Ok);
    QCOMPARE(r.message.type, static_cast<uint32_t>(MsgType::Properties));
    QCOMPARE(decodeProperties(r.message.payload).value().size(), size_t(kMaxPropertiesJson));
    QCOMPARE(p.a.sendProperties(std::string(kMaxPropertiesJson + 1, 'j')), ChannelStatus::TooLarge);
}

void TestBridgeChannel::fdsArePassedAndUsable()
{
    Pair p = makePair();
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, O_CLOEXEC), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);

    const int before = openFdCount();
    const std::array<int, 2> sent{wr.get(), rd.get()};
    QCOMPARE(p.a.send(MsgType::Hello, encode(Hello{1, 2}), sent), ChannelStatus::Ok);
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Ok);
    QCOMPARE(r.message.fds.size(), size_t(2));
    QCOMPARE(openFdCount(), before + 2);

    // write through the received write end, read from the original read end
    QCOMPARE(::write(r.message.fds[0].get(), "hi", 2), ssize_t(2));
    char buf[4] = {};
    QCOMPARE(::read(rd.get(), buf, 2), ssize_t(2));
    QCOMPARE(QByteArray(buf, 2), QByteArray("hi"));

    r.message.fds.clear();
    QCOMPARE(openFdCount(), before);
}

void TestBridgeChannel::fdsAreCloexecOnReceive()
{
    Pair p = makePair();
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, 0), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);
    const std::array<int, 1> sent{rd.get()};
    QCOMPARE(p.a.send(MsgType::Hello, encode(Hello{1, 2}), sent), ChannelStatus::Ok);
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Ok);
    QVERIFY((::fcntl(r.message.fds[0].get(), F_GETFD) & FD_CLOEXEC) != 0);
}

void TestBridgeChannel::oversizeAndTruncatedAreRejectedWithoutLeaks()
{
    Pair p = makePair();
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, O_CLOEXEC), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);
    const int before = openFdCount();

    // datagram larger than the cap, carrying an fd
    std::vector<uint8_t> big(kMaxMessage + 100, 0);
    MsgHeader h{static_cast<uint32_t>(MsgType::Properties), static_cast<uint32_t>(big.size() - 8)};
    std::memcpy(big.data(), &h, sizeof(h));
    sendRaw(p.a.fd(), big.data(), big.size(), {rd.get()});
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::TooLarge);
    QVERIFY(r.message.fds.empty());
    QCOMPARE(openFdCount(), before);

    // header declares more than the cap while the datagram is small
    MsgHeader lie{static_cast<uint32_t>(MsgType::Properties), static_cast<uint32_t>(kMaxPayload + 1)};
    sendRaw(p.a.fd(), &lie, sizeof(lie), {rd.get()});
    QCOMPARE(p.b.receive().status, ChannelStatus::TooLarge);
    QCOMPARE(openFdCount(), before);

    // datagram shorter than the header
    const char tiny[3] = {1, 2, 3};
    sendRaw(p.a.fd(), tiny, sizeof(tiny), {rd.get()});
    QCOMPARE(p.b.receive().status, ChannelStatus::Malformed);
    QCOMPARE(openFdCount(), before);

    // header size disagrees with the datagram length
    struct {
        MsgHeader h;
        uint32_t body;
    } mismatch{{static_cast<uint32_t>(MsgType::Fps), 8}, 1};
    sendRaw(p.a.fd(), &mismatch, sizeof(mismatch), {rd.get(), wr.get()});
    QCOMPARE(p.b.receive().status, ChannelStatus::Malformed);
    QCOMPARE(openFdCount(), before);

    // the channel is still usable afterwards
    QCOMPARE(p.a.send(Fps{60}), ChannelStatus::Ok);
    QCOMPARE(p.b.receive().status, ChannelStatus::Ok);
}

void TestBridgeChannel::tooManyFdsRejectedWithoutLeaks()
{
    Pair p = makePair();
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, O_CLOEXEC), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);
    const int before = openFdCount();

    const MsgHeader h{static_cast<uint32_t>(MsgType::Hello), 0};
    sendRaw(p.a.fd(), &h, sizeof(h), std::vector<int>(kMaxFds + 8, rd.get()));
    const ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Malformed);
    QVERIFY(r.message.fds.empty());
    QCOMPARE(openFdCount(), before);
}

void TestBridgeChannel::sendRejectsOverCaps()
{
    Pair p = makePair();
    std::vector<uint8_t> big(kMaxPayload + 1);
    QCOMPARE(p.a.send(MsgType::Properties, big), ChannelStatus::TooLarge);
    const std::vector<int> manyFds(kMaxFds + 1, 0);
    QCOMPARE(p.a.send(MsgType::Hello, {}, manyFds), ChannelStatus::TooLarge);
    // nothing reached the peer
    p.b.setNonblocking(true);
    QCOMPARE(p.b.receive().status, ChannelStatus::WouldBlock);
}

void TestBridgeChannel::nonblockingReceiveReportsWouldBlock()
{
    Pair p = makePair();
    QVERIFY(p.b.setNonblocking(true));
    QCOMPARE(p.b.receive().status, ChannelStatus::WouldBlock);
    QCOMPARE(p.a.send(Quit{}), ChannelStatus::Ok);
    QCOMPARE(p.b.receive().status, ChannelStatus::Ok);
    QCOMPARE(p.b.receive().status, ChannelStatus::WouldBlock);
}

void TestBridgeChannel::closedPeerReportsClosed()
{
    Pair p = makePair();
    p.a = Channel();
    QCOMPARE(p.b.receive().status, ChannelStatus::Closed);
    QCOMPARE(p.b.send(Quit{}), ChannelStatus::Closed);
}

void TestBridgeChannel::ringRoundTripsKnownPattern()
{
    const ShmGeometry g{3, 64, 32, 64 * 4 + 16};
    ShmError err = ShmError::BadFd;
    auto producer = ShmRing::create(g, &err);
    QVERIFY(producer);
    QCOMPARE(err, ShmError::Ok);
    QVERIFY(producer->writable());
    QCOMPARE(producer->frameBytes(), size_t(g.stride) * g.height);
    QVERIFY(producer->frame(3).empty());

    for (uint32_t i = 0; i < g.count; ++i) {
        auto f = producer->frame(i);
        QCOMPARE(f.size(), producer->frameBytes());
        for (size_t o = 0; o < f.size(); ++o) {
            f[o] = static_cast<uint8_t>((o * 7 + i * 31) & 0xff);
        }
    }

    // pass the memfd through a channel like the real protocol does
    Pair p = makePair();
    const std::array<int, 1> fds{producer->fd()};
    QCOMPARE(p.a.send(MsgType::ShmBuffers, encode(ShmBuffers{0, g.count, g.width, g.height, g.stride, kFormatRgba8, 0}), fds),
             ChannelStatus::Ok);
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Ok);
    QCOMPARE(r.message.fds.size(), size_t(1));
    ShmBuffers sb{};
    QVERIFY(decode<ShmBuffers>(r.message.payload, sb));

    auto consumer = ShmRing::open(std::move(r.message.fds[0]), {sb.count, sb.width, sb.height, sb.stride}, &err);
    QVERIFY(consumer);
    QVERIFY(!consumer->writable());
    for (uint32_t i = 0; i < g.count; ++i) {
        const std::span<const uint8_t> f = std::as_const(*consumer).frame(i);
        QCOMPARE(f.size(), producer->frameBytes());
        for (size_t o = 0; o < f.size(); ++o) {
            if (f[o] != static_cast<uint8_t>((o * 7 + i * 31) & 0xff)) {
                QFAIL("ring content differs");
            }
        }
    }
    // a read-only consumer has no mutable access
    QVERIFY(consumer->frame(0).empty());

    // later producer writes are visible (shared mapping)
    producer->frame(1)[5] = 0xAB;
    QCOMPARE(std::as_const(*consumer).frame(1)[5], uint8_t(0xAB));
}

void TestBridgeChannel::ringRejectsBadGeometry()
{
    const ShmGeometry good{2, 16, 16, 64};
    QVERIFY(validGeometry(good));
    QVERIFY(!validGeometry({0, 16, 16, 64}));
    QVERIFY(!validGeometry({4, 16, 16, 64}));
    QVERIFY(!validGeometry({2, 0, 16, 64}));
    QVERIFY(!validGeometry({2, 16, 0, 64}));
    QVERIFY(!validGeometry({2, 16384 + 1, 16, 70000}));
    QVERIFY(!validGeometry({2, 16, 16384 + 1, 64}));
    QVERIFY(!validGeometry({2, 16, 16, 63})); // stride < width * 4
    QVERIFY(!validGeometry({3, 16384, 16384, 0xffffffffu})); // would overflow 32 bits and exceed the cap
    QVERIFY(validGeometry({3, 16384, 16384, 16384 * 4}));

    ShmError err = ShmError::Ok;
    QVERIFY(!ShmRing::create({2, 16, 16, 63}, &err));
    QCOMPARE(err, ShmError::BadGeometry);
    QVERIFY(!ShmRing::open(UniqueFd(::memfd_create("x", 0)), {2, 16, 16, 63}, &err));
    QCOMPARE(err, ShmError::BadGeometry);
}

void TestBridgeChannel::ringRejectsSizeAndSealMismatch()
{
    const ShmGeometry g{2, 16, 16, 64};
    ShmError err = ShmError::Ok;

    // genuine ring, but the peer declares a different geometry
    auto ring = ShmRing::create(g);
    QVERIFY(ring);
    for (const ShmGeometry &wrong : {ShmGeometry{3, 16, 16, 64}, ShmGeometry{2, 17, 16, 68}, ShmGeometry{2, 16, 15, 64},
                                     ShmGeometry{2, 16, 16, 128}}) {
        QVERIFY(!ShmRing::open(UniqueFd(::dup(ring->fd())), wrong, &err));
        QCOMPARE(err, ShmError::SizeMismatch);
    }
    QVERIFY(ShmRing::open(UniqueFd(::dup(ring->fd())), g, &err));

    // right size but not sealed at all
    UniqueFd unsealed(::memfd_create("u", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    QCOMPARE(::ftruncate(unsealed.get(), 2 * 64 * 16), 0);
    QVERIFY(!ShmRing::open(std::move(unsealed), g, &err));
    QCOMPARE(err, ShmError::NotSealed);

    // sealed against shrink/grow only (no F_SEAL_SEAL): sealing can still be changed, so reject
    UniqueFd partial(::memfd_create("p", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    QCOMPARE(::ftruncate(partial.get(), 2 * 64 * 16), 0);
    QCOMPARE(::fcntl(partial.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW), 0);
    QVERIFY(!ShmRing::open(std::move(partial), g, &err));
    QCOMPARE(err, ShmError::NotSealed);

    // not a regular file at all
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, O_CLOEXEC), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);
    QVERIFY(!ShmRing::open(std::move(rd), g, &err));
    QCOMPARE(err, ShmError::BadFd);

    QVERIFY(!ShmRing::open(UniqueFd(), g, &err));
    QCOMPARE(err, ShmError::BadFd);
}

void TestBridgeChannel::sendWouldBlockKeepsFds()
{
    Pair p = makePair();
    int pipefd[2];
    QCOMPARE(::pipe2(pipefd, O_CLOEXEC), 0);
    UniqueFd rd(pipefd[0]);
    UniqueFd wr(pipefd[1]);
    const std::array<int, 1> fds{rd.get()};
    size_t sent = 0;
    ChannelStatus st = ChannelStatus::Ok;
    while (st == ChannelStatus::Ok && sent < 1000000) {
        st = p.a.send(Hello{1, 2}, fds);
        sent += st == ChannelStatus::Ok ? 1 : 0;
    }
    QCOMPARE(st, ChannelStatus::WouldBlock);
    QVERIFY(sent > 0);
    const int before = openFdCount();
    QCOMPARE(p.a.send(Hello{1, 2}, fds), ChannelStatus::WouldBlock); // still full, no side effects
    QCOMPARE(openFdCount(), before);
    QVERIFY(::fcntl(rd.get(), F_GETFD) >= 0); // caller keeps its fd

    // draining frees room; exactly the successful sends arrive, each with its fd
    size_t received = 0;
    for (;;) {
        ReceiveResult r = p.b.receive();
        if (r.status != ChannelStatus::Ok) {
            break;
        }
        QCOMPARE(r.message.fds.size(), size_t(1));
        ++received;
        if (received == sent) {
            break;
        }
    }
    QCOMPARE(received, sent);
    QCOMPARE(p.a.send(Hello{1, 2}, fds), ChannelStatus::Ok);
}

void TestBridgeChannel::unknownTypePassesThrough()
{
    Pair p = makePair();
    const std::array<uint8_t, 3> body{1, 2, 3};
    QCOMPARE(p.a.send(static_cast<MsgType>(9999), body), ChannelStatus::Ok);
    ReceiveResult r = p.b.receive();
    QCOMPARE(r.status, ChannelStatus::Ok);
    QCOMPARE(r.message.type, 9999u);
    QCOMPARE(r.message.payload.size(), size_t(3));
}

void TestBridgeChannel::zeroLengthDatagramIsClosed()
{
    Pair p = makePair();
    sendRaw(p.a.fd(), "", 0);
    QCOMPARE(p.b.receive().status, ChannelStatus::Closed);
}

void TestBridgeChannel::drainHonoursLimit()
{
    Pair p = makePair();
    QVERIFY(p.b.setNonblocking(true));
    for (uint32_t i = 0; i < 5; ++i) {
        QCOMPARE(p.a.send(Fps{i}), ChannelStatus::Ok);
    }
    std::vector<uint32_t> seen;
    auto handle = [&](Message &&m) {
        Fps f{};
        QVERIFY(decode<Fps>(m.payload, f));
        seen.push_back(f.fps);
    };
    DrainResult d = p.b.drain(handle, 3);
    QCOMPARE(d.handled, size_t(3));
    QCOMPARE(d.last, ChannelStatus::Ok);
    d = p.b.drain(handle, 10);
    QCOMPARE(d.handled, size_t(2));
    QCOMPARE(d.last, ChannelStatus::WouldBlock);
    QCOMPARE(seen, (std::vector<uint32_t>{0, 1, 2, 3, 4}));
}

void TestBridgeChannel::validateRules()
{
    DmabufBuffers d{};
    d.count = 2;
    d.planes = 2;
    d.fourcc = 1;
    d.width = 64;
    d.height = 64;
    for (uint32_t b = 0; b < 2; ++b) {
        for (uint32_t pl = 0; pl < 2; ++pl) {
            d.plane[b][pl] = {0, 256};
        }
    }
    QVERIFY(validate(d, 4));
    QVERIFY(!validate(d, 3)); // fd count must equal count * planes
    DmabufBuffers bad = d;
    bad.count = 4;
    QVERIFY(!validate(bad, 8));
    bad = d;
    bad.planes = 5;
    QVERIFY(!validate(bad, 10));
    bad = d;
    bad.fourcc = 0;
    QVERIFY(!validate(bad, 4));
    bad = d;
    bad.width = kMaxDimension + 1;
    QVERIFY(!validate(bad, 4));
    bad = d;
    bad.plane[1][1].stride = 0;
    QVERIFY(!validate(bad, 4));
    bad = d;
    bad.plane[0][1] = {0xffffffffu, 0xffffffffu}; // would overflow 32 bits; exceeds 4 GiB-class limit
    QVERIFY(!validate(bad, 4));

    ShmBuffers s{0, 3, 16, 16, 64, kFormatRgba8, 0};
    QVERIFY(validate(s, 1));
    QVERIFY(!validate(s, 0));
    QVERIFY(!validate(s, 2));
    s.stride = 66; // not a multiple of 4
    QVERIFY(!validate(s, 1));
    s.stride = 64;
    s.format = 9;
    QVERIFY(!validate(s, 1));

    QVERIFY(validate(Resize{0, 1, kMaxDimension}, 0));
    QVERIFY(!validate(Resize{0, 0, 10}, 0));
    QVERIFY(!validate(Resize{0, 10, kMaxDimension + 1}, 0));
    QVERIFY(validate(Volume{128}, 0));
    QVERIFY(!validate(Volume{129}, 0));
    QVERIFY(validate(Scaling{2}, 0));
    QVERIFY(!validate(Scaling{3}, 0));
    // SpanView: the viewport must lie inside a sane canvas; "off" needs no geometry.
    const auto span = [](double cw, double ch, double x, double y, double w, double h) { return makeSpanView(true, cw, ch, x, y, w, h); };
    QVERIFY(validate(span(3840, 1080, 1920, 0, 1920, 1080), 0));
    QVERIFY(validate(span(3840, 1080, 0.5, 0.25, 1919.5, 1079.5), 0));
    QVERIFY(validate(makeSpanView(false, 9, 9, 9, 9, 9, 9), 0));
    QCOMPARE(makeSpanView(false, 9, 9, 9, 9, 9, 9).canvasW, 0); // off carries no geometry
    SpanView badSpan = span(3840, 1080, 0, 0, 1920, 1080);
    badSpan.enabled = 2;
    QVERIFY(!validate(badSpan, 0));
    QVERIFY(!validate(span(3840, 1080, 0, 0, 1920, 1080), 1)); // no fds
    QVERIFY(!validate(span(0, 1080, 0, 0, 100, 100), 0));
    QVERIFY(!validate(span(3840, 1080, 0, 0, 0, 100), 0));
    QVERIFY(!validate(span(3840, 1080, 2000, 0, 1920, 1080), 0)); // reaches past the canvas
    QVERIFY(!validate(span(3840, 1080, -1, 0, 100, 100), 0));
    QVERIFY(!validate(span(70000, 1080, 0, 0, 100, 100), 0));
    QVERIFY(!validate(span(3840, 1080, 0, 0, 100, 1081), 0));
    SpanView huge = span(3840, 1080, 0, 0, 100, 100);
    huge.x = INT32_MAX; // no overflow when adding the extent
    QVERIFY(!validate(huge, 0));
    QVERIFY(validate(Pointer{0, -10, 5000, 7}, 0));
    QVERIFY(!validate(Pointer{0, INT32_MIN, 0, 0}, 0));
    QVERIFY(!validate(Pointer{0, 0, 0, 8}, 0));
    QVERIFY(validate(Frame{0, 0, 1, 1, 3}, 0));
    QVERIFY(!validate(Frame{0, 0, 1, 2, 3}, 0));
    QVERIFY(!validate(Frame{0, 0, 1, 0, 3}, 1)); // stray fd
    QVERIFY(!validate(Quit{}, 1));
}

void TestBridgeChannel::decodeZeroesUnusedPlanes()
{
    DmabufBuffers d{};
    d.count = 1;
    d.planes = 1;
    for (auto &row : d.plane) {
        for (auto &pl : row) {
            pl = {9, 9};
        }
    }
    DmabufBuffers out{};
    QVERIFY(decode<DmabufBuffers>(encode(d), out));
    QCOMPARE(out.plane[0][0].stride, 9u);
    QCOMPARE(out.plane[0][1].stride, 0u);
    QCOMPARE(out.plane[1][0].stride, 0u);
    QCOMPARE(out.plane[2][3].offset, 0u);
}

void TestBridgeChannel::regularFileRejectedByRing()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const ShmGeometry g{1, 16, 16, 64};
    QCOMPARE(::ftruncate(file.handle(), 64 * 16), 0);
    ShmError err = ShmError::Ok;
    QVERIFY(!ShmRing::open(UniqueFd(::dup(file.handle())), g, &err));
    QCOMPARE(err, ShmError::NotSealed);
}

QTEST_APPLESS_MAIN(TestBridgeChannel)
#include "tst_bridgechannel.moc"
