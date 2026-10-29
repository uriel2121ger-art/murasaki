#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QQuickWebEngineProfile;

namespace murasaki {

class FileAccessInterceptor;
class ItemSchemeHandler;
class NetworkGate;

// Counts the views that use one profile. The view of a retired wallpaper stays on screen until the next one has
// drawn its first frame, which can take longer than any fixed delay; QtWebEngine crashes when a profile goes before
// its views, so a retired profile is deleted only once the last of its views has been destroyed (WebItem.qml reports
// both ends).
class WebProfileLease : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE void viewOpened() { ++m_views; }
    Q_INVOKABLE void viewClosed();
    int views() const { return m_views; }

signals:
    void idle(); // the last view is gone

private:
    int m_views = 0;
};

// One web profile per shown web wallpaper. A profile is a network context of its own (off the record: no cookies, no
// cache on disk, and no shared connections, authentication cache or hints with any other wallpaper), with its own scheme
// handler and request interceptor that know whether this wallpaper may use the internet. A wallpaper with the permission
// also gets a token of the NetworkGate and answers the proxy's challenge with it (WebItem.qml); the others get none, so
// even what the engine does behind the interceptor's back is refused at the proxy.
class WebProfileFactory : public QObject
{
    Q_OBJECT

public:
    explicit WebProfileFactory(NetworkGate *gate, QObject *parent = nullptr);
    ~WebProfileFactory() override;

    struct Handle {
        QQuickWebEngineProfile *profile = nullptr;
        QString itemToken;  // the host of the item's murasaki-item:// URLs under this profile
        QString proxyToken; // credentials for the gate; empty without the network permission
        bool networkAllowed = false; // what applies: the permission, and a gate that is listening
        WebProfileLease *lease = nullptr; // the views of the profile report to it
    };

    // The profile for what `slot` (an output) shows. The same key returns the same profile; another key retires the old
    // one (its token is revoked at once) and makes a new one.
    Handle acquire(const QString &slot, const QString &key, const QString &contentDir, bool networkAllowed);
    // The slot shows no web wallpaper any more. The profile is deleted a little later, when its view is gone.
    void release(const QString &slot);

    // Canonical paths a page may read through file:// (every shown wallpaper's); applied to all profiles.
    void setRoots(const QStringList &roots);

    int profileCount() const { return int(m_entries.size()); }
    // The profile is deleted after release() once this time has passed and its last view is gone.
    void setRetireDelayMs(int ms) { m_retireDelayMs = ms; }
    // Upper bound for a view that never reports its end.
    void setRetireCapMs(int ms) { m_retireCapMs = ms; }
    // Released profiles still waiting for their views.
    int retiredCount() const { return int(m_retired.size()); }

private:
    struct Entry;

    NetworkGate *m_gate;
    void dropWhenIdle(const std::shared_ptr<Entry> &entry);

    QHash<QString, std::shared_ptr<Entry>> m_entries;
    QList<std::shared_ptr<Entry>> m_retired;
    QStringList m_roots;
    int m_retireDelayMs = 10000;
    int m_retireCapMs = 600000;
};

} // namespace murasaki
