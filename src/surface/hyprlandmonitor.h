#pragma once

#include "sessionmonitor.h"

#include <QJsonArray>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QTimer>

#include <functional>
#include <optional>

namespace murasaki {

// Hyprland: line events on .socket2.sock trigger j/monitors, j/clients and j/workspaces queries on .socket.sock
// (one connection per query, closed by Hyprland after the reply).
class HyprlandMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    bool knowsOcclusion() const override { return true; }
    explicit HyprlandMonitor(QString socketDir, QObject *parent = nullptr);

    // $XDG_RUNTIME_DIR/hypr/<signature> (Hyprland ≥ 0.40) or /tmp/hypr/<signature>; empty if neither exists.
    static QString socketDir(const QProcessEnvironment &env);

    void start() override;

private:
    void connectEvents();
    void onEventData();
    void refresh();
    void query(const QByteArray &command, std::function<void(const std::optional<QJsonArray> &)> done);

    QString m_dir;
    QLocalSocket m_events;
    QByteArray m_eventBuffer;
    QTimer m_refresh;
    QTimer m_reconnect;
    bool m_refreshing = false;
    bool m_dirty = false;
};

} // namespace murasaki
