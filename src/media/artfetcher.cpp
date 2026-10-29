#include "media/artfetcher.h"

#include "media/mediapolicy.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

namespace murasaki::media {

ArtFetcher::ArtFetcher(QObject *parent)
    : QObject(parent)
{
    // direct, no system proxy. The name is resolved once to be judged and again by the connection: they can differ (DNS rebinding), a
    // risk accepted because the address comes from a local player (artfetcher.h)
    m_manager.setProxy(QNetworkProxy::NoProxy);
    m_total.setSingleShot(true);
    m_total.setInterval(kDownloadTotalMs);
    connect(&m_total, &QTimer::timeout, this, [this] {
        if (!m_active) {
            return;
        }
        cancel(); // the token moves on: whatever the aborted reply or a lookup still says is stale
        emit finished(m_origin, {});
    });
}

void ArtFetcher::fetch(const QUrl &url)
{
    cancel();
    m_origin = url;
    m_active = true;
    m_total.start();
    check(url, 0, m_token);
}

void ArtFetcher::cancel()
{
    ++m_token;
    m_active = false;
    m_total.stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void ArtFetcher::done(const QByteArray &body, quint64 token)
{
    if (token != m_token) {
        return;
    }
    m_active = false;
    m_total.stop();
    emit finished(m_origin, body);
}

// Resolves the host and lets the download go on only when every address is public.
void ArtFetcher::check(const QUrl &url, int hops, quint64 token)
{
    if (url.scheme().toLower() != QLatin1String("https") || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        done({}, token);
        return;
    }
    const QString host = url.host();
    QHostAddress literal;
    if (literal.setAddress(host)) {
        if (isBlockedAddress(literal)) {
            done({}, token);
        } else {
            request(url, hops, token);
        }
        return;
    }
    QHostInfo::lookupHost(host, this, [this, url, hops, token](const QHostInfo &info) {
        if (token != m_token) {
            return;
        }
        const QList<QHostAddress> addresses = info.addresses();
        const bool allPublic = info.error() == QHostInfo::NoError && !addresses.isEmpty()
            && std::none_of(addresses.begin(), addresses.end(), [](const QHostAddress &a) { return isBlockedAddress(a); });
        if (allPublic) {
            request(url, hops, token);
        } else {
            done({}, token);
        }
    });
}

void ArtFetcher::request(const QUrl &url, int hops, quint64 token)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setTransferTimeout(kDownloadTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Murasaki"));
    req.setRawHeader("Accept", "image/*");
    req.setRawHeader("Accept-Encoding", "identity"); // no compressed body to grow past the limit
    QNetworkReply *reply = m_manager.get(req);
    m_reply = reply;
    reply->setReadBufferSize(256 * 1024);
    // the body is kept as it arrives: over the limit the download stops
    auto *body = new QByteArray;
    connect(reply, &QObject::destroyed, [body] { delete body; });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, body, token] {
        if (token != m_token) {
            return;
        }
        body->append(reply->readAll());
        if (body->size() > kMaxArtBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, url, hops, token] {
        reply->deleteLater();
        if (token != m_token) {
            return;
        }
        m_reply = nullptr;
        body->append(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            const QUrl target = url.resolved(reply->header(QNetworkRequest::LocationHeader).toUrl());
            if (hops >= kMaxRedirects || !target.isValid()) {
                done({}, token);
            } else {
                check(target, hops + 1, token); // https only, and the address check again
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status != 200 || body->size() > kMaxArtBytes) {
            done({}, token);
            return;
        }
        done(*body, token);
    });
}

} // namespace murasaki::media
