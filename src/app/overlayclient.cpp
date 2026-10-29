#include "overlayclient.h"

#include "constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace murasaki {

OverlayClient::OverlayClient(DBusBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_backend->setParent(this);
    connect(m_backend, &DBusBackend::availabilityChanged, this, &OverlayClient::onAvailability);
    connect(m_backend, &DBusBackend::signalReceived, this, [this](const QString &name) {
        if (name == u"WindowsChanged"_s)
            refreshWindows();
        else if (name == u"ShortcutsChanged"_s)
            refresh(); // the shortcuts finished registering (the portal may have waited for the user), or the user gave one a key
    });
}

OverlayClient::OverlayClient(QObject *parent)
    : OverlayClient(new SessionBusBackend(kOverlayDBusService, kOverlayDBusPath, kOverlayDBusInterface, {u"WindowsChanged"_s, u"ShortcutsChanged"_s}), parent)
{
}

QString OverlayClient::defaultShortcut() const
{
    return QString(kArrangeShortcutText);
}

void OverlayClient::start()
{
    m_backend->start();
}

void OverlayClient::onAvailability(bool up)
{
    if (up == m_available)
        return;
    m_available = up;
    ++m_generation; // an answer of the previous process is not about this one
    emit availableChanged();
    if (up)
        refresh();
    else {
        setShortcutBackend({});
        setShortcutTriggers({});
        setShortcutState({});
        setWindows({});
    }
}

void OverlayClient::setWindows(const QVariantList &windows)
{
    if (windows == m_windows)
        return;
    m_windows = windows;
    emit windowsChanged();
}

void OverlayClient::refreshWindows()
{
    if (!m_available)
        return;
    const quint64 generation = m_generation;
    m_backend->call(u"Windows"_s, {}, [this, generation](const DBusBackend::Reply &reply) {
        if (generation != m_generation)
            return;
        QVariantList windows;
        if (reply.ok) {
            const QJsonArray array = QJsonDocument::fromJson(reply.value.toString().toUtf8()).array();
            for (const QJsonValue &value : array)
                windows.append(value.toObject().toVariantMap());
        }
        setWindows(windows);
    });
}

void OverlayClient::resetFailures(const QString &petId)
{
    if (!m_available)
        return;
    m_backend->call(u"ResetFailures"_s, {petId}, [this](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki: ResetFailures: %s", qPrintable(reply.message));
        refreshWindows();
    });
}

void OverlayClient::playPetSound(const QString &petId)
{
    if (!m_available)
        return;
    m_backend->call(u"PlayPetSound"_s, {petId}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki: PlayPetSound: %s", qPrintable(reply.message));
    });
}

void OverlayClient::previewPetState(const QString &petId, const QString &state)
{
    if (!m_available)
        return;
    m_backend->call(u"PreviewPetState"_s, {petId, state}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki: PreviewPetState: %s", qPrintable(reply.message));
    });
}

void OverlayClient::stopPetSounds()
{
    if (m_available)
        m_backend->call(u"StopPetSounds"_s, {}, [](const DBusBackend::Reply &) {});
}

void OverlayClient::setShortcutBackend(const QString &backend)
{
    if (backend == m_shortcutBackend)
        return;
    m_shortcutBackend = backend;
    emit shortcutBackendChanged();
}

void OverlayClient::setShortcutState(const QString &state)
{
    if (state == m_shortcutState)
        return;
    m_shortcutState = state;
    emit shortcutStateChanged();
}

void OverlayClient::setShortcutTriggers(const QVariantMap &triggers)
{
    if (triggers == m_shortcutTriggers)
        return;
    m_shortcutTriggers = triggers;
    emit shortcutTriggersChanged();
}

void OverlayClient::refresh()
{
    if (!m_available)
        return;
    const quint64 generation = m_generation;
    m_backend->property(u"ShortcutBackend"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation == m_generation)
            setShortcutBackend(reply.ok ? reply.value.toString() : QString());
    });
    m_backend->property(u"ShortcutState"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation == m_generation)
            setShortcutState(reply.ok ? reply.value.toString() : QString()); // an overlay older than revision 12 has none
    });
    m_backend->property(u"ShortcutTriggers"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation != m_generation)
            return;
        // an overlay older than revision 12 has none
        const QJsonObject object = QJsonDocument::fromJson(reply.ok ? reply.value.toString().toUtf8() : QByteArray()).object();
        setShortcutTriggers(object.toVariantMap());
    });
    refreshWindows();
}

} // namespace murasaki
