#include "itemschemehandler.h"

#include "pathutil.h"
#include "wallpapercontent.h"

#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUuid>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

namespace murasaki {

void ItemSchemeHandler::registerScheme()
{
    QWebEngineUrlScheme scheme(QByteArray(kItemScheme.data(), kItemScheme.size()));
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    // Secure, so pages get secure-context APIs; not Local, so they behave like a website rather than file://.
    // LocalAccessAllowed lets them load file:// URLs, which FileAccessInterceptor confines to their own dirs.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled
                     | QWebEngineUrlScheme::LocalAccessAllowed
                     | QWebEngineUrlScheme::FetchApiAllowed
    );
    QWebEngineUrlScheme::registerScheme(scheme);
}

QString ItemSchemeHandler::tokenFor(const QString &contentDir)
{
    const auto it = m_tokenByDir.constFind(contentDir);
    if (it != m_tokenByDir.cend())
        return *it;
    const QString token = QUuid::createUuid().toString(QUuid::Id128); // 32 lowercase hex digits: a valid host
    m_tokenByDir.insert(contentDir, token);
    m_dirByToken.insert(token, contentDir);
    return token;
}

void ItemSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (job->requestMethod() != "GET") {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
    }
    const QUrl url = job->requestUrl();
    const QString dir = m_dirByToken.value(url.host());
    const QString path = dir.isEmpty() ? QString() : containedExistingFile(dir, url.path(QUrl::FullyDecoded).mid(1));
    if (path.isEmpty()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    auto *file = new QFile(path, job);
    if (!file->open(QIODevice::ReadOnly)) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }
    static const QMimeDatabase mimeDb;
    const QMimeType type = mimeDb.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    const QString mime = type.isDefault() ? QStringLiteral("application/octet-stream") : type.name();
    // Without the network permission the page may only connect to its own files: connect-src governs fetch, XHR,
    // EventSource, beacons, WebSocket, WebTransport and the ICE servers of WebRTC, the channels Chromium does not route
    // through the request interceptor. Documents and workers carry it; frames and workers they create inherit it.
    if (!m_networkAllowed) {
        job->setAdditionalResponseHeaders(
            {{QByteArrayLiteral("Content-Security-Policy"),
              QByteArrayLiteral("connect-src murasaki-item: data: blob: file:")}});
    }
    // QIODevice replies do not support Range requests, so seeking inside <video> in web items may not work.
    job->reply(mime.toUtf8(), file);
}

} // namespace murasaki
