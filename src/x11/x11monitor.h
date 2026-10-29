#pragma once

#include "sessionmonitor.h"
#include "x11state.h"

#include <QAbstractNativeEventFilter>
#include <QTimer>

#include <functional>

namespace murasaki {

// The session monitor of an X11 session: follows the EWMH state of the window manager (desktops, windows, the active
// one) and turns it into a SessionState (see buildX11State). It listens for PropertyNotify on the root and on the client
// windows, coalesces bursts, and re-reads on a slow timer as well for managers that miss a notification.
class X11Monitor : public SessionMonitor, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit X11Monitor(QObject *parent = nullptr);
    ~X11Monitor() override;

    void start() override;

    // What Qt knows of the outputs, in root coordinates. Tests replace it.
    void setOutputProvider(std::function<QList<X11Output>()> provider) { m_outputs = std::move(provider); }
    void setPollInterval(int ms) { m_poll.setInterval(ms); }

    // The snapshot the monitor reads from the X server right now (also used by tests).
    X11Snapshot snapshot() const;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void refresh();
    void watchClients(const QList<quint32> &clients);

    std::function<QList<X11Output>()> m_outputs;
    QTimer m_coalesce;
    QTimer m_poll;
    QList<quint32> m_watched;
};

} // namespace murasaki
