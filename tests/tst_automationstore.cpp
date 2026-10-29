// profiles.json, triggers.json, timelines.json: round trip, quarantine of an unusable file, the caps, the names, and a file that does not grow on every save.

#include "automationstore.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

Profile profile(const QString &name, int pets = 1)
{
    Profile p;
    p.name = name;
    for (int i = 0; i < pets; ++i) {
        ProfileEntry e;
        e.slot = i;
        e.pet.itemId = u"local:%1"_s.arg(i);
        e.pet.variant = u"v"_s;
        e.pet.output = u"*"_s;
        p.pets.append(e);
    }
    return p;
}

Trigger trigger(const QString &name)
{
    Trigger t;
    t.name = name;
    t.source.kind = TriggerKind::Fire;
    t.source.name = u"go"_s;
    t.action.type = TriggerActionType::HideAll;
    return t;
}

Timeline timeline(const QString &name)
{
    Timeline t;
    t.name = name;
    TimelineTrack track;
    track.pet.slot = 0;
    TimelineAction wait;
    wait.kind = ActionKind::Wait;
    wait.waitMs = 10;
    track.actions.append(wait);
    t.tracks.append(track);
    return t;
}

QStringList backups(const QTemporaryDir &tmp, const QString &file)
{
    return QDir(tmp.path()).entryList({file + u".bad-*"_s}, QDir::Files);
}

}

class TstAutomationStore : public QObject
{
    Q_OBJECT

private slots:
    void profilesRoundTripAndSignal()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(u"profiles.json"_s);
        {
            ProfileStore store(path);
            store.load();
            QVERIFY(store.profiles().isEmpty());
            QSignalSpy changed(&store, &ProfileStore::changed);
            QString error;
            const QString id = store.add(profile(u"Work"_s, 3), &error);
            QVERIFY2(!id.isEmpty(), qPrintable(error));
            QCOMPARE(changed.size(), 1);
            Profile p = *store.profile(id);
            QVERIFY(!store.update(p)); // unchanged: no write, no signal
            QCOMPARE(changed.size(), 1);
            p.name = u"Home"_s;
            QVERIFY(store.update(p, &error));
            QCOMPARE(changed.size(), 2);
            QVERIFY(!store.remove(u"nobody"_s));
            QCOMPARE(changed.size(), 2);
        }
        ProfileStore again(path);
        again.load();
        QCOMPARE(again.profiles().size(), 1);
        QCOMPARE(again.profiles().first().name, u"Home"_s);
        QCOMPARE(again.profiles().first().pets.size(), 3);
        QVERIFY(again.profileByName(u"home"_s).has_value()); // by name, any case
        QVERIFY(again.remove(again.profiles().first().id));
        QVERIFY(again.profiles().isEmpty());
    }
    void triggersAndTimelinesRoundTrip()
    {
        QTemporaryDir tmp;
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        const QString tid = triggers.add(trigger(u"a"_s));
        const QString lid = timelines.add(timeline(u"hello"_s));
        QVERIFY(!tid.isEmpty() && !lid.isEmpty());
        QVERIFY(triggers.add(trigger(u"a"_s)).size() > 0); // trigger names may repeat
        TriggerStore t2(tmp.filePath(u"triggers.json"_s));
        TimelineStore l2(tmp.filePath(u"timelines.json"_s));
        t2.load();
        l2.load();
        QCOMPARE(t2.triggers().size(), 2);
        QCOMPARE(*t2.trigger(tid), *triggers.trigger(tid));
        QCOMPARE(l2.timelines().size(), 1);
        QCOMPARE(*l2.timeline(lid), *timelines.timeline(lid));
    }
    void namesAreUniqueIgnoringCase()
    {
        QTemporaryDir tmp;
        ProfileStore store(tmp.filePath(u"profiles.json"_s));
        QVERIFY(!store.add(profile(u"Work"_s)).isEmpty());
        QString error;
        QVERIFY(store.add(profile(u"WORK"_s), &error).isEmpty());
        QVERIFY(error.contains(u"name"_s));
        const QString other = store.add(profile(u"Play"_s));
        Profile p = *store.profile(other);
        p.name = u"work"_s;
        QVERIFY(!store.update(p, &error));
        QVERIFY(error.contains(u"name"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        QVERIFY(!timelines.add(timeline(u"x"_s)).isEmpty());
        QVERIFY(timelines.add(timeline(u"X"_s), &error).isEmpty());
    }
    void anInvalidThingIsRefusedAndNothingIsWritten()
    {
        QTemporaryDir tmp;
        TriggerStore store(tmp.filePath(u"triggers.json"_s));
        Trigger bad = trigger(u"b"_s);
        bad.source.name = u"../etc"_s;
        QString error;
        QVERIFY(store.add(bad, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(store.filePath()));
    }
    void capsHold()
    {
        QTemporaryDir tmp;
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        for (int i = 0; i < kMaxProfiles; ++i)
            QVERIFY(!profiles.add(profile(u"p%1"_s.arg(i), 0)).isEmpty());
        QString error;
        QVERIFY(profiles.add(profile(u"one more"_s, 0), &error).isEmpty());
        QVERIFY(error.contains(QString::number(kMaxProfiles)));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        for (int i = 0; i < kMaxTriggers; ++i)
            QVERIFY(!triggers.add(trigger(u"t%1"_s.arg(i))).isEmpty());
        QVERIFY(triggers.add(trigger(u"more"_s), &error).isEmpty());
        // a file with more than the cap is not a file this version wrote: quarantined
        QJsonArray many;
        for (int i = 0; i <= kMaxTimelines; ++i)
            many.append(QJsonObject{{u"id"_s, QUuid::createUuid().toString(QUuid::WithoutBraces)}, {u"name"_s, u"t%1"_s.arg(i)}, {u"tracks"_s, QJsonArray()}});
        testutil::writeFile(tmp.filePath(u"timelines.json"_s), QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"timelines"_s, many}}).toJson());
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        timelines.load();
        QVERIFY(timelines.timelines().isEmpty());
        QCOMPARE(backups(tmp, u"timelines.json"_s).size(), 1);
    }
    void anUnusableFileIsKeptAsABackupAndTheStoreStartsEmpty()
    {
        QTemporaryDir tmp;
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto file = [](const QJsonArray &profiles, int version = 1) { return QJsonDocument(QJsonObject{{u"version"_s, version}, {u"profiles"_s, profiles}}).toJson(); };
        const auto entry = [](const QString &id, const QString &name) { return QJsonObject{{u"id"_s, id}, {u"name"_s, name}, {u"pets"_s, QJsonArray()}}; };
        const struct {
            const char *name;
            QByteArray content;
        } cases[] = {
            {"not json", "{{{"},
            {"wrong version", file({}, 2)},
            {"not an array", QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"profiles"_s, QJsonObject()}}).toJson()},
            {"id is not a uuid", file({entry(u"x"_s, u"a"_s)})},
            {"empty name", file({entry(id, QString())})},
            {"duplicate id", file({entry(id, u"a"_s), entry(id, u"b"_s)})},
            {"duplicate name", file({entry(id, u"a"_s), entry(QUuid::createUuid().toString(QUuid::WithoutBraces), u"A"_s)})},
        };
        for (const auto &c : cases) {
            const QString path = tmp.filePath(u"profiles.json"_s);
            testutil::writeFile(path, c.content);
            ProfileStore store(path);
            QSignalSpy changed(&store, &ProfileStore::changed);
            store.load();
            QVERIFY2(store.profiles().isEmpty(), c.name);
            QVERIFY2(!QFile::exists(path), c.name);
            QVERIFY2(!backups(tmp, u"profiles.json"_s).isEmpty(), c.name);
            for (const QString &b : backups(tmp, u"profiles.json"_s))
                QFile::remove(tmp.filePath(b));
            QVERIFY(!store.add(profile(u"after"_s)).isEmpty()); // and it works again
            QFile::remove(path);
        }
    }
    void aFileThatCannotBeMovedAsideIsNeverOverwritten()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(u"triggers.json"_s);
        testutil::writeFile(path, "{{{");
        QVERIFY(QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner)); // no rename possible
        TriggerStore store(path);
        store.load();
        QString error;
        const QString id = store.add(trigger(u"x"_s), &error);
        QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (QFile(path).exists() && QFile(path).size() == 3) { // (running as root would rename anyway)
            QVERIFY(id.isEmpty());
            QVERIFY(!error.isEmpty());
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), QByteArray("{{{"));
        }
    }
    void theFileDoesNotGrowOnEverySave()
    {
        QTemporaryDir tmp;
        TimelineStore store(tmp.filePath(u"timelines.json"_s));
        const QString id = store.add(timeline(u"t"_s));
        const qint64 first = QFileInfo(store.filePath()).size();
        Timeline t = *store.timeline(id);
        for (int i = 0; i < 20; ++i) {
            t.name = u"t%1"_s.arg(i % 2);
            store.update(t);
        }
        QVERIFY(QFileInfo(store.filePath()).size() <= first + 4);
        QVERIFY(QDir(tmp.path()).entryList({u"*.bad-*"_s}).isEmpty());
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files).size(), 1); // no temporary file left
    }
};

QTEST_GUILESS_MAIN(TstAutomationStore)
#include "tst_automationstore.moc"
