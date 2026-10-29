#include "hyprlandpointersource.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <unistd.h>

#include <cmath>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr double kMaxCoordinate = 1e6;

std::optional<QPointF> checked(double x, double y)
{
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > kMaxCoordinate || std::fabs(y) > kMaxCoordinate)
        return std::nullopt;
    return QPointF(x, y);
}

} // namespace

HyprlandPointerSource::HyprlandPointerSource(QString socketDir, int intervalMs, QObject *parent)
    : PointerSource(parent)
    , m_dir(std::move(socketDir))
{
    m_timer.setInterval(intervalMs < 1 ? kDefaultIntervalMs : intervalMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &HyprlandPointerSource::poll);
}

QString HyprlandPointerSource::socketDir(const QProcessEnvironment &env)
{
    const QString signature = env.value(u"HYPRLAND_INSTANCE_SIGNATURE"_s);
    if (signature.isEmpty() || signature.contains(u'/') || signature.contains(u".."_s))
        return {};
    const QString runtime = env.value(u"XDG_RUNTIME_DIR"_s);
    if (!runtime.isEmpty()) { // (an empty value would make the base "/hypr/")
        const QString dir = runtime + u"/hypr/"_s + signature;
        if (QFileInfo::exists(dir + u"/.socket.sock"_s))
            return dir;
    }
    // Older Hyprland: /tmp is shared, so the directory and its socket must be ours.
    const QString dir = u"/tmp/hypr/"_s + signature;
    if (QFileInfo::exists(dir + u"/.socket.sock"_s) && ownedByCurrentUser(dir) && ownedByCurrentUser(dir + u"/.socket.sock"_s))
        return dir;
    return {};
}

bool HyprlandPointerSource::ownedByCurrentUser(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.ownerId() == getuid();
}

std::optional<QPointF> HyprlandPointerSource::parseCursor(const QByteArray &reply)
{
    const QByteArray trimmed = reply.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 256)
        return std::nullopt;
    if (trimmed.startsWith('{')) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed);
        if (!doc.isObject())
            return std::nullopt;
        const QJsonObject object = doc.object();
        if (!object.value(u"x"_s).isDouble() || !object.value(u"y"_s).isDouble())
            return std::nullopt;
        return checked(object.value(u"x"_s).toDouble(), object.value(u"y"_s).toDouble());
    }
    const QList<QByteArray> parts = trimmed.split(',');
    if (parts.size() != 2)
        return std::nullopt;
    bool okX = false, okY = false;
    const double x = parts.at(0).trimmed().toDouble(&okX);
    const double y = parts.at(1).trimmed().toDouble(&okY);
    return okX && okY ? checked(x, y) : std::nullopt;
}

void HyprlandPointerSource::wanted(bool on)
{
    if (on == m_timer.isActive())
        return;
    if (on) {
        m_last.reset();
        m_failures = 0;
        m_failingSince.invalidate();
        m_gaveUp = false;
        m_timer.start();
        poll();
    } else {
        m_timer.stop();
    }
}

void HyprlandPointerSource::poll()
{
    if (m_inFlight || m_gaveUp)
        return;
    m_inFlight = true;
    auto *socket = new QLocalSocket(this);
    auto buffer = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);
    const auto finishRequest = [this, socket, buffer, done](bool ok) {
        if (std::exchange(*done, true))
            return;
        if (socket->isOpen())
            *buffer += socket->readAll(); // bytes that arrived with the close
        socket->abort();
        socket->deleteLater();
        finish(*buffer, ok);
    };
    connect(socket, &QLocalSocket::connected, socket, [socket] { socket->write("j/cursorpos"); });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer] {
        if (buffer->size() < 4096)
            *buffer += socket->readAll();
        else
            socket->readAll();
    });
    connect(socket, &QLocalSocket::disconnected, this, [finishRequest] { finishRequest(true); });
    connect(socket, &QLocalSocket::errorOccurred, this, [finishRequest] { finishRequest(false); });
    QTimer::singleShot(kRequestTimeoutMs, this, [finishRequest] { finishRequest(false); });
    socket->connectToServer(m_dir + u"/.socket.sock"_s);
}

void HyprlandPointerSource::finish(const QByteArray &reply, bool ok)
{
    m_inFlight = false;
    if (!m_timer.isActive())
        return; // stopped while the request was out
    (void)ok; // Hyprland closes the socket after the reply: a close is not a failure, an empty or wrong reply is
    const std::optional<QPointF> position = parseCursor(reply);
    if (!position) {
        if (!m_failingSince.isValid())
            m_failingSince.start();
        if (++m_failures >= kFailuresBeforeUnavailable && m_failingSince.elapsed() >= m_giveUpWindowMs && !m_gaveUp) {
            m_gaveUp = true;
            m_timer.stop();
            emit unavailable(u"Hyprland's request socket does not answer j/cursorpos"_s);
        }
        return;
    }
    m_failures = 0;
    m_failingSince.invalidate();
    if (m_last && *m_last == *position)
        return;
    m_last = position;
    emit moved(*position);
}

} // namespace murasaki
