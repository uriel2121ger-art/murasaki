#include "swaymonitor.h"

#include "i3ipc.h"

#include <QJsonDocument>
#include <QSignalBlocker>

#include <utility>

namespace murasaki {
namespace {

constexpr quint32 kGetWorkspaces = 1;
constexpr quint32 kSubscribe = 2;
constexpr quint32 kGetTree = 4;

} // namespace

SwayMonitor::SwayMonitor(QString socketPath, QObject *parent)
    : SessionMonitor(parent)
    , m_path(std::move(socketPath))
{
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(50);
    connect(&m_refresh, &QTimer::timeout, this, &SwayMonitor::refresh);
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(2000);
    connect(&m_reconnect, &QTimer::timeout, this, &SwayMonitor::connectSockets);

    connect(&m_events, &QLocalSocket::connected, this,
            [this] { m_events.write(i3ipcFrame(kSubscribe, "[\"workspace\",\"window\",\"output\"]")); });
    connect(&m_queries, &QLocalSocket::connected, this, &SwayMonitor::refresh);
    connect(&m_events, &QLocalSocket::readyRead, this, &SwayMonitor::onEventData);
    connect(&m_queries, &QLocalSocket::readyRead, this, &SwayMonitor::onQueryData);
    for (QLocalSocket *socket : {&m_events, &m_queries}) {
        connect(socket, &QLocalSocket::disconnected, this, &SwayMonitor::armReconnect);
        connect(socket, &QLocalSocket::errorOccurred, this, &SwayMonitor::armReconnect);
    }
}

void SwayMonitor::start()
{
    connectSockets();
}

void SwayMonitor::connectSockets()
{
    // The last state stays in effect while reconnecting.
    for (QLocalSocket *socket : {&m_events, &m_queries}) {
        {
            // abort() on a connected socket emits disconnected(), which must not re-arm the reconnect timer.
            const QSignalBlocker blocker(socket);
            socket->abort();
        }
        socket->connectToServer(m_path);
    }
    m_eventBuffer.clear();
    m_queryBuffer.clear();
}

void SwayMonitor::armReconnect()
{
    if (!m_reconnect.isActive())
        armReconnect();
}

void SwayMonitor::refresh()
{
    if (m_queries.state() != QLocalSocket::ConnectedState)
        return;
    // Replies arrive in request order, so the tree always follows its workspaces.
    m_queries.write(i3ipcFrame(kGetWorkspaces, {}));
    m_queries.write(i3ipcFrame(kGetTree, {}));
}

void SwayMonitor::onEventData()
{
    m_eventBuffer += m_events.readAll();
    bool corrupt = false;
    for (const I3IpcMessage &message : i3ipcTake(m_eventBuffer, &corrupt)) {
        if (message.type & kI3IpcEventBit)
            m_refresh.start();
    }
    if (corrupt)
        armReconnect(); // desynchronised stream; the last state stays in effect meanwhile
}

void SwayMonitor::onQueryData()
{
    m_queryBuffer += m_queries.readAll();
    bool corrupt = false;
    const QList<I3IpcMessage> messages = i3ipcTake(m_queryBuffer, &corrupt);
    for (const I3IpcMessage &message : messages) {
        const QJsonDocument doc = QJsonDocument::fromJson(message.payload);
        if (message.type == kGetWorkspaces)
            m_workspaces = doc.array();
        else if (message.type == kGetTree)
            setState(parseSwayState(m_workspaces, doc.object()));
    }
    if (corrupt)
        armReconnect();
}

} // namespace murasaki
