#include "itempermissions.h"
#include "testutil.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestItemPermissions : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToNoNetwork();
    void setNetworkPersistsAcrossLoads();
    void revokingForgetsTheItem();
    void changedFiresOnlyOnRealChanges();
    void rejectsBadItemIds();
    void badFileIsQuarantinedAndStartsEmpty();
    void failedSaveRollsBack();
    void limitsTheNumberOfGrants();
};

void TestItemPermissions::defaultsToNoNetwork()
{
    QTemporaryDir tmp;
    ItemPermissionStore store(tmp.filePath(u"permissions.json"_s));
    store.load();
    QVERIFY(!store.network(u"we:1"_s));
    QVERIFY(store.networkItems().isEmpty());
}

void TestItemPermissions::setNetworkPersistsAcrossLoads()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"permissions.json"_s);
    {
        ItemPermissionStore store(path);
        store.load();
        QVERIFY(store.setNetwork(u"we:1"_s, true));
    }
    ItemPermissionStore again(path);
    again.load();
    QVERIFY(again.network(u"we:1"_s));
    QVERIFY(!again.network(u"we:2"_s));
}

void TestItemPermissions::revokingForgetsTheItem()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"permissions.json"_s);
    ItemPermissionStore store(path);
    store.load();
    QVERIFY(store.setNetwork(u"we:1"_s, true));
    QVERIFY(store.setNetwork(u"we:1"_s, false));
    QVERIFY(!store.network(u"we:1"_s));
    ItemPermissionStore again(path);
    again.load();
    QVERIFY(again.networkItems().isEmpty());
}

void TestItemPermissions::changedFiresOnlyOnRealChanges()
{
    QTemporaryDir tmp;
    ItemPermissionStore store(tmp.filePath(u"permissions.json"_s));
    store.load();
    QSignalSpy spy(&store, &ItemPermissionStore::changed);
    QVERIFY(store.setNetwork(u"we:1"_s, true));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.last().at(0).toString(), u"we:1"_s);
    QVERIFY(store.setNetwork(u"we:1"_s, true)); // already allowed: success, no signal
    QVERIFY(store.setNetwork(u"we:9"_s, false)); // already denied
    QCOMPARE(spy.size(), 1);
}

void TestItemPermissions::rejectsBadItemIds()
{
    QTemporaryDir tmp;
    ItemPermissionStore store(tmp.filePath(u"permissions.json"_s));
    store.load();
    QString error;
    QVERIFY(!store.setNetwork(QString(), true, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.setNetwork(QString(201, QLatin1Char('x')), true, &error));
    QVERIFY(store.networkItems().isEmpty());
}

void TestItemPermissions::badFileIsQuarantinedAndStartsEmpty()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"permissions.json"_s);
    testutil::writeFile(path, R"({"version": 1, "items": {"we:1": {"network": "yes"}}})");
    ItemPermissionStore store(path);
    store.load();
    QVERIFY(!store.network(u"we:1"_s)); // a malformed grant never counts as a grant
    QVERIFY(!QFile::exists(path));
    QVERIFY(!QDir(tmp.path()).entryList({u"permissions.json.bad-*"_s}).isEmpty());
    QVERIFY(store.setNetwork(u"we:2"_s, true)); // and the store is usable again
}

void TestItemPermissions::failedSaveRollsBack()
{
    QTemporaryDir tmp;
    // The parent "directory" is a file, so the write fails.
    testutil::writeFile(tmp.filePath(u"blocker"_s), "x");
    ItemPermissionStore store(tmp.filePath(u"blocker/permissions.json"_s));
    store.load();
    QSignalSpy spy(&store, &ItemPermissionStore::changed);
    QString error;
    QVERIFY(!store.setNetwork(u"we:1"_s, true, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.network(u"we:1"_s));
    QCOMPARE(spy.size(), 0);
}

void TestItemPermissions::limitsTheNumberOfGrants()
{
    QTemporaryDir tmp;
    ItemPermissionStore store(tmp.filePath(u"permissions.json"_s));
    store.load();
    for (int i = 0; i < kMaxPermissionGrants; ++i)
        QVERIFY(store.setNetwork(u"we:%1"_s.arg(i), true));
    QString error;
    QVERIFY(!store.setNetwork(u"we:extra"_s, true, &error));
    QVERIFY(store.setNetwork(u"we:0"_s, false)); // revoking always works
}

QTEST_GUILESS_MAIN(TestItemPermissions)
#include "tst_itempermissions.moc"
