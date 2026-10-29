#include "overlayservice.h"

#include "petaudiocontroller.h"

#include "overlaycontroller.h"

#include <QDBusError>
#include <QJsonDocument>

namespace murasaki {

OverlayService::OverlayService(OverlayController *controller, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
{
    m_lastWindows = Windows();
    connect(m_controller, &OverlayController::stateChanged, this, [this] {
        const QString now = Windows();
        if (now == m_lastWindows)
            return;
        m_lastWindows = now;
        emit WindowsChanged();
    });
}

bool OverlayService::sessionPaused() const
{
    return m_controller->sessionPaused();
}

QString OverlayService::Windows() const
{
    return QString::fromUtf8(QJsonDocument(m_controller->describe()).toJson(QJsonDocument::Compact));
}

void OverlayService::ToggleArrange()
{
    m_controller->toggleArrange();
}

void OverlayService::ResetFailures(const QString &petId)
{
    if (!m_controller->resetFailures(petId))
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no such pet"));
}

void OverlayService::PlayPetSound(const QString &petId)
{
    if (!m_controller->audio() || !m_controller->audio()->playNow(petId))
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no such pet, or it has no sound"));
}

void OverlayService::PreviewPetState(const QString &petId, const QString &state)
{
    if (!m_controller->previewState(petId, state))
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no such pet, or it has no states or is not shown, or the state is not idle, grabbed, moving or released"));
}

void OverlayService::StopPetSounds()
{
    if (m_controller->audio())
        m_controller->audio()->stopAll();
}

void OverlayService::StopOneShotPetSounds()
{
    if (m_controller->audio())
        m_controller->audio()->stopOneShots();
}

void OverlayService::SimulateEdit(const QString &petId, const QString &kind, double a, double b)
{
    if (qEnvironmentVariableIntValue("MURASAKI_OVERLAY_TEST_HOOKS") != 1) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("test hooks are disabled"));
        return;
    }
    if (!m_controller->simulateEdit(petId, kind, a, b))
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no such pet window or edit kind"));
}

void OverlayService::Quit()
{
    if (quitHandler)
        quitHandler();
}

} // namespace murasaki
