// The host side of the media bridge: the policy of what a player and its cover URL may be (pure functions), the cover files, and
// the players of an isolated session bus (fakes) as the hub sees them. No network: https covers are covered by the URL policy
// and the address checks only.

#include "bridge/channel.h"
#include "bridge/mediaview.h"
#include "bridgeclient.h"
#include "media/artdecode.h"
#include "media/mediahub.h"
#include "media/mediapolicy.h"
#include "media/mediawatcher.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki::media;
using murasaki::BridgeClient;
namespace bridge = murasaki::bridge;

namespace {

// The org.mpris.MediaPlayer2.Player interface of a fake player: what a test sets is what GetAll answers.
class PlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus MEMBER status)
    Q_PROPERTY(QVariantMap Metadata MEMBER metadata)
    Q_PROPERTY(qlonglong Position MEMBER position)
    Q_PROPERTY(double Rate MEMBER rate)

public:
    explicit PlayerAdaptor(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    QString status = u"Stopped"_s;
    QVariantMap metadata;
    qlonglong position = 0;
    double rate = 1.0;
};

class FakePlayer : public QObject
{
    Q_OBJECT

public:
    FakePlayer(const QString &name, const QString &connectionName)
        : m_bus(QDBusConnection::connectToBus(QDBusConnection::sessionBus().name().isEmpty() ? QDBusConnection::SessionBus : QDBusConnection::SessionBus, connectionName))
        , m_name(name)
        , m_adaptor(new PlayerAdaptor(this))
    {
        QVERIFY(m_bus.isConnected());
        QVERIFY(m_bus.registerObject(u"/org/mpris/MediaPlayer2"_s, this, QDBusConnection::ExportAdaptors));
        QVERIFY(m_bus.registerService(m_name));
    }
    ~FakePlayer() override { QDBusConnection::disconnectFromBus(m_bus.name()); }

    void set(const QString &status, const QString &title, const QString &artUrl = {}, const QStringList &artists = {})
    {
        m_adaptor->status = status;
        m_adaptor->metadata = {{u"xesam:title"_s, title}, {u"xesam:artist"_s, artists}, {u"mpris:artUrl"_s, artUrl}, {u"mpris:length"_s, qlonglong(200000000)}};
        m_adaptor->position = 5000000;
        QDBusMessage changed = QDBusMessage::createSignal(u"/org/mpris/MediaPlayer2"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s);
        changed << u"org.mpris.MediaPlayer2.Player"_s << QVariantMap{{u"PlaybackStatus"_s, status}} << QStringList{};
        m_bus.send(changed);
    }
    void leave() { m_bus.unregisterService(m_name); }

private:
    QDBusConnection m_bus;
    QString m_name;
    PlayerAdaptor *m_adaptor;
};

QByteArray png(int w, int h, QRgb color = qRgb(200, 30, 30))
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QString writeFile(QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(f.fileName()));
    f.write(bytes);
    return f.fileName();
}

// an engine's bridge as the host sees it: a client on one end of a socketpair, the test as the engine on the other
struct Rig {
    bridge::Channel engine;
    std::unique_ptr<BridgeClient> client;
    Rig()
    {
        auto sp = bridge::makeSocketPair();
        if (!sp)
            qFatal("socketpair");
        engine = bridge::Channel(std::move(sp->first));
        engine.setNonblocking(true);
        client = std::make_unique<BridgeClient>(std::move(sp->second));
        engine.send(bridge::msg::Hello{bridge::kMediaVersion, 1});
        QSignalSpy ready(client.get(), &BridgeClient::ready);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    }
    void wants(bool w) { engine.send(bridge::msg::MediaWants{w ? 1u : 0u}); }
    // every message the host sent so far: media JSON texts, and the thumbnails (width, with the pixels' size, or 0)
    void drain(QStringList *states, QList<QPair<uint32_t, qsizetype>> *thumbs)
    {
        QTest::qWait(20);
        for (;;) {
            bridge::ReceiveResult r = engine.receive();
            if (r.status != bridge::ChannelStatus::Ok)
                return;
            if (r.message.type == static_cast<uint32_t>(bridge::kMediaType)) {
                const auto json = bridge::decodeProperties(r.message.payload);
                states->append(QString::fromUtf8(json->data(), static_cast<qsizetype>(json->size())));
            } else if (r.message.type == static_cast<uint32_t>(bridge::kMediaThumbnailType)) {
                bridge::msg::MediaThumbnail m{};
                bridge::decode(r.message.payload, m);
                const auto px = m.width ? bridge::readSealedPixels(std::move(r.message.fds[0]), m.width, m.height) : std::nullopt;
                thumbs->append({m.width, px ? static_cast<qsizetype>(px->size()) : 0});
            }
        }
    }
};


} // namespace

class TstMediaHost : public QObject
{
    Q_OBJECT

private slots:
    void textIsCleanedAndBounded()
    {
        QCOMPARE(cleanText(u"  Song\tOne\nTwo\u0007\u0085  "_s), u"Song One Two"_s);
        const QString cut = cleanText(QString(300, u'a'));
        QCOMPARE(cut.toUtf8().size(), kMaxTextBytes);
        // a character that straddles the limit goes whole: 255 bytes + a two-byte letter
        const QString straddle = cleanText(QString(255, u'a') + u"é"_s + u"zz"_s);
        QCOMPARE(straddle, QString(255, u'a'));
        const QString emoji = cleanText(QString(254, u'a') + QString::fromUtf8("\xF0\x9F\x8E\xB5")); // a 4-byte character
        QCOMPARE(emoji, QString(254, u'a'));
    }

    void thePlayerToShowIsTheOneThatPlaysAndAmongThoseTheMostRecent()
    {
        std::vector<PlayerSnapshot> p(3);
        p[0].owner = u":1.1"_s;
        p[0].status = u"Paused"_s;
        p[0].title = u"a"_s;
        p[0].playingSince = 900;
        p[1].owner = u":1.2"_s;
        p[1].status = u"Playing"_s;
        p[1].title = u"b"_s;
        p[1].playingSince = 100;
        p[2].owner = u":1.3"_s;
        p[2].status = u"Playing"_s;
        p[2].title = u"c"_s;
        p[2].playingSince = 500;
        QCOMPARE(choosePlayer(p), 2); // two play: the one that became Playing last
        p[2].status = u"Paused"_s;
        p[1].status = u"Paused"_s;
        QCOMPARE(choosePlayer(p), 0); // none plays: the one that played last
        p[0].playingSince = p[1].playingSince = p[2].playingSince = 0;
        QCOMPARE(choosePlayer(p), 0); // none ever played: the first with a title, by name
        p[0].title.clear();
        QCOMPARE(choosePlayer(p), 1);
        p[1].title.clear();
        p[2].title.clear();
        QCOMPARE(choosePlayer(p), -1);
        QCOMPARE(choosePlayer({}), -1);
    }

    void theJsonIsBoundedAndTheMissingPlayerIsDisabled()
    {
        QCOMPARE(mediaJson(nullptr), QByteArray(R"({"enabled":false})"));
        PlayerSnapshot s;
        s.status = u"Playing"_s;
        s.title = QString(1000, u'x');
        s.positionSec = 1e30;
        s.rate = std::numeric_limits<double>::quiet_NaN();
        s.durationSec = -5;
        for (int i = 0; i < 20; ++i)
            s.genres << u"g"_s;
        const QByteArray json = mediaJson(&s);
        QVERIFY(json.size() < 2000);
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        QCOMPARE(o.value("state").toString(), u"playing"_s);
        QCOMPARE(o.value("title").toString().toUtf8().size(), kMaxTextBytes);
        QCOMPARE(o.value("position").toDouble(), 86400.0);
        QCOMPARE(o.value("rate").toDouble(), 1.0);
        QCOMPARE(o.value("duration").toDouble(), 0.0);
        QCOMPARE(o.value("genres").toArray().size(), kMaxGenres);
    }

    void onlyFileAndHttpsCoverUrlsAreAccepted()
    {
        QCOMPARE(classifyArtUrl(u"file:///home/me/cover.jpg"_s).kind, ArtSource::Kind::File);
        QCOMPARE(classifyArtUrl(u"file:///home/me/cover.jpg"_s).path, u"/home/me/cover.jpg"_s);
        QCOMPARE(classifyArtUrl(u"file://localhost/tmp/a.png"_s).kind, ArtSource::Kind::File);
        QCOMPARE(classifyArtUrl(u"https://i.scdn.co/image/abc"_s).kind, ArtSource::Kind::Https);
        for (const char *bad : {"http://example.com/a.png", "file://otherhost/etc/passwd", "ftp://example.com/a", "data:image/png;base64,AAAA",
                                "javascript:1", "cover.jpg", "/tmp/cover.jpg", "", "https://user:pw@example.com/a.png", "https:///x", "file:relative"}) {
            QCOMPARE(classifyArtUrl(QString::fromLatin1(bad)).kind, ArtSource::Kind::None);
        }
        QCOMPARE(classifyArtUrl(u"https://example.com/"_s + QString(3000, u'a')).kind, ArtSource::Kind::None);
    }

    void privateAndLocalAddressesAreRefused_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("blocked");
        for (const char *a : {"127.0.0.1", "127.255.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254", "100.64.0.1",
                              "100.127.255.255", "0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255", "240.0.0.1", "::", "::1", "fc00::1", "fd12:3456::1",
                              "fe80::1", "febf::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1", "::ffff:192.168.0.9", "::7f00:1", "::a00:1",
                              "64:ff9b::7f00:1", "64:ff9b::c0a8:101", "198.18.0.1", "198.19.255.255", "192.0.0.8", "192.0.2.1", "198.51.100.7",
                              "203.0.113.9", "2002:7f00:1::1", "2002:c0a8:101::", "2001:0:4136:e378:8000:63bf:3fff:fdd2", "64:ff9b:1::1", "fec0::1",
                              "2001:db8::1", "2001:0:7f00:1::1"})
            QTest::newRow(a) << QString::fromLatin1(a) << true;
        for (const char *a : {"8.8.8.8", "1.1.1.1", "172.15.0.1", "172.32.0.1", "100.63.255.255", "100.128.0.1", "192.167.1.1", "169.253.0.1", "223.255.255.254",
                              "2606:4700:4700::1111", "2001:4860:4860::8888", "::ffff:8.8.8.8", "64:ff9b::808:808", "198.17.255.255", "198.20.0.1", "192.0.1.1",
                              "2002:808:808::1", "198.51.101.1", "203.0.114.1"})
            QTest::newRow(a) << QString::fromLatin1(a) << false;
    }
    void privateAndLocalAddressesAreRefused()
    {
        QFETCH(QString, address);
        QFETCH(bool, blocked);
        QHostAddress a(address);
        QVERIFY2(!a.isNull(), qPrintable(address));
        QCOMPARE(isBlockedAddress(a), blocked);
    }

    void theKernelAndDevicePathsAreForbidden()
    {
        for (const char *p : {"/proc/self/environ", "/proc", "/sys/class/x", "/dev/zero", "/dev"})
            QVERIFY2(isForbiddenArtPath(QString::fromLatin1(p)), p);
        for (const char *p : {"/home/me/dev/a.png", "/tmp/a.png", "/devices/x", "/processing/a"})
            QVERIFY2(!isForbiddenArtPath(QString::fromLatin1(p)), p);
    }

    void aCoverFileIsDecodedAndScaled()
    {
        QTemporaryDir dir;
        const auto big = loadArtFile(writeFile(dir, u"big.png"_s, png(1000, 500)));
        QVERIFY(big.has_value());
        QCOMPARE(big->width, 512u);
        QCOMPARE(big->height, 256u);
        QCOMPARE(big->rgba.size(), qsizetype(512 * 256 * 4));
        QCOMPARE(uchar(big->rgba[0]), uchar(200));
        QCOMPARE(uchar(big->rgba[3]), uchar(255));
        const auto small = loadArtFile(writeFile(dir, u"small.png"_s, png(30, 20)));
        QVERIFY(small && small->width == 30 && small->height == 20); // never enlarged
        // a symlink to a plain picture is followed (the target is what is judged)
        QVERIFY(QFile::link(dir.filePath(u"small.png"_s), dir.filePath(u"link.png"_s)));
        QVERIFY(loadArtFile(dir.filePath(u"link.png"_s)).has_value());
    }

    void aCoverThatIsNotAPictureWithinTheLimitsIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(!loadArtFile(writeFile(dir, u"junk.png"_s, QByteArray("not a picture at all"))));
        QVERIFY(!loadArtFile(writeFile(dir, u"empty.png"_s, QByteArray())));
        QVERIFY(!loadArtFile(writeFile(dir, u"wide.png"_s, png(kMaxArtSide + 1, 1)))); // judged by its header
        QVERIFY(!loadArtFile(writeFile(dir, u"svg.svg"_s, QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>"))));
        QByteArray truncated = png(100, 100);
        truncated.truncate(truncated.size() / 2);
        QVERIFY(!decodeArt(truncated).has_value());
        QVERIFY(!loadArtFile(dir.filePath(u"missing.png"_s)));
        QVERIFY(!loadArtFile(dir.path())); // a directory
        // a file over the size limit is not even read
        QFile big(dir.filePath(u"huge.png"_s));
        QVERIFY(big.open(QIODevice::WriteOnly));
        QVERIFY(big.resize(kMaxArtBytes + 1));
        big.close();
        QVERIFY(!loadArtFile(big.fileName()));
    }

    void onlyThePlainRasterFormatsOfCoversAreDecoded()
    {
        QTemporaryDir dir;
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(loadArtFile(writeFile(dir, u"ok.png"_s, png(16, 16))).has_value());
        // pictures that are valid but of other formats (XPM, BMP, PPM, XBM) are refused, whatever the file is called
        for (const char *format : {"XPM", "BMP", "PPM", "XBM"}) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, format))
                continue; // this Qt cannot write it: nothing to refuse
            QVERIFY2(!loadArtFile(writeFile(dir, u"cover.png"_s, bytes)).has_value(), format);
            QVERIFY2(!decodeArt(bytes).has_value(), format);
        }
        // a PSD header and a TGA footer, valid or not, are not pictures here either
        QVERIFY(!decodeArt(QByteArray("8BPS\x00\x01") + QByteArray(64, '\0')).has_value());
        QVERIFY(!decodeArt(QByteArray(64, '\0') + QByteArray("TRUEVISION-XFILE.\0", 18)).has_value());
        // the pixel budget is 16 Mpixel: a 5000 x 4000 picture (20 Mpixel, a few KB of PNG) is refused by its header
        QImage huge(5000, 4000, QImage::Format_Grayscale8);
        huge.fill(7);
        QByteArray hugePng;
        QBuffer hugeBuffer(&hugePng);
        hugeBuffer.open(QIODevice::WriteOnly);
        QVERIFY(huge.save(&hugeBuffer, "PNG"));
        QVERIFY(hugePng.size() < kMaxArtBytes);
        QVERIFY(!decodeArt(hugePng).has_value());
    }

    void aHostileStringIsNotWalkedToItsEnd()
    {
        QElapsedTimer t;
        t.start();
        const QString cleaned = cleanText(QString(50 * 1000 * 1000, u'\t')); // 100 MB of text
        QVERIFY(t.elapsed() < 500);
        QVERIFY(cleaned.isEmpty() || cleaned.size() <= kMaxRawText);
        QCOMPARE(cleanText(QString(kMaxRawText + 100, u'a')).toUtf8().size(), kMaxTextBytes);
    }

    void onlyARegularFileIsRead()
    {
        QTemporaryDir dir;
        // a FIFO nobody writes to must not stall the read
        const QString fifo = dir.filePath(u"pipe.png"_s);
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QVERIFY(!loadArtFile(fifo));
        // links to the kernel's and the devices' files lead nowhere
        QVERIFY(QFile::link(u"/dev/zero"_s, dir.filePath(u"zero.png"_s)));
        QVERIFY(!loadArtFile(dir.filePath(u"zero.png"_s)));
        QVERIFY(QFile::link(u"/proc/self/status"_s, dir.filePath(u"status.png"_s)));
        QVERIFY(!loadArtFile(dir.filePath(u"status.png"_s)));
        QVERIFY(!loadArtFile(u"/dev/zero"_s));
        QVERIFY(!loadArtFile(u"/proc/self/environ"_s));
    }

    void aCoverFileAndThePlayerReachAnEngineThatUsesTheMedia()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        QTemporaryDir dir;
        const QString art = writeFile(dir, u"cover.png"_s, png(64, 32, qRgb(10, 200, 30)));
        MediaHub hub;
        hub.setShare(true);
        hub.setRemoteArt(false);
        Rig rig;
        hub.attach(rig.client.get());
        FakePlayer first(u"org.mpris.MediaPlayer2.fake1"_s, u"fake1"_s);
        first.set(u"Playing"_s, u"First Song"_s, QUrl::fromLocalFile(art).toString(), {u"The Artist"_s});
        QTest::qWait(300);
        QVERIFY(!hub.active()); // no scene uses the media: the players are not even read
        QVERIFY(!hub.cover().has_value());
        QStringList states;
        QList<QPair<uint32_t, qsizetype>> thumbs;
        rig.drain(&states, &thumbs);
        QVERIFY(states.isEmpty() && thumbs.isEmpty());

        rig.wants(true);
        QTRY_VERIFY(hub.active());
        QTRY_VERIFY(hub.cover().has_value() && hub.cover()->width == 64);
        QCOMPARE(hub.cover()->height, 32u);
        QCOMPARE(uchar(hub.cover()->rgba[1]), uchar(200));
        QTRY_VERIFY(QJsonDocument::fromJson(hub.stateJson()).object().value("title").toString() == u"First Song"_s);
        QTest::qWait(400);
        rig.drain(&states, &thumbs);
        QVERIFY(!states.isEmpty());
        const QJsonObject o = QJsonDocument::fromJson(states.last().toUtf8()).object();
        QCOMPARE(o.value("title").toString(), u"First Song"_s);
        QCOMPARE(o.value("artist").toString(), u"The Artist"_s);
        QCOMPARE(o.value("state").toString(), u"playing"_s);
        QCOMPARE(o.value("duration").toDouble(), 200.0);
        QVERIFY(!thumbs.isEmpty());
        QCOMPARE(thumbs.last().first, 64u);
        QCOMPARE(thumbs.last().second, qsizetype(64 * 32 * 4));

        // a second player that starts Playing later takes over (its https cover is refused: no cover, and no download either)
        FakePlayer second(u"org.mpris.MediaPlayer2.fake2"_s, u"fake2"_s);
        QTest::qWait(30);
        second.set(u"Playing"_s, u"Second Song"_s, u"https://127.0.0.1/never.png"_s);
        QTRY_COMPARE(QJsonDocument::fromJson(hub.stateJson()).object().value("title").toString(), u"Second Song"_s);
        QCOMPARE(hub.downloadsStarted(), 0); // remote art is off
        second.leave();
        QTRY_COMPARE(QJsonDocument::fromJson(hub.stateJson()).object().value("title").toString(), u"First Song"_s);
        first.leave();
        QTRY_COMPARE(hub.stateJson(), QByteArray(R"({"enabled":false})"));

        // the scene stops using it: nothing is read any more
        rig.wants(false);
        QTRY_VERIFY(!hub.active());
    }

    void nothingIsReadOrSentUntilTheSettingsAreKnownToAllowIt()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        FakePlayer player(u"org.mpris.MediaPlayer2.fake3"_s, u"fake3"_s);
        player.set(u"Playing"_s, u"Private Song"_s);
        MediaHub hub; // settings not read yet
        Rig rig;
        hub.attach(rig.client.get());
        rig.wants(true);
        QTest::qWait(300);
        QVERIFY(!hub.active());
        QStringList states;
        QList<QPair<uint32_t, qsizetype>> thumbs;
        rig.drain(&states, &thumbs);
        for (const QString &s : states)
            QVERIFY2(!s.contains(u"Private"_s), qPrintable(s)); // at most "nothing plays"
        hub.setShare(false);
        QTest::qWait(300);
        QVERIFY(!hub.active());
        states.clear();
        rig.drain(&states, &thumbs);
        for (const QString &s : states)
            QCOMPARE(s, QString::fromUtf8(R"({"enabled":false})"));
        QVERIFY(thumbs.isEmpty() || thumbs.last().first == 0u); // no cover, as far as the scene knows
        // allowed: the player is read and told
        hub.setShare(true);
        QTRY_VERIFY(hub.active());
        QTRY_VERIFY(QJsonDocument::fromJson(hub.stateJson()).object().value("title").toString() == u"Private Song"_s);
        // and withdrawn: the scene is told nothing plays
        hub.setShare(false);
        QTRY_VERIFY(!hub.active());
        QCOMPARE(hub.stateJson(), QByteArray(R"({"enabled":false})"));
    }

    void theSameCoverRepublishedWhileItLoadsIsNotStartedOver()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        QTemporaryDir dir;
        const QString art = QUrl::fromLocalFile(writeFile(dir, u"cover.png"_s, png(8, 8, qRgb(1, 2, 3)))).toString();
        MediaHub hub;
        hub.setDecodeDelayForTests(900); // the worker is slow: the cover is "on its way" for a while
        hub.setShare(true);
        hub.setRemoteArt(false);
        Rig rig;
        hub.attach(rig.client.get());
        rig.wants(true);
        FakePlayer player(u"org.mpris.MediaPlayer2.fake5"_s, u"fake5"_s);
        player.set(u"Playing"_s, u"Song"_s, art);
        // the player says the same thing again and again (every 250 ms, as a seeking or buffering player does)
        for (int i = 0; i < 3; ++i) {
            QTest::qWait(250);
            player.set(u"Playing"_s, u"Song"_s, art);
        }
        QTRY_VERIFY_WITH_TIMEOUT(hub.cover().has_value() && hub.cover()->width == 8, 5000);
        QCOMPARE(hub.loadsStarted(), 1);
    }

    void aHubGoingAwayWithASlowLoadDoesNotCrash()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        QTemporaryDir dir;
        const QString art = QUrl::fromLocalFile(writeFile(dir, u"cover.png"_s, png(8, 8))).toString();
        FakePlayer player(u"org.mpris.MediaPlayer2.fake6"_s, u"fake6"_s);
        player.set(u"Playing"_s, u"Song"_s, art);
        auto hub = std::make_unique<MediaHub>();
        hub->setDecodeDelayForTests(800);
        hub->setShare(true);
        hub->setRemoteArt(false);
        auto rig = std::make_unique<Rig>();
        hub->attach(rig->client.get());
        rig->wants(true);
        QTRY_COMPARE(hub->loadsStarted(), 1);
        QTest::qWait(100); // the worker is in its delay
        QElapsedTimer t;
        t.start();
        hub.reset(); // waits for the worker, which then answers nobody
        QVERIFY(t.elapsed() < 4000);
        QTest::qWait(100); // (a late answer to a dead hub would crash here)
        rig.reset();
    }

    void aSceneFlippingItsWishIsFollowedAtMostOnceASecond()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        FakePlayer player(u"org.mpris.MediaPlayer2.fake7"_s, u"fake7"_s);
        player.set(u"Playing"_s, u"Song"_s);
        MediaHub hub;
        hub.setShare(true);
        Rig rig;
        hub.attach(rig.client.get());
        rig.wants(true);
        QTRY_VERIFY(hub.active());
        rig.wants(false);
        rig.wants(true); // within the second: applied when it is over, to what it ended as
        rig.wants(false);
        QTest::qWait(300);
        QVERIFY(hub.active()); // not yet withdrawn
        QTRY_VERIFY_WITH_TIMEOUT(!hub.active(), 3000);
    }

    void anHttpsCoverIsNotDownloadedUntilTheSettingIsKnownToAllowIt()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
            QSKIP("needs the isolated session bus (dbus-run-session)");
        FakePlayer player(u"org.mpris.MediaPlayer2.fake4"_s, u"fake4"_s);
        player.set(u"Playing"_s, u"Song"_s, u"https://127.0.0.1/c.png"_s);
        MediaHub hub;
        Rig rig;
        hub.attach(rig.client.get());
        hub.setShare(true); // the setting of remote art is not known yet: an engine that connects first gets no download
        rig.wants(true);
        QTRY_VERIFY(hub.active());
        QTRY_VERIFY(hub.cover().has_value());
        QTest::qWait(2300);
        QCOMPARE(hub.downloadsStarted(), 0);
        hub.setRemoteArt(false);
        QTest::qWait(300);
        QCOMPARE(hub.downloadsStarted(), 0);
        // known to be on: the download starts (to a loopback address, which the policy refuses before any connection)
        hub.setRemoteArt(true);
        QTRY_COMPARE_WITH_TIMEOUT(hub.downloadsStarted(), 1, 5000);
        QTRY_VERIFY(hub.cover().has_value() && hub.cover()->width == 0);
    }

    void aNewClientGetsTheCurrentStateAndNoCoverMeansNone()
    {
        MediaHub hub;
        QVERIFY(!hub.cover().has_value()); // nothing read yet
        QCOMPARE(hub.stateJson(), QByteArray(R"({"enabled":false})"));
    }
};

QTEST_MAIN(TstMediaHost)
#include "tst_mediahost.moc"
