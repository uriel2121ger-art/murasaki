#include "usenowflow.h"

#include "daemonclient.h"
#include "librarymodel.h"

using namespace Qt::StringLiterals;

namespace murasaki {

UseNowFlow::UseNowFlow(DaemonClient *client, LibraryModel *library, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_library(library)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_state == u"waiting")
            set(u"failed"_s, tr("Steam has not finished installing this wallpaper yet. Try again in a moment."));
        else if (m_state == u"applying")
            set(u"failed"_s, tr("The Murasaki service did not confirm the change."));
    });
    connect(m_library, &LibraryModel::countChanged, this, &UseNowFlow::onLibraryChanged);
    connect(m_client, &DaemonClient::assignmentsChanged, this, &UseNowFlow::onAssignmentsChanged);
    connect(m_client, &DaemonClient::error, this, &UseNowFlow::onError);
}

void UseNowFlow::set(const QString &state, const QString &message)
{
    if (state != u"waiting" && state != u"applying")
        m_timer.stop();
    if (m_state == state && m_message == message)
        return;
    m_state = state;
    m_message = message;
    emit stateChanged();
}

void UseNowFlow::start(const QString &itemId, bool replaceOverrides, const QStringList &knownDesktops)
{
    m_itemId = itemId;
    m_replace = replaceOverrides;
    m_knownDesktops = knownDesktops;
    m_timer.stop();
    if (!m_client->available()) {
        set(u"failed"_s, tr("The Murasaki service is not running."));
        return;
    }
    if (m_library->indexOf(itemId) < 0) {
        set(u"waiting"_s);
        m_timer.start(m_waitForItemMs);
        return;
    }
    apply();
}

void UseNowFlow::reset()
{
    m_itemId.clear();
    set(u"idle"_s);
}

bool UseNowFlow::satisfied() const
{
    bool wildcardIsTheItem = false;
    for (const QVariant &entry : m_client->assignments()) {
        const QVariantMap a = entry.toMap();
        if (a.value(u"output"_s).toString() == u"*" && a.value(u"desktop"_s).toString() == u"*" && a.value(u"itemId"_s).toString() == m_itemId)
            wildcardIsTheItem = true;
    }
    if (!wildcardIsTheItem)
        return false;
    return !m_replace
        || (m_client->overridingAssignments(u"*"_s, u"*"_s, m_knownDesktops).isEmpty()
            && m_client->orphanAssignments(u"*"_s, u"*"_s, m_knownDesktops).isEmpty());
}

void UseNowFlow::apply()
{
    // What the daemon already has needs no call, and a call that changes nothing would never be answered.
    if (satisfied()) {
        set(u"applied"_s);
        return;
    }
    set(u"applying"_s);
    m_timer.start(m_confirmMs);
    if (m_replace) {
        const QVariantList doomed = m_client->overridingAssignments(u"*"_s, u"*"_s, m_knownDesktops)
            + m_client->orphanAssignments(u"*"_s, u"*"_s, m_knownDesktops);
        for (const QVariant &entry : doomed)
            m_client->clearAssignment(entry.toMap().value(u"output"_s).toString(), entry.toMap().value(u"desktop"_s).toString());
    }
    m_client->setAssignment(u"*"_s, u"*"_s, m_itemId, {});
}

void UseNowFlow::onLibraryChanged()
{
    if (m_state == u"waiting" && m_library->indexOf(m_itemId) >= 0) {
        m_timer.stop();
        apply();
    }
}

void UseNowFlow::onAssignmentsChanged()
{
    if (m_state == u"applying" && satisfied())
        set(u"applied"_s);
}

void UseNowFlow::onError(const QString &message)
{
    if (m_state == u"applying" || m_state == u"waiting")
        set(u"failed"_s, message);
}

} // namespace murasaki
