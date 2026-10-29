#include "testutil.h"
#include "workshopmetadataclient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <functional>

using namespace Qt::StringLiterals;
using namespace murasaki;

// A minimal HTTP/1.1 server on 127.0.0.1 standing in for Steam's API and CDN.
class FakeSteam : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray body;
        qint64 at = 0; // ms since the server started
    };
    struct Response {
        Response(int s = 200, QByteArray b = {}, QByteArray type = "application/json", QByteArray headers = {},
                 bool h = false)
            : status(s), body(std::move(b)), contentType(std::move(type)), extraHeaders(std::move(headers)), hang(h)
        {
        }
        static Response hanging() { return Response(200, {}, {}, {}, true); }
        int status;
        QByteArray body;
        QByteArray contentType;
        QByteArray extraHeaders;
        bool hang; // never answer
    };

    FakeSteam()
    {
        m_clock.start();
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    void close() { m_server.close(); }
    quint16 port() const { return m_server.serverPort(); }
    QUrl url(const QString &path) const { return QUrl(u"http://127.0.0.1:%1%2"_s.arg(port()).arg(path)); }

    std::function<Response(const Request &)> handler;
    QList<Request> requests;

    int count(const QByteArray &method, const QByteArray &pathPrefix = {}) const
    {
        int n = 0;
        for (const Request &r : requests)
            n += r.method == method && r.path.startsWith(pathPrefix);
        return n;
    }

private:
    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> first = lines.at(0).trimmed().split(' ');
        qsizetype length = 0;
        for (const QByteArray &line : lines) {
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < headerEnd + 4 + length)
            return;
        Request request{first.value(0), first.value(1), buffer.mid(headerEnd + 4, length), m_clock.elapsed()};
        m_buffers.remove(socket);
        requests.append(request);
        const Response response = handler ? handler(request) : Response{404, "{}"};
        if (response.hang)
            return;
        QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: " + response.contentType
            + "\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n"
            + response.extraHeaders + "\r\n" + response.body;
        socket->write(out);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QElapsedTimer m_clock;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

static const QByteArray kJpeg = QByteArray("\xFF\xD8\xFF\xE0", 4) + "fake jpeg bytes";
static const QByteArray kPng = QByteArray("\x89PNG\r\n\x1a\n", 8) + "fake png bytes";

class TestWorkshopMetadata : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void fetchesBatchedKeylessAndCaches();
    void secondRunServesTheCacheWithoutRequests();
    void newTimeUpdatedRefetchesAndReplacesPreview();
    void unchangedTimeUpdatedKeepsPreview();
    void splitsLargeBatchesWithAMinimumGap();
    void offlineThenOnlineRecovers();
    void rateLimitHonoursRetryAfter();
    void unknownItemsAreNotAskedAgainWithinTheTtl();
    void badPreviewsAreIgnored();
    void hangingServerTimesOut();
    void nonNumericIdsAreIgnored();
    void endpointOverrideFromEnvironment();
    void garbageCacheFileIsIgnored();
    void unexpectedBodiesDoNotPoisonTheCache();
    void oversizedApiResponseIsRejected();
    void insecurePreviewUrlsAreNotFetched();
    void previewUrlAllowlist();
    void previewRedirectsToOtherHostsAreRefused();
    void pruneDeletesOrphanedPreviews();

private:
    static QByteArray details(const QList<std::tuple<QString, int, QString, QStringList, QString>> &items)
    {
        QJsonArray array;
        for (const auto &[id, result, title, tags, preview] : items) {
            QJsonArray tagArray;
            for (const QString &tag : tags)
                tagArray.append(QJsonObject{{u"tag"_s, tag}, {u"display_name"_s, tag}});
            array.append(QJsonObject{{u"publishedfileid"_s, id},
                                     {u"result"_s, result},
                                     {u"title"_s, title},
                                     {u"tags"_s, tagArray},
                                     {u"time_updated"_s, 1000},
                                     {u"preview_url"_s, preview}});
        }
        return QJsonDocument(QJsonObject{{u"response"_s, QJsonObject{{u"result"_s, 1},
                                                                     {u"resultcount"_s, int(array.size())},
                                                                     {u"publishedfiledetails"_s, array}}}})
            .toJson(QJsonDocument::Compact);
    }
    WorkshopMetadataClient::Options fast(const QUrl &endpoint) const
    {
        WorkshopMetadataClient::Options o;
        o.endpoint = endpoint;
        o.minGapMs = 0;
        o.retryInitialMs = 50;
        o.retryMaxMs = 200;
        o.requestTimeoutMs = 2000;
        return o;
    }
    // The fake serves id 11 and 22 with previews on /img/<id>.
    void serveTwo()
    {
        m_steam.handler = [this](const FakeSteam::Request &r) -> FakeSteam::Response {
            if (r.method == "POST") {
                return {200,
                        details({{u"11"_s, 1, u"Teto"_s, {u"Pet"_s, u"Anime"_s}, m_steam.url(u"/img/11"_s).toString()},
                                 {u"22"_s, 1, u"Miku"_s, {u"Pet"_s}, m_steam.url(u"/img/22"_s).toString()}})};
            }
            return {200, m_previewBody, "image/jpeg"};
        };
    }

    FakeSteam m_steam;
    QTemporaryDir *m_tmp = nullptr;
    QByteArray m_previewBody = kJpeg;
};

void TestWorkshopMetadata::init()
{
    delete m_tmp;
    m_tmp = new QTemporaryDir;
    m_steam.requests.clear();
    m_steam.handler = nullptr;
    m_previewBody = kJpeg;
    if (!m_steam.url(u"/"_s).port())
        QVERIFY(m_steam.listen());
}

void TestWorkshopMetadata::fetchesBatchedKeylessAndCaches()
{
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    QSignalSpy spy(&client, &WorkshopMetadataClient::metadataChanged);

    QVERIFY(!client.metadata(u"11"_s));
    client.refresh({u"11"_s, u"22"_s, u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"22"_s) && !client.metadata(u"22"_s)->previewPath.isEmpty()
                                         && !client.metadata(u"11"_s)->previewPath.isEmpty(); }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return !client.busy(); }, 5000));

    QCOMPARE(m_steam.count("POST"), 1); // one batch for both ids
    const auto &post = m_steam.requests.first();
    QCOMPARE(post.path, QByteArray("/api"));
    const QUrlQuery form{QString::fromUtf8(post.body)};
    QCOMPARE(form.queryItemValue(u"itemcount"_s), u"2"_s);
    QCOMPARE(form.queryItemValue(u"publishedfileids%5B0%5D"_s), u"11"_s);
    QCOMPARE(form.queryItemValue(u"publishedfileids%5B1%5D"_s), u"22"_s);
    QVERIFY(!post.body.toLower().contains("key")); // never a key

    const auto teto = client.metadata(u"11"_s);
    QCOMPARE(teto->title, u"Teto"_s);
    QCOMPARE(teto->tags, QStringList({u"Pet"_s, u"Anime"_s}));
    QCOMPARE(teto->previewPath, m_tmp->filePath(u"murasaki/previews/11.jpg"_s));
    QCOMPARE(testutil::canonical(teto->previewPath), testutil::canonical(m_tmp->filePath(u"murasaki/previews/11.jpg"_s)));
    QFile preview(teto->previewPath);
    QVERIFY(preview.open(QIODevice::ReadOnly));
    QCOMPARE(preview.readAll(), kJpeg);

    QVERIFY(QTest::qWaitFor([&] { return spy.count() >= 1; }, 3000));
    QStringList all;
    for (const auto &args : spy)
        all += args.at(0).toStringList();
    QVERIFY(all.contains(u"11"_s) && all.contains(u"22"_s));
    QVERIFY(QFile::exists(m_tmp->filePath(u"murasaki/workshop-meta.json"_s)));
}

void TestWorkshopMetadata::secondRunServesTheCacheWithoutRequests()
{
    serveTwo();
    {
        WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
        client.setOptions(fast(m_steam.url(u"/api"_s)));
        client.refresh({u"11"_s, u"22"_s});
        QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"22"_s) && !client.metadata(u"22"_s)->previewPath.isEmpty()
                                             && !client.metadata(u"11"_s)->previewPath.isEmpty() && !client.busy(); }, 5000));
    }
    const int before = m_steam.requests.size();

    WorkshopMetadataClient again(m_tmp->filePath(u"murasaki"_s));
    again.setOptions(fast(m_steam.url(u"/api"_s)));
    QCOMPARE(again.metadata(u"11"_s)->title, u"Teto"_s); // available before any network
    QVERIFY(!again.metadata(u"11"_s)->previewPath.isEmpty());
    again.refresh({u"11"_s, u"22"_s});
    QTest::qWait(300);
    QCOMPARE(m_steam.requests.size(), before);
}

void TestWorkshopMetadata::newTimeUpdatedRefetchesAndReplacesPreview()
{
    int version = 1000;
    m_steam.handler = [&](const FakeSteam::Request &r) -> FakeSteam::Response {
        if (r.method == "POST") {
            QJsonObject d{{u"publishedfileid"_s, u"11"_s}, {u"result"_s, 1}, {u"title"_s, version == 1000 ? u"Old"_s : u"New"_s},
                          {u"time_updated"_s, version}, {u"preview_url"_s, m_steam.url(u"/img/11"_s).toString()}};
            return {200, QJsonDocument(QJsonObject{{u"response"_s, QJsonObject{{u"publishedfiledetails"_s, QJsonArray{d}}}}})
                             .toJson()};
        }
        return {200, version == 1000 ? kJpeg : kPng, version == 1000 ? "image/jpeg" : "image/png"};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    auto options = fast(m_steam.url(u"/api"_s));
    options.ttlSeconds = 0; // always ask
    client.setOptions(options);
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s) && !client.metadata(u"11"_s)->previewPath.isEmpty() && !client.busy(); }, 5000));
    QVERIFY(client.metadata(u"11"_s)->previewPath.endsWith(u"11.jpg"_s));

    version = 2000;
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s)->title == u"New"_s && client.metadata(u"11"_s)->previewPath.endsWith(u"11.png"_s); }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return !client.busy(); }, 5000));
    QVERIFY(!QFile::exists(m_tmp->filePath(u"murasaki/previews/11.jpg"_s))); // the stale one is gone
    QCOMPARE(m_steam.count("GET"), 2);
}

void TestWorkshopMetadata::unchangedTimeUpdatedKeepsPreview()
{
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    auto options = fast(m_steam.url(u"/api"_s));
    options.ttlSeconds = 0;
    client.setOptions(options);
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s) && !client.metadata(u"11"_s)->previewPath.isEmpty() && !client.busy(); }, 5000));
    QCOMPARE(m_steam.count("GET"), 1);

    QTest::qWait(400); // let the coalesced notification of the first fetch go out
    QSignalSpy spy(&client, &WorkshopMetadataClient::metadataChanged);
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return m_steam.count("POST") == 2 && !client.busy(); }, 5000));
    QTest::qWait(400);
    QCOMPARE(m_steam.count("GET"), 1); // the preview is downloaded once per version
    QCOMPARE(spy.count(), 0);          // and nothing changed, so nobody is told
}

void TestWorkshopMetadata::splitsLargeBatchesWithAMinimumGap()
{
    m_steam.handler = [](const FakeSteam::Request &) -> FakeSteam::Response { return {200, details({})}; };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    auto options = fast(m_steam.url(u"/api"_s));
    options.batchSize = 2;
    options.minGapMs = 300;
    client.setOptions(options);
    client.refresh({u"1"_s, u"2"_s, u"3"_s});
    QVERIFY(QTest::qWaitFor([&] { return m_steam.count("POST") == 2 && !client.busy(); }, 5000));
    QVERIFY2(m_steam.requests.at(1).at - m_steam.requests.at(0).at >= 290,
             qPrintable(QString::number(m_steam.requests.at(1).at - m_steam.requests.at(0).at)));
    QUrlQuery first{QString::fromUtf8(m_steam.requests.at(0).body)};
    QCOMPARE(first.queryItemValue(u"itemcount"_s), u"2"_s);
    QUrlQuery second{QString::fromUtf8(m_steam.requests.at(1).body)};
    QCOMPARE(second.queryItemValue(u"itemcount"_s), u"1"_s);
}

void TestWorkshopMetadata::offlineThenOnlineRecovers()
{
    // Reserve a port nobody listens on.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 deadPort = probe.serverPort();
    probe.close();

    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    auto options = fast(QUrl(u"http://127.0.0.1:%1/api"_s.arg(deadPort)));
    client.setOptions(options);
    client.refresh({u"11"_s});
    QTest::qWait(300);
    QVERIFY(!client.metadata(u"11"_s)); // nothing, and nothing crashed
    QVERIFY(client.busy());             // still retrying

    FakeSteam late;
    QVERIFY(late.listen());
    late.handler = [&late](const FakeSteam::Request &) -> FakeSteam::Response {
        return {200, details({{u"11"_s, 1, u"Teto"_s, {}, QString()}})};
    };
    options.endpoint = late.url(u"/api"_s);
    client.setOptions(options); // the next retry goes to the live server
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 5000));
    QCOMPARE(client.metadata(u"11"_s)->title, u"Teto"_s);
}

void TestWorkshopMetadata::rateLimitHonoursRetryAfter()
{
    m_steam.handler = [](const FakeSteam::Request &r) -> FakeSteam::Response {
        static int calls = 0;
        Q_UNUSED(r)
        if (++calls == 1)
            return {429, "{}", "application/json", "Retry-After: 1\r\n"};
        return {200, details({{u"11"_s, 1, u"Teto"_s, {}, QString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 8000));
    QCOMPARE(m_steam.count("POST"), 2);
    QVERIFY2(m_steam.requests.at(1).at - m_steam.requests.at(0).at >= 950,
             qPrintable(QString::number(m_steam.requests.at(1).at - m_steam.requests.at(0).at)));
}

void TestWorkshopMetadata::unknownItemsAreNotAskedAgainWithinTheTtl()
{
    m_steam.handler = [](const FakeSteam::Request &) -> FakeSteam::Response {
        return {200, details({{u"11"_s, 9, QString(), {}, QString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s, u"33"_s}); // 33 is left out of the answer altogether
    QVERIFY(QTest::qWaitFor([&] { return m_steam.count("POST") == 1 && !client.busy(); }, 5000));
    QVERIFY(!client.metadata(u"11"_s));
    QVERIFY(!client.metadata(u"33"_s));
    client.refresh({u"11"_s, u"33"_s});
    QTest::qWait(300);
    QCOMPARE(m_steam.count("POST"), 1);
}

void TestWorkshopMetadata::badPreviewsAreIgnored()
{
    m_previewBody = "<html>not an image</html>";
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value() && m_steam.count("GET") >= 1 && !client.busy(); }, 5000));
    QCOMPARE(client.metadata(u"11"_s)->title, u"Teto"_s);
    QVERIFY(client.metadata(u"11"_s)->previewPath.isEmpty());
    QVERIFY(!QDir(m_tmp->filePath(u"murasaki/previews"_s)).exists()
            || QDir(m_tmp->filePath(u"murasaki/previews"_s)).isEmpty());

    // Oversized: aborted rather than stored.
    m_previewBody = kJpeg + QByteArray(5000, 'x');
    auto options = fast(m_steam.url(u"/api"_s));
    options.maxPreviewBytes = 1000;
    options.ttlSeconds = 0;
    client.setOptions(options);
    client.refresh({u"22"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"22"_s).has_value() && !client.busy(); }, 5000));
    QVERIFY(client.metadata(u"22"_s)->previewPath.isEmpty());
}

void TestWorkshopMetadata::hangingServerTimesOut()
{
    int calls = 0;
    m_steam.handler = [&](const FakeSteam::Request &) -> FakeSteam::Response {
        if (++calls == 1)
            return FakeSteam::Response::hanging();
        return {200, details({{u"11"_s, 1, u"Teto"_s, {}, QString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    auto options = fast(m_steam.url(u"/api"_s));
    options.requestTimeoutMs = 300;
    client.setOptions(options);
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 8000));
    QCOMPARE(m_steam.count("POST"), 2);
}

void TestWorkshopMetadata::nonNumericIdsAreIgnored()
{
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"local"_s, u"../x"_s, QString(), u"1 2"_s});
    QTest::qWait(200);
    QCOMPARE(m_steam.requests.size(), 0);
    QVERIFY(!client.busy());
}

void TestWorkshopMetadata::endpointOverrideFromEnvironment()
{
    qunsetenv(kWorkshopApiUrlEnv);
    QCOMPARE(WorkshopMetadataClient::defaultOptions().endpoint.toString(), QString(kSteamPublishedFileDetailsUrl));
    qputenv(kWorkshopApiUrlEnv, "http://127.0.0.1:9/x");
    QCOMPARE(WorkshopMetadataClient::defaultOptions().endpoint.toString(), u"http://127.0.0.1:9/x"_s);
    qunsetenv(kWorkshopApiUrlEnv);
}

void TestWorkshopMetadata::garbageCacheFileIsIgnored()
{
    testutil::writeFile(m_tmp->filePath(u"murasaki/workshop-meta.json"_s), "{{{{");
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    QVERIFY(!client.metadata(u"11"_s));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return !client.busy(); }, 5000));
    QFile file(m_tmp->filePath(u"murasaki/workshop-meta.json"_s));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(saved.value(u"version"_s).toInt(), 1);
}

void TestWorkshopMetadata::unexpectedBodiesDoNotPoisonTheCache()
{
    int calls = 0;
    m_steam.handler = [&](const FakeSteam::Request &) -> FakeSteam::Response {
        ++calls;
        if (calls == 1)
            return {200, "<html>captive portal</html>"};
        if (calls == 2)
            return {200, "{\"response\": {\"result\": 1}}"}; // no publishedfiledetails
        if (calls == 3)
            return {200, "[1, 2]"};
        return {200, details({{u"11"_s, 1, u"Teto"_s, {}, QString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 8000));
    QCOMPARE(m_steam.count("POST"), 4); // three bad answers were retried, none was cached as "unknown"
    QCOMPARE(client.metadata(u"11"_s)->title, u"Teto"_s);
}

void TestWorkshopMetadata::oversizedApiResponseIsRejected()
{
    int calls = 0;
    m_steam.handler = [&](const FakeSteam::Request &) -> FakeSteam::Response {
        if (++calls == 1)
            return {200, "{\"response\": {\"publishedfiledetails\": [], \"pad\": \"" + QByteArray(5 * 1024 * 1024, 'x') + "\"}}"};
        return {200, details({{u"11"_s, 1, u"Teto"_s, {}, QString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value(); }, 8000));
    QCOMPARE(m_steam.count("POST"), 2);
}

void TestWorkshopMetadata::insecurePreviewUrlsAreNotFetched()
{
    m_steam.handler = [](const FakeSteam::Request &) -> FakeSteam::Response {
        // Plain http to a non-loopback host, and a non-http scheme: neither may be requested.
        return {200, details({{u"11"_s, 1, u"A"_s, {}, u"http://192.0.2.1/p.jpg"_s},
                              {u"22"_s, 1, u"B"_s, {}, u"ftp://192.0.2.1/p.jpg"_s}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s, u"22"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"22"_s).has_value(); }, 5000));
    QVERIFY(!client.busy()); // no preview download was even started
    QVERIFY(client.metadata(u"11"_s)->previewPath.isEmpty());
    QCOMPARE(m_steam.count("GET"), 0);
}

void TestWorkshopMetadata::previewUrlAllowlist()
{
    const auto ok = [](const char *url, bool loopback = false) {
        return WorkshopMetadataClient::previewUrlAllowed(QUrl(QString::fromLatin1(url)), loopback);
    };
    QVERIFY(ok("https://images.steamusercontent.com/ugc/1/2/"));
    QVERIFY(ok("https://steamusercontent.com/x.jpg"));
    QVERIFY(ok("https://cdn.akamaihd.net/x.jpg"));
    QVERIFY(ok("https://steamcdn-a.akamaihd.net/x.jpg"));
    QVERIFY(ok("https://community.cloudflare.steamstatic.com/x.jpg"));
    QVERIFY(ok("https://IMAGES.STEAMSTATIC.COM/x.jpg"));
    QVERIFY(!ok("http://images.steamusercontent.com/x.jpg"));
    QVERIFY(!ok("https://evil.example/x.jpg"));
    QVERIFY(!ok("https://notsteamusercontent.com/x.jpg"));
    QVERIFY(!ok("https://steamusercontent.com.evil.example/x.jpg"));
    QVERIFY(!ok("https://user@steamusercontent.com/x.jpg"));
    QVERIFY(!ok("https://steamusercontent.com:8443/x.jpg"));
    QVERIFY(!ok("https://192.0.2.1/x.jpg"));
    QVERIFY(!ok("https://[2001:db8::1]/x.jpg"));
    QVERIFY(!ok("https://127.0.0.1/x.jpg"));
    QVERIFY(!ok("ftp://images.steamusercontent.com/x.jpg"));
    QVERIFY(!ok("http://127.0.0.1:8080/p.jpg"));
    QVERIFY(ok("http://127.0.0.1:8080/p.jpg", true));
    QVERIFY(ok("http://localhost:8080/p.jpg", true));
    QVERIFY(!ok("http://192.0.2.1/p.jpg", true));
}

void TestWorkshopMetadata::previewRedirectsToOtherHostsAreRefused()
{
    m_steam.handler = [this](const FakeSteam::Request &request) -> FakeSteam::Response {
        if (request.path.startsWith("/img/"))
            return {302, {}, "text/plain", "Location: http://192.0.2.1/p.jpg\r\n"};
        return {200, details({{u"11"_s, 1, u"A"_s, {}, m_steam.url(u"/img/11"_s).toString()}})};
    };
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"11"_s).has_value() && !client.busy(); }, 5000));
    QTest::qWait(100);
    QVERIFY(client.metadata(u"11"_s)->previewPath.isEmpty());
}

void TestWorkshopMetadata::pruneDeletesOrphanedPreviews()
{
    serveTwo();
    WorkshopMetadataClient client(m_tmp->filePath(u"murasaki"_s));
    client.setOptions(fast(m_steam.url(u"/api"_s)));
    client.refresh({u"11"_s, u"22"_s});
    QVERIFY(QTest::qWaitFor([&] { return client.metadata(u"22"_s) && !client.metadata(u"22"_s)->previewPath.isEmpty()
                                         && !client.metadata(u"11"_s)->previewPath.isEmpty() && !client.busy(); }, 5000));
    testutil::writeFile(m_tmp->filePath(u"murasaki/previews/999.png"_s), "stray");

    client.prune({u"11"_s});
    QVERIFY(client.metadata(u"11"_s).has_value());
    QVERIFY(!client.metadata(u"22"_s).has_value());
    QVERIFY(QFile::exists(m_tmp->filePath(u"murasaki/previews/11.jpg"_s)));
    QVERIFY(!QFile::exists(m_tmp->filePath(u"murasaki/previews/22.jpg"_s)));
    QVERIFY(!QFile::exists(m_tmp->filePath(u"murasaki/previews/999.png"_s)));

    // And it is persisted: a new client does not know item 22 either.
    WorkshopMetadataClient again(m_tmp->filePath(u"murasaki"_s));
    QVERIFY(again.metadata(u"11"_s).has_value());
    QVERIFY(!again.metadata(u"22"_s).has_value());
}

QTEST_GUILESS_MAIN(TestWorkshopMetadata)
#include "tst_workshopmetadata.moc"
