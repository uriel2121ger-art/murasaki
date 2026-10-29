// The automation calls of DaemonClient (API revision 18, docs/pets/automation.md).

#include "daemonclient.h"

#include <QDBusArgument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QJsonDocument parseText(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8());
}

QString jsonText(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

} // namespace

bool DaemonClient::requireAutomation()
{
    if (automationSupported())
        return true;
    emit error(tr("Profiles and triggers need a newer Murasaki service; update and restart it."));
    return false;
}

void DaemonClient::clearAutomation()
{
    const bool had = !m_profiles.isEmpty() || !m_activeProfile.isEmpty() || m_profileModified;
    m_profiles.clear();
    m_activeProfile.clear();
    m_profileModified = false;
    if (had)
        emit profilesChanged();
    if (!m_lastSwitchReport.isEmpty()) {
        m_lastSwitchReport.clear();
        emit lastSwitchReportChanged();
    }
    if (!m_triggers.isEmpty()) {
        m_triggers.clear();
        emit triggersChanged();
    }
    if (!m_triggerStates.isEmpty()) {
        m_triggerStates.clear();
        emit triggerStatesChanged();
    }
    clearScreen();
    if (!m_timelines.isEmpty() || !m_runningTimelines.isEmpty()) {
        m_timelines.clear();
        m_runningTimelines.clear();
        emit timelinesChanged();
    }
}

void DaemonClient::refreshAutomation()
{
    if (!automationSupported())
        return;
    refreshProfiles();
    refreshTriggers();
    refreshTimelines();
}

void DaemonClient::refreshProfiles()
{
    if (!automationSupported())
        return;
    call(u"ListProfiles"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parseText(value);
        if (!doc.isArray())
            return;
        const QVariantList list = doc.array().toVariantList();
        const quint64 generation = m_generation;
        m_backend->property(u"ActiveProfile"_s, [this, list, generation](const DBusBackend::Reply &active) {
            if (generation != m_generation)
                return;
            m_backend->property(u"ProfileModified"_s, [this, list, generation, active](const DBusBackend::Reply &modified) {
                if (generation != m_generation)
                    return;
                const QString activeId = active.ok ? active.value.toString() : QString();
                const bool isModified = modified.ok && modified.value.toBool();
                if (list == m_profiles && activeId == m_activeProfile && isModified == m_profileModified)
                    return;
                m_profiles = list;
                m_activeProfile = activeId;
                m_profileModified = isModified;
                emit profilesChanged();
            });
        });
    });
}

void DaemonClient::refreshTriggers()
{
    if (!automationSupported())
        return;
    call(u"ListTriggers"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parseText(value);
        if (doc.isArray() && doc.array().toVariantList() != m_triggers) {
            m_triggers = doc.array().toVariantList();
            emit triggersChanged();
        }
    });
    call(u"TriggerState"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parseText(value);
        if (doc.isObject() && doc.object().toVariantMap() != m_triggerStates) {
            m_triggerStates = doc.object().toVariantMap();
            emit triggerStatesChanged();
        }
    });
}

void DaemonClient::refreshTimelines()
{
    if (!automationSupported())
        return;
    call(u"ListTimelines"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = parseText(value);
        if (!doc.isArray())
            return;
        const QVariantList list = doc.array().toVariantList();
        call(u"RunningTimelines"_s, {}, [this, list](const QVariant &running) {
            QStringList ids;
            if (running.metaType() == QMetaType::fromType<QDBusArgument>())
                ids = qdbus_cast<QStringList>(running);
            else
                ids = running.toStringList();
            if (list == m_timelines && ids == m_runningTimelines)
                return;
            m_timelines = list;
            m_runningTimelines = ids;
            emit timelinesChanged();
        });
    });
}

void DaemonClient::saveProfile(const QString &name)
{
    if (requireAutomation())
        call(u"SaveProfile"_s, {name}, [this, name](const QVariant &id) { emit profileSaved(name, id.toString()); }, {}, Ungated);
}

void DaemonClient::switchProfile(const QString &idOrName)
{
    if (!requireAutomation())
        return;
    call(
        u"SwitchProfile"_s, {idOrName},
        [this](const QVariant &value) {
            m_lastSwitchReport = parseText(value).object().toVariantMap();
            emit lastSwitchReportChanged();
            emit profileSwitched(m_lastSwitchReport);
        },
        {}, Ungated);
}

void DaemonClient::renameProfile(const QString &idOrName, const QString &newName)
{
    if (requireAutomation())
        call(u"RenameProfile"_s, {idOrName, newName}, {}, {}, Ungated);
}

void DaemonClient::deleteProfile(const QString &idOrName)
{
    if (requireAutomation())
        call(u"DeleteProfile"_s, {idOrName}, {}, {}, Ungated);
}

void DaemonClient::setTrigger(const QString &triggerId, const QVariantMap &changes)
{
    if (requireAutomation())
        call(u"SetTrigger"_s, {triggerId, jsonText(changes)}, [this](const QVariant &id) { emit triggerSaved(id.toString()); }, {}, Ungated);
}

void DaemonClient::deleteTrigger(const QString &triggerId)
{
    if (requireAutomation())
        call(u"DeleteTrigger"_s, {triggerId}, {}, {}, Ungated);
}

void DaemonClient::setTriggerEnabled(const QString &triggerId, bool enabled)
{
    if (requireAutomation())
        call(u"SetTriggerEnabled"_s, {triggerId, enabled}, {}, {}, Ungated);
}

void DaemonClient::fireTrigger(const QString &name)
{
    if (requireAutomation())
        call(u"FireTrigger"_s, {name}, {}, {}, Ungated);
}

void DaemonClient::testTrigger(const QString &triggerId)
{
    if (requireAutomation())
        call(u"TestTrigger"_s, {triggerId}, [this, triggerId](const QVariant &ran) { emit triggerTested(triggerId, ran.toBool()); }, {}, Ungated);
}

void DaemonClient::setTimeline(const QString &timelineId, const QVariantMap &changes)
{
    if (requireAutomation())
        call(u"SetTimeline"_s, {timelineId, jsonText(changes)}, [this](const QVariant &id) { emit timelineSaved(id.toString()); }, {}, Ungated);
}

void DaemonClient::deleteTimeline(const QString &idOrName)
{
    if (requireAutomation())
        call(u"DeleteTimeline"_s, {idOrName}, {}, {}, Ungated);
}

void DaemonClient::runTimeline(const QString &idOrName)
{
    if (requireAutomation())
        call(u"RunTimeline"_s, {idOrName}, {}, {}, Ungated);
}

void DaemonClient::stopTimeline(const QString &idOrName)
{
    if (requireAutomation())
        call(u"StopTimeline"_s, {idOrName}, {}, {}, Ungated);
}

void DaemonClient::timelineFromScene(const QString &itemId, int index)
{
    if (requireAutomation())
        call(
            u"TimelineFromScene"_s, {itemId, index},
            [this](const QVariant &id) { emit packAutomationMade(QVariantMap{{u"timeline"_s, id.toString()}, {u"triggers"_s, QVariantList()}, {u"notes"_s, QVariantList()}}); }, {},
            Ungated);
}

void DaemonClient::importPackAutomation(const QString &itemId)
{
    if (requireAutomation())
        call(u"ImportPackAutomation"_s, {itemId}, [this](const QVariant &value) { emit packAutomationMade(parseText(value).object().toVariantMap()); }, {}, Ungated);
}

void DaemonClient::resumeAutomation()
{
    if (requireAutomation())
        call(u"ResumeAutomation"_s, {}, {}, {}, Ungated);
}

void DaemonClient::clearPetRuntime(const QString &petId)
{
    if (requireAutomation())
        call(u"ClearPetRuntime"_s, {petId}, {}, {}, Ungated);
}

} // namespace murasaki
