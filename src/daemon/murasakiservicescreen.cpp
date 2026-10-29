// The screen trigger calls of MurasakiService (revision 21, docs/pets/automation.md "Screen triggers"): starting and stopping the watch, its state, and the explicit capture of a reference
// picture. The work is ScreenWatch's (screenwatch.h); this file is the bus facade and the wiring to the automation.

#include "murasakiservice.h"

#include "automationmanager.h"
#include "automationstore.h"
#include "constants.h"
#include "screenrefstore.h"
#include "screenwatch.h"

#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>

#include <time.h>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

QString text(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

qint64 monotonicMs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// One notification when watching starts ("Murasaki is watching N regions of your screen"); a desktop without the service simply shows none.
void notifyWatching(const QDBusConnection &bus, const QString &body)
{
    QDBusMessage message = QDBusMessage::createMethodCall(u"org.freedesktop.Notifications"_s, u"/org/freedesktop/Notifications"_s, u"org.freedesktop.Notifications"_s, u"Notify"_s);
    message.setAutoStartService(false);
    message.setArguments({u"Murasaki"_s, uint(0), u"view-preview"_s, u"Murasaki"_s, body, QStringList(), QVariantMap{{u"urgency"_s, QVariant::fromValue(uchar(1))}}, 8000});
    QDBusConnection(bus).asyncCall(message, 2000);
}

} // namespace

void MurasakiService::attachScreen(ScreenRefStore *refs, const QString &helper, const QString &sandbox, bool requireSandbox, const QString &crashDir, const QString &statePath,
                                   const QDBusConnection &bus)
{
    if (!m_automation || !m_triggerStore)
        return;
    m_screenRefs = refs;
    ScreenWatch::Config config;
    config.helperPath = helper;
    config.sandboxPath = sandbox;
    config.requireSandbox = requireSandbox;
    config.crashDir = crashDir;
    config.statePath = statePath;
    config.bus = bus;
    ScreenWatch::Hooks hooks;
    hooks.nowMs = monotonicMs;
    // Our own pets, where they are on the desktop now (the runtime layer on top of the stored place), for the regions they cover.
    hooks.petRects = [this]() -> QList<screenmatch::LogicalRect> {
        QList<screenmatch::LogicalRect> rects;
        if (m_petsHidden)
            return rects;
        const QHash<QString, PetRuntime> layer = m_automation->layer();
        for (const PetPlacement &pet : m_pets->pets()) {
            const PetRuntime runtime = layer.value(pet.petId);
            if (runtime.hidden)
                continue;
            const auto item = m_library->item(pet.itemId);
            if (!item || item->variants.isEmpty())
                continue;
            const PetVariant *chosen = &item->variants.first();
            for (const PetVariant &v : item->variants)
                if (v.name == (runtime.variant.isEmpty() ? pet.variant : runtime.variant))
                    chosen = &v;
            if (chosen->width <= 0 || chosen->height <= 0)
                continue;
            const PetPosition at = effectivePosition(runtime, PetPosition{pet.x, pet.y}, monotonicMs());
            // Where the pet's output is: unknown means the pet is left out of the mask (a wrong place would hide the wrong part of a region).
            QPoint origin;
            bool known = false;
            const QString output = runtime.output.isEmpty() ? pet.output : runtime.output;
            if (const auto info = m_outputInfo.constFind(output); info != m_outputInfo.constEnd() && info->rect.isValid()) {
                origin = info->rect.topLeft();
                known = true;
            } else if (output == u"*") {
                for (const OutputInfo &o : std::as_const(m_outputInfo))
                    if (o.primary && o.rect.isValid()) {
                        origin = o.rect.topLeft();
                        known = true;
                    }
            }
            if (!known)
                continue;
            rects.append({origin.x() + at.x, origin.y() + at.y, pet.width, pet.width * chosen->height / chosen->width});
        }
        return rects;
    };
    hooks.scaleOf = [this](int x, int y, int width, int height) -> double {
        for (const OutputInfo &o : std::as_const(m_outputInfo))
            if (o.rect.isValid() && o.rect.contains(QPoint(x + width / 2, y + height / 2)))
                return o.scale;
        // One output known: its scale. None told yet: 1, as before. Several and none holds the monitor: not known (0), so the regions on it are named, not mapped wrongly.
        if (m_outputInfo.isEmpty())
            return 1.0;
        return m_outputInfo.size() == 1 ? m_outputInfo.cbegin()->scale : 0.0;
    };
    hooks.actionRunning = [this](const QString &triggerId) {
        const auto trigger = m_triggerStore->trigger(triggerId);
        return trigger && trigger->action.type == TriggerActionType::Timeline && m_automation->runningTimelines().contains(trigger->action.id);
    };
    m_screenBus = bus;
    hooks.notify = [bus](const QString &body) { notifyWatching(bus, body); };
    m_screenWatch = new ScreenWatch(m_triggerStore, m_settings, refs, config, hooks, this);

    connect(m_screenWatch, &ScreenWatch::activated, this, [this](const QString &id) { m_automation->screenActive(id, true); });
    connect(m_screenWatch, &ScreenWatch::deactivated, this, [this](const QString &id) { m_automation->screenActive(id, false); });
    connect(m_screenWatch, &ScreenWatch::changed, this, &MurasakiService::ScreenWatchChanged);
    // A capture holds pixels of the screen: it goes to the connection that asked, as a unicast signal, and to nobody else.
    connect(m_screenWatch, &ScreenWatch::captured, this, [this](const QString &request, const QString &json) {
        if (request != m_captureRequest || m_captureOwner.isEmpty())
            return;
        QDBusMessage signal = QDBusMessage::createTargetedSignal(m_captureOwner, QString(kDBusPath), QString(kDBusInterface), u"ScreenCaptured"_s);
        signal << request << json;
        m_screenBus.send(signal);
    });
    connect(m_automation, &AutomationManager::lockedChanged, m_screenWatch, &ScreenWatch::setLocked);
    connect(m_automation, &AutomationManager::batteryChanged, m_screenWatch, &ScreenWatch::setOnBattery);
    connect(m_automation, &AutomationManager::rescued, m_screenWatch, [this] { m_screenWatch->stop(u"rescue"_s); });
    connect(m_automation, &AutomationManager::runtimeChanged, m_screenWatch, &ScreenWatch::petsMoved);
    connect(this, &MurasakiService::PetsChanged, m_screenWatch, &ScreenWatch::petsMoved);
    connect(m_settings, &SettingsStore::changed, m_screenWatch, &ScreenWatch::settingsChanged);
    // A deleted screen trigger takes its reference pictures with it.
    auto known = std::make_shared<QSet<QString>>();
    for (const Trigger &t : m_triggerStore->triggers())
        known->insert(t.id);
    connect(m_triggerStore, &TriggerStore::changed, m_screenWatch, [this, known] {
        QSet<QString> now;
        for (const Trigger &t : m_triggerStore->triggers())
            now.insert(t.id);
        for (const QString &id : std::as_const(*known))
            if (!now.contains(id) && m_screenRefs)
                m_screenRefs->removeTrigger(id);
        *known = now;
        m_screenWatch->triggersChanged();
    });
    m_screenWatch->setLocked(m_automation->locked());
    m_screenWatch->setOnBattery(m_automation->onBattery());
    if (m_settings->automationSettings().screenWatchAtLogin)
        QTimer::singleShot(0, m_screenWatch, &ScreenWatch::startAtLogin);
}

QString MurasakiService::screenWatchState() const
{
    return m_screenWatch ? text(m_screenWatch->stateJson()) : u"{\"state\":\"off\",\"reason\":\"\",\"regions\":0,\"ticksPerSecond\":0,\"since\":0,\"paused\":false,\"notes\":[]}"_s;
}

void MurasakiService::StartScreenWatch()
{
    QString problem;
    if (!m_screenWatch)
        rejectCall(u"screen triggers are not available"_s);
    else if (!m_screenWatch->start(&problem))
        rejectCall(problem);
}

void MurasakiService::StartScreenWatchLeased()
{
    const bool running = m_screenWatch && (m_screenWatch->state() == ScreenWatch::State::Asking || m_screenWatch->state() == ScreenWatch::State::Watching);
    StartScreenWatch();
    // (a refused start was rejected above; a watch that was already running is the user's, not this caller's to end)
    if (m_screenWatch && !running && calledFromDBus())
        m_screenWatch->lease(message().service());
}

void MurasakiService::StopScreenWatch()
{
    if (m_screenWatch)
        m_screenWatch->stop(); // (also ends a lease)
}

QString MurasakiService::CaptureScreenReference(const QString &triggerId, int region, int steps)
{
    QString problem;
    if (!m_screenWatch) {
        rejectCall(u"screen triggers are not available"_s);
        return {};
    }
    const QString id = m_screenWatch->capture(triggerId, region, steps, &problem);
    if (id.isEmpty()) {
        rejectCall(problem);
        return id;
    }
    m_captureRequest = id;
    m_captureOwner = calledFromDBus() ? message().service() : QString();
    return id;
}

void MurasakiService::DiscardScreenReference(const QString &request)
{
    if (!m_screenWatch)
        rejectCall(u"screen triggers are not available"_s);
    else if (calledFromDBus() && !m_captureOwner.isEmpty() && request == m_captureRequest && message().service() != m_captureOwner)
        rejectCall(u"that capture belongs to another client"_s);
    else if (!m_screenWatch->discardCapture(request))
        rejectCall(u"no such capture"_s);
}

void MurasakiService::CommitScreenReference(const QString &request)
{
    QString problem;
    if (!m_screenWatch)
        rejectCall(u"screen triggers are not available"_s);
    else if (calledFromDBus() && !m_captureOwner.isEmpty() && request == m_captureRequest && message().service() != m_captureOwner)
        rejectCall(u"that capture belongs to another client"_s);
    else if (!m_screenWatch->commitCapture(request, &problem))
        rejectCall(problem);
}

} // namespace murasaki
