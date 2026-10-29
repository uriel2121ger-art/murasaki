#pragma once

#include <QMutex>
#include <atomic>
#include <QStringList>
#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>

namespace murasaki {

// Confines what a web item's page can reach, for every request the profile makes:
//  - file:// URLs (Wallpaper Engine pages build them from "file" properties, which the item scheme allows with
//    LocalAccessAllowed) are narrowed to the readable roots of the items on screen, so a Workshop page cannot load
//    anything else from disk;
//  - anything that is not the item scheme, data:, blob: or about: is the network, and is blocked unless the user allowed
//    the wallpaper of this profile to use the internet. That covers HTTP(S) in all its forms (fetch, XHR, images, scripts,
//    frames, beacons, navigations) and WebSocket handshakes, which Qt reports to interceptors. Channels Chromium does not
//    route through here (WebRTC, WebTransport, hints) are closed by the Content-Security-Policy of ItemSchemeHandler, the
//    page script of WebItem.qml and, behind them all, the credentials of the NetworkGate.
class FileAccessInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    // Canonical paths (WallpaperContent::readableRoots of every shown web item).
    void setRoots(const QStringList &roots)
    {
        const QMutexLocker locker(&m_lock);
        m_roots = roots;
    }

    // Whether the wallpaper of this profile may use the internet. Each shown wallpaper has a profile of its own.
    void setNetworkAllowed(bool allowed) { m_networkAllowed = allowed; }
    bool networkAllowed() const { return m_networkAllowed; }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;


private:
    mutable QMutex m_lock; // the engine may call interceptRequest() from its IO thread
    QStringList m_roots;   // guarded by m_lock
    std::atomic<bool> m_networkAllowed{false};
};

} // namespace murasaki
