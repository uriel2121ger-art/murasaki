#pragma once

#include "sessionmonitor.h"

#include <QJsonArray>
#include <QLocalSocket>
#include <QTimer>

namespace murasaki {

// Sway's i3-compatible IPC: one socket subscribed to events, one for GET_WORKSPACES/GET_TREE queries.
class SwayMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    bool knowsOcclusion() const override { return true; }
    explicit SwayMonitor(QString socketPath, QObject *parent = nullptr);

    void start() override;

private:
    void connectSockets();
    void armReconnect();
    void refresh();
    void onEventData();
    void onQueryData();

    QString m_path;
    QLocalSocket m_events;
    QLocalSocket m_queries;
    QByteArray m_eventBuffer;
    QByteArray m_queryBuffer;
    QJsonArray m_workspaces;
    QTimer m_refresh;
    QTimer m_reconnect;
};

} // namespace murasaki
