#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <optional>

class QNetworkReply;

namespace murasaki {

inline constexpr QLatin1String kSteamPublishedFileDetailsUrl(
    "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/");
// Tests (and offline setups) point the client elsewhere with this variable.
inline constexpr char kWorkshopApiUrlEnv[] = "MURASAKI_WORKSHOP_API_URL";

struct WorkshopMeta {
    QString title;
    QStringList tags;
    QString previewPath; // absolute path of the cached preview image, or empty
};

// Fetches title, tags and preview image of Workshop items from Steam's public, keyless GetPublishedFileDetails
// endpoint (one batched POST for all ids; no key or account is ever sent) and caches them. The cache is
// <cacheDir>/workshop-meta.json, keyed by id and the item's time_updated, with previews in
// <cacheDir>/previews/<id>.<ext>, downloaded once per version of the item.
//
// Offline, timeouts, HTTP errors and rate limiting (HTTP 429, honouring Retry-After) never block anything: the
// cached values keep being served and the request is retried with exponential backoff. API requests are at least
// minGapMs apart.
class WorkshopMetadataClient : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QUrl endpoint;
        int requestTimeoutMs = 15000;
        int minGapMs = 1000;
        int retryInitialMs = 30000;
        int retryMaxMs = 15 * 60 * 1000;
        qint64 ttlSeconds = 6 * 3600;        // how long a known item is trusted before asking Steam again
        qint64 missingTtlSeconds = 3600;     // same, for ids Steam does not know
        int batchSize = 100;
        qint64 maxPreviewBytes = 8 * 1024 * 1024;
    };

    explicit WorkshopMetadataClient(QString cacheDir, QObject *parent = nullptr);

    // Whether a preview may be downloaded from url: https on Steam's CDN domains (steamusercontent.com,
    // akamaihd.net, steamstatic.com and their subdomains), never an IP literal. allowLoopback also admits plain
    // http to a loopback host.
    static bool previewUrlAllowed(const QUrl &url, bool allowLoopback = false);

    static Options defaultOptions(); // endpoint from the environment, else Steam's
    void setOptions(const Options &options) { m_options = options; }

    // Cached metadata for a Workshop id; nullopt when nothing is known (or Steam does not know the item).
    std::optional<WorkshopMeta> metadata(const QString &workshopId) const;

    // Asks Steam about the ids whose cached data is missing or older than the TTL (ids that are not plain
    // numbers are ignored) and downloads missing previews. Returns immediately.
    void refresh(const QStringList &workshopIds);

    // Forgets every item that is not in workshopIds and deletes its preview (and any preview file that belongs to
    // no cached item). Call it with the ids the library currently has.
    void prune(const QStringList &workshopIds);

    // True while a request or a preview download is running or scheduled.
    bool busy() const;

signals:
    // Workshop ids whose title, tags or preview changed, coalesced over a short interval.
    void metadataChanged(const QStringList &workshopIds);

private:
    bool previewAllowed(const QUrl &url) const;
    struct Entry {
        qint64 timeUpdated = 0;
        QString title;
        QStringList tags;
        QString previewUrl;
        QString previewFile; // file name inside previews/, empty until downloaded
        qint64 fetchedAt = 0;
        bool missing = false;
    };

    void loadCache();
    void saveCache() const;
    bool isFresh(const Entry &entry, qint64 now) const;
    void schedule(int delayMs);
    void sendBatch();
    void onBatchFinished(QNetworkReply *reply);
    void fail(int retryAfterMs);
    void applyResponse(const QJsonDocument &doc);
    void nextPreview();
    void onPreviewFinished(QNetworkReply *reply, const QString &id);
    QString previewDir() const;
    void markChanged(const QString &id);

    QString m_cacheDir;
    Options m_options;
    QNetworkAccessManager m_nam;
    QHash<QString, Entry> m_entries;
    QSet<QString> m_pending;      // ids to ask about
    QStringList m_batch;          // ids of the request in flight
    QStringList m_previewQueue;   // ids whose preview is to be downloaded
    QSet<QString> m_previewTried; // ids whose preview download failed in this run
    QNetworkReply *m_reply = nullptr;
    QNetworkReply *m_previewReply = nullptr;
    QTimer m_timer;
    QTimer m_changeTimer;
    QSet<QString> m_changed;
    QElapsedTimer m_lastRequest;
    int m_retryMs = 0;
};

} // namespace murasaki
