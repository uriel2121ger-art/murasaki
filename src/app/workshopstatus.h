#pragma once

#include "workshopstate.h"

#include <QObject>

namespace murasaki {

// QML-friendly face of WorkshopState: state() is invokable and `revision` changes whenever Steam's files do,
// so a binding such as `Workshop.revision, Workshop.state(id)` re-evaluates.
class WorkshopStatus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit WorkshopStatus(const QStringList &workshopRoots, const QString &appId, QObject *parent = nullptr);

    int revision() const { return m_revision; }

    // "installed", "downloading" or "none"
    Q_INVOKABLE QString state(const QString &workshopId) const;

signals:
    void revisionChanged();

private:
    WorkshopState m_state;
    int m_revision = 0;
};

} // namespace murasaki
