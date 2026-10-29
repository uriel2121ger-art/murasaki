// A web wallpaper has no network unless the user allowed it for that item. The test runs the real WebItem on the real
// profile wiring (item scheme, request interceptor) against servers on 127.0.0.1 that it starts itself, and counts
// what reaches them: HTTP, WebSocket, WebRTC (STUN over UDP, TURN over TCP), WebTransport and the rest. Without the
// permission nothing may arrive; with it the same page gets through. Offscreen and software rendered: no display,
// GPU or desktop session is touched.

#include "itemschemehandler.h"
#include "networkgate.h"
#include "webprofilefactory.h"
#include "testutil.h"
#include "wallpapercontent.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <unistd.h>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWebEngineProfile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtQml/qqmlextensionplugin.h>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

Q_IMPORT_QML_PLUGIN(Murasaki_RenderPlugin)

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// What reached the "internet" (127.0.0.1 here), by channel.
class EvilServer : public QObject
{
    Q_OBJECT

public:
    EvilServer()
    {
        QVERIFY2(m_tcp.listen(QHostAddress::LocalHost, 0), "tcp listen");
        QVERIFY2(m_udp.bind(QHostAddress::LocalHost, 0), "udp bind");
        QVERIFY2(m_turn.listen(QHostAddress::LocalHost, 0), "turn listen");
        connect(&m_turn, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_turn.nextPendingConnection()) {
                ++turnConnections;
                socket->setParent(&m_turn);
            }
        });
        connect(&m_tcp, &QTcpServer::newConnection, this, &EvilServer::onConnection);
        connect(&m_udp, &QUdpSocket::readyRead, this, [this] {
            while (m_udp.hasPendingDatagrams()) {
                m_udp.receiveDatagram();
                ++udpDatagrams;
            }
        });
    }

    quint16 tcpPort() const { return m_tcp.serverPort(); }
    quint16 udpPort() const { return m_udp.localPort(); }
    quint16 turnPort() const { return m_turn.serverPort(); }

    QSet<QString> httpPaths;
    bool webSocketHandshake = false;
    int tcpConnections = 0; // any TCP connection at all (TURN, preconnect, ...)
    int udpDatagrams = 0;   // STUN, QUIC, ...
    int turnConnections = 0; // WebRTC relay over TCP

    bool sawAnything() const
    {
        return !httpPaths.isEmpty() || webSocketHandshake || tcpConnections > 0 || udpDatagrams > 0 || turnConnections > 0;
    }
    QString summary() const
    {
        QStringList paths(httpPaths.begin(), httpPaths.end());
        paths.sort();
        return u"http=[%1] ws=%2 tcp=%3 udp=%4 turn=%5"_s.arg(paths.join(u','), QString::number(webSocketHandshake),
                                                             QString::number(tcpConnections),
                                                             QString::number(udpDatagrams),
                                                             QString::number(turnConnections));
    }

private:
    void onConnection()
    {
        while (QTcpSocket *socket = m_tcp.nextPendingConnection()) {
            ++tcpConnections;
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                const QByteArray data = socket->readAll();
                const int eol = data.indexOf("\r\n");
                const QByteArray first = eol < 0 ? QByteArray() : data.left(eol);
                const QList<QByteArray> parts = first.split(' ');
                if (parts.size() >= 2 && first.endsWith("HTTP/1.1"))
                    httpPaths.insert(QString::fromLatin1(parts.at(1)).section(u'?', 0, 0));
                if (data.contains("Upgrade: websocket") || data.contains("Upgrade: WebSocket")) {
                    webSocketHandshake = true;
                    QByteArray key;
                    for (const QByteArray &line : data.split('\n')) {
                        if (line.toLower().startsWith("sec-websocket-key:"))
                            key = line.mid(18).trimmed();
                    }
                    const QByteArray accept = QCryptographicHash::hash(
                        key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
                    socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
                    return;
                }
                if (first.isEmpty())
                    return; // binary (TURN): counted as a connection
                socket->write("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/plain\r\n"
                              "Content-Length: 2\r\nConnection: close\r\n\r\nok");
                socket->disconnectFromHost();
            });
        }
    }

    QTcpServer m_tcp;
    QTcpServer m_turn;
    QUdpSocket m_udp;
};

// The page every probe runs in. It tries every way out it can think of, then reports.
const char *kProbeJs = R"JS(
const q = new URLSearchParams(location.search);
const E = '127.0.0.1:' + q.get('e'), U = q.get('u'), T = '127.0.0.1:' + q.get('t');
const log = m => console.log(m);
const skip = (q.get('skip') || '').split(',');
fetch('data.json').then(r => r.json()).then(j => log('LOCAL_OK ' + j.v)).catch(e => log('LOCAL_FAIL ' + e));
new Image().src = 'data:image/gif;base64,R0lGODlhAQABAAAAACw=';
fetch('http://' + E + '/fetch', {mode: 'no-cors'}).catch(() => {});
const x = new XMLHttpRequest(); x.open('GET', 'http://' + E + '/xhr'); x.send();
new Image().src = 'http://' + E + '/img';
const s = document.createElement('script'); s.src = 'http://' + E + '/script.js'; document.head.appendChild(s);
const l = document.createElement('link'); l.rel = 'stylesheet'; l.href = 'http://' + E + '/css'; document.head.appendChild(l);
for (const rel of skip.includes('pre') ? [] : ['preconnect', 'prefetch', 'dns-prefetch']) {
  const h = document.createElement('link'); h.rel = rel; h.href = 'https://' + E + '/' + rel; document.head.appendChild(h);
}
navigator.sendBeacon('http://' + E + '/beacon', 'x');
try { new EventSource('http://' + E + '/sse'); } catch (e) {}
try { const w = new WebSocket('ws://' + E + '/ws'); w.onopen = () => log('WS_OPEN'); } catch (e) {}
if (!skip.includes('wt')) try { new WebTransport('https://127.0.0.1:' + U + '/wt').ready.catch(() => {}); } catch (e) {}
function rtc(win) {
  const pc = new win.RTCPeerConnection({iceServers: [{urls: 'stun:127.0.0.1:' + U},
                                                     {urls: 'turn:' + T + '?transport=tcp', username: 'a', credential: 'b'}]});
  pc.createDataChannel('x');
  pc.createOffer().then(o => pc.setLocalDescription(o));
}
try { rtc(window); } catch (e) { log('RTC_UNAVAILABLE'); }
if (!skip.includes('frame')) try {
  const f = document.createElement('iframe'); document.body.appendChild(f); rtc(f.contentWindow);
} catch (e) { log('RTC_FRAME_UNAVAILABLE'); }
const f2 = document.createElement('iframe'); f2.src = 'http://' + E + '/iframe'; document.body.appendChild(f2);
try {
  const blob = new Blob(["fetch('http://" + E + "/workerfetch', {mode: 'no-cors'}).catch(() => {});" +
                         "try { new WebSocket('ws://" + E + "/workerws'); } catch (e) {}"]);
  new Worker(URL.createObjectURL(blob));
} catch (e) {}
setTimeout(() => log('PROBE_DONE'), 3000);
setTimeout(() => { location.href = 'http://' + E + '/navigate'; }, 3500);
)JS";

class Console : public QObject
{
    Q_OBJECT

public:
    QStringList lines;

public slots:
    void add(const QString &line) { lines.append(line); }
};

const char *kHostQml = R"QML(
import QtQuick
import Murasaki.Render

Item {
    id: root
    property var profileObject
    property url contentUrl
    property bool network: false
    property string proxyToken: ""
    signal consoleMessage(string message)
    WebItem {
        anchors.fill: parent
        profile: root.profileObject
        contentUrl: root.contentUrl
        networkAllowed: root.network
        proxyToken: root.proxyToken
        script: ""
        onJavaScriptConsoleMessage: function (level, message, line, source) { root.consoleMessage(message) }
    }
}
)QML";

} // namespace

// Resident memory of this process and everything below it (the web engine's renderer, GPU and network processes), MiB.
double treeRssMib()
{
    const auto readStat = [](const QString &pid, qint64 *ppid, qint64 *rssPages) {
        QFile f(u"/proc/%1/stat"_s.arg(pid));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QByteArray data = f.readAll();
        const QList<QByteArray> rest = data.mid(data.lastIndexOf(')') + 2).split(' ');
        if (rest.size() < 22)
            return false;
        *ppid = rest.at(1).toLongLong();
        *rssPages = rest.at(21).toLongLong();
        return true;
    };
    QHash<qint64, QList<qint64>> children;
    QHash<qint64, qint64> rss;
    for (const QString &name : QDir(u"/proc"_s).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 pid = name.toLongLong(&ok);
        qint64 ppid = 0, pages = 0;
        if (ok && readStat(name, &ppid, &pages)) {
            children[ppid].append(pid);
            rss[pid] = pages;
        }
    }
    double total = 0;
    QList<qint64> stack{QCoreApplication::applicationPid()};
    while (!stack.isEmpty()) {
        const qint64 pid = stack.takeLast();
        total += double(rss.value(pid)) * double(sysconf(_SC_PAGESIZE)) / (1024 * 1024);
        stack += children.value(pid);
    }
    return total;
}

class TestWebNetwork : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void deniedByDefaultNothingGetsOut();
    void allowedItemReachesTheNetwork();
    void withoutCredentialsTheGateStopsEvenAnAllowedProfile();
    void aHostilePageBesideAnAllowedOneStillGetsNothing();
    void revokingTheItemCutsItsConnections();
    void aRetiredProfileWaitsForItsViews();
    void withoutAGateNoItemGetsTheNetwork();
    void handlerKnowsItsPermission();

private:
    // One shown wallpaper: a view on the probe page under a profile of its own.
    struct Shown {
        std::unique_ptr<QQuickView> view;
        Console console;
    };
    // Starts the probe page for `slot`. `credentials` false withholds the gate token from the page (a bug, or an attack).
    std::unique_ptr<Shown> show(EvilServer &server, const QString &slot, bool network, bool credentials = true);
    // Waits for the page to finish and for late traffic.
    void settle(Shown &shown);

    QTemporaryDir m_dir;
    std::unique_ptr<WebProfileFactory> m_factory;
    QString m_itemDir;
    int m_serial = 0;
};

void TestWebNetwork::initTestCase()
{
    m_itemDir = m_dir.filePath(u"item"_s);
    testutil::writeFile(m_itemDir + u"/index.html"_s,
                        "<!doctype html><html><body><script src='probe.js'></script></body></html>");
    testutil::writeFile(m_itemDir + u"/probe.js"_s, kProbeJs);
    testutil::writeFile(m_itemDir + u"/data.json"_s, R"({"v": 42})");
    testutil::writeFile(m_dir.filePath(u"Host.qml"_s), kHostQml);
    m_factory = std::make_unique<WebProfileFactory>(NetworkGate::instance());
    m_factory->setRoots({testutil::canonical(m_itemDir)});
    m_factory->setRetireDelayMs(100);
}

std::unique_ptr<TestWebNetwork::Shown> TestWebNetwork::show(EvilServer &server, const QString &slot, bool network,
                                                            bool credentials)
{
    const QString dir = testutil::canonical(m_itemDir);
    const auto handle = m_factory->acquire(slot, u"key-%1"_s.arg(++m_serial), dir, network);
    QUrl url = itemEntryUrl(handle.itemToken, dir, dir + u"/index.html"_s);
    url.setQuery(u"e=%1&u=%2&t=%3&skip=%4"_s.arg(server.tcpPort()).arg(server.udpPort()).arg(server.turnPort())
                     .arg(qEnvironmentVariable("PROBE_SKIP")));

    auto shown = std::make_unique<Shown>();
    shown->view = std::make_unique<QQuickView>();
    QQuickView &view = *shown->view;
    view.engine()->addImportPath(u"qrc:/qt/qml"_s);
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(640, 360);
    view.setInitialProperties({{u"profileObject"_s, QVariant::fromValue(static_cast<QObject *>(handle.profile))},
                               {u"contentUrl"_s, url},
                               {u"network"_s, network},
                               {u"proxyToken"_s, credentials ? handle.proxyToken : QString()}});
    view.setSource(QUrl::fromLocalFile(m_dir.filePath(u"Host.qml"_s)));
    if (view.status() != QQuickView::Ready) {
        qWarning() << view.errors();
        shown->console.lines.append(u"QML_ERROR"_s);
        return shown;
    }
    QObject::connect(view.rootObject(), SIGNAL(consoleMessage(QString)), &shown->console, SLOT(add(QString)));
    view.show();
    return shown;
}

void TestWebNetwork::settle(Shown &shown)
{
    if (!QTest::qWaitFor([&] { return shown.console.lines.contains(u"PROBE_DONE"_s); }, 30000))
        shown.console.lines.append(u"TIMEOUT"_s);
    QTest::qWait(2500); // late traffic: the navigation attempt, WebRTC gathering, QUIC retries
}

void TestWebNetwork::deniedByDefaultNothingGetsOut()
{
    EvilServer server;
    auto shown = show(server, u"a"_s, /*network=*/false);
    settle(*shown);
    const QStringList console = shown->console.lines;
    qInfo().noquote() << "[off] server:" << server.summary();
    QVERIFY2(console.contains(u"PROBE_DONE"_s), qPrintable(console.join(u'|')));
    QVERIFY2(console.contains(u"LOCAL_OK 42"_s), qPrintable(console.join(u'|'))); // the item's own files still load
    QVERIFY2(!console.contains(u"WS_OPEN"_s), qPrintable(console.join(u'|')));
    QVERIFY2(!server.sawAnything(), qPrintable(server.summary()));
    m_factory->release(u"a"_s);
}

void TestWebNetwork::allowedItemReachesTheNetwork()
{
    EvilServer server;
    auto shown = show(server, u"a"_s, /*network=*/true);
    settle(*shown);
    const QStringList console = shown->console.lines;
    qInfo().noquote() << "[net] server:" << server.summary() << "| process tree RSS with 1 profile:" << treeRssMib() << "MiB";
    QVERIFY2(console.contains(u"LOCAL_OK 42"_s), qPrintable(console.join(u'|')));
    for (const QString &path : {u"/fetch"_s, u"/xhr"_s, u"/img"_s, u"/beacon"_s, u"/iframe"_s})
        QVERIFY2(server.httpPaths.contains(path), qPrintable(path + u" missing: "_s + server.summary()));
    QVERIFY2(server.webSocketHandshake, qPrintable(server.summary()));
    QVERIFY2(console.contains(u"WS_OPEN"_s), qPrintable(console.join(u'|')));
    QVERIFY2(server.udpDatagrams == 0, qPrintable(server.summary())); // the engine has no UDP for WebRTC (see networkgate.h)
    m_factory->release(u"a"_s);
}

// Defense in depth: the profile is allowed (no interceptor block, no policy, WebRTC exists), but the page was never given
// the gate's credentials: the proxy refuses it, including for the channels nothing else sees.
void TestWebNetwork::withoutCredentialsTheGateStopsEvenAnAllowedProfile()
{
    EvilServer server;
    auto shown = show(server, u"a"_s, /*network=*/true, /*credentials=*/false);
    settle(*shown);
    const QStringList console = shown->console.lines;
    QVERIFY2(console.contains(u"PROBE_DONE"_s), qPrintable(console.join(u'|')));
    QVERIFY2(console.contains(u"LOCAL_OK 42"_s), qPrintable(console.join(u'|')));
    QVERIFY2(!console.contains(u"WS_OPEN"_s), qPrintable(console.join(u'|')));
    QVERIFY2(!server.sawAnything(), qPrintable(server.summary()));
    m_factory->release(u"a"_s);
}

// The gap of a shared gate: in one process an allowed wallpaper and a hostile one without the permission are on screen at
// the same time. The gate is open (the first one has a token). The second one's fetch, WebSocket, WebRTC in an
// about:blank frame over TURN/TCP and its preconnect hints all fail, while the first one's requests succeed.
void TestWebNetwork::aHostilePageBesideAnAllowedOneStillGetsNothing()
{
    EvilServer friendly;
    EvilServer hostile;
    auto allowed = show(friendly, u"allowed"_s, /*network=*/true);
    auto evil = show(hostile, u"evil"_s, /*network=*/false);
    QVERIFY(NetworkGate::instance()->isOpen());
    settle(*allowed);
    settle(*evil);
    qInfo().noquote() << "[mixed] allowed:" << friendly.summary() << "| hostile:" << hostile.summary()
                      << "| process tree RSS with 2 profiles:" << treeRssMib() << "MiB";
    QVERIFY2(friendly.httpPaths.contains(u"/fetch"_s) && friendly.webSocketHandshake && friendly.turnConnections > 0,
             qPrintable(friendly.summary()));
    QVERIFY2(!hostile.sawAnything(), qPrintable(hostile.summary()));
    QVERIFY2(!evil->console.lines.contains(u"WS_OPEN"_s), qPrintable(evil->console.lines.join(u'|')));
    QCOMPARE(m_factory->profileCount(), 2);
    m_factory->release(u"allowed"_s);
    m_factory->release(u"evil"_s);
}

// Turning the permission off retires the profile: its token stops working at once and its connections are dropped.
void TestWebNetwork::revokingTheItemCutsItsConnections()
{
    EvilServer server;
    auto shown = show(server, u"a"_s, /*network=*/true);
    settle(*shown);
    QVERIFY(NetworkGate::instance()->isOpen());
    m_factory->setRetireDelayMs(1000); // the view goes first, as it does on screen (it fades out, then is destroyed)
    m_factory->release(u"a"_s);
    QVERIFY(!NetworkGate::instance()->isOpen());
    shown.reset();
    QTest::qWait(1500); // the retired profile is deleted after its view
    m_factory->setRetireDelayMs(100);
    QCOMPARE(m_factory->profileCount(), 0);
}

// The old wallpaper's view stays until the next one has a frame, which can take longer than any delay: the profile
// must not go before it (QtWebEngine crashes), and must go soon after.
void TestWebNetwork::aRetiredProfileWaitsForItsViews()
{
    const QString dir = testutil::canonical(m_itemDir);
    const auto handle = m_factory->acquire(u"lease"_s, u"lease-key"_s, dir, /*networkAllowed=*/false);
    QVERIFY(handle.lease);
    handle.lease->viewOpened();
    m_factory->release(u"lease"_s);
    QCOMPARE(m_factory->retiredCount(), 1);
    QTest::qWait(400); // well past the retire delay (100 ms)
    QCOMPARE(m_factory->retiredCount(), 1); // the view is still there
    handle.lease->viewClosed();
    QTRY_COMPARE_WITH_TIMEOUT(m_factory->retiredCount(), 0, 2000);

    // A view that never reports its end does not keep the profile forever.
    m_factory->setRetireCapMs(300);
    const auto stuck = m_factory->acquire(u"stuck"_s, u"stuck-key"_s, dir, false);
    stuck.lease->viewOpened();
    m_factory->release(u"stuck"_s);
    QTRY_COMPARE_WITH_TIMEOUT(m_factory->retiredCount(), 0, 2000);
    m_factory->setRetireCapMs(600000);
}

void TestWebNetwork::withoutAGateNoItemGetsTheNetwork()
{
    QTcpServer taken;
    QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
    NetworkGate dead(taken.serverPort()); // cannot listen
    WebProfileFactory factory(&dead);
    const auto handle = factory.acquire(u"a"_s, u"k"_s, testutil::canonical(m_itemDir), /*networkAllowed=*/true);
    QVERIFY(handle.profile);
    QVERIFY(!handle.networkAllowed);
    QVERIFY(handle.proxyToken.isEmpty());
}

void TestWebNetwork::handlerKnowsItsPermission()
{
    ItemSchemeHandler closed;
    ItemSchemeHandler open(true);
    QVERIFY(!closed.networkAllowed());
    QVERIFY(open.networkAllowed());
    const QString token = closed.tokenFor(u"/w/1"_s);
    QCOMPARE(closed.tokenFor(u"/w/1"_s), token);
    QVERIFY(closed.tokenFor(u"/w/2"_s) != token);
}

int main(int argc, char **argv)
{
    // Offscreen and software rendered, whatever the environment says: this never opens a window on a live session.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_ACCESSIBILITY", "0");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    ItemSchemeHandler::registerScheme(); // before the application object
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    NetworkGate::instance()->setAllowLocalDestinationsForTests(true); // the probe servers are on 127.0.0.1
    applyGateEngineFlags(*NetworkGate::instance()); // before the first profile
    TestWebNetwork test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_webnetwork.moc"
