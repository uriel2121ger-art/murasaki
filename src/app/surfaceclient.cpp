#include "surfaceclient.h"

#include "constants.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace Qt::StringLiterals;

namespace murasaki {

SurfaceClient::SurfaceClient(DBusBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_backend->setParent(this);
    m_pollTimer.setInterval(PollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &SurfaceClient::refresh);
    connect(m_backend, &DBusBackend::availabilityChanged, this, &SurfaceClient::onAvailability);
    connect(m_backend, &DBusBackend::signalReceived, this, [this](const QString &name) {
        if (name == u"OutputsChanged")
            refresh();
    });
}

SurfaceClient::SurfaceClient(QObject *parent)
    : SurfaceClient(new SessionBusBackend(kSurfaceDBusService, kSurfaceDBusPath, kSurfaceDBusInterface,
                                          {u"OutputsChanged"_s}),
                    parent)
{
}

void SurfaceClient::start()
{
    m_backend->start();
}

void SurfaceClient::setPoll(bool poll)
{
    if (poll == m_pollTimer.isActive())
        return;
    if (poll) {
        m_pollTimer.start();
        refresh(); // do not make the page wait for the first tick
    } else {
        m_pollTimer.stop();
    }
    emit pollChanged();
}

void SurfaceClient::onAvailability(bool up)
{
    if (up == m_available)
        return;
    m_available = up;
    ++m_generation; // answers still in flight belong to the previous owner of the name
    emit availableChanged();
    if (up) {
        refresh();
    } else {
        // Nothing renders without the surface: do not show stale outputs.
        m_outputsLoaded = m_desktopsLoaded = false;
        if (!m_outputs.isEmpty()) {
            m_outputs.clear();
            emit outputsChanged();
        }
        if (!m_desktops.isEmpty()) {
            m_desktops.clear();
            emit desktopsChanged();
        }
    }
}

void SurfaceClient::refresh()
{
    if (!m_available)
        return;
    fetch(u"Outputs"_s, m_outputs, &SurfaceClient::outputsChanged);
    fetch(u"Desktops"_s, m_desktops, &SurfaceClient::desktopsChanged);
}

void SurfaceClient::retryOutput(const QString &output)
{
    if (m_available)
        m_backend->call(u"RetryOutput"_s, {output}, [](const DBusBackend::Reply &) {});
}


void SurfaceClient::fetch(const QString &method, QVariantList &cache, void (SurfaceClient::*changed)())
{
    const bool isOutputs = &cache == &m_outputs;
    const quint64 generation = m_generation;
    m_backend->call(method, {}, [this, &cache, changed, isOutputs, generation](const DBusBackend::Reply &reply) {
        if (!reply.ok || !m_available || generation != m_generation)
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(reply.value.toString().toUtf8());
        if (!doc.isArray())
            return;
        bool &loaded = isOutputs ? m_outputsLoaded : m_desktopsLoaded;
        const QVariantList list = doc.array().toVariantList();
        if (loaded && list == cache)
            return;
        loaded = true;
        cache = list;
        emit (this->*changed)();
    });
}

} // namespace murasaki
