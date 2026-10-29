#include "assignment.h"
#include "testutil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestAssignmentStore : public QObject
{
    Q_OBJECT

private slots:
    void makeAssignmentAppliesDefaults();
    void makeAssignmentValidates();
    void resolveFollowsPrecedence();
    void persistsAcrossInstances();
    void unchangedSetOrClearIsSilent();
    void missingFileIsEmpty();
    void corruptFileIsKeptAsBackup();
    void invalidEntryIsKeptAsBackup();
    void referencesItem();
    void keyPartLengthBoundary();
    void setRejectsInvalidAssignment();
    void oversizedPropertiesAreRejected();
    void unsupportedVersionIsKeptAsBackup();
    void nonObjectEntryIsKeptAsBackup();
    void propertiesRoundTrip();
    void fifoIsKeptAsBackupWithoutBlocking();
    void spanIsOnlyValidOnTheWildcardOutput();
    void spanRoundTripsAndStaysOutOfTheJsonWhenOff();
    void aSpanAssignmentInTheFileOnASingleOutputIsKeptAsBackup();

private:
    static Assignment make(const QString &output, const QString &desktop, const QString &itemId)
    {
        return *makeAssignment(output, desktop, itemId, {});
    }
};

void TestAssignmentStore::makeAssignmentAppliesDefaults()
{
    const auto a = makeAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, {});
    QVERIFY(a);
    QCOMPARE(a->output, u"DP-1"_s);
    QCOMPARE(a->desktop, u"*"_s);
    QCOMPARE(a->itemId, u"we:1"_s);
    QCOMPARE(a->muted, false);
    QCOMPARE(a->fill, u"fill"_s);
    QVERIFY(a->properties.isEmpty());

    const auto b = makeAssignment(u"*"_s, u"*"_s, u"we:1"_s,
                                  testutil::json("{\"muted\": true, \"fill\": \"fit\", \"properties\": {\"k\": 1}, \"extra\": 5}"));
    QVERIFY(b);
    QCOMPARE(b->muted, true);
    QCOMPARE(b->fill, u"fit"_s);
    QCOMPARE(b->properties.value(u"k"_s).toInt(), 1);
}

void TestAssignmentStore::makeAssignmentValidates()
{
    QString error;
    QVERIFY(!makeAssignment(QString(), u"*"_s, u"we:1"_s, {}, &error));
    QVERIFY(error.contains(u"output"_s));
    QVERIFY(!makeAssignment(u"*"_s, QString(201, u'x'), u"we:1"_s, {}, &error));
    QVERIFY(error.contains(u"desktop"_s));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, QString(), {}, &error));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"muted\": \"yes\"}"), &error));
    QVERIFY(error.contains(u"muted"_s));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"fill\": \"zoom\"}"), &error));
    QVERIFY(error.contains(u"fill"_s));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"properties\": [1]}"), &error));
    QVERIFY(error.contains(u"properties"_s));
    QVERIFY(isValidKeyPart(u"DP-1"_s));
    QVERIFY(!isValidKeyPart(QString()));
}

void TestAssignmentStore::resolveFollowsPrecedence()
{
    QTemporaryDir tmp;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    store.set(make(u"*"_s, u"*"_s, u"we:any"_s));
    store.set(make(u"*"_s, u"d1"_s, u"we:desktop"_s));
    store.set(make(u"DP-1"_s, u"*"_s, u"we:output"_s));
    store.set(make(u"DP-1"_s, u"d1"_s, u"we:both"_s));

    QCOMPARE(store.resolve(u"DP-1"_s, u"d1"_s)->itemId, u"we:both"_s);
    QCOMPARE(store.resolve(u"DP-1"_s, u"d2"_s)->itemId, u"we:output"_s);
    QCOMPARE(store.resolve(u"HDMI-A-1"_s, u"d1"_s)->itemId, u"we:desktop"_s);
    QCOMPARE(store.resolve(u"HDMI-A-1"_s, u"d2"_s)->itemId, u"we:any"_s);

    store.clear(u"*"_s, u"*"_s);
    QVERIFY(!store.resolve(u"HDMI-A-1"_s, u"d2"_s));
}

void TestAssignmentStore::persistsAcrossInstances()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"config/murasaki/assignments.json"_s); // parent dirs do not exist yet
    {
        AssignmentStore store(path);
        store.load();
        store.set(*makeAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, testutil::json("{\"muted\": true, \"fill\": \"stretch\"}")));
        store.set(make(u"*"_s, u"*"_s, u"we:2"_s));
    }
    AssignmentStore reloaded(path);
    reloaded.load();
    const QList<Assignment> all = reloaded.assignments();
    QCOMPARE(all.size(), 2);
    QCOMPARE(all[0].output, u"*"_s); // sorted by (output, desktop)
    QCOMPARE(all[1].itemId, u"we:1"_s);
    QCOMPARE(all[1].muted, true);
    QCOMPARE(all[1].fill, u"stretch"_s);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value(u"version"_s).toInt(), 1);
}

void TestAssignmentStore::unchangedSetOrClearIsSilent()
{
    QTemporaryDir tmp;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    QSignalSpy spy(&store, &AssignmentStore::changed);

    QVERIFY(store.set(make(u"*"_s, u"*"_s, u"we:1"_s)));
    QVERIFY(!store.set(make(u"*"_s, u"*"_s, u"we:1"_s)));
    QVERIFY(!store.clear(u"DP-1"_s, u"*"_s));
    QCOMPARE(spy.count(), 1);
    QVERIFY(store.clear(u"*"_s, u"*"_s));
    QCOMPARE(spy.count(), 2);
}

void TestAssignmentStore::missingFileIsEmpty()
{
    QTemporaryDir tmp;
    AssignmentStore store(tmp.filePath(u"none.json"_s));
    store.load();
    QVERIFY(store.assignments().isEmpty());
    QVERIFY(!QFile::exists(tmp.filePath(u"none.json"_s))); // loading never creates the file
}

void TestAssignmentStore::corruptFileIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    testutil::writeFile(path, "{not json");
    AssignmentStore store(path);
    store.load();

    QVERIFY(store.assignments().isEmpty());
    QVERIFY(!QFile::exists(path));
    const QStringList backups = QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    QFile backup(tmp.filePath(backups.first()));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), QByteArray("{not json"));

    QVERIFY(store.set(make(u"*"_s, u"*"_s, u"we:1"_s)));
    QVERIFY(QFile::exists(path));
}

void TestAssignmentStore::invalidEntryIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"assignments\": [{\"output\": \"*\", \"desktop\": \"*\", "
                              "\"itemId\": \"we:1\", \"fill\": \"zoom\"}]}");
    AssignmentStore store(path);
    store.load();
    QVERIFY(store.assignments().isEmpty());
    QCOMPARE(QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::Files).size(), 1);
}

void TestAssignmentStore::referencesItem()
{
    QTemporaryDir tmp;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    store.set(make(u"DP-1"_s, u"d1"_s, u"we:1"_s));
    QVERIFY(store.referencesItem(u"we:1"_s));
    QVERIFY(!store.referencesItem(u"we:2"_s));
}

void TestAssignmentStore::keyPartLengthBoundary()
{
    QVERIFY(makeAssignment(QString(200, u'x'), u"*"_s, QString(200, u'y'), {}));
    QVERIFY(!makeAssignment(QString(201, u'x'), u"*"_s, u"we:1"_s, {}));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, QString(201, u'y'), {}));
}

void TestAssignmentStore::setRejectsInvalidAssignment()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    AssignmentStore store(path);
    QSignalSpy spy(&store, &AssignmentStore::changed);

    Assignment bad = make(u"*"_s, u"*"_s, u"we:1"_s);
    bad.fill = u"zoom"_s;
    QVERIFY(!store.set(bad));
    bad = make(u"*"_s, u"*"_s, u"we:1"_s);
    bad.output = QString();
    QVERIFY(!store.set(bad));
    bad = make(u"*"_s, u"*"_s, u"we:1"_s);
    bad.desktop = QString(201, u'x');
    QVERIFY(!store.set(bad));
    bad = make(u"*"_s, u"*"_s, u"we:1"_s);
    bad.itemId = QString();
    QVERIFY(!store.set(bad));

    QCOMPARE(spy.count(), 0);
    QVERIFY(store.assignments().isEmpty());
    QVERIFY(!QFile::exists(path));
}

void TestAssignmentStore::oversizedPropertiesAreRejected()
{
    QJsonObject big;
    big.insert(u"blob"_s, QString(1024 * 1024 + 1, u'x'));
    QString error;
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, QJsonObject{{u"properties"_s, big}}, &error));
    QVERIFY(error.contains(u"1 MiB"_s));
    QJsonObject fits;
    fits.insert(u"blob"_s, QString(1000 * 1000, u'x'));
    QVERIFY(makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, QJsonObject{{u"properties"_s, fits}}));

    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    AssignmentStore store(path);
    Assignment bad = make(u"*"_s, u"*"_s, u"we:1"_s);
    bad.properties = big;
    QVERIFY(!store.set(bad));
    QVERIFY(!QFile::exists(path));
}

void TestAssignmentStore::unsupportedVersionIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    testutil::writeFile(path, "{\"version\": 2, \"assignments\": []}");
    AssignmentStore store(path);
    store.load();
    QVERIFY(!QFile::exists(path));
    QCOMPARE(QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::Files).size(), 1);
}

void TestAssignmentStore::nonObjectEntryIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"assignments\": [5]}");
    AssignmentStore store(path);
    store.load();
    QVERIFY(store.assignments().isEmpty());
    QCOMPARE(QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::Files).size(), 1);
}

void TestAssignmentStore::propertiesRoundTrip()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    {
        AssignmentStore store(path);
        QVERIFY(store.set(*makeAssignment(u"*"_s, u"*"_s, u"we:1"_s,
                                          testutil::json("{\"properties\": {\"speed\": 2, \"name\": \"x\"}}"))));
    }
    AssignmentStore reloaded(path);
    reloaded.load();
    QCOMPARE(reloaded.assignments().size(), 1);
    const QJsonObject properties = reloaded.assignments().first().properties;
    QCOMPARE(properties.value(u"speed"_s).toInt(), 2);
    QCOMPARE(properties.value(u"name"_s).toString(), u"x"_s);
}

void TestAssignmentStore::fifoIsKeptAsBackupWithoutBlocking()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    if (mkfifo(QFile::encodeName(path).constData(), 0600) != 0)
        QSKIP("mkfifo is not available here");
    AssignmentStore store(path);
    QElapsedTimer timer;
    timer.start();
    store.load();
    QVERIFY2(timer.elapsed() < 2000, "load() blocked on a FIFO");
    QVERIFY(store.assignments().isEmpty());
    QVERIFY(!QFile::exists(path));
    QCOMPARE(QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).size(), 1);
}

void TestAssignmentStore::spanIsOnlyValidOnTheWildcardOutput()
{
    QString error;
    const auto all = makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"span\": true}"), &error);
    QVERIFY2(all, qPrintable(error));
    QVERIFY(all->span);
    // a specific desktop is fine: span is about outputs
    QVERIFY(makeAssignment(u"*"_s, u"d1"_s, u"we:1"_s, testutil::json("{\"span\": true}")));
    QVERIFY(!makeAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, testutil::json("{\"span\": true}"), &error));
    QVERIFY(error.contains(u"span"_s));
    QVERIFY(error.contains(u"*"_s));
    // false is valid anywhere
    QVERIFY(makeAssignment(u"DP-1"_s, u"*"_s, u"we:1"_s, testutil::json("{\"span\": false}")));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"span\": \"yes\"}"), &error));
    QVERIFY(error.contains(u"span"_s));
    QVERIFY(!makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, testutil::json("{\"span\": 1}")));

    // set() applies the same rule as makeAssignment
    QTemporaryDir tmp;
    AssignmentStore store(tmp.filePath(u"assignments.json"_s));
    Assignment bad = make(u"DP-1"_s, u"*"_s, u"we:1"_s);
    bad.span = true;
    QVERIFY(!store.set(bad));
    QVERIFY(store.assignments().isEmpty());
}

void TestAssignmentStore::spanRoundTripsAndStaysOutOfTheJsonWhenOff()
{
    QVERIFY(!make(u"*"_s, u"*"_s, u"we:1"_s).toJson().contains(u"span"_s)); // keeps every older key the same
    Assignment spanned = make(u"*"_s, u"*"_s, u"we:1"_s);
    spanned.span = true;
    QCOMPARE(spanned.toJson().value(u"span"_s).toBool(), true);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    {
        AssignmentStore store(path);
        store.load();
        QVERIFY(store.set(spanned));
    }
    AssignmentStore reloaded(path);
    reloaded.load();
    QVERIFY(reloaded.resolve(u"DP-1"_s, u"d"_s)->span);
    // turning it off is a change
    QSignalSpy spy(&reloaded, &AssignmentStore::changed);
    QVERIFY(reloaded.set(make(u"*"_s, u"*"_s, u"we:1"_s)));
    QCOMPARE(spy.count(), 1);
    QVERIFY(!reloaded.resolve(u"DP-1"_s, u"d"_s)->span);
}

void TestAssignmentStore::aSpanAssignmentInTheFileOnASingleOutputIsKeptAsBackup()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"assignments.json"_s);
    testutil::writeFile(path, "{\"version\": 1, \"assignments\": [{\"output\": \"DP-1\", \"desktop\": \"*\","
                              " \"itemId\": \"we:1\", \"span\": true}]}");
    AssignmentStore store(path);
    store.load();
    QVERIFY(store.assignments().isEmpty());
    QCOMPARE(QDir(tmp.path()).entryList({u"assignments.json.bad-*"_s}, QDir::Files).size(), 1);
}

QTEST_GUILESS_MAIN(TestAssignmentStore)
#include "tst_assignmentstore.moc"
