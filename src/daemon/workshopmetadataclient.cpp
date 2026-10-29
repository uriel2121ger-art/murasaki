#include "workshopmetadataclient.h"

#include "jsonfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr qint64 kMaxApiResponseBytes = 4 * 1024 * 1024;
constexpr int kChangeCoalesceMs = 200;

bool isWorkshopId(const QString &id)
{
    static const QRegularExpression digits(u"\\A\\d{1,20}\\z"_s);
    return digits.match(id).hasMatch();
}

// jpg, png, gif or webp by magic number; empty when it is not an image we show.
QString imageExtension(const QByteArray &data)
{
    if (data.startsWith("\xFF\xD8\xFF"))
        return u"jpg"_s;
    if (data.startsWith("\x89PNG\r\n\x1a\n"))
        return u"png"_s;
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a"))
        return u"gif"_s;
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return u"webp"_s;
    return {};
}

bool isLoopbackHost(const QString &host)
{
    return host == u"localhost" || QHostAddress(host).isLoopback();
}

QNetworkRequest baseRequest(const QUrl &url, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs);
    request.setRawHeader("User-Agent", "murasaki");
    // Every redirect is checked by the caller (see the redirected signal) before it is followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    return request;
}

// Stops the transfer once it exceeds limit bytes, so a hostile or broken server cannot fill the memory.
void capDownload(QNetworkReply *reply, qint64 limit)
{
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit](qint64 received, qint64 total) {
        if (received > limit || total > limit)
            reply->abort();
    });
}

} // namespace

bool WorkshopMetadataClient::previewUrlAllowed(const QUrl &url, bool allowLoopback)
{
    const QString host = url.host().toLower();
    if (host.isEmpty() || !url.userInfo().isEmpty())
        return false;
    if (allowLoopback && url.scheme() == u"http" && isLoopbackHost(host))
        return true;
    if (url.scheme() != u"https" || (url.port() != -1 && url.port() != 443))
        return false;
    if (!QHostAddress(host).isNull())
        return false; // an IP literal, whatever the address
    static const QStringList domains = {u"steamusercontent.com"_s, u"akamaihd.net"_s, u"steamstatic.com"_s};
    for (const QString &domain : domains) {
        if (host == domain || host.endsWith(u'.' + domain))
            return true;
    }
    return false;
}

bool WorkshopMetadataClient::previewAllowed(const QUrl &url) const
{
    // Plain http to loopback exists for the tests and the MURASAKI_WORKSHOP_API_URL override: only when the API
    // endpoint itself is a loopback http one.
    const bool loopbackApi = m_options.endpoint.scheme() == u"http" && isLoopbackHost(m_options.endpoint.host().toLower());
    return previewUrlAllowed(url, loopbackApi);
}

WorkshopMetadataClient::Options WorkshopMetadataClient::defaultOptions()
{
    Options options;
    const QByteArray override = qgetenv(kWorkshopApiUrlEnv);
    options.endpoint = QUrl(override.isEmpty() ? QString(kSteamPublishedFileDetailsUrl) : QString::fromUtf8(override));
    return options;
}

WorkshopMetadataClient::WorkshopMetadataClient(QString cacheDir, QObject *parent)
    : QObject(parent)
    , m_cacheDir(std::move(cacheDir))
    , m_options(defaultOptions())
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &WorkshopMetadataClient::sendBatch);
    m_changeTimer.setSingleShot(true);
    connect(&m_changeTimer, &QTimer::timeout, this, [this] {
        QStringList ids(m_changed.cbegin(), m_changed.cend());
        m_changed.clear();
        ids.sort();
        if (!ids.isEmpty())
            emit metadataChanged(ids);
    });
    loadCache();
}

QString WorkshopMetadataClient::previewDir() const
{
    return m_cacheDir + u"/previews"_s;
}

void WorkshopMetadataClient::loadCache()
{
    const JsonFileRead read = readJsonObjectFile(m_cacheDir + u"/workshop-meta.json"_s);
    if (!read.exists)
        return;
    if (!read.problem.isEmpty() || read.root.value(u"version"_s).toInt() != 1) {
        // Only a cache: it is rebuilt from Steam and overwritten on the next save.
        qWarning("murasakid: ignoring the unreadable Workshop metadata cache");
        return;
    }
    const QJsonObject items = read.root.value(u"items"_s).toObject();
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (!isWorkshopId(it.key()))
            continue;
        const QJsonObject o = it.value().toObject();
        Entry entry;
        entry.timeUpdated = o.value(u"timeUpdated"_s).toInteger();
        entry.title = o.value(u"title"_s).toString();
        for (const QJsonValue &tag : o.value(u"tags"_s).toArray())
            entry.tags.append(tag.toString());
        entry.previewUrl = o.value(u"previewUrl"_s).toString();
        entry.previewFile = QFileInfo(o.value(u"previewFile"_s).toString()).fileName(); // never a path
        entry.fetchedAt = o.value(u"fetchedAt"_s).toInteger();
        entry.missing = o.value(u"missing"_s).toBool();
        m_entries.insert(it.key(), entry);
    }
}

void WorkshopMetadataClient::saveCache() const
{
    QJsonObject items;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const Entry &e = it.value();
        items.insert(it.key(), QJsonObject{{u"timeUpdated"_s, e.timeUpdated},
                                           {u"title"_s, e.title},
                                           {u"tags"_s, QJsonArray::fromStringList(e.tags)},
                                           {u"previewUrl"_s, e.previewUrl},
                                           {u"previewFile"_s, e.previewFile},
                                           {u"fetchedAt"_s, e.fetchedAt},
                                           {u"missing"_s, e.missing}});
    }
    writeJsonObjectFile(m_cacheDir + u"/workshop-meta.json"_s, QJsonObject{{u"version"_s, 1}, {u"items"_s, items}});
}

std::optional<WorkshopMeta> WorkshopMetadataClient::metadata(const QString &workshopId) const
{
    const auto it = m_entries.constFind(workshopId);
    if (it == m_entries.cend() || it->missing)
        return std::nullopt;
    WorkshopMeta meta;
    meta.title = it->title;
    meta.tags = it->tags;
    if (!it->previewFile.isEmpty()) {
        const QString path = previewDir() + u'/' + it->previewFile;
        if (QFileInfo(path).isFile())
            meta.previewPath = path;
    }
    return meta;
}

bool WorkshopMetadataClient::isFresh(const Entry &entry, qint64 now) const
{
    const qint64 ttl = entry.missing ? m_options.missingTtlSeconds : m_options.ttlSeconds;
    return entry.fetchedAt > 0 && now - entry.fetchedAt < ttl && entry.fetchedAt <= now + 60;
}

bool WorkshopMetadataClient::busy() const
{
    return m_reply || m_previewReply || m_timer.isActive() || !m_pending.isEmpty() || !m_previewQueue.isEmpty();
}

void WorkshopMetadataClient::refresh(const QStringList &workshopIds)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QString &id : workshopIds) {
        if (!isWorkshopId(id))
            continue;
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend() || !isFresh(*it, now)) {
            m_pending.insert(id);
        } else if (!it->missing && previewAllowed(QUrl(it->previewUrl)) && !m_previewTried.contains(id)
                   && !QFileInfo(previewDir() + u'/' + it->previewFile).isFile() && !m_previewQueue.contains(id)) {
            m_previewQueue.append(id); // the metadata is fresh but the image never arrived
        }
    }
    if (!m_pending.isEmpty() && !m_reply && !m_timer.isActive())
        schedule(0);
    if (!m_previewQueue.isEmpty() && !m_previewReply)
        nextPreview();
}

void WorkshopMetadataClient::schedule(int delayMs)
{
    int delay = std::max(0, delayMs);
    if (m_lastRequest.isValid()) {
        const qint64 sinceLast = m_lastRequest.elapsed();
        delay = std::max<qint64>(delay, m_options.minGapMs - sinceLast);
    }
    m_timer.start(delay);
}

void WorkshopMetadataClient::sendBatch()
{
    if (m_reply || m_pending.isEmpty())
        return;
    QStringList ids(m_pending.cbegin(), m_pending.cend());
    ids.sort();
    ids = ids.mid(0, m_options.batchSize);
    m_batch = ids;
    for (const QString &id : std::as_const(m_batch))
        m_pending.remove(id);

    QUrlQuery form;
    form.addQueryItem(u"itemcount"_s, QString::number(m_batch.size()));
    for (qsizetype i = 0; i < m_batch.size(); ++i)
        form.addQueryItem(u"publishedfileids%5B%1%5D"_s.arg(i), m_batch.at(i));
    QNetworkRequest request = baseRequest(m_options.endpoint, m_options.requestTimeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    m_lastRequest.start();
    m_reply = m_nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    capDownload(m_reply, kMaxApiResponseBytes);
    connect(m_reply, &QNetworkReply::redirected, this, [this, reply = m_reply](const QUrl &target) {
        // The API may move within its own host; never to another host or from https down to http.
        if (target.host() == m_options.endpoint.host() && (target.scheme() == u"https" || m_options.endpoint.scheme() == u"http"))
            reply->redirectAllowed();
        else
            reply->abort();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply] { onBatchFinished(reply); });
}

void WorkshopMetadataClient::fail(int retryAfterMs)
{
    for (const QString &id : std::as_const(m_batch))
        m_pending.insert(id);
    m_batch.clear();
    // The server's Retry-After wins over our own backoff, even beyond its cap.
    const int backoff = std::min(m_retryMs > 0 ? m_retryMs : m_options.retryInitialMs, m_options.retryMaxMs);
    m_retryMs = std::min(m_options.retryMaxMs, backoff * 2);
    schedule(retryAfterMs > 0 ? retryAfterMs : backoff);
}

void WorkshopMetadataClient::onBatchFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        int retryAfterMs = 0;
        if (status == 429 || status == 503) {
            bool ok = false;
            const int seconds = reply->rawHeader("Retry-After").toInt(&ok);
            retryAfterMs = ok ? std::clamp(seconds, 1, 3600) * 1000 : std::max(m_retryMs, 60000);
        }
        qWarning("murasakid: Workshop metadata request failed (%s, HTTP %d); retrying later",
                 qPrintable(reply->errorString()), status);
        fail(retryAfterMs);
        return;
    }
    const QByteArray body = reply->read(kMaxApiResponseBytes + 1);
    if (body.size() > kMaxApiResponseBytes) {
        fail(0);
        return;
    }
    // A 200 that is not the expected document (a captive portal, an error page) must not touch the cache.
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject() || !doc.object().value(u"response"_s).isObject()
        || !doc.object().value(u"response"_s).toObject().value(u"publishedfiledetails"_s).isArray()) {
        qWarning("murasakid: unexpected Workshop metadata response; retrying later");
        fail(0);
        return;
    }
    m_retryMs = 0;
    applyResponse(doc);
    m_batch.clear();
    saveCache();
    if (!m_pending.isEmpty())
        schedule(m_options.minGapMs);
    nextPreview();
}

void WorkshopMetadataClient::applyResponse(const QJsonDocument &doc)
{
    const QJsonArray details = doc.object().value(u"response"_s).toObject().value(u"publishedfiledetails"_s).toArray();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSet<QString> seen;
    for (const QJsonValue &value : details) {
        const QJsonObject d = value.toObject();
        const QString id = d.value(u"publishedfileid"_s).toVariant().toString();
        if (!m_batch.contains(id) || seen.contains(id))
            continue; // an id we did not ask about is ignored
        seen.insert(id);

        Entry entry = m_entries.value(id);
        const Entry before = entry;
        if (d.value(u"result"_s).toInt() != 1) {
            entry = Entry();
            entry.missing = true;
            entry.fetchedAt = now;
            m_entries.insert(id, entry);
            if (!before.missing)
                markChanged(id);
            continue;
        }
        const qint64 timeUpdated = d.value(u"time_updated"_s).toInteger();
        const bool sameVersion = !before.missing && before.fetchedAt > 0 && before.timeUpdated == timeUpdated;
        entry.missing = false;
        entry.timeUpdated = timeUpdated;
        entry.title = d.value(u"title"_s).toString().left(500).trimmed();
        entry.tags.clear();
        for (const QJsonValue &tag : d.value(u"tags"_s).toArray()) {
            const QJsonObject t = tag.toObject();
            const QString name = (t.contains(u"display_name"_s) ? t.value(u"display_name"_s) : t.value(u"tag"_s))
                                     .toString().left(100).trimmed();
            if (!name.isEmpty() && entry.tags.size() < 50)
                entry.tags.append(name);
        }
        entry.previewUrl = d.value(u"preview_url"_s).toString();
        entry.fetchedAt = now;
        if (sameVersion && !entry.previewFile.isEmpty() && !QFileInfo(previewDir() + u'/' + entry.previewFile).isFile())
            entry.previewFile.clear(); // deleted behind our back: fetch it again
        if (!sameVersion) {
            // A new version of the item: its old preview is stale.
            if (!before.previewFile.isEmpty())
                QFile::remove(previewDir() + u'/' + before.previewFile);
            entry.previewFile.clear();
        }
        m_entries.insert(id, entry);
        if (entry.title != before.title || entry.tags != before.tags || entry.missing != before.missing
            || entry.previewFile != before.previewFile)
            markChanged(id);
        if (entry.previewFile.isEmpty() && previewAllowed(QUrl(entry.previewUrl)) && !m_previewQueue.contains(id))
            m_previewQueue.append(id);
    }
    // Ids Steam left out are treated as unknown, so they are not asked again right away.
    for (const QString &id : std::as_const(m_batch)) {
        if (seen.contains(id))
            continue;
        Entry entry;
        entry.missing = true;
        entry.fetchedAt = now;
        m_entries.insert(id, entry);
    }
}

void WorkshopMetadataClient::nextPreview()
{
    if (m_previewReply)
        return;
    while (!m_previewQueue.isEmpty()) {
        const QString id = m_previewQueue.takeFirst();
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend() || it->missing)
            continue;
        const QUrl url(it->previewUrl);
        if (!previewAllowed(url))
            continue;
        m_previewReply = m_nam.get(baseRequest(url, m_options.requestTimeoutMs * 2));
        capDownload(m_previewReply, m_options.maxPreviewBytes);
        // A redirect leaves the checked host: follow it only to another allowed address.
        connect(m_previewReply, &QNetworkReply::redirected, this, [this, reply = m_previewReply](const QUrl &target) {
            if (previewAllowed(target))
                reply->redirectAllowed();
            else
                reply->abort();
        });
        connect(m_previewReply, &QNetworkReply::finished, this,
                [this, reply = m_previewReply, id] { onPreviewFinished(reply, id); });
        return;
    }
}

void WorkshopMetadataClient::onPreviewFinished(QNetworkReply *reply, const QString &id)
{
    reply->deleteLater();
    if (reply != m_previewReply)
        return;
    m_previewReply = nullptr;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->error() == QNetworkReply::NoError && status == 200
        ? reply->read(m_options.maxPreviewBytes + 1)
        : QByteArray();
    const QString ext = data.size() <= m_options.maxPreviewBytes ? imageExtension(data) : QString();
    auto entry = m_entries.find(id);
    if (ext.isEmpty() || entry == m_entries.end()) {
        qWarning("murasakid: could not download the preview of Workshop item %s", qPrintable(id));
        m_previewTried.insert(id);
    } else {
        const QString file = id + u'.' + ext;
        QDir().mkpath(previewDir());
        QSaveFile out(previewDir() + u'/' + file);
        if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit()) {
            if (!entry->previewFile.isEmpty() && entry->previewFile != file)
                QFile::remove(previewDir() + u'/' + entry->previewFile);
            entry->previewFile = file;
            saveCache();
            markChanged(id);
        } else {
            qWarning("murasakid: cannot write the preview of Workshop item %s", qPrintable(id));
            m_previewTried.insert(id);
        }
    }
    nextPreview();
}

void WorkshopMetadataClient::prune(const QStringList &workshopIds)
{
    const QSet<QString> known(workshopIds.cbegin(), workshopIds.cend());
    bool removed = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (known.contains(it.key())) {
            ++it;
            continue;
        }
        if (!it->previewFile.isEmpty())
            QFile::remove(previewDir() + u'/' + it->previewFile);
        m_pending.remove(it.key());
        m_previewQueue.removeAll(it.key());
        it = m_entries.erase(it);
        removed = true;
    }
    // Previews no cached item owns (a crash between the download and the cache write, or an old format).
    QSet<QString> owned;
    for (const Entry &entry : std::as_const(m_entries))
        owned.insert(entry.previewFile);
    for (const QString &name : QDir(previewDir()).entryList(QDir::Files)) {
        if (!owned.contains(name) && m_previewReply == nullptr)
            QFile::remove(previewDir() + u'/' + name);
    }
    if (removed)
        saveCache();
}

void WorkshopMetadataClient::markChanged(const QString &id)
{
    m_changed.insert(id);
    m_changeTimer.start(kChangeCoalesceMs);
}

} // namespace murasaki
