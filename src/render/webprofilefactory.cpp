#include "webprofilefactory.h"

#include "fileaccessinterceptor.h"
#include "itemschemehandler.h"
#include "networkgate.h"
#include "wallpapercontent.h"

#include <QQuickWebEngineProfile>
#include <QTimer>

namespace murasaki {

struct WebProfileFactory::Entry {
    QString key;
    QString proxyToken;
    // The profile goes first when the entry is destroyed; the handler and the interceptor must outlive it.
    std::unique_ptr<ItemSchemeHandler> handler;
    std::unique_ptr<FileAccessInterceptor> interceptor;
    std::unique_ptr<QQuickWebEngineProfile> profile;
    std::unique_ptr<WebProfileLease> lease;
    bool minElapsed = false; // set after release(): the view had its time to appear and to go
};

void WebProfileLease::viewClosed()
{
    if (m_views > 0 && --m_views == 0)
        emit idle();
}

WebProfileFactory::WebProfileFactory(NetworkGate *gate, QObject *parent)
    : QObject(parent)
    , m_gate(gate)
{
}

WebProfileFactory::~WebProfileFactory()
{
    for (auto &entry : m_entries)
        m_gate->revokeToken(entry->proxyToken);
}

WebProfileFactory::Handle WebProfileFactory::acquire(const QString &slot, const QString &key, const QString &contentDir,
                                                     bool networkAllowed)
{
    if (networkAllowed && !m_gate->isListening()) {
        // Fail closed: without a gate the engine has no proxy to speak of, so no wallpaper gets the network.
        qWarning("murasaki: the network gate is not listening; web wallpapers have no network");
        networkAllowed = false;
    }
    auto it = m_entries.find(slot);
    if (it != m_entries.end() && (*it)->key != key) {
        release(slot);
        it = m_entries.end();
    }
    if (it == m_entries.end()) {
        auto entry = std::make_shared<Entry>();
        entry->key = key;
        entry->handler = std::make_unique<ItemSchemeHandler>(networkAllowed);
        entry->interceptor = std::make_unique<FileAccessInterceptor>();
        entry->interceptor->setNetworkAllowed(networkAllowed);
        entry->interceptor->setRoots(m_roots);
        if (networkAllowed)
            entry->proxyToken = m_gate->issueToken();
        entry->lease = std::make_unique<WebProfileLease>();
        entry->profile = std::make_unique<QQuickWebEngineProfile>(); // off the record, a network context of its own
        entry->profile->installUrlSchemeHandler(QByteArray(kItemScheme.data(), kItemScheme.size()), entry->handler.get());
        entry->profile->setUrlRequestInterceptor(entry->interceptor.get());
        it = m_entries.insert(slot, entry);
    }
    Entry &entry = **it;
    return {entry.profile.get(), entry.handler->tokenFor(contentDir), entry.proxyToken, entry.interceptor->networkAllowed(), entry.lease.get()};
}

void WebProfileFactory::release(const QString &slot)
{
    const auto it = m_entries.find(slot);
    if (it == m_entries.end())
        return;
    std::shared_ptr<Entry> entry = *it;
    m_entries.erase(it);
    m_gate->revokeToken(entry->proxyToken); // from now on its connections are refused and the live ones dropped
    // The view that uses the profile fades out and is destroyed after this call, maybe much later (it stays until the
    // next wallpaper has a frame); the profile must outlive it.
    m_retired.append(entry);
    // The timers and the lease must not keep the entry alive: only m_retired does.
    const std::weak_ptr<Entry> weak = entry;
    QTimer::singleShot(m_retireDelayMs, this, [this, weak]() {
        if (const auto entry = weak.lock()) {
            entry->minElapsed = true;
            dropWhenIdle(entry);
        }
    });
    QTimer::singleShot(m_retireCapMs, this, [this, weak]() {
        if (const auto entry = weak.lock())
            m_retired.removeOne(entry);
    });
    connect(entry->lease.get(), &WebProfileLease::idle, this, [this, weak]() {
        if (const auto entry = weak.lock())
            dropWhenIdle(entry);
    });
}

void WebProfileFactory::dropWhenIdle(const std::shared_ptr<Entry> &entry)
{
    if (!entry->minElapsed || entry->lease->views() != 0)
        return;
    // Deferred: this can run inside the lease's own call, and the lease goes with the entry.
    QMetaObject::invokeMethod(this, [this, weak = std::weak_ptr<Entry>(entry)]() {
        if (const auto entry = weak.lock(); entry && entry->lease->views() == 0)
            m_retired.removeOne(entry);
    }, Qt::QueuedConnection);
}

void WebProfileFactory::setRoots(const QStringList &roots)
{
    m_roots = roots;
    for (auto &entry : m_entries)
        entry->interceptor->setRoots(roots);
}

} // namespace murasaki
