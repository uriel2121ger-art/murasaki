#include "surfaceservice.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr qsizetype kMaxReportSize = 1024 * 1024;
constexpr qsizetype kMaxOutputNameSize = 200;
constexpr int kMaxHostedOutputs = 64;    // any session client can register; keep the bookkeeping bounded
constexpr int kMaxOutputsPerCaller = 8;  // and one connection cannot use them all up

QString compact(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

QString SurfaceService::Outputs() const
{
    return compact(outputsProvider ? outputsProvider() : QJsonArray());
}

void SurfaceService::notifyOutputsChanged()
{
    const QString current = Outputs();
    if (current == m_lastOutputs)
        return;
    m_lastOutputs = current;
    emit OutputsChanged();
}

void SurfaceService::setPetAudioMuted(bool muted)
{
    if (muted == m_petAudioMuted)
        return;
    m_petAudioMuted = muted;
    emit PetAudioMutedChanged(muted);
}

// Murasaki's own daemon and overlay (the owners of their bus names) read everything; any other process on the session bus gets only where a fullscreen or covering
// window is, never which application it is. In-process calls (the tests) are trusted.
bool SurfaceService::calledByTrusted() const
{
    if (!calledFromDBus())
        return true;
    const auto interface = QDBusConnection::sessionBus().interface();
    for (const QString &name : {QString(kOverlayDBusService), QString(kDBusService)}) {
        const QDBusReply<QString> owner = interface->serviceOwner(name);
        if (owner.isValid() && message().service() == owner.value())
            return true;
    }
    return false;
}

QString SurfaceService::SessionFacts() const
{
    return calledByTrusted() ? m_text.facts : m_text.publicFacts;
}

QString SurfaceService::OutputFullscreen() const
{
    return calledByTrusted() ? m_text.outputFullscreen : m_text.publicOutputFullscreen;
}

void SurfaceService::setSessionFacts(const SessionFactsText &text)
{
    if (text.facts == m_text.facts && text.outputFullscreen == m_text.outputFullscreen && text.publicFacts == m_text.publicFacts
        && text.publicOutputFullscreen == m_text.publicOutputFullscreen)
        return;
    if (!m_factsTimer.isSingleShot()) { // first use
        m_factsTimer.setSingleShot(true);
        if (m_factsTimer.interval() <= 0)
            m_factsTimer.setInterval(250);
        connect(&m_factsTimer, &QTimer::timeout, this, [this] {
            if (m_factsDirty) {
                flushFacts();
                m_factsTimer.start(); // what comes next waits a full interval again
            }
        });
    }
    m_text = text;
    m_factsDirty = true;
    if (!m_factsTimer.isActive()) { // the leading edge: the first change goes out at once
        flushFacts();
        m_factsTimer.start();
    }
}

void SurfaceService::flushFacts()
{
    m_factsDirty = false;
    emit SessionFactsChanged();
    if (m_text.outputFullscreen != m_emittedOutputFullscreen) {
        m_emittedOutputFullscreen = m_text.outputFullscreen;
        emit OutputFullscreenChanged();
    }
}

QString SurfaceService::Desktops() const
{
    return compact(desktopsProvider ? desktopsProvider() : QJsonArray());
}

void SurfaceService::ReportKWinState(const QString &json)
{
    if (json.size() > kMaxReportSize)
        return; // any session client can call this; a report only affects pausing, but keep it bounded
    // Only KWin's scripting engine may feed the pause logic.
    if (!calledByKWin())
        return;
    emit kwinStateReported(json);
}

void SurfaceService::ReportShellState(const QString &json)
{
    if (json.size() > kMaxReportSize)
        return;
    // Only the shell itself (the extension runs inside it) may feed the pause logic.
    if (!calledByGnomeShell())
        return;
    emit shellStateReported(json);
}

void SurfaceService::ReportKWinCursor(int x, int y)
{
    if (!calledByKWin())
        return;
    emit kwinCursorReported(QPointF(x, y));
}

void SurfaceService::ReportHostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY)
{
    // Not an error reply for a bad value: the overlay sends these at the rate of the mouse and has nobody to tell.
    if (output.isEmpty() || output.size() > kMaxOutputNameSize || !calledByOverlay())
        return;
    sendHostPointer(output, QPointF(x, y), QSizeF(width, height), buttons, wheelX, wheelY);
}

// Direct (non-D-Bus) calls, e.g. tests, skip the check.
bool SurfaceService::calledByOverlay() const
{
    if (!calledFromDBus())
        return true;
    const QDBusReply<QString> owner = QDBusConnection::sessionBus().interface()->serviceOwner(QString(kOverlayDBusService));
    return owner.isValid() && message().service() == owner.value();
}

bool SurfaceService::sendHostPointer(const QString &output, const QPointF &local, const QSizeF &size, uint buttons, int wheelX, int wheelY)
{
    constexpr int kMaxWheel = 120 * 32; // a turn of the wheel is 120 per notch; more is not a hand
    if (!std::isfinite(local.x()) || !std::isfinite(local.y()) || !std::isfinite(size.width()) || !std::isfinite(size.height())
        || !(size.width() > 0) || !(size.height() > 0))
        return false;
    if (!sendToHost(output, u"HostPointer"_s,
                    {output, local.x(), local.y(), size.width(), size.height(), buttons & 7u, std::clamp(wheelX, -kMaxWheel, kMaxWheel),
                     std::clamp(wheelY, -kMaxWheel, kMaxWheel)}))
        return false;
    if ((buttons & 7u) != 0 || m_lastPointer.contains(output))
        m_lastPointer.insert(output, LastPointer{local, size, buttons & 7u});
    if ((buttons & 7u) == 0)
        m_lastPointer.remove(output);
    else
        watchOverlay();
    return true;
}

// A button that was down when the overlay went away (a crash, the unit stopped) would stay down in the host and turn every cursor move into a
// drag: the release is sent for it.
void SurfaceService::watchOverlay()
{
    if (m_overlayWatcher || !QDBusConnection::sessionBus().isConnected())
        return;
    m_overlayWatcher = new QDBusServiceWatcher(QString(kOverlayDBusService), QDBusConnection::sessionBus(),
                                               QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_overlayWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { releaseHeldButtons(); });
}

void SurfaceService::releaseHeldButtons()
{
    const auto held = std::exchange(m_lastPointer, {});
    for (auto it = held.cbegin(); it != held.cend(); ++it) {
        if (it->buttons != 0)
            sendHostPointer(it.key(), it->position, it->size, 0, 0, 0);
    }
}

// Direct (non-D-Bus) calls, e.g. tests, skip the check.
bool SurfaceService::calledByKWin() const
{
    if (!calledFromDBus())
        return true;
    const QDBusReply<QString> owner = QDBusConnection::sessionBus().interface()->serviceOwner(u"org.kde.KWin"_s);
    return owner.isValid() && message().service() == owner.value();
}

bool SurfaceService::calledByGnomeShell() const
{
    if (!calledFromDBus())
        return true;
    const QDBusReply<QString> owner = QDBusConnection::sessionBus().interface()->serviceOwner(u"org.gnome.Shell"_s);
    return owner.isValid() && message().service() == owner.value();
}

bool SurfaceService::hasVisibleHost() const
{
    return std::any_of(m_owners.keyBegin(), m_owners.keyEnd(),
                       [this](const QString &output) { return !m_hidden.contains(output); });
}

bool SurfaceService::sendHostCursor(const QString &output, const QPointF &local, const QSizeF &size)
{
    return sendToHost(output, u"HostCursor"_s, {output, local.x(), local.y(), size.width(), size.height()});
}

bool SurfaceService::sendHostCursorLeft(const QString &output)
{
    return sendToHost(output, u"HostCursorLeft"_s, {output});
}

// Not a broadcast: the other hosts (and everything else on the bus) have no use for it and cursor signals are
// frequent.
bool SurfaceService::sendToHost(const QString &output, const QString &signal, const QVariantList &arguments)
{
    const auto owner = m_owners.constFind(output);
    if (owner == m_owners.constEnd() || m_hidden.contains(output))
        return false;
    QDBusMessage message = QDBusMessage::createTargetedSignal(*owner, dbusPath, QString(kSurfaceDBusInterface), signal);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().send(message);
}

bool SurfaceService::sendHostCommand(const QString &output, const QString &command)
{
    return sendToHost(output, u"HostCommand"_s, {output, command});
}

// An empty or oversized name is refused (with an error reply for bus callers).
bool SurfaceService::rejectOutput(const QString &output) const
{
    if (!output.isEmpty() && output.size() <= kMaxOutputNameSize)
        return false;
    if (calledFromDBus())
        const_cast<SurfaceService *>(this)->sendErrorReply(QDBusError::InvalidArgs, u"invalid output name"_s);
    return true;
}

QString SurfaceService::hostStateJson(const QString &output) const
{
    return QString::fromUtf8(
        QJsonDocument(hostStateProvider ? hostStateProvider(output) : QJsonObject()).toJson(QJsonDocument::Compact));
}

void SurfaceService::RegisterHost(const QString &output)
{
    // The caller's unique bus name is the registration's identity; a direct (in-process) call has none.
    if (!calledFromDBus() || rejectOutput(output))
        return;
    const QString caller = message().service();
    const auto existing = m_owners.constFind(output);
    if (existing != m_owners.constEnd()) {
        if (*existing == caller)
            return; // twice from the same name is harmless
        // One host per output: a second one would draw the same screen twice. The newcomer is told so and retries
        // (a restarted host usually registers before the bus has dropped its predecessor's name).
        sendErrorReply(QDBusError::AccessDenied, u"output %1 is already hosted by %2"_s.arg(output, *existing));
        return;
    }
    if (m_owners.size() >= kMaxHostedOutputs) {
        sendErrorReply(QDBusError::LimitsExceeded, u"too many hosted outputs"_s);
        return;
    }
    if (std::count(m_owners.cbegin(), m_owners.cend(), caller) >= kMaxOutputsPerCaller) {
        sendErrorReply(QDBusError::LimitsExceeded, u"too many outputs hosted by one connection"_s);
        return;
    }
    m_owners.insert(output, caller);
    watchOwner(caller); // drops the registration again when the caller is already gone
    if (m_owners.value(output) != caller)
        return;
    emit hostsChanged(output);
    m_lastHostState.insert(output, hostStateJson(output));
}

void SurfaceService::ReportOutputHealth(const QString &output, const QString &json)
{
    if (!calledFromDBus() || rejectOutput(output) || json.size() > kMaxReportSize)
        return;
    if (m_owners.value(output) != message().service())
        return; // only the host of the output speaks for it
    QJsonObject failure;
    if (!json.isEmpty()) {
        failure = QJsonDocument::fromJson(json.toUtf8()).object();
        if (failure.value(u"kind"_s).toString().isEmpty())
            return;
    }
    if (failure.isEmpty() ? m_hostFailures.remove(output) == 0 : m_hostFailures.value(output) == failure)
        return;
    if (!failure.isEmpty())
        m_hostFailures.insert(output, failure);
    notifyOutputsChanged();
}

void SurfaceService::RetryOutput(const QString &output)
{
    if (rejectOutput(output))
        return;
    emit retryRequested(output);
}


void SurfaceService::UnregisterHost(const QString &output)
{
    if (!calledFromDBus() || rejectOutput(output))
        return;
    removeOwner(output, message().service());
}

void SurfaceService::SetHostVisible(const QString &output, bool visible)
{
    if (!calledFromDBus() || rejectOutput(output))
        return;
    if (m_owners.value(output) != message().service())
        return; // not registered by this caller: ignored
    if (visible == !m_hidden.contains(output))
        return;
    if (visible)
        m_hidden.remove(output);
    else
        m_hidden.insert(output);
    emit hostsChanged(output);
}

QString SurfaceService::HostState(const QString &output) const
{
    if (rejectOutput(output))
        return {};
    return hostStateJson(output);
}

void SurfaceService::notifyHostStates()
{
    const QStringList outputs = m_owners.keys();
    for (const QString &output : outputs) {
        const auto last = m_lastHostState.constFind(output);
        if (last == m_lastHostState.constEnd())
            continue; // registration still in progress; RegisterHost() records the baseline
        const QString current = hostStateJson(output);
        if (*last == current)
            continue;
        m_lastHostState.insert(output, current);
        emit HostStateChanged(output);
    }
}

void SurfaceService::watchOwner(const QString &name)
{
    if (!m_watcher) {
        m_watcher = new QDBusServiceWatcher(QString(), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForUnregistration, this);
        connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &SurfaceService::dropOwner);
    }
    m_watcher->addWatchedService(name);
    // The name may have left before the watch was in place.
    const QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (bus && !bus->isServiceRegistered(name))
        dropOwner(name);
}

// A bus name left: all its registrations go.
void SurfaceService::dropOwner(const QString &name)
{
    const QStringList outputs = m_owners.keys(name);
    for (const QString &output : outputs)
        removeOwner(output, name);
}

// Removes the registration if `name` holds it. The hidden flag belongs to the registration, so it goes with it.
void SurfaceService::removeOwner(const QString &output, const QString &name)
{
    const auto it = m_owners.find(output);
    if (it == m_owners.end() || *it != name)
        return;
    m_owners.erase(it);
    m_hidden.remove(output);
    m_lastHostState.remove(output);
    if (m_hostFailures.remove(output))
        notifyOutputsChanged();
    if (m_watcher && !std::count(m_owners.cbegin(), m_owners.cend(), name))
        m_watcher->removeWatchedService(name);
    emit hostsChanged(output);
}

} // namespace murasaki
