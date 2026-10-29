#include "murasakistatus.h"

#include "constants.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki {

OutputResolver *OutputResolver::instance()
{
    // Owned by the application so it dies with the event loop's last object, whichever thread loaded the module.
    static QPointer<OutputResolver> resolver;
    if (!resolver)
        resolver = new OutputResolver(QCoreApplication::instance());
    return resolver;
}

OutputResolver::OutputResolver(QObject *parent)
    : QObject(parent)
    , m_desktop(u"*"_s)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString path(kDBusPath);
    const QString iface(kDBusInterface);
    bus.connect(QString(kDBusService), path, iface, u"AssignmentsChanged"_s, this, SLOT(resolveAll()));
    bus.connect(QString(kDBusService), path, iface, u"ItemsChanged"_s, this,
                SLOT(onItemsChanged(QStringList,QStringList,QStringList)));
    auto *watcher = new QDBusServiceWatcher(QString(kDBusService), bus, QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &OutputResolver::resolveAll);
}

void OutputResolver::setDesktop(const QString &desktop)
{
    const QString value = desktop.isEmpty() ? u"*"_s : desktop;
    if (value == m_desktop)
        return;
    m_desktop = value;
    emit desktopChanged();
    resolveAll();
}

void OutputResolver::watch(const QString &output)
{
    if (output.isEmpty())
        return;
    if (m_watchers[output]++ == 0)
        resolve(output);
}

void OutputResolver::unwatch(const QString &output)
{
    const auto it = m_watchers.find(output);
    if (it == m_watchers.end())
        return;
    if (--*it > 0)
        return;
    m_watchers.erase(it);
    m_info.remove(output);
    m_requests.remove(output); // a reply still in flight is dropped
}

void OutputResolver::onItemsChanged(const QStringList &, const QStringList &, const QStringList &)
{
    resolveAll();
}

void OutputResolver::resolveAll()
{
    const QList<QString> outputs = m_watchers.keys();
    for (const QString &output : outputs)
        resolve(output);
}

void OutputResolver::resolve(const QString &output)
{
    // The daemon's ApiVersion gates the lookup: an older daemon (not restarted after an update) is unavailable.
    QDBusMessage versionCall = QDBusMessage::createMethodCall(
        QString(kDBusService), QString(kDBusPath), u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    versionCall.setArguments({QString(kDBusInterface), u"ApiVersion"_s});
    versionCall.setAutoStartService(false); // a lookup must never launch the daemon
    const quint64 request = ++m_nextRequest;
    m_requests.insert(output, request);
    auto *versionWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(versionCall), this);
    connect(versionWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, output, request](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (m_requests.value(output) != request)
            return; // superseded by a newer lookup, or nobody watches the output any more
        const QDBusPendingReply<QDBusVariant> version = *w;
        if (version.isError() || version.value().variant().toUInt() < kMinDBusApiVersion) {
            apply(output, {}); // no daemon (yet; its registration triggers another lookup) or one too old
            return;
        }
        QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                           QString(kDBusInterface), u"ResolveAssignment"_s);
        call.setArguments({output, m_desktop});
        call.setAutoStartService(false);
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, output, request](QDBusPendingCallWatcher *r) {
            r->deleteLater();
            if (m_requests.value(output) != request)
                return;
            const QDBusPendingReply<QString> reply = *r;
            if (reply.isError()) {
                apply(output, {});
                return;
            }
            const QJsonObject item =
                QJsonDocument::fromJson(reply.value().toUtf8()).object().value(u"item"_s).toObject();
            OutputInfo info;
            const QString preview = item.value(u"preview"_s).toString();
            if (!preview.isEmpty())
                info.preview = QUrl::fromLocalFile(preview);
            info.itemId = item.value(u"id"_s).toString();
            info.kind = item.value(u"kind"_s).toString();
            apply(output, info);
        });
    });
}

void OutputResolver::apply(const QString &output, const OutputInfo &info)
{
    if (m_info.contains(output) && m_info.value(output) == info)
        return;
    m_info.insert(output, info);
    emit infoChanged(output);
}

MurasakiStatus::MurasakiStatus(QObject *parent)
    : QObject(parent)
{
    connect(OutputResolver::instance(), &OutputResolver::desktopChanged, this, &MurasakiStatus::desktopChanged);
}

QString MurasakiStatus::desktop() const
{
    return OutputResolver::instance()->desktop();
}

void MurasakiStatus::setDesktop(const QString &desktop)
{
    OutputResolver::instance()->setDesktop(desktop);
}

bool MurasakiStatus::openMurasaki(const QString &hint)
{
    for (const QString &candidate : {hint, QStringLiteral(MURASAKI_APP_PROGRAM)}) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable())
            return QProcess::startDetached(candidate, {});
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("murasaki"));
    return !found.isEmpty() && QProcess::startDetached(found, {});
}

MurasakiOutputStatus::MurasakiOutputStatus(QObject *parent)
    : QObject(parent)
{
    connect(OutputResolver::instance(), &OutputResolver::infoChanged, this, [this](const QString &output) {
        if (output == m_output)
            emit infoChanged();
    });
}

MurasakiOutputStatus::~MurasakiOutputStatus()
{
    if (QCoreApplication::instance())
        OutputResolver::instance()->unwatch(m_output);
}

void MurasakiOutputStatus::setOutput(const QString &output)
{
    if (output == m_output)
        return;
    OutputResolver::instance()->unwatch(m_output);
    m_output = output;
    emit outputChanged();
    OutputResolver::instance()->watch(m_output);
    emit infoChanged(); // the cached answer of the new output, or empty until it arrives
}

} // namespace murasaki
