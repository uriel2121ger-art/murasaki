#pragma once

// The host's media service: follows the players (MediaWatcher), picks the one to show (choosePlayer), turns its cover into a
// thumbnail (a file read or a download, both decoded on a worker thread) and sends both to the engine bridges whose scene uses them.
// One per process. Nothing here trusts the player: see mediapolicy.h and artfetcher.h.
//
// Privacy: nothing is read from the players, and nothing is sent, unless the setting `mediaShare` is known to be on AND an attached
// engine told us (MediaWants) that its scene shows the cover or listens to the media events. A setting not read yet is "no". With
// `mediaShare` off a scene is told that no player runs (enabled: false, no cover). Downloads of https covers also wait for
// `mediaRemoteArt` to be known to be on.

#include "media/artdecode.h"
#include "media/artfetcher.h"
#include "media/mediapolicy.h"
#include "media/mediawatcher.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>
#include <functional>
#include <list>
#include <optional>
#include <vector>

namespace murasaki {
class BridgeClient;
}

namespace murasaki::media {

class MediaHub : public QObject
{
    Q_OBJECT

public:
    static constexpr int kStateDelayMs = 200;      // state messages are coalesced over this
    static constexpr int kThumbnailGapMs = 1000;   // between two covers sent

    explicit MediaHub(const QDBusConnection &bus = QDBusConnection::sessionBus(), QObject *parent = nullptr);
    ~MediaHub() override;

    // The process's hub, made on the heap under the application object (it goes with it, before the event loop's resources).
    static MediaHub &shared();

    // The settings, as the daemon says them: until they are, the answer is "no".
    void setShare(bool on);
    void setRemoteArt(bool on);
    bool shareKnownOn() const { return m_share == Tri::On; }
    bool remoteArtKnownOn() const { return m_remoteArt == Tri::On; }

    // An engine's bridge: it is sent the media once it says it uses them (BridgeClient::mediaWanted) and every change after, until it is
    // destroyed. A producer that predates the media messages is never sent any.
    void attach(BridgeClient *client);

    bool active() const { return m_watcher.running(); }    // the players are being read
    QByteArray stateJson() const { return m_stateJson; }
    // The cover of the track in view: none until one was read, width 0 for "no cover".
    const std::optional<Cover> &cover() const { return m_cover; }
    uint32_t generation() const { return m_generation; }
    // Downloads started so far, and covers (files or downloads) wanted at all (tests).
    int downloadsStarted() const { return m_downloads; }
    int loadsStarted() const { return m_loads; }
    // Tests: a worker waits this long before it decodes (a slow disk or a hung mount).
    void setDecodeDelayForTests(int ms) { m_decodeDelayMs = ms; }

signals:
    void stateChanged(const QByteArray &json);
    void thumbnailChanged(uint32_t width, uint32_t height, const QByteArray &rgba, uint32_t generation);

private:
    enum class Tri { Unknown, On, Off };
    enum class Load { None, File, Https };

    void refreshActive();
    int wanting() const;
    void sendState(BridgeClient *only = nullptr);
    void sendCover(BridgeClient *only = nullptr);
    void onPlayers();
    void showCover(const QString &artUrl);
    void schedule(Load kind);
    void startLoad();
    void decodeAsync(std::function<std::optional<Cover>()> job, const QUrl &remember);
    void runJob();
    void onDecoded(quint64 token, const QUrl &remember, const std::optional<Cover> &cover);
    void applyActive();
    void onFetched(const QUrl &url, const QByteArray &body);
    void remember(const QUrl &url, const std::optional<Cover> &cover);
    void setCover(std::optional<Cover> cover);
    void publishThumbnail();

    MediaWatcher m_watcher;
    ArtFetcher m_fetcher;
    QTimer m_stateTimer;
    QTimer m_thumbnailTimer;
    QTimer m_loadTimer;
    QElapsedTimer m_thumbnailClock;
    QElapsedTimer m_fileClock;
    QElapsedTimer m_fetchClock;
    QByteArray m_stateJson;
    QByteArray m_sentJson;
    QString m_artUrl;                     // the one in view
    std::optional<Cover> m_cover;         // what it gave
    uint32_t m_generation = 0;
    uint32_t m_sentGeneration = 0;
    Load m_pendingKind = Load::None;      // a cover waiting for its turn, and which
    QString m_pendingPath;
    QUrl m_pendingUrl;
    // changes with every new cover wanted: an answer for an older one is dropped. Shared with the worker, which touches nothing else of ours.
    std::shared_ptr<std::atomic<quint64>> m_loadToken = std::make_shared<std::atomic<quint64>>(0);
    bool m_loading = false;               // a cover is scheduled, downloading, or being decoded
    struct Job {
        std::function<std::optional<Cover>()> run;
        QUrl remember;
        quint64 token = 0;
    };
    std::optional<Job> m_queued;          // the last cover asked for while a job runs (one job at a time: a hung read holds no more)
    bool m_jobRunning = false;
    QTimer m_activeTimer;                 // MediaWants changes are applied at most once a second
    bool m_activePending = false;
    int m_decodeDelayMs = 0;
    int m_loads = 0;
    std::vector<QPointer<BridgeClient>> m_clients;
    struct Cached {
        QUrl url;
        std::optional<Cover> cover;
    };
    std::list<Cached> m_cache;
    Tri m_share = Tri::Unknown;
    Tri m_remoteArt = Tri::Unknown;
    int m_downloads = 0;
    QThreadPool m_pool; // ours, one thread; declared last, so it is gone (its worker waited for) before anything the worker answers to
};

} // namespace murasaki::media
