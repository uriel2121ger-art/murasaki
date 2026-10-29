#include "hyprlandmonitor.h"

#include <QFileInfo>
#include <QJsonDocument>

#include <memory>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr int kQueryTimeoutMs = 2000;

bool isRelevantEvent(const QByteArray &name)
{
    static const QList<QByteArray> kEvents{
        "workspace", "workspacev2", "focusedmon", "focusedmonv2", "openwindow", "closewindow", "movewindow",
        "movewindowv2", "fullscreen", "changefloatingmode", "monitoradded", "monitoraddedv2", "monitorremoved",
        "createworkspace", "createworkspacev2", "destroyworkspace", "destroyworkspacev2", "renameworkspace",
        "minimized"};
    return kEvents.contains(name);
}

} // namespace

HyprlandMonitor::HyprlandMonitor(QString socketDir, QObject *parent)
    : SessionMonitor(parent)
    , m_dir(std::move(socketDir))
{
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(50);
    connect(&m_refresh, &QTimer::timeout, this, &HyprlandMonitor::refresh);
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(2000);
    connect(&m_reconnect, &QTimer::timeout, this, &HyprlandMonitor::connectEvents);
    connect(&m_events, &QLocalSocket::connected, this, &HyprlandMonitor::refresh); // re-read after a reconnect
    connect(&m_events, &QLocalSocket::readyRead, this, &HyprlandMonitor::onEventData);
    connect(&m_events, &QLocalSocket::disconnected, &m_reconnect, qOverload<>(&QTimer::start));
    connect(&m_events, &QLocalSocket::errorOccurred, &m_reconnect, qOverload<>(&QTimer::start));
}

QString HyprlandMonitor::socketDir(const QProcessEnvironment &env)
{
    const QString signature = env.value(u"HYPRLAND_INSTANCE_SIGNATURE"_s);
    if (signature.isEmpty() || signature.contains(u'/'))
        return {};
    for (const QString &base : {env.value(u"XDG_RUNTIME_DIR"_s) + u"/hypr/"_s, u"/tmp/hypr/"_s}) {
        if (QFileInfo::exists(base + signature + u"/.socket2.sock"_s))
            return base + signature;
    }
    return {};
}

void HyprlandMonitor::start()
{
    connectEvents();
    refresh();
}

void HyprlandMonitor::connectEvents()
{
    m_events.abort();
    m_eventBuffer.clear();
    m_events.connectToServer(m_dir + u"/.socket2.sock"_s);
}

void HyprlandMonitor::onEventData()
{
    m_eventBuffer += m_events.readAll();
    qsizetype newline;
    while ((newline = m_eventBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_eventBuffer.left(newline);
        m_eventBuffer.remove(0, newline + 1);
        if (isRelevantEvent(line.left(line.indexOf(">>"))))
            m_refresh.start();
    }
}

void HyprlandMonitor::refresh()
{
    if (m_refreshing) {
        m_dirty = true; // run again once the current round finishes
        return;
    }
    m_refreshing = true;
    // A failed query (timeout, socket error, non-array reply) fails the whole round: the last state stays in effect
    // rather than being replaced by one built from partial data.
    const auto endRound = [this](const std::optional<SessionState> &state) {
        m_refreshing = false;
        if (state)
            setState(*state);
        if (std::exchange(m_dirty, false))
            refresh();
    };
    query("j/monitors", [this, endRound](const std::optional<QJsonArray> &monitors) {
        if (!monitors || monitors->isEmpty())
            return endRound(std::nullopt);
        query("j/clients", [this, endRound, monitors](const std::optional<QJsonArray> &clients) {
            if (!clients)
                return endRound(std::nullopt);
            query("j/workspaces", [endRound, monitors, clients](const std::optional<QJsonArray> &workspaces) {
                if (!workspaces)
                    return endRound(std::nullopt);
                endRound(parseHyprlandState(*monitors, *clients, *workspaces));
            });
        });
    });
}

void HyprlandMonitor::query(const QByteArray &command, std::function<void(const std::optional<QJsonArray> &)> done)
{
    auto *socket = new QLocalSocket(this);
    auto buffer = std::make_shared<QByteArray>();
    auto finished = std::make_shared<bool>(false);
    const auto finish = [socket, buffer, finished, done] {
        if (std::exchange(*finished, true))
            return;
        *buffer += socket->readAll(); // bytes that arrived together with the close
        socket->abort();
        socket->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(*buffer);
        done(doc.isArray() ? std::optional<QJsonArray>(doc.array()) : std::nullopt);
    };
    connect(socket, &QLocalSocket::connected, socket, [socket, command] { socket->write(command); });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer] { *buffer += socket->readAll(); });
    connect(socket, &QLocalSocket::disconnected, this, finish);
    connect(socket, &QLocalSocket::errorOccurred, this, finish);
    QTimer::singleShot(kQueryTimeoutMs, this, finish);
    socket->connectToServer(m_dir + u"/.socket.sock"_s);
}

} // namespace murasaki
