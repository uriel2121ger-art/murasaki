#include "media/mediawatcher.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

#include <algorithm>

namespace murasaki::media {

namespace {

const QString kPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");

QVariant plain(const QVariant &v)
{
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

// A value as text: a string, or the first string of an array of them.
QString asText(const QVariant &raw)
{
    const QVariant v = plain(raw);
    if (v.metaType().id() == QMetaType::QString) {
        return v.toString().left(kMaxRawText);
    }
    if (v.metaType().id() == QMetaType::QStringList) {
        const QStringList l = v.toStringList();
        return l.isEmpty() ? QString() : l.first().left(kMaxRawText);
    }
    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::ArrayType) {
            arg.beginArray();
            QString first;
            if (!arg.atEnd()) {
                const QVariant item = arg.asVariant();
                first = item.metaType().id() == QMetaType::QString ? item.toString().left(kMaxRawText) : QString();
            }
            return first;
        }
    }
    return {};
}

// An array of strings, at most `limit` of them (the rest of a hostile list is never read).
QStringList asTexts(const QVariant &raw, int limit)
{
    const QVariant v = plain(raw);
    QStringList out;
    if (v.metaType().id() == QMetaType::QString) {
        out << v.toString().left(kMaxRawText);
    } else if (v.metaType().id() == QMetaType::QStringList) {
        out = v.toStringList().mid(0, limit);
        for (QString &t : out) {
            t = t.left(kMaxRawText);
        }
    } else if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::ArrayType) {
            arg.beginArray();
            while (!arg.atEnd() && out.size() < limit) {
                const QVariant item = arg.asVariant();
                if (item.metaType().id() == QMetaType::QString) {
                    out << item.toString().left(kMaxRawText);
                }
            }
        }
    }
    return out;
}

double asNumber(const QVariant &raw, double fallback)
{
    const QVariant v = plain(raw);
    switch (v.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Long:
    case QMetaType::ULong:
        return v.toDouble();
    default:
        return fallback;
    }
}

QVariantMap asMap(const QVariant &raw)
{
    const QVariant v = plain(raw);
    if (v.metaType().id() == QMetaType::QVariantMap) {
        return v.toMap();
    }
    if (v.canConvert<QDBusArgument>()) {
        QVariantMap map;
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::MapType) {
            arg.beginMap();
            int n = 0;
            while (!arg.atEnd() && n++ < 64) { // a hostile map is read as far as the fields that count
                QString key;
                QDBusVariant value;
                arg.beginMapEntry();
                arg >> key >> value;
                arg.endMapEntry();
                map.insert(key, value.variant());
            }
        }
        return map;
    }
    return {};
}

} // namespace

MediaWatcher::MediaWatcher(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
{
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(kRefreshDelayMs);
    connect(&m_refresh, &QTimer::timeout, this, &MediaWatcher::flush);
    m_clock.start();
}

void MediaWatcher::start()
{
    if (m_started || !m_bus.isConnected()) {
        return;
    }
    m_started = true;
    m_bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
                  QStringLiteral("NameOwnerChanged"), this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    m_bus.connect(QString(), kPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QDBusMessage)));
    m_bus.connect(QString(), kPath, kPlayer, QStringLiteral("Seeked"), this, SLOT(onSeeked(QDBusMessage)));
    if (QDBusConnectionInterface *bus = m_bus.interface()) {
        const QStringList names = bus->registeredServiceNames().value();
        for (const QString &name : names) {
            if (name.startsWith(kPrefix)) {
                add(name, bus->serviceOwner(name).value());
            }
        }
    }
}

void MediaWatcher::stop()
{
    if (!m_started) {
        return;
    }
    m_started = false;
    m_refresh.stop();
    m_bus.disconnect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
                     QStringLiteral("NameOwnerChanged"), this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    m_bus.disconnect(QString(), kPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"), this,
                     SLOT(onPropertiesChanged(QDBusMessage)));
    m_bus.disconnect(QString(), kPath, kPlayer, QStringLiteral("Seeked"), this, SLOT(onSeeked(QDBusMessage)));
    m_players.clear(); // (answers still on their way find no player)
    publish();
}

void MediaWatcher::onNameOwnerChanged(const QString &name, const QString &, const QString &newOwner)
{
    if (!name.startsWith(kPrefix)) {
        return;
    }
    if (newOwner.isEmpty()) {
        remove(name);
    } else {
        remove(name); // a new owner of the name is a new player
        add(name, newOwner);
    }
}

void MediaWatcher::add(const QString &service, const QString &owner)
{
    if (owner.isEmpty() || m_players.contains(service)) {
        return;
    }
    if (m_players.size() >= kMaxPlayers) {
        // names are free to take: a flood of them must not push out the player that plays. The oldest one that is not Playing goes; if
        // every one plays, the newcomer is not followed.
        QString oldest;
        quint64 oldestOrder = ~quint64{0};
        for (const Player &q : std::as_const(m_players)) {
            if (q.snapshot.status != QLatin1String("Playing") && q.order < oldestOrder) {
                oldest = q.service;
                oldestOrder = q.order;
            }
        }
        if (oldest.isEmpty()) {
            return;
        }
        m_players.remove(oldest);
    }
    Player p;
    p.order = ++m_order;
    p.service = service;
    p.owner = owner;
    p.snapshot.owner = owner;
    m_players.insert(service, p);
    read(service);
}

void MediaWatcher::remove(const QString &service)
{
    if (m_players.remove(service) > 0) {
        publish();
    }
}

void MediaWatcher::touchOwner(const QString &owner)
{
    for (Player &p : m_players) {
        if (p.owner == owner) {
            p.stale = true;
        }
    }
    if (!m_refresh.isActive()) {
        m_refresh.start();
    }
}

void MediaWatcher::onPropertiesChanged(const QDBusMessage &message)
{
    if (!message.arguments().isEmpty() && message.arguments().first().toString() == kPlayer) {
        touchOwner(message.service());
    }
}

void MediaWatcher::onSeeked(const QDBusMessage &message)
{
    touchOwner(message.service());
}

void MediaWatcher::flush()
{
    for (auto it = m_players.begin(); it != m_players.end(); ++it) {
        if (it->stale && !it->reading) {
            it->stale = false;
            read(it.key());
        }
    }
}

void MediaWatcher::read(const QString &service)
{
    auto it = m_players.find(service);
    if (it == m_players.end() || it->reading) {
        return;
    }
    it->reading = true;
    QDBusMessage call = QDBusMessage::createMethodCall(service, kPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    call << kPlayer;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    const QString owner = it->owner;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, service, owner](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        auto it = m_players.find(service);
        if (it == m_players.end() || it->owner != owner) {
            return; // gone, or another player now
        }
        it->reading = false;
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            return;
        }
        const QVariantMap all = reply.value();
        PlayerSnapshot s = it->snapshot;
        const QString status = asText(all.value(QStringLiteral("PlaybackStatus")));
        s.status = status == QLatin1String("Playing") || status == QLatin1String("Paused") ? status : QStringLiteral("Stopped");
        if (s.status == QLatin1String("Playing") && it->snapshot.status != QLatin1String("Playing")) {
            s.playingSince = m_clock.elapsed() + 1; // (0 means it never played)
        }
        const QVariantMap meta = asMap(all.value(QStringLiteral("Metadata")));
        s.title = asText(meta.value(QStringLiteral("xesam:title")));
        s.artist = asText(meta.value(QStringLiteral("xesam:artist")));
        s.albumTitle = asText(meta.value(QStringLiteral("xesam:album")));
        s.albumArtist = asText(meta.value(QStringLiteral("xesam:albumArtist")));
        s.genres = asTexts(meta.value(QStringLiteral("xesam:genre")), kMaxGenres);
        s.artUrl = asText(meta.value(QStringLiteral("mpris:artUrl")));
        s.durationSec = asNumber(meta.value(QStringLiteral("mpris:length")), 0.0) / 1.0e6;
        s.positionSec = asNumber(all.value(QStringLiteral("Position")), 0.0) / 1.0e6;
        s.rate = asNumber(all.value(QStringLiteral("Rate")), 1.0);
        it->snapshot = s;
        publish();
        if (it->stale) {
            m_refresh.start(); // it changed again while this answer was on its way
        }
    });
}

void MediaWatcher::publish()
{
    m_snapshots.clear();
    for (const Player &p : std::as_const(m_players)) {
        m_snapshots.push_back(p.snapshot);
    }
    emit playersChanged();
}

} // namespace murasaki::media
