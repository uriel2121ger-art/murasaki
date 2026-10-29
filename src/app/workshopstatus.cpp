#include "workshopstatus.h"

namespace murasaki {

WorkshopStatus::WorkshopStatus(const QStringList &workshopRoots, const QString &appId, QObject *parent)
    : QObject(parent)
    , m_state(workshopRoots, appId)
{
    connect(&m_state, &WorkshopState::changed, this, [this] {
        ++m_revision;
        emit revisionChanged();
    });
}

QString WorkshopStatus::state(const QString &workshopId) const
{
    return m_state.state(workshopId);
}

} // namespace murasaki
