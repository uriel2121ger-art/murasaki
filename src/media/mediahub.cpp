#include "media/mediahub.h"

#include "bridge/qml/bridgeclient.h"

#include <QCoreApplication>
#include <QThread>
#include <QThreadPool>

#include <algorithm>

namespace murasaki::media {

MediaHub::MediaHub(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_watcher(bus, this)
    , m_fetcher(this)
{
    m_pool.setMaxThreadCount(1);
    m_activeTimer.setSingleShot(true);
    m_activeTimer.setInterval(1000);
    connect(&m_activeTimer, &QTimer::timeout, this, [this] {
        if (std::exchange(m_activePending, false)) {
            applyActive();
        }
    });
    m_stateJson = mediaJson(nullptr);
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(kStateDelayMs);
    connect(&m_stateTimer, &QTimer::timeout, this, [this] {
        if (m_stateJson != m_sentJson) {
            m_sentJson = m_stateJson;
            emit stateChanged(m_stateJson);
            sendState();
        }
    });
    m_thumbnailTimer.setSingleShot(true);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, &MediaHub::publishThumbnail);
    m_loadTimer.setSingleShot(true);
    connect(&m_loadTimer, &QTimer::timeout, this, &MediaHub::startLoad);
    connect(&m_watcher, &MediaWatcher::playersChanged, this, &MediaHub::onPlayers);
    connect(&m_fetcher, &ArtFetcher::finished, this, &MediaHub::onFetched);
}

MediaHub::~MediaHub()
{
    m_loadToken->fetch_add(1); // (the pool, the last member, waits for a worker still decoding)
}

MediaHub &MediaHub::shared()
{
    static MediaHub *hub = new MediaHub(QDBusConnection::sessionBus(), QCoreApplication::instance());
    return *hub;
}

void MediaHub::setShare(bool on)
{
    const Tri now = on ? Tri::On : Tri::Off;
    if (m_share == now) {
        return;
    }
    m_share = now;
    applyActive(); // a withdrawn permission is not held back
}

void MediaHub::setRemoteArt(bool on)
{
    const Tri now = on ? Tri::On : Tri::Off;
    if (m_remoteArt == now) {
        return;
    }
    m_remoteArt = now;
    if (!on) {
        m_fetcher.cancel();
        if (m_pendingKind == Load::Https) {
            m_pendingKind = Load::None;
            m_loadTimer.stop();
        }
    }
    const QString url = m_artUrl;
    m_artUrl.clear(); // read again under the new setting
    if (m_watcher.running()) {
        showCover(url);
    }
}

int MediaHub::wanting() const
{
    return static_cast<int>(std::count_if(m_clients.begin(), m_clients.end(), [](const QPointer<BridgeClient> &c) { return c && c->wantsMedia(); }));
}

void MediaHub::attach(BridgeClient *client)
{
    if (!client || !client->supportsMedia()) {
        return;
    }
    std::erase_if(m_clients, [](const QPointer<BridgeClient> &c) { return c.isNull(); });
    m_clients.emplace_back(client);
    connect(client, &BridgeClient::mediaWanted, this, [this, client](bool wants) {
        refreshActive();
        if (wants) {
            sendState(client); // what is known now; the changes follow
            sendCover(client);
        }
    });
    connect(client, &QObject::destroyed, this, [this] { refreshActive(); });
    if (client->wantsMedia()) {
        refreshActive();
        sendState(client);
        sendCover(client);
    }
}

// A scene that flips its wish back and forth is followed at most once a second.
void MediaHub::refreshActive()
{
    if (m_activeTimer.isActive()) {
        m_activePending = true;
        return;
    }
    m_activeTimer.start();
    applyActive();
}

// The players are read only while the setting says yes and some scene uses them.
void MediaHub::applyActive()
{
    const bool want = m_share == Tri::On && wanting() > 0;
    if (want && !m_watcher.running()) {
        m_watcher.start();
    } else if (!want && m_watcher.running()) {
        m_watcher.stop(); // (publishes: no player, then the state goes back to disabled)
    }
    if (m_share != Tri::On) {
        // not shared: the scenes are told nothing plays, whatever did
        m_stateJson = mediaJson(nullptr);
        m_artUrl.clear();
        m_fetcher.cancel();
        m_pendingKind = Load::None;
        m_loadTimer.stop();
        m_loadToken->fetch_add(1);
        m_loading = false;
        m_cover = Cover{};
        ++m_generation;
        sendState();
        sendCover();
        m_sentJson = m_stateJson;
        m_sentGeneration = m_generation;
    }
}

void MediaHub::sendState(BridgeClient *only)
{
    for (const QPointer<BridgeClient> &c : m_clients) {
        if (c && c->wantsMedia() && (!only || c == only)) {
            c->sendMedia(m_share == Tri::On ? m_stateJson : mediaJson(nullptr));
        }
    }
}

void MediaHub::sendCover(BridgeClient *only)
{
    for (const QPointer<BridgeClient> &c : m_clients) {
        if (!c || !c->wantsMedia() || (only && c != only)) {
            continue;
        }
        if (m_share == Tri::On && m_cover) {
            c->sendMediaThumbnail(m_cover->width, m_cover->height, m_cover->rgba, m_generation);
        } else {
            c->sendMediaThumbnail(0, 0, {}, m_generation);
        }
    }
}

void MediaHub::onPlayers()
{
    if (m_share != Tri::On) {
        return;
    }
    const std::vector<PlayerSnapshot> &players = m_watcher.players();
    const int chosen = choosePlayer(players);
    const PlayerSnapshot *player = chosen >= 0 ? &players[static_cast<size_t>(chosen)] : nullptr;
    m_stateJson = mediaJson(player);
    if (!m_stateTimer.isActive()) {
        m_stateTimer.start();
    }
    showCover(player ? player->artUrl : QString());
}

void MediaHub::showCover(const QString &artUrl)
{
    if (artUrl == m_artUrl && (m_cover || m_loading)) {
        return; // in view, or on its way (scheduled, downloading or decoding): the same address republished does not start it over
    }
    m_artUrl = artUrl;
    m_fetcher.cancel();
    m_pendingKind = Load::None;
    m_loadTimer.stop();
    m_loadToken->fetch_add(1); // whatever a worker was decoding is for another cover
    m_loading = false;
    const ArtSource source = classifyArtUrl(artUrl);
    switch (source.kind) {
    case ArtSource::Kind::None:
        setCover(Cover{}); // no cover
        return;
    case ArtSource::Kind::File:
        m_pendingPath = source.path;
        m_loading = true;
        schedule(Load::File);
        return;
    case ArtSource::Kind::Https:
        break;
    }
    if (m_remoteArt != Tri::On) {
        setCover(Cover{}); // not downloaded: off, or not known yet
        return;
    }
    for (const Cached &c : m_cache) {
        if (c.url == source.url) {
            setCover(c.cover ? c.cover : std::optional<Cover>(Cover{}));
            return;
        }
    }
    // the previous track's cover stays up until the new one arrives
    m_pendingUrl = source.url;
    m_loading = true;
    schedule(Load::Https);
}

// A new cover is read at most every kMinFileGapMs (a file) or kMinFetchGapMs (a download).
void MediaHub::schedule(Load kind)
{
    m_pendingKind = kind;
    const QElapsedTimer &clock = kind == Load::File ? m_fileClock : m_fetchClock;
    const qint64 gap = kind == Load::File ? kMinFileGapMs : kMinFetchGapMs;
    const qint64 wait = clock.isValid() ? gap - clock.elapsed() : 0;
    m_loadTimer.start(static_cast<int>(std::max<qint64>(0, wait)));
}

void MediaHub::startLoad()
{
    const Load kind = std::exchange(m_pendingKind, Load::None);
    if (kind == Load::File) {
        m_fileClock.start();
        const QString path = m_pendingPath;
        ++m_loads;
        decodeAsync([path] { return loadArtFile(path); }, QUrl());
    } else if (kind == Load::Https && !m_pendingUrl.isEmpty() && m_remoteArt == Tri::On && m_share == Tri::On) {
        m_fetchClock.start();
        ++m_downloads;
        ++m_loads;
        m_fetcher.fetch(std::exchange(m_pendingUrl, QUrl()));
    }
}

// The decoding is on a worker thread of our own pool, one job at a time: a read that hangs (a dead mount) holds that one and the cover
// asked for last waits behind it. Its answer counts only if no other cover has been wanted since. The worker touches only the token.
void MediaHub::decodeAsync(std::function<std::optional<Cover>()> job, const QUrl &remember)
{
    m_queued = Job{std::move(job), remember, m_loadToken->fetch_add(1) + 1};
    if (!m_jobRunning) {
        runJob();
    }
}

void MediaHub::runJob()
{
    if (!m_queued) {
        return;
    }
    Job job = std::move(*m_queued);
    m_queued.reset();
    m_jobRunning = true;
    QPointer<MediaHub> self(this);
    const std::shared_ptr<std::atomic<quint64>> tokenBox = m_loadToken;
    const int delay = m_decodeDelayMs;
    m_pool.start([self, tokenBox, job = std::move(job), delay] {
        if (delay > 0) {
            QThread::msleep(static_cast<unsigned long>(delay));
        }
        std::optional<Cover> cover;
        if (tokenBox->load() == job.token) { // (an older cover is not worth decoding)
            cover = job.run();
        }
        const bool current = tokenBox->load() == job.token;
        // the answer goes to the GUI thread, which checks that the hub is still there
        QMetaObject::invokeMethod(
            self.data(), [self, token = job.token, remember = job.remember, cover = std::move(cover), current] {
                if (!self) {
                    return;
                }
                self->m_jobRunning = false;
                self->onDecoded(token, remember, current ? cover : std::optional<Cover>());
                self->runJob();
            },
            Qt::QueuedConnection);
    });
}

void MediaHub::onDecoded(quint64 token, const QUrl &url, const std::optional<Cover> &cover)
{
    if (!url.isEmpty() && cover) {
        remember(url, cover); // (a download is worth keeping even if the track has moved on)
    }
    if (token != m_loadToken->load() || m_share != Tri::On) {
        return;
    }
    if (!url.isEmpty() && !cover) {
        remember(url, std::nullopt);
    }
    setCover(cover ? cover : std::optional<Cover>(Cover{}));
}

void MediaHub::onFetched(const QUrl &url, const QByteArray &body)
{
    if (classifyArtUrl(m_artUrl).url != url) {
        return; // the track changed while it came
    }
    if (body.isEmpty()) {
        remember(url, std::nullopt);
        setCover(Cover{});
        return;
    }
    decodeAsync([body] { return decodeArt(body); }, url);
}

void MediaHub::remember(const QUrl &url, const std::optional<Cover> &cover)
{
    std::erase_if(m_cache, [&](const Cached &c) { return c.url == url; });
    m_cache.push_front({url, cover});
    while (m_cache.size() > static_cast<size_t>(kCoverCacheSize)) {
        m_cache.pop_back();
    }
}

void MediaHub::setCover(std::optional<Cover> cover)
{
    m_loading = false;
    m_cover = std::move(cover);
    ++m_generation;
    const qint64 wait = m_thumbnailClock.isValid() ? kThumbnailGapMs - m_thumbnailClock.elapsed() : 0;
    if (wait > 0) {
        m_thumbnailTimer.start(static_cast<int>(wait));
    } else {
        publishThumbnail();
    }
}

void MediaHub::publishThumbnail()
{
    if (!m_cover || m_generation == m_sentGeneration) {
        return;
    }
    m_sentGeneration = m_generation;
    m_thumbnailClock.start();
    emit thumbnailChanged(m_cover->width, m_cover->height, m_cover->rgba, m_generation);
    sendCover();
}

} // namespace murasaki::media
