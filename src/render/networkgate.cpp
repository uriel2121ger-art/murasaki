#include "networkgate.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QRandomGenerator>

#include <cstring>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr qsizetype kMaxHeadBytes = 16 * 1024;
constexpr int kHeadTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kMaxConnections = 512;
constexpr qint64 kMaxPendingWrite = 4 * 1024 * 1024; // per direction: a slow reader stalls the sender instead

QByteArray statusLine(int code, const char *text)
{
    QByteArray line = "HTTP/1.1 " + QByteArray::number(code) + ' ' + text + "\r\nConnection: close\r\nContent-Length: 0\r\n";
    if (code == 407)
        line += "Proxy-Authenticate: Basic realm=\"murasaki\"\r\n";
    return line + "\r\n";
}

struct Target {
    QString host;
    quint16 port = 0;
    bool valid() const { return !host.isEmpty() && port != 0; }
};

// "host:port" or "[v6]:port" (CONNECT).
Target parseAuthority(const QString &text)
{
    const qsizetype colon = text.lastIndexOf(u':');
    if (colon <= 0)
        return {};
    QString host = text.left(colon);
    if (host.startsWith(u'[') && host.endsWith(u']'))
        host = host.mid(1, host.size() - 2);
    bool ok = false;
    const uint port = text.mid(colon + 1).toUInt(&ok);
    if (!ok || port == 0 || port > 65535 || host.isEmpty() || host.contains(u' '))
        return {};
    return {host, quint16(port)};
}

bool ipv4IsPublic(quint32 a)
{
    const auto in = [a](quint32 net, int bits) { return (a & (~0u << (32 - bits))) == net; };
    return !(in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8) || in(0xA9FE0000, 16)
             || in(0xAC100000, 12) || in(0xC0000000, 24) || in(0xC0A80000, 16) || in(0xC6120000, 15) || in(0xE0000000, 4)
             || in(0xF0000000, 4));
}

} // namespace

bool isPublicAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return ipv4IsPublic(address.toIPv4Address());
    const Q_IPV6ADDR v6 = address.toIPv6Address();
    const auto word = [&v6](int i) { return quint32(v6[i]) << 24 | quint32(v6[i + 1]) << 16 | quint32(v6[i + 2]) << 8 | v6[i + 3]; };
    bool zeroPrefix = true; // bytes 0..9
    for (int i = 0; i < 10; ++i)
        zeroPrefix = zeroPrefix && v6[i] == 0;
    if (zeroPrefix && v6[10] == 0xff && v6[11] == 0xff)
        return ipv4IsPublic(word(12)); // IPv4-mapped
    if (zeroPrefix && v6[10] == 0 && v6[11] == 0)
        return false; // :: , ::1 and the deprecated IPv4-compatible form
    if (v6[0] == 0x00 && v6[1] == 0x64 && v6[2] == 0xff && v6[3] == 0x9b && word(4) == 0 && word(8) == 0)
        return ipv4IsPublic(word(12)); // NAT64
    if ((v6[0] & 0xfe) == 0xfc)
        return false; // unique local
    if (v6[0] == 0xfe && (v6[1] & 0xc0) == 0x80)
        return false; // link local
    if (v6[0] == 0xfe && (v6[1] & 0xc0) == 0xc0)
        return false; // site local (deprecated)
    if (v6[0] == 0xff)
        return false; // multicast
    return true;
}

// One client of the proxy: reads the request head, connects upstream, then pipes bytes both ways.
class GateConnection : public QObject
{
    Q_OBJECT

public:
    GateConnection(QTcpSocket *client, NetworkGate *gate)
        : QObject(gate)
        , m_gate(gate)
        , m_client(client)
        , m_upstream(new QTcpSocket(this))
    {
        m_client->setParent(this);
        m_headTimer.setSingleShot(true);
        m_headTimer.setInterval(kHeadTimeoutMs);
        connect(&m_headTimer, &QTimer::timeout, this, [this] { reject(408, "Request Timeout"); });
        m_headTimer.start();
        m_idle.setSingleShot(true);
        connect(&m_idle, &QTimer::timeout, this, [this] { abort(); }); // silent both ways for too long
        connect(m_client, &QTcpSocket::readyRead, this, &GateConnection::onClientData);
        connect(m_client, &QTcpSocket::disconnected, this, &GateConnection::onClientClosed);
        connect(m_upstream, &QTcpSocket::connected, this, &GateConnection::onUpstreamConnected);
        connect(m_upstream, &QTcpSocket::errorOccurred, this, &GateConnection::onUpstreamError);
        connect(m_upstream, &QTcpSocket::readyRead, this, [this] { pump(m_upstream, m_client); });
        connect(m_upstream, &QTcpSocket::disconnected, this, &GateConnection::onUpstreamClosed);
        connect(m_client, &QTcpSocket::bytesWritten, this, [this] { pump(m_upstream, m_client); });
        connect(m_upstream, &QTcpSocket::bytesWritten, this, [this] { pump(m_client, m_upstream); });
    }

    // Closes at once, without a reply (the gate shut).
    void abort()
    {
        m_state = State::Done;
        m_upstream->abort();
        m_client->abort();
        deleteLater();
    }

    const QString &token() const { return m_token; }

private:
    enum class State { Head, Connecting, Piping, Done };

    void onClientData()
    {
        if (m_state == State::Head) {
            m_head += m_client->readAll();
            const qsizetype end = m_head.indexOf("\r\n\r\n");
            if (end < 0) {
                if (m_head.size() > kMaxHeadBytes)
                    reject(400, "Bad Request");
                return;
            }
            m_headTimer.stop();
            if (end + 4 > kMaxHeadBytes) {
                reject(400, "Bad Request");
                return;
            }
            m_body = m_head.mid(end + 4); // bytes the client sent behind the head already
            handleHead(m_head.left(end));
            m_head.clear();
        } else if (m_state == State::Piping) {
            pump(m_client, m_upstream);
        }
        // While connecting, the data stays in the client's buffer until piping starts.
    }

    void handleHead(const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> request = lines.first().trimmed().split(' ');
        if (request.size() != 3 || !request.at(2).startsWith("HTTP/1.")) {
            reject(400, "Bad Request");
            return;
        }
        // Credentials first: a request without a valid token is not even looked at.
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (line.toLower().startsWith("proxy-authorization:")) {
                const QByteArray value = line.mid(line.indexOf(':') + 1).trimmed();
                if (value.toLower().startsWith("basic ")) {
                    const QByteArray decoded = QByteArray::fromBase64(value.mid(6).trimmed());
                    const qsizetype colon = decoded.indexOf(':');
                    if (colon > 0 && decoded.left(colon) == kGateUser) {
                        const QString token = QString::fromLatin1(decoded.mid(colon + 1));
                        if (m_gate->tokenValid(token))
                            m_token = token;
                    }
                }
            }
        }
        if (m_token.isEmpty()) {
            reject(407, "Proxy Authentication Required");
            return;
        }
        if (m_gate->connectionsOf(m_token) > m_gate->connectionsPerToken()) { // this one is counted already
            m_token.clear();
            reject(503, "Service Unavailable");
            return;
        }
        const QByteArray method = request.at(0);
        const QString target = QString::fromLatin1(request.at(1));
        // The priming request (see kGatePrimeUrl): answered here, so that the engine learns the credentials before any
        // request that can not ask for them (a WebSocket handshake, a socket of WebRTC).
        if (method == "GET" && target == QLatin1String(kGatePrimeUrl)) {
            m_state = State::Done;
            m_client->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
            m_client->disconnectFromHost();
            connect(m_client, &QTcpSocket::disconnected, this, &QObject::deleteLater);
            QTimer::singleShot(2000, this, [this] { deleteLater(); });
            return;
        }
        Target destination;
        if (method == "CONNECT") {
            destination = parseAuthority(target);
            m_tunnel = true;
        } else if (target.startsWith(u"http://", Qt::CaseInsensitive)) {
            const QUrl url(target);
            if (url.isValid() && !url.host().isEmpty()) {
                destination = {url.host(), quint16(url.port(80))};
                QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
                if (path.isEmpty())
                    path = "/";
                if (url.hasQuery())
                    path += '?' + url.query(QUrl::FullyEncoded).toLatin1();
                // Origin-form request, no proxy or connection headers, and one request per connection.
                m_forward = method + ' ' + path + ' ' + request.at(2) + "\r\n";
                for (qsizetype i = 1; i < lines.size(); ++i) {
                    const QByteArray line = lines.at(i).trimmed();
                    const QByteArray lower = line.toLower();
                    if (line.isEmpty() || lower.startsWith("proxy-") || lower.startsWith("connection:")
                        || lower.startsWith("keep-alive:"))
                        continue;
                    m_forward += line + "\r\n";
                }
                m_forward += "Connection: close\r\n\r\n";
            }
        }
        if (!destination.valid()) {
            reject(400, "Bad Request");
            return;
        }
        m_state = State::Connecting;
        m_connectTimer.setSingleShot(true);
        connect(&m_connectTimer, &QTimer::timeout, this, [this] { reject(504, "Gateway Timeout"); });
        m_connectTimer.start(kConnectTimeoutMs);
        // Resolve here and connect to the address that was checked: the name is not looked up a second time, so it can
        // not be answered differently the second time (DNS rebinding).
        const quint16 port = destination.port;
        const QHostAddress literal(destination.host);
        if (!literal.isNull()) {
            connectChecked({literal}, port);
            return;
        }
        QHostInfo::lookupHost(destination.host, this, [this, port](const QHostInfo &info) {
            if (m_state != State::Connecting)
                return;
            if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
                reject(502, "Bad Gateway");
                return;
            }
            connectChecked(info.addresses(), port);
        });
    }

    // Every address of the answer must be public (one private address among them is a rebinding attempt).
    void connectChecked(const QList<QHostAddress> &addresses, quint16 port)
    {
        if (!m_gate->allowLocalDestinations()) {
            for (const QHostAddress &address : addresses) {
                if (!isPublicAddress(address)) {
                    reject(403, "Forbidden");
                    return;
                }
            }
        }
        m_upstream->connectToHost(addresses.first(), port);
    }

    void onUpstreamConnected()
    {
        if (m_state != State::Connecting)
            return;
        m_connectTimer.stop();
        m_state = State::Piping;
        if (m_tunnel)
            m_client->write("HTTP/1.1 200 Connection Established\r\n\r\n");
        else
            m_upstream->write(m_forward);
        if (!m_body.isEmpty()) {
            m_upstream->write(m_body);
            m_body.clear();
        }
        pump(m_client, m_upstream);
        pump(m_upstream, m_client);
    }

    void onUpstreamError()
    {
        if (m_state == State::Connecting)
            reject(502, "Bad Gateway");
    }

    void onClientClosed()
    {
        if (m_state == State::Done)
            return;
        pump(m_client, m_upstream);
        finish(m_upstream);
    }

    void onUpstreamClosed()
    {
        if (m_state == State::Connecting) {
            reject(502, "Bad Gateway");
            return;
        }
        if (m_state == State::Done)
            return;
        pump(m_upstream, m_client);
        finish(m_client);
    }

    // Moves what `from` has to `to`, unless `to` is too far behind (it asks again when it has written).
    void pump(QTcpSocket *from, QTcpSocket *to)
    {
        if (m_state != State::Piping)
            return;
        m_idle.start(m_gate->idleTimeoutMs());
        while (from->bytesAvailable() > 0 && to->isOpen() && to->bytesToWrite() < kMaxPendingWrite) {
            const QByteArray chunk = from->read(64 * 1024);
            if (chunk.isEmpty())
                break;
            to->write(chunk);
        }
    }

    // Lets `last` flush what it has and close, then ends the connection.
    void finish(QTcpSocket *last)
    {
        m_state = State::Done;
        if (last->state() == QAbstractSocket::ConnectedState && last->bytesToWrite() > 0) {
            connect(last, &QTcpSocket::bytesWritten, this, [this, last] {
                if (last->bytesToWrite() == 0)
                    last->disconnectFromHost();
            });
            connect(last, &QTcpSocket::disconnected, this, &QObject::deleteLater);
            return;
        }
        last->abort();
        m_client->abort();
        m_upstream->abort();
        deleteLater();
    }

    void reject(int code, const char *text)
    {
        if (m_state == State::Done)
            return;
        m_state = State::Done;
        m_headTimer.stop();
        m_connectTimer.stop();
        m_upstream->abort();
        if (code != 408 && m_client->state() == QAbstractSocket::ConnectedState)
            m_client->write(statusLine(code, text));
        m_client->disconnectFromHost();
        QTimer::singleShot(2000, this, [this] { deleteLater(); });
        connect(m_client, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    }

    NetworkGate *m_gate;
    QTcpSocket *m_client;
    QTcpSocket *m_upstream;
    QString m_token;
    State m_state = State::Head;
    bool m_tunnel = false;
    QByteArray m_head;
    QByteArray m_body;
    QByteArray m_forward;
    QTimer m_headTimer;
    QTimer m_connectTimer;
    QTimer m_idle;
};

NetworkGate::NetworkGate(quint16 port, QObject *parent)
    : QObject(parent)
{
    if (!m_server.listen(QHostAddress::LocalHost, port))
        qCritical("murasaki: the network gate cannot listen (%s): web wallpapers have no network at all",
                  qPrintable(m_server.errorString()));
    connect(&m_server, &QTcpServer::newConnection, this, &NetworkGate::onNewConnection);
}

NetworkGate::~NetworkGate()
{
    dropAll();
}

NetworkGate *NetworkGate::instance()
{
    static NetworkGate *gate = new NetworkGate(0, QCoreApplication::instance());
    return gate;
}

int NetworkGate::connectionsOf(const QString &token) const
{
    int count = 0;
    for (const GateConnection *connection : m_connections)
        count += connection->token() == token;
    return count;
}

QString NetworkGate::issueToken()
{
    if (!isListening())
        return {};
    QByteArray random(16, 0);
    for (int i = 0; i < random.size(); i += 4) {
        const quint32 word = QRandomGenerator::system()->generate();
        std::memcpy(random.data() + i, &word, 4);
    }
    const QString token = QString::fromLatin1(random.toHex());
    const bool before = isOpen();
    m_tokens.insert(token);
    if (!before)
        emit openChanged(true);
    return token;
}

void NetworkGate::revokeToken(const QString &token)
{
    if (!m_tokens.remove(token))
        return;
    const QSet<GateConnection *> live = m_connections;
    for (GateConnection *connection : live) {
        if (connection->token() == token) {
            m_connections.remove(connection);
            connection->abort();
        }
    }
    if (!isOpen())
        emit openChanged(false);
}

void NetworkGate::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        if (m_connections.size() >= kMaxConnections) {
            socket->write(statusLine(503, "Service Unavailable"));
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(2000, socket, &QObject::deleteLater);
            continue;
        }
        auto *connection = new GateConnection(socket, this);
        m_connections.insert(connection);
        connect(connection, &QObject::destroyed, this,
                [this, connection] { m_connections.remove(connection); });
    }
}

void NetworkGate::dropAll()
{
    const QSet<GateConnection *> live = m_connections;
    for (GateConnection *connection : live)
        connection->abort();
    m_connections.clear();
}

QByteArray gateEngineFlags(const NetworkGate &gate)
{
    // <-loopback> removes Chromium's implicit "never proxy localhost": the gate decides, for local services too.
    return "--proxy-server=http://127.0.0.1:" + QByteArray::number(gate.isListening() ? gate.port() : 1)
        + " --proxy-bypass-list=<-loopback> --force-webrtc-ip-handling-policy=disable_non_proxied_udp";
}

void applyGateEngineFlags(const NetworkGate &gate)
{
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!flags.isEmpty())
        flags += ' ';
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags + gateEngineFlags(gate));
}

} // namespace murasaki

#include "networkgate.moc"
