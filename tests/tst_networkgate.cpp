#include "networkgate.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A server on 127.0.0.1 that answers a fixed HTTP reply (when it sees a full request head) and counts connections.
class Upstream : public QObject
{
    Q_OBJECT

public:
    Upstream()
    {
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), "listen");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                ++connections;
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    received += socket->readAll();
                    if (received.contains("\r\n\r\n") && received.startsWith("GET")) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
                        socket->disconnectFromHost();
                    } else if (!received.startsWith("GET") && !received.isEmpty()) {
                        socket->write("echo:" + received); // a raw tunnel
                        received.clear();
                    }
                });
            }
        });
    }
    quint16 port() const { return server.serverPort(); }

    QTcpServer server;
    int connections = 0;
    QByteArray received;
};

// The gate runs in this thread, so everything waits through the event loop.
// "CONNECT host:port" with Basic credentials for `token` (none when empty).
QByteArray connectRequest(quint16 port, const QString &token = {})
{
    QByteArray request = "CONNECT 127.0.0.1:" + QByteArray::number(port) + " HTTP/1.1\r\nHost: x\r\n";
    if (!token.isEmpty())
        request += "Proxy-Authorization: Basic " + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n";
    return request + "\r\n";
}

QByteArray readAllUntilClosed(QTcpSocket &socket, int timeoutMs = 5000)
{
    QByteArray data;
    (void)QTest::qWaitFor([&] {
        data += socket.readAll();
        return socket.state() == QAbstractSocket::UnconnectedState;
    }, timeoutMs);
    data += socket.readAll();
    return data;
}

} // namespace

class TestNetworkGate : public QObject
{
    Q_OBJECT

private slots:
    void listensOnLoopbackOnly();
    void closedGateRefusesEverything();
    void openGateTunnelsConnect();
    void openGateForwardsPlainHttp();
    void closingDropsLiveTunnels();
    void connectToUnreachableHostFails();
    void rejectsGarbageAndOversizedHeads();
    void primingRequestIsAnsweredLocallyForValidTokensOnly();
    void aGateThatCannotListenFailsClosed();
    void idleTunnelsAreClosed();
    void aTokenHasAConnectionCap();
    void publicAddressesAreTheOnlyDestinations_data();
    void publicAddressesAreTheOnlyDestinations();
    void localTargetsAreRefusedEvenWithAValidToken();
    void aNameThatResolvesToALocalAddressIsRefused();
    void tokensOpenAndCloseTheGate();
    void aWrongOrMissingTokenGets407();
    void revokingDropsOnlyThatTokensTunnels();
};

void TestNetworkGate::listensOnLoopbackOnly()
{
    NetworkGate gate;
    QVERIFY(gate.port() != 0);
    QCOMPARE(gate.address(), QHostAddress(QHostAddress::LocalHost));
}

void TestNetworkGate::closedGateRefusesEverything()
{
    NetworkGate gate;
    Upstream upstream;
    QVERIFY(!gate.isOpen());
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    client.write(connectRequest(upstream.port(), u"0123456789abcdef"_s)); // a token nobody issued
    const QByteArray reply = readAllUntilClosed(client);
    QVERIFY2(reply.startsWith("HTTP/1.1 407"), reply.constData());
    QVERIFY(reply.contains("Proxy-Authenticate: Basic"));
    QCOMPARE(upstream.connections, 0);
}

void TestNetworkGate::openGateTunnelsConnect()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true); // the test servers are on 127.0.0.1
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    client.write(connectRequest(upstream.port(), token));
    QByteArray reply;
    QVERIFY(QTest::qWaitFor([&] { reply += client.readAll(); return reply.contains("\r\n\r\n"); }, 5000));
    QVERIFY2(reply.startsWith("HTTP/1.1 200"), reply.constData());
    client.write("ping");
    QByteArray echoed;
    QVERIFY(QTest::qWaitFor([&] { echoed += client.readAll(); return echoed.contains("echo:ping"); }, 5000));
}

void TestNetworkGate::openGateForwardsPlainHttp()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true); // the test servers are on 127.0.0.1
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    const QByteArray target = "127.0.0.1:" + QByteArray::number(upstream.port());
    client.write("GET http://" + target + "/path?q=1 HTTP/1.1\r\nHost: " + target
                 + "\r\nProxy-Connection: keep-alive\r\nProxy-Authorization: Basic "
                 + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n\r\n");
    const QByteArray reply = readAllUntilClosed(client);
    QVERIFY2(reply.startsWith("HTTP/1.1 200"), reply.constData());
    QVERIFY(reply.endsWith("hello"));
    // The upstream saw an origin-form request, without the proxy header.
    QVERIFY2(upstream.received.startsWith("GET /path?q=1 HTTP/1.1\r\n"), upstream.received.constData());
    QVERIFY(!upstream.received.toLower().contains("proxy-connection"));
}

void TestNetworkGate::closingDropsLiveTunnels()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true); // the test servers are on 127.0.0.1
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    client.write(connectRequest(upstream.port(), token));
    QByteArray reply;
    QVERIFY(QTest::qWaitFor([&] { reply += client.readAll(); return reply.contains("\r\n\r\n"); }, 5000));
    gate.revokeToken(token); // a WebSocket that is up must not outlive the permission
    QVERIFY(QTest::qWaitFor([&] { client.readAll(); return client.state() == QAbstractSocket::UnconnectedState; }, 5000));
}

void TestNetworkGate::connectToUnreachableHostFails()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true); // the test servers are on 127.0.0.1
    const QString token = gate.issueToken();
    quint16 deadPort;
    {
        QTcpServer probe; // a port nobody listens on
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        deadPort = probe.serverPort();
    }
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    client.write(connectRequest(deadPort, token));
    const QByteArray reply = readAllUntilClosed(client);
    QVERIFY2(reply.startsWith("HTTP/1.1 502"), reply.constData());
}

void TestNetworkGate::rejectsGarbageAndOversizedHeads()
{
    NetworkGate gate;
    const QString token = gate.issueToken();
    const QByteArray auth = "Proxy-Authorization: Basic " + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n";
    for (const QByteArray &bad : {QByteArray("\x16\x03\x01 not http\r\n\r\n"),
                                  QByteArray("CONNECT nonsense HTTP/1.1\r\n") + auth + "\r\n",
                                  QByteArray("GET /relative HTTP/1.1\r\n") + auth + "\r\n",
                                  QByteArray("CONNECT 127.0.0.1:1 HTTP/1.1\r\nX: ") + QByteArray(100000, 'a')}) {
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, gate.port());
        QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
        client.write(bad);
        const QByteArray reply = readAllUntilClosed(client);
        QVERIFY2(reply.isEmpty() || reply.startsWith("HTTP/1.1 400") || reply.startsWith("HTTP/1.1 407"), reply.constData());
        QCOMPARE(client.state(), QAbstractSocket::UnconnectedState);
    }
}

void TestNetworkGate::primingRequestIsAnsweredLocallyForValidTokensOnly()
{
    NetworkGate gate;
    const QString token = gate.issueToken();
    const auto ask = [&](const QByteArray &auth) {
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, gate.port());
        (void)QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000);
        client.write(QByteArray("GET ") + kGatePrimeUrl + " HTTP/1.1\r\nHost: murasaki-gate.invalid\r\n" + auth + "\r\n");
        return readAllUntilClosed(client);
    };
    const QByteArray good = ask("Proxy-Authorization: Basic " + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n");
    QVERIFY2(good.startsWith("HTTP/1.1 200") && good.endsWith("ok"), good.constData());
    QVERIFY(ask({}).startsWith("HTTP/1.1 407"));
}

void TestNetworkGate::publicAddressesAreTheOnlyDestinations_data()
{
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("allowed");
    for (const char *a : {"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.32.0.1", "100.63.255.255", "100.128.0.1", "2606:4700:4700::1111",
                          "2001:db8::1", "64:ff9b::808:808"})
        QTest::newRow(a) << QString::fromLatin1(a) << true;
    for (const char *a : {"127.0.0.1", "127.255.255.254", "0.0.0.0", "0.1.2.3", "10.0.0.1", "10.255.255.255", "172.16.0.1",
                          "172.31.255.255", "192.168.1.1", "100.64.0.1", "100.127.255.255", "169.254.169.254", "224.0.0.1",
                          "239.255.255.250", "240.0.0.1", "255.255.255.255", "192.0.0.1", "::", "::1", "fc00::1", "fd12:3456::1",
                          "fe80::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1", "::ffff:192.168.0.1", "64:ff9b::7f00:1",
                          "64:ff9b::a00:1"})
        QTest::newRow(a) << QString::fromLatin1(a) << false;
}

void TestNetworkGate::publicAddressesAreTheOnlyDestinations()
{
    QFETCH(QString, address);
    QFETCH(bool, allowed);
    QCOMPARE(isPublicAddress(QHostAddress(address)), allowed);
}

void TestNetworkGate::localTargetsAreRefusedEvenWithAValidToken()
{
    NetworkGate gate;
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    client.write(connectRequest(upstream.port(), token)); // 127.0.0.1 is where the test server is
    const QByteArray reply = readAllUntilClosed(client);
    QVERIFY2(reply.startsWith("HTTP/1.1 403"), reply.constData());
    QCOMPARE(upstream.connections, 0);
    // The plain HTTP form too.
    QTcpSocket plain;
    plain.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return plain.state() == QAbstractSocket::ConnectedState; }, 2000));
    plain.write("GET http://127.0.0.1:" + QByteArray::number(upstream.port()) + "/ HTTP/1.1\r\nProxy-Authorization: Basic "
                + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n\r\n");
    QVERIFY(readAllUntilClosed(plain).startsWith("HTTP/1.1 403"));
    QCOMPARE(upstream.connections, 0);
}

void TestNetworkGate::aNameThatResolvesToALocalAddressIsRefused()
{
    NetworkGate gate;
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
    // "localhost" resolves to loopback: the name is not what is judged, the address is.
    client.write("CONNECT localhost:" + QByteArray::number(upstream.port()) + " HTTP/1.1\r\nProxy-Authorization: Basic "
                 + (QByteArray(kGateUser) + ':' + token.toLatin1()).toBase64() + "\r\n\r\n");
    QVERIFY(readAllUntilClosed(client).startsWith("HTTP/1.1 403"));
    QCOMPARE(upstream.connections, 0);
}

void TestNetworkGate::aGateThatCannotListenFailsClosed()
{
    QTcpServer taken;
    QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
    NetworkGate gate(taken.serverPort()); // that port is busy
    QVERIFY(!gate.isListening());
    QVERIFY(gate.issueToken().isEmpty()); // nobody can be given a way out
    QVERIFY(!gate.isOpen());
    const QByteArray flags = gateEngineFlags(gate);
    QVERIFY2(flags.contains("--proxy-server=http://127.0.0.1:1 "), flags.constData()); // a reserved port nobody serves
    QVERIFY(!flags.contains("127.0.0.1:0"));
    NetworkGate working;
    QVERIFY(working.isListening());
    QVERIFY(gateEngineFlags(working).contains("127.0.0.1:" + QByteArray::number(working.port())));
}

void TestNetworkGate::idleTunnelsAreClosed()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true);
    gate.setIdleTimeoutMs(300);
    const QString token = gate.issueToken();
    Upstream upstream;
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, gate.port());
    (void)QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000);
    client.write(connectRequest(upstream.port(), token));
    QByteArray reply;
    QVERIFY(QTest::qWaitFor([&] { reply += client.readAll(); return reply.contains("\r\n\r\n"); }, 5000));
    // silence on both sides: the gate lets go
    QVERIFY(QTest::qWaitFor([&] { client.readAll(); return client.state() == QAbstractSocket::UnconnectedState; }, 5000));
}

void TestNetworkGate::aTokenHasAConnectionCap()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true);
    gate.setConnectionsPerToken(3);
    const QString token = gate.issueToken();
    Upstream upstream;
    QList<QTcpSocket *> open;
    int refused = 0;
    for (int i = 0; i < 5; ++i) {
        auto *client = new QTcpSocket(this);
        client->connectToHost(QHostAddress::LocalHost, gate.port());
        (void)QTest::qWaitFor([&] { return client->state() == QAbstractSocket::ConnectedState; }, 2000);
        client->write(connectRequest(upstream.port(), token));
        QByteArray reply;
        (void)QTest::qWaitFor([&] { reply += client->readAll(); return reply.contains("\r\n\r\n"); }, 5000);
        if (reply.startsWith("HTTP/1.1 200"))
            open.append(client);
        else if (reply.startsWith("HTTP/1.1 503"))
            ++refused;
    }
    QCOMPARE(open.size(), 3);
    QCOMPARE(refused, 2);
    qDeleteAll(open);
}

void TestNetworkGate::tokensOpenAndCloseTheGate()
{
    NetworkGate gate;
    QSignalSpy spy(&gate, &NetworkGate::openChanged);
    QVERIFY(!gate.isOpen());
    const QString a = gate.issueToken();
    const QString b = gate.issueToken();
    QVERIFY(a != b);
    QCOMPARE(a.size(), 32);
    QVERIFY(gate.isOpen());
    gate.revokeToken(a);
    QVERIFY(gate.isOpen());
    QVERIFY(!gate.tokenValid(a));
    gate.revokeToken(b);
    gate.revokeToken(b); // twice: nothing
    QVERIFY(!gate.isOpen());
    QCOMPARE(spy.count(), 2); // opened once, closed once
}

void TestNetworkGate::aWrongOrMissingTokenGets407()
{
    NetworkGate gate;
    const QString token = gate.issueToken();
    Upstream upstream;
    for (const QByteArray &request : {connectRequest(upstream.port()), connectRequest(upstream.port(), token + u"x"_s),
                                      connectRequest(upstream.port(), QString(token).replace(0, 1, token[0] == u'0' ? u'1' : u'0')),
                                      QByteArray("CONNECT 127.0.0.1:") + QByteArray::number(upstream.port())
                                          + " HTTP/1.1\r\nProxy-Authorization: Basic "
                                          + (QByteArray("someone:") + token.toLatin1()).toBase64() + "\r\n\r\n"}) {
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, gate.port());
        QVERIFY(QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000));
        client.write(request);
        QVERIFY(readAllUntilClosed(client).startsWith("HTTP/1.1 407"));
    }
    QCOMPARE(upstream.connections, 0);
}

void TestNetworkGate::revokingDropsOnlyThatTokensTunnels()
{
    NetworkGate gate;
    gate.setAllowLocalDestinationsForTests(true); // the test servers are on 127.0.0.1
    const QString first = gate.issueToken();
    const QString second = gate.issueToken();
    Upstream upstream;
    const auto open = [&](QTcpSocket &client, const QString &token) {
        client.connectToHost(QHostAddress::LocalHost, gate.port());
        (void)QTest::qWaitFor([&] { return client.state() == QAbstractSocket::ConnectedState; }, 2000);
        client.write(connectRequest(upstream.port(), token));
        QByteArray reply;
        return QTest::qWaitFor([&] { reply += client.readAll(); return reply.contains("\r\n\r\n"); }, 5000);
    };
    QTcpSocket a, b;
    QVERIFY(open(a, first));
    QVERIFY(open(b, second));
    gate.revokeToken(first);
    QVERIFY(QTest::qWaitFor([&] { a.readAll(); return a.state() == QAbstractSocket::UnconnectedState; }, 5000));
    QTest::qWait(200);
    QCOMPARE(b.state(), QAbstractSocket::ConnectedState);
    b.write("ping");
    QByteArray echoed;
    QVERIFY(QTest::qWaitFor([&] { echoed += b.readAll(); return echoed.contains("echo:ping"); }, 5000));
}

QTEST_GUILESS_MAIN(TestNetworkGate)
#include "tst_networkgate.moc"
