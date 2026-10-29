#include "interactcontroller.h"

#include "constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

InteractPointer clampedInteractPointer(InteractPointer pointer)
{
    const auto clampAxis = [](double value, double extent) {
        if (!std::isfinite(value))
            return 0.0;
        return extent > 0 ? std::clamp(value, 0.0, extent) : value;
    };
    pointer.position = QPointF(clampAxis(pointer.position.x(), pointer.size.width()), clampAxis(pointer.position.y(), pointer.size.height()));
    pointer.buttons &= kInteractLeft | kInteractRight | kInteractMiddle;
    return pointer;
}

InteractController::InteractController(DBusBackend *daemon, DBusBackend *surface, SurfaceFactory factory, QObject *parent)
    : QObject(parent)
    , m_daemon(daemon)
    , m_surface(surface)
    , m_factory(std::move(factory))
{
    connect(m_daemon, &DBusBackend::availabilityChanged, this, &InteractController::onDaemonAvailability);
    connect(m_daemon, &DBusBackend::signalReceived, this, &InteractController::onDaemonSignal);
    connect(m_surface, &DBusBackend::availabilityChanged, this, &InteractController::onSurfaceAvailability);
    connect(m_surface, &DBusBackend::signalReceived, this, [this](const QString &name, const QVariantList &) { onSurfaceSignal(name); });
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        if (m_interact)
            requestOff("no pointer input");
    });
    m_hardCap.setSingleShot(true);
    connect(&m_hardCap, &QTimer::timeout, this, [this] {
        if (m_interact)
            requestOff("the time limit");
    });
    m_grace.setSingleShot(true);
    connect(&m_grace, &QTimer::timeout, this, [this] {
        if (m_interact && m_catchers.empty())
            requestOff("no interactive wallpaper on the screen");
    });
}

InteractController::~InteractController()
{
    dropAllCatchers();
}

QStringList InteractController::catcherOutputs() const
{
    QStringList names;
    for (const auto &[name, catcher] : m_catchers)
        names << name;
    return names;
}

void InteractController::onShortcut(const QString &actionId)
{
    if (actionId == u"toggle-interact"_s)
        toggle();
}

void InteractController::toggle()
{
    if (!m_daemonUp) {
        qWarning("murasaki-overlay: interact with the wallpaper: the daemon is not there, nothing was done");
        return;
    }
    if (!m_interact && m_sessionPaused)
        return;
    m_daemon->call(u"SetWallpaperInteract"_s, {!m_interact}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki-overlay: SetWallpaperInteract: %s", qPrintable(reply.message));
    });
}

void InteractController::setSessionPaused(bool paused)
{
    m_sessionPaused = paused;
    if (paused && m_interact)
        requestOff("the session is locked");
}

void InteractController::onDaemonAvailability(bool available)
{
    m_daemonUp = available;
    if (!available) {
        // The mode is the daemon's session state, which starts again with it off.
        setInteract(false);
        return;
    }
    // Read again on every connection: a daemon that restarted has it off, and a property has no signal for that. A daemon older than
    // revision 13 has none (the call fails): off.
    m_daemon->property(u"WallpaperInteract"_s, [this](const DBusBackend::Reply &reply) {
        const bool value = reply.ok && reply.value.toBool();
        if (value && m_sessionPaused)
            requestOff("the session is locked");
        else
            setInteract(value);
    });
}

void InteractController::onDaemonSignal(const QString &name, const QVariantList &args)
{
    if (name == u"WallpaperInteractChanged"_s && !args.isEmpty()) {
        const bool value = args.constFirst().toBool();
        if (value && m_sessionPaused)
            requestOff("the session is locked");
        else
            setInteract(value);
    } else if (name == u"PetsArrangeChanged"_s && !args.isEmpty() && args.constFirst().toBool()) {
        // Arranging wins; the daemon turns the mode off too, this only saves the window a round trip.
        setInteract(false);
    }
}

void InteractController::onSurfaceAvailability(bool available)
{
    m_surfaceUp = available;
    if (!available) {
        dropAllCatchers();
        if (m_interact)
            requestOff("the surface is gone");
        return;
    }
    refreshOutputs();
}

void InteractController::onSurfaceSignal(const QString &name)
{
    if (name == u"OutputsChanged"_s)
        refreshOutputs();
}

void InteractController::setInteract(bool interact)
{
    if (interact == m_interact)
        return;
    m_interact = interact;
    if (!interact) {
        ++m_refreshSeq;
        m_idle.stop();
        m_hardCap.stop();
        m_grace.stop();
        dropAllCatchers();
        return;
    }
    m_hardCap.start(m_hardCapMs); // (not restarted by input: a window nobody can close does not stay for ever)
    touchIdle();
    refreshOutputs();
}

void InteractController::touchIdle()
{
    if (m_interact)
        m_idle.start(m_idleMs);
}

void InteractController::requestOff(const char *why)
{
    qInfo("murasaki-overlay: interaction with the wallpaper ends: %s", why);
    setInteract(false); // (at once: the releases of buttons still down reach the hosts, and the windows go away)
    if (!m_daemonUp)
        return;
    m_daemon->call(u"SetWallpaperInteract"_s, {false}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki-overlay: SetWallpaperInteract: %s", qPrintable(reply.message));
    });
}

void InteractController::refreshOutputs()
{
    if (!m_interact || !m_surfaceUp)
        return;
    const quint64 seq = ++m_refreshSeq;
    m_surface->call(u"Outputs"_s, {}, [this, seq](const DBusBackend::Reply &reply) {
        if (seq != m_refreshSeq || !m_interact)
            return; // a newer answer is on its way, or the mode ended
        QSet<QString> wanted;
        if (reply.ok) {
            const QJsonArray outputs = QJsonDocument::fromJson(reply.value.toString().toUtf8()).array();
            for (const QJsonValue &value : outputs) {
                const QJsonObject output = value.toObject();
                // Only a hosted output needs a catcher (a window of the surface's own gets the pointer itself), and only one whose wallpaper takes it.
                if (output.value(u"hosted"_s).toBool() && output.value(u"interactive"_s).toBool())
                    wanted.insert(output.value(u"name"_s).toString());
            }
        }
        applyOutputs(wanted);
        if (!m_catchers.empty()) {
            m_grace.stop();
        } else if (m_graceMs <= 0) {
            requestOff("no interactive wallpaper on the screen");
        } else if (!m_grace.isActive()) {
            m_grace.start(m_graceMs); // a wallpaper change shows no interactive output for a moment
        }
    });
}

void InteractController::applyOutputs(const QSet<QString> &wanted)
{
    const QStringList have = catcherOutputs();
    for (const QString &name : have) {
        if (!wanted.contains(name))
            dropCatcher(name);
    }
    for (const QString &name : wanted) {
        if (m_catchers.count(name) != 0 || name.isEmpty())
            continue;
        std::unique_ptr<InteractSurface> surface = m_factory(name);
        if (!surface)
            continue; // no screen of that name here
        surface->onPointer = [this, name](const InteractPointer &pointer) { onCatcherPointer(name, pointer); };
        surface->onEscape = [this] {
            if (m_interact)
                requestOff("Esc");
        };
        Catcher &catcher = m_catchers[name];
        catcher.surface = std::move(surface);
        catcher.flush = std::make_unique<QTimer>();
        catcher.flush->setSingleShot(true);
        connect(catcher.flush.get(), &QTimer::timeout, this, [this, name] { flushPending(name); });
    }
}

void InteractController::dropCatcher(const QString &output)
{
    const auto it = m_catchers.find(output);
    if (it == m_catchers.end())
        return;
    // A button still down when the window goes would stay down in the page: send the release where the pointer was.
    flushPending(output);
    Catcher &catcher = it->second;
    if (catcher.sentAny && catcher.last.buttons != 0) {
        InteractPointer release = catcher.last;
        release.buttons = 0;
        release.wheel = QPoint();
        send(output, release);
    }
    catcher.surface->onPointer = nullptr;
    catcher.surface->onEscape = nullptr;
    m_catchers.erase(it);
}

void InteractController::dropAllCatchers()
{
    for (const QString &name : catcherOutputs())
        dropCatcher(name);
}

void InteractController::onCatcherPointer(const QString &output, const InteractPointer &raw)
{
    const auto it = m_catchers.find(output);
    if (it == m_catchers.end() || !m_interact)
        return;
    Catcher &catcher = it->second;
    const InteractPointer pointer = clampedInteractPointer(raw);
    touchIdle();
    const bool moveOnly = catcher.sentAny && pointer.buttons == catcher.last.buttons && pointer.wheel.isNull();
    if (!moveOnly) {
        flushPending(output); // (the order of events on the wire is the order the user made them)
        send(output, pointer);
        return;
    }
    if (pointer == catcher.last)
        return;
    if (catcher.sinceSend.isValid() && catcher.sinceSend.elapsed() < kMoveIntervalMs) {
        catcher.pending = pointer;
        catcher.hasPending = true;
        if (!catcher.flush->isActive())
            catcher.flush->start(kMoveIntervalMs - int(catcher.sinceSend.elapsed()));
        return;
    }
    send(output, pointer);
}

void InteractController::flushPending(const QString &output)
{
    const auto it = m_catchers.find(output);
    if (it == m_catchers.end())
        return;
    Catcher &catcher = it->second;
    catcher.flush->stop();
    if (!catcher.hasPending)
        return;
    catcher.hasPending = false;
    send(output, catcher.pending);
}

void InteractController::send(const QString &output, const InteractPointer &pointer)
{
    const auto it = m_catchers.find(output);
    if (it == m_catchers.end())
        return;
    Catcher &catcher = it->second;
    catcher.hasPending = false;
    catcher.last = pointer;
    catcher.last.wheel = QPoint(); // (a wheel turn is an event, not a state)
    catcher.sentAny = true;
    catcher.sinceSend.start();
    if (!m_surfaceUp)
        return;
    m_surface->call(u"ReportHostPointer"_s,
                    {output, pointer.position.x(), pointer.position.y(), pointer.size.width(), pointer.size.height(), uint(pointer.buttons),
                     pointer.wheel.x(), pointer.wheel.y()},
                    [](const DBusBackend::Reply &) {});
}

} // namespace murasaki
