#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>

namespace murasaki {

class GateConnection;

// Whether the proxy may connect to this address: only public unicast addresses. Loopback, private (RFC 1918), shared
// (100.64.0.0/10), link-local, unique-local (fc00::/7), multicast, reserved and unspecified addresses are refused, and so
// are IPv4 addresses hidden in IPv6 (::ffff:a.b.c.d, 64:ff9b::/96): a page must not reach the machine or its network
// through the gate.
bool isPublicAddress(const QHostAddress &address);

// The doorway of all network traffic of the web engine. The engine is told to send everything through this proxy (see
// applyGateEngineFlags()), and the proxy serves only requests that carry a valid token (HTTP Basic credentials in
// Proxy-Authorization). A token is issued to one web profile, which belongs to one shown wallpaper that has the network
// permission; wallpapers without it get no token, so everything their engine does on its own (preconnect hints, DNS
// prefetch, WebRTC's TCP sockets, ...) meets a 407 and goes nowhere. Revoking a token drops the connections made with
// it. With no token issued, the proxy refuses everything.
//
// The proxy is a plain forwarder for CONNECT (https, wss) and absolute-form HTTP requests, bound to 127.0.0.1 only.
// Credentials are checked for every connection (one request each), not once per process.
class NetworkGate : public QObject
{
    Q_OBJECT

public:
    // `port` 0 picks a free one. A gate that cannot listen (isListening() false) fails closed: it issues no tokens.
    explicit NetworkGate(quint16 port = 0, QObject *parent = nullptr);
    ~NetworkGate() override;

    // The one gate of this process (the engine's proxy setting is process-wide too).
    static NetworkGate *instance();

    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    QHostAddress address() const { return m_server.serverAddress(); }

    // A fresh random token (128 bits, hex). Give it to the one profile that may use the network.
    QString issueToken();
    // The token stops working now and the connections made with it are dropped. Unknown tokens are ignored.
    void revokeToken(const QString &token);
    bool tokenValid(const QString &token) const { return !token.isEmpty() && m_tokens.contains(token); }
    // How long a connection may stay silent in both directions before it is dropped, and how many one token may hold.
    void setIdleTimeoutMs(int ms) { m_idleTimeoutMs = ms; }
    int idleTimeoutMs() const { return m_idleTimeoutMs; }
    void setConnectionsPerToken(int count) { m_perToken = count; }
    int connectionsPerToken() const { return m_perToken; }
    int connectionsOf(const QString &token) const;
    bool isOpen() const { return !m_tokens.isEmpty(); }

    // Tests only: their servers are on 127.0.0.1. Never set outside tests.
    void setAllowLocalDestinationsForTests(bool allow) { m_allowLocal = allow; }
    bool allowLocalDestinations() const { return m_allowLocal; }

signals:
    void openChanged(bool open);

private:
    void onNewConnection();
    void dropAll();
    friend class GateConnection;

// Whether the proxy may connect to this address: only public unicast addresses. Loopback, private (RFC 1918), shared
// (100.64.0.0/10), link-local, unique-local (fc00::/7), multicast, reserved and unspecified addresses are refused, and so
// are IPv4 addresses hidden in IPv6 (::ffff:a.b.c.d, 64:ff9b::/96): a page must not reach the machine or its network
// through the gate.
bool isPublicAddress(const QHostAddress &address);

    QTcpServer m_server;
    QSet<QString> m_tokens;
    bool m_allowLocal = false;
    int m_idleTimeoutMs = 120000;
    int m_perToken = 128;
    QSet<GateConnection *> m_connections;
};

// The command line flags that make the web engine use the gate: the proxy, no implicit loopback bypass (a page must not
// reach local services around the gate either), and no UDP for WebRTC (it can not go through an HTTP proxy). Appended
// to QTWEBENGINE_CHROMIUM_FLAGS; call before the first web profile of the process is created.
void applyGateEngineFlags(const NetworkGate &gate);
// The flags themselves. A gate that is not listening gets a proxy on port 1 (tcpmux, which nobody serves): the engine
// then has no way out whatever a page does, instead of a proxy setting that points nowhere sensible.
QByteArray gateEngineFlags(const NetworkGate &gate);

// The credentials a profile answers the proxy's challenge with: user name and password for a token.
inline constexpr char kGateUser[] = "murasaki";
// A profile with a token loads this first: the gate answers it itself once the credentials are right, and the engine
// keeps the credentials for every later request, including those that can not prompt for them.
inline constexpr char kGatePrimeUrl[] = "http://murasaki-gate.invalid/prime";

} // namespace murasaki
