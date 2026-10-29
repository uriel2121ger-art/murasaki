#pragma once

// Follows the MPRIS players of the session bus (org.mpris.MediaPlayer2.*): their Metadata, PlaybackStatus, Position and Rate,
// re-read (GetAll) after every PropertiesChanged and Seeked, at most every kRefreshDelayMs. Reads only from names that have an
// owner (a call to a free name could start a service). A player is untrusted: wrong types are ignored, lists are cut, at most
// kMaxPlayers are followed, and a call that does not answer in time is dropped.

#include "media/mediapolicy.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <vector>

class QDBusMessage;

namespace murasaki::media {

class MediaWatcher : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxPlayers = 16;
    static constexpr int kRefreshDelayMs = 200;
    static constexpr int kCallTimeoutMs = 2000;

    explicit MediaWatcher(const QDBusConnection &bus = QDBusConnection::sessionBus(), QObject *parent = nullptr);

    // Subscribes and reads the players that are there. Call after connecting to playersChanged().
    void start();
    // Stops following: the bus is left alone and nothing of the players is kept (a hub that must not read them any more).
    void stop();
    bool running() const { return m_started; }
    const std::vector<PlayerSnapshot> &players() const { return m_snapshots; }

signals:
    // Something a player says changed (or a player came or went).
    void playersChanged();

private slots:
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void onPropertiesChanged(const QDBusMessage &message);
    void onSeeked(const QDBusMessage &message);
    void flush();

private:
    struct Player {
        QString service;  // the well-known name
        QString owner;    // its unique name
        PlayerSnapshot snapshot;
        quint64 order = 0; // when it was first seen
        bool stale = false;
        bool reading = false;
    };
    void add(const QString &service, const QString &owner);
    void remove(const QString &service);
    void touchOwner(const QString &owner);
    void read(const QString &service);
    void publish();

    QDBusConnection m_bus;
    QMap<QString, Player> m_players; // by well-known name
    std::vector<PlayerSnapshot> m_snapshots;
    QTimer m_refresh;
    QElapsedTimer m_clock;
    quint64 m_order = 0;
    bool m_started = false;
};

} // namespace murasaki::media
