#include "playlist.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPlaylistStore : public QObject
{
    Q_OBJECT

private slots:
    void itemIdHelpers();
    void defaultsAndValidation();
    void applyChangesMergesAndValidates();
    void createAssignsUuidAndPersists();
    void updateAndRemoveSignalOnlyOnChange();
    void missingFileIsEmpty();
    void corruptFileIsKeptAsBackup();
    void invalidEntryIsKeptAsBackup();
    void duplicateIdIsKeptAsBackup();
    void limitIsEnforced();
    void containsItem();

private:
    static QStringList backups(const QTemporaryDir &tmp)
    {
        return QDir(tmp.path()).entryList({u"playlists.json.bad-*"_s}, QDir::Files);
    }
};

void TestPlaylistStore::itemIdHelpers()
{
    QCOMPARE(playlistItemId(u"abc"_s), u"playlist:abc"_s);
    QCOMPARE(playlistIdOf(u"playlist:abc"_s), u"abc"_s);
    QCOMPARE(playlistIdOf(u"we:123"_s), QString());
    QCOMPARE(playlistIdOf(u"playlist:"_s), QString());
    QVERIFY(isPlaylistItemId(u"playlist:x"_s));
    QVERIFY(!isPlaylistItemId(u"we:1"_s));
}

void TestPlaylistStore::defaultsAndValidation()
{
    Playlist p;
    p.id = u"id"_s;
    p.name = u"Mine"_s;
    QCOMPARE(p.order, PlaylistOrder::Sequential);
    QCOMPARE(p.intervalMinutes, 15);
    QVERIFY(!p.changeOnLogin);
    QVERIFY(validatePlaylist(p));

    QString error;
    Playlist bad = p;
    bad.name = u"  "_s;
    QVERIFY(!validatePlaylist(bad, &error));
    QVERIFY(error.contains(u"name"_s));
    bad = p;
    bad.name = QString(101, u'x');
    QVERIFY(!validatePlaylist(bad));
    bad = p;
    bad.intervalMinutes = 0;
    QVERIFY(!validatePlaylist(bad, &error));
    QVERIFY(error.contains(u"intervalMinutes"_s));
    bad.intervalMinutes = 1441;
    QVERIFY(!validatePlaylist(bad));
    bad.intervalMinutes = 1;
    QVERIFY(validatePlaylist(bad));
    bad.intervalMinutes = 1440;
    QVERIFY(validatePlaylist(bad));
    bad = p;
    bad.items = {u"we:1"_s, u"we:1"_s};
    QVERIFY(!validatePlaylist(bad, &error));
    QVERIFY(error.contains(u"duplicate"_s));
    bad.items = {QString()};
    QVERIFY(!validatePlaylist(bad));
    bad.items = {u"playlist:other"_s};
    QVERIFY(!validatePlaylist(bad, &error));
    QVERIFY(error.contains(u"playlist"_s));
    bad.items.clear();
    for (int i = 0; i <= kMaxPlaylistItems; ++i)
        bad.items.append(u"we:%1"_s.arg(i));
    QVERIFY(!validatePlaylist(bad));
}

void TestPlaylistStore::applyChangesMergesAndValidates()
{
    Playlist base;
    base.id = u"id"_s;
    base.name = u"Mine"_s;
    QString error;
    const auto changed = applyPlaylistChanges(
        base,
        testutil::json("{\"name\":\"New\",\"items\":[\"we:1\",\"we:2\"],\"order\":\"shuffle\","
                       "\"intervalMinutes\":30,\"changeOnLogin\":true,\"unknown\":1}"),
        &error);
    QVERIFY2(changed, qPrintable(error));
    QCOMPARE(changed->id, u"id"_s);
    QCOMPARE(changed->name, u"New"_s);
    QCOMPARE(changed->items, (QStringList{u"we:1"_s, u"we:2"_s}));
    QCOMPARE(changed->order, PlaylistOrder::Shuffle);
    QCOMPARE(changed->intervalMinutes, 30);
    QVERIFY(changed->changeOnLogin);

    for (const char *json : {"{\"name\":5}", "{\"items\":\"x\"}", "{\"items\":[1]}", "{\"order\":\"random\"}",
                             "{\"order\":3}", "{\"intervalMinutes\":\"5\"}", "{\"intervalMinutes\":2.5}",
                             "{\"intervalMinutes\":0}", "{\"changeOnLogin\":1}"}) {
        QVERIFY2(!applyPlaylistChanges(base, testutil::json(json), &error), json);
        QVERIFY(!error.isEmpty());
    }
    // An integral double is an integer in JSON.
    QVERIFY(applyPlaylistChanges(base, testutil::json("{\"intervalMinutes\":30.0}")));
    // id never changes.
    QCOMPARE(applyPlaylistChanges(base, testutil::json("{\"id\":\"other\"}"))->id, u"id"_s);
}

void TestPlaylistStore::createAssignsUuidAndPersists()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"playlists.json"_s);
    QString id;
    {
        PlaylistStore store(path);
        store.load();
        QSignalSpy spy(&store, &PlaylistStore::changed);
        QString error;
        id = store.create(u"Calm"_s, &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QVERIFY(!QUuid::fromString(id).isNull());
        QCOMPARE(spy.count(), 1);
        Playlist p = *store.playlist(id);
        p.items = {u"we:1"_s, u"we:2"_s};
        p.order = PlaylistOrder::Shuffle;
        p.intervalMinutes = 5;
        p.changeOnLogin = true;
        QVERIFY(store.update(p));
    }
    PlaylistStore reloaded(path);
    reloaded.load();
    QCOMPARE(reloaded.playlists().size(), 1);
    const auto p = reloaded.playlist(id);
    QVERIFY(p);
    QCOMPARE(p->name, u"Calm"_s);
    QCOMPARE(p->items, (QStringList{u"we:1"_s, u"we:2"_s}));
    QCOMPARE(p->order, PlaylistOrder::Shuffle);
    QCOMPARE(p->intervalMinutes, 5);
    QVERIFY(p->changeOnLogin);
    QVERIFY(!reloaded.playlist(u"nope"_s));
}

void TestPlaylistStore::updateAndRemoveSignalOnlyOnChange()
{
    QTemporaryDir tmp;
    PlaylistStore store(tmp.filePath(u"playlists.json"_s));
    store.load();
    const QString id = store.create(u"A"_s);
    QSignalSpy spy(&store, &PlaylistStore::changed);

    QVERIFY(!store.update(*store.playlist(id))); // unchanged
    QCOMPARE(spy.count(), 0);
    Playlist p = *store.playlist(id);
    p.name = u"B"_s;
    QVERIFY(store.update(p));
    QCOMPARE(spy.count(), 1);
    p.intervalMinutes = 0;
    QString error;
    QVERIFY(!store.update(p, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.playlist(id)->name, u"B"_s);
    Playlist ghost = p;
    ghost.id = u"ghost"_s;
    ghost.intervalMinutes = 15;
    QVERIFY(!store.update(ghost));

    QVERIFY(!store.remove(u"ghost"_s));
    QCOMPARE(spy.count(), 1);
    QVERIFY(store.remove(id));
    QCOMPARE(spy.count(), 2);
    QVERIFY(store.playlists().isEmpty());
}

void TestPlaylistStore::missingFileIsEmpty()
{
    QTemporaryDir tmp;
    PlaylistStore store(tmp.filePath(u"playlists.json"_s));
    store.load();
    QVERIFY(store.playlists().isEmpty());
    QVERIFY(!QFile::exists(store.filePath()));
}

void TestPlaylistStore::corruptFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"playlists.json"_s);
    testutil::writeFile(path, "{not json");
    PlaylistStore store(path);
    store.load();
    QVERIFY(store.playlists().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
    QVERIFY(!store.create(u"X"_s).isEmpty()); // saving works again
    QVERIFY(QFile::exists(path));
}

void TestPlaylistStore::invalidEntryIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"playlists.json"_s);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    testutil::writeFile(path,
                        QJsonDocument(QJsonObject{{u"version"_s, 1},
                                                  {u"playlists"_s,
                                                   QJsonArray{QJsonObject{{u"id"_s, id},
                                                                          {u"name"_s, u"Ok"_s},
                                                                          {u"intervalMinutes"_s, 99999}}}}})
                            .toJson());
    PlaylistStore store(path);
    store.load();
    QVERIFY(store.playlists().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
}

void TestPlaylistStore::duplicateIdIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"playlists.json"_s);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject entry{{u"id"_s, id}, {u"name"_s, u"Ok"_s}};
    testutil::writeFile(path, QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"playlists"_s, QJsonArray{entry, entry}}})
                                  .toJson());
    PlaylistStore store(path);
    store.load();
    QVERIFY(store.playlists().isEmpty());
    QCOMPARE(backups(tmp).size(), 1);
}

void TestPlaylistStore::limitIsEnforced()
{
    QTemporaryDir tmp;
    PlaylistStore store(tmp.filePath(u"playlists.json"_s));
    store.load();
    for (int i = 0; i < kMaxPlaylists; ++i)
        QVERIFY(!store.create(u"P%1"_s.arg(i)).isEmpty());
    QString error;
    QVERIFY(store.create(u"one more"_s, &error).isEmpty());
    QVERIFY(error.contains(u"100"_s));
}

void TestPlaylistStore::containsItem()
{
    QTemporaryDir tmp;
    PlaylistStore store(tmp.filePath(u"playlists.json"_s));
    store.load();
    const QString id = store.create(u"A"_s);
    Playlist p = *store.playlist(id);
    p.items = {u"we:1"_s};
    QVERIFY(store.update(p));
    QVERIFY(store.referencesItem(u"we:1"_s));
    QVERIFY(!store.referencesItem(u"we:2"_s));
}

QTEST_GUILESS_MAIN(TestPlaylistStore)
#include "tst_playliststore.moc"
