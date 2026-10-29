#pragma once

#include "dbusbackend.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace murasaki {

// Asynchronous client of the murasaki-surface D-Bus API: which outputs exist and what plays on them, and the
// compositor's desktops. Refreshed on OutputsChanged; `poll` adds a 2 s timer for pages that show live state.
class SurfaceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(QVariantList desktops READ desktops NOTIFY desktopsChanged)
    Q_PROPERTY(bool poll READ poll WRITE setPoll NOTIFY pollChanged)

public:
    static constexpr int PollIntervalMs = 2000;

    // Takes ownership of the backend.
    explicit SurfaceClient(DBusBackend *backend, QObject *parent = nullptr);
    // Talks to the real surface on the session bus.
    explicit SurfaceClient(QObject *parent = nullptr);

    bool available() const { return m_available; }
    QVariantList outputs() const { return m_outputs; }
    QVariantList desktops() const { return m_desktops; }
    bool poll() const { return m_pollTimer.isActive(); }
    void setPoll(bool poll);

    void start();

    Q_INVOKABLE void refresh();
    // The user asks for another try at the wallpaper of an output that could not be shown. The new state arrives with
    // OutputsChanged.
    Q_INVOKABLE void retryOutput(const QString &output);

signals:
    void availableChanged();
    void outputsChanged();
    void desktopsChanged();
    void pollChanged();

private:
    void onAvailability(bool up);
    void fetch(const QString &method, QVariantList &cache, void (SurfaceClient::*changed)());

    DBusBackend *m_backend;
    bool m_available = false;
    quint64 m_generation = 0;
    QVariantList m_outputs;
    QVariantList m_desktops;
    bool m_outputsLoaded = false;
    bool m_desktopsLoaded = false;
    QTimer m_pollTimer;
};

} // namespace murasaki
