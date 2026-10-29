// The things the triggers listen to: the signal files (hostile names, links, floods, a directory that is not the user's), the surface's session facts over a private bus,
// and that nothing is subscribed while no enabled trigger needs it.

#include "automationsources.h"
#include "constants.h"
#include "sessionwatch.h"
#include "testutil.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

void touch(const QString &path, const QByteArray &content = "1")
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

// The surface's SessionFacts() answered by a fake on the (private) session bus.
class FakeSurface : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Surface1")

public:
    QString facts = u"{\"known\":false,\"outputs\":{}}"_s;
    int reads = 0;

public slots:
    Q_SCRIPTABLE QString SessionFacts()
    {
        ++reads;
        return facts;
    }

signals:
    Q_SCRIPTABLE void SessionFactsChanged();
};


// logind: the manager answers GetSessionByPID; the session has its own object (not the "auto" alias, which this fake does not even have).
class FakeManager : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")

public:
    QString path = u"/org/freedesktop/login1/session/_31"_s;
    int asked = 0;

public slots:
    Q_SCRIPTABLE QDBusObjectPath GetSessionByPID(uint)
    {
        ++asked;
        return QDBusObjectPath(path);
    }
};

class FakeSession : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Session")
    Q_PROPERTY(bool IdleHint READ idleHint)

public:
    bool idle = false;
    bool idleHint() const { return idle; }
};

}

class TstAutomationSources : public QObject
{
    Q_OBJECT

private slots:
    void nothingIsWatchedOrCreatedUntilActivated()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(u"run/murasaki/triggers"_s);
        SignalFileWatcher watcher(dir);
        QVERIFY(!watcher.active());
        QVERIFY(!QFile::exists(dir)); // not even the directory
        watcher.setActive(true);
        QVERIFY(watcher.active());
        QVERIFY(QFileInfo(dir).isDir());
        QCOMPARE(QFileInfo(dir).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther), QFileDevice::Permissions());
        watcher.setActive(false);
        QVERIFY(!watcher.active());
    }
    void aNewOrTouchedFileFiresAndTheContentsAreNeverRead()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(u"t"_s);
        QDir().mkpath(dir);
        touch(dir + u"/old"_s); // there before: the starting point, not a trigger
        SignalFileWatcher watcher(dir);
        QSignalSpy spy(&watcher, &SignalFileWatcher::touched);
        watcher.setActive(true);
        QTest::qWait(100);
        QCOMPARE(spy.size(), 0);
        touch(dir + u"/sleepy"_s, "anything\0at all"); // (a file full of garbage is as good as "1")
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
        QCOMPARE(spy.at(0).at(0).toString(), u"sleepy"_s);
        QTest::qWait(30);
        touch(dir + u"/sleepy"_s, "again, longer");
        QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 2, 3000);
        touch(dir + u"/old"_s, "changed");
        QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 3, 3000);
        QCOMPARE(spy.last().at(0).toString(), u"old"_s);
    }
    void hostileNamesLinksAndDirectoriesAreIgnored()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(u"t"_s);
        QDir().mkpath(dir);
        SignalFileWatcher watcher(dir);
        QSignalSpy spy(&watcher, &SignalFileWatcher::touched);
        watcher.setActive(true);
        touch(tmp.filePath(u"outside"_s));
        QVERIFY(QFile::link(tmp.filePath(u"outside"_s), dir + u"/link"_s)); // a symbolic link
        QDir().mkpath(dir + u"/adir"_s);                                  // a directory
        touch(dir + u"/has space"_s);
        touch(dir + u"/semi;colon"_s);
        touch(dir + u"/" + QString(65, u'a'));                            // too long
        touch(dir + u"/ünïcode"_s);
        touch(dir + u"/good.name-1_x"_s);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
        QTest::qWait(100);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), u"good.name-1_x"_s);
    }
    void aFloodOfTouchesIsAFewScans()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(u"t"_s);
        QDir().mkpath(dir);
        SignalFileWatcher watcher(dir);
        QSignalSpy spy(&watcher, &SignalFileWatcher::touched);
        watcher.setActive(true);
        for (int i = 0; i < 300; ++i)
            touch(dir + u"/storm"_s, QByteArray::number(i)); // the same file, 300 times, at once
        QTest::qWait(300);
        QVERIFY2(spy.size() >= 1 && spy.size() <= 30, qPrintable(QString::number(spy.size())));
        // and many files: only the first kMaxFiles are looked at
        for (int i = 0; i < SignalFileWatcher::kMaxFiles + 50; ++i)
            touch(dir + u"/f%1"_s.arg(i));
        QTest::qWait(300);
        QVERIFY(spy.size() <= SignalFileWatcher::kMaxFiles + 30);
    }
    void aDirectoryThatIsALinkOrNotMineIsNotUsed()
    {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.filePath(u"real"_s));
        QVERIFY(QFile::link(tmp.filePath(u"real"_s), tmp.filePath(u"link"_s)));
        SignalFileWatcher watcher(tmp.filePath(u"link"_s));
        watcher.setActive(true);
        QVERIFY(!watcher.active()); // refused
        SignalFileWatcher empty{QString()};
        empty.setActive(true);
        QVERIFY(!empty.active());
    }

    void theFactsOfTheSurfaceFeedTheTriggersOnlyWhileTheyAreNeeded()
    {
        QTemporaryDir tmp;
        QDBusConnection bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        FakeSurface surface;
        QVERIFY(bus.registerObject(QString(kSurfaceDBusPath), &surface, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(QString(kSurfaceDBusService)));

        PetStore pets(tmp.filePath(u"pets.json"_s));
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        SettingsStore settings(tmp.filePath(u"settings.json"_s));
        QStringList calls;
        AutomationHooks hooks;
        hooks.setPetsHidden = [&](bool h) { calls << (h ? u"hide"_s : u"show"_s); };
        AutomationManager manager(&pets, &profiles, &triggers, &timelines, &settings, hooks);
        manager.load();
        SessionWatch session;
        AutomationSources sources(&manager, bus, &session, tmp.filePath(u"run"_s));
        QVERIFY(!sources.followsFacts() && !sources.followsIdle() && !sources.watchesFiles() && sources.dbusRules() == 0); // a fresh install follows nothing
        QCOMPARE(surface.reads, 0);

        Trigger t;
        t.name = u"f"_s;
        t.source.kind = TriggerKind::AppFocus;
        t.source.app = u"firefox"_s;
        t.action.type = TriggerActionType::HideAll;
        const QString id = triggers.add(t);
        QVERIFY(!id.isEmpty());
        QVERIFY(sources.followsFacts());
        QTRY_VERIFY_WITH_TIMEOUT(surface.reads >= 1, 3000);
        surface.facts = u"{\"known\":true,\"activeApp\":\"konsole\",\"openApps\":[\"konsole\"],\"outputs\":{}}"_s;
        emit surface.SessionFactsChanged();
        QTRY_VERIFY_WITH_TIMEOUT(surface.reads >= 2, 3000);
        surface.facts = u"{\"known\":true,\"activeApp\":\"firefox\",\"openApps\":[\"firefox\"],\"outputs\":{}}"_s;
        emit surface.SessionFactsChanged();
        QTRY_COMPARE_WITH_TIMEOUT(calls, QStringList{u"hide"_s}, 3000);
        triggers.remove(id);
        QVERIFY(!sources.followsFacts());
        const int reads = surface.reads;
        emit surface.SessionFactsChanged();
        QTest::qWait(100);
        QCOMPARE(surface.reads, reads); // not read any more

        // a signal-file trigger watches the directory, and stops with the trigger
        Trigger f;
        f.name = u"s"_s;
        f.source.kind = TriggerKind::SignalFile;
        f.source.name = u"go"_s;
        f.action.type = TriggerActionType::ShowAll;
        const QString fid = triggers.add(f);
        QVERIFY(sources.watchesFiles());
        touch(tmp.filePath(u"run/go"_s));
        QTRY_VERIFY_WITH_TIMEOUT(calls.contains(u"show"_s), 3000);
        triggers.remove(fid);
        QVERIFY(!sources.watchesFiles());
        bus.unregisterService(QString(kSurfaceDBusService));
        bus.unregisterObject(QString(kSurfaceDBusPath));
    }
    void idleFollowsTheSessionObjectLogindEmitsOn()
    {
        QTemporaryDir tmp;
        QDBusConnection bus = QDBusConnection::sessionBus();
        FakeManager manager;
        FakeSession session;
        QVERIFY(bus.registerObject(u"/org/freedesktop/login1"_s, &manager, QDBusConnection::ExportScriptableSlots));
        QVERIFY(bus.registerObject(manager.path, &session, QDBusConnection::ExportAllProperties));
        QVERIFY(bus.registerService(u"org.freedesktop.login1"_s));
        PetStore pets(tmp.filePath(u"pets.json"_s));
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        SettingsStore settings(tmp.filePath(u"settings.json"_s));
        AutomationManager automation(&pets, &profiles, &triggers, &timelines, &settings, AutomationHooks{});
        automation.load();
        AutomationSources sources(&automation, bus, nullptr, QString(), nullptr, bus);
        QVERIFY(!sources.followsIdle());
        QCOMPARE(manager.asked, 0);
        Trigger t;
        t.name = u"idle"_s;
        t.source.kind = TriggerKind::Idle;
        t.source.minutes = 5;
        t.action.type = TriggerActionType::HideAll;
        const QString id = triggers.add(t);
        QVERIFY(sources.followsIdle());
        QTRY_COMPARE_WITH_TIMEOUT(manager.asked, 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(automation.triggerStates().at(0).toObject().value(u"available"_s).toBool(), 3000); // the hint was read on the real object
        QVERIFY(!automation.engineTimerActive());                                                                    // not idle: nothing to wait for
        // logind says the session went idle on ITS object
        QDBusMessage changed = QDBusMessage::createSignal(manager.path, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s);
        changed << u"org.freedesktop.login1.Session"_s << QVariantMap{{u"IdleHint"_s, true}} << QStringList();
        bus.send(changed);
        QTRY_VERIFY_WITH_TIMEOUT(automation.engineTimerActive(), 3000);
        // and not idle again; a signal on the alias path (which logind never uses) is not listened to
        changed = QDBusMessage::createSignal(manager.path, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s);
        changed << u"org.freedesktop.login1.Session"_s << QVariantMap{{u"IdleHint"_s, false}} << QStringList();
        bus.send(changed);
        QTRY_VERIFY_WITH_TIMEOUT(!automation.engineTimerActive(), 3000);
        triggers.remove(id);
        QVERIFY(!sources.followsIdle());
        bus.unregisterService(u"org.freedesktop.login1"_s);
        bus.unregisterObject(manager.path);
        bus.unregisterObject(u"/org/freedesktop/login1"_s);
    }
    void aNamespaceRuleUsesATableKeptByTheBusAndNeverBlocks()
    {
        QTemporaryDir tmp;
        QDBusConnection bus = QDBusConnection::sessionBus();
        PetStore pets(tmp.filePath(u"pets.json"_s));
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        SettingsStore settings(tmp.filePath(u"settings.json"_s));
        int hides = 0;
        AutomationHooks hooks;
        hooks.setPetsHidden = [&](bool) { ++hides; };
        AutomationManager automation(&pets, &profiles, &triggers, &timelines, &settings, hooks);
        automation.load();
        AutomationSources sources(&automation, bus, nullptr, QString());
        QVERIFY(!sources.followsOwners());
        Trigger t;
        t.name = u"ns"_s;
        t.source.kind = TriggerKind::DBus;
        t.source.interface = u"org.example.Ns"_s;
        t.source.member = u"Ping"_s;
        t.source.senderNamespace = u"org.example.app"_s;
        t.action.type = TriggerActionType::HideAll;
        t.cooldownMs = 100;
        triggers.add(t);
        QVERIFY(sources.followsOwners());
        QTest::qWait(100);
        const auto ping = [&] { bus.send(QDBusMessage::createSignal(u"/x"_s, u"org.example.Ns"_s, u"Ping"_s)); };
        ping(); // this connection owns no name in the namespace yet
        QTest::qWait(150);
        QCOMPARE(hides, 0);
        QVERIFY(bus.registerService(u"org.example.app.One"_s));
        QTest::qWait(150);
        ping();
        QTRY_COMPARE_WITH_TIMEOUT(hides, 1, 3000);
        // the name goes: the table follows the bus
        bus.unregisterService(u"org.example.app.One"_s);
        QTest::qWait(300);
        ping();
        QTest::qWait(200);
        QCOMPARE(hides, 1);
        // a rule without a namespace keeps no table
        triggers.remove(triggers.triggers().first().id);
        QVERIFY(!sources.followsOwners());
    }
    void factsThatStopBeingFollowedDoNotInventAnEdgeWhenFollowedAgain()
    {
        QTemporaryDir tmp;
        QDBusConnection bus = QDBusConnection::sessionBus();
        FakeSurface surface;
        QVERIFY(bus.registerObject(QString(kSurfaceDBusPath), &surface, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(QString(kSurfaceDBusService)));
        PetStore pets(tmp.filePath(u"pets.json"_s));
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        SettingsStore settings(tmp.filePath(u"settings.json"_s));
        int hides = 0;
        AutomationHooks hooks;
        hooks.setPetsHidden = [&](bool) { ++hides; };
        AutomationManager automation(&pets, &profiles, &triggers, &timelines, &settings, hooks);
        automation.load();
        AutomationSources sources(&automation, bus, nullptr, QString());
        Trigger t;
        t.name = u"f"_s;
        t.source.kind = TriggerKind::AppFocus;
        t.source.app = u"firefox"_s;
        t.action.type = TriggerActionType::HideAll;
        t.cooldownMs = 100;
        surface.facts = u"{\"known\":true,\"activeApp\":\"konsole\",\"openApps\":[],\"outputs\":{}}"_s;
        QString id = triggers.add(t);
        QTRY_VERIFY_WITH_TIMEOUT(surface.reads >= 1, 3000);
        QTest::qWait(100);
        triggers.remove(id); // not followed any more; meanwhile the focus moves to firefox
        surface.facts = u"{\"known\":true,\"activeApp\":\"firefox\",\"openApps\":[],\"outputs\":{}}"_s;
        id = triggers.add(t); // followed again: what is true now is a starting point, not a "gets the focus"
        QTRY_VERIFY_WITH_TIMEOUT(surface.reads >= 2, 3000);
        QTest::qWait(200);
        QCOMPARE(hides, 0);
        bus.unregisterService(QString(kSurfaceDBusService));
        bus.unregisterObject(QString(kSurfaceDBusPath));
    }
    void dbusRulesAreMatchedByInterfaceAndMember()
    {
        QTemporaryDir tmp;
        QDBusConnection bus = QDBusConnection::sessionBus();
        PetStore pets(tmp.filePath(u"pets.json"_s));
        ProfileStore profiles(tmp.filePath(u"profiles.json"_s));
        TriggerStore triggers(tmp.filePath(u"triggers.json"_s));
        TimelineStore timelines(tmp.filePath(u"timelines.json"_s));
        SettingsStore settings(tmp.filePath(u"settings.json"_s));
        int hides = 0;
        AutomationHooks hooks;
        hooks.setPetsHidden = [&](bool) { ++hides; };
        AutomationManager manager(&pets, &profiles, &triggers, &timelines, &settings, hooks);
        manager.load();
        AutomationSources sources(&manager, bus, nullptr, QString());
        Trigger t;
        t.name = u"d"_s;
        t.source.kind = TriggerKind::DBus;
        t.source.interface = u"org.example.Test"_s;
        t.source.member = u"Ping"_s;
        t.action.type = TriggerActionType::HideAll;
        triggers.add(t);
        QCOMPARE(sources.dbusRules(), 1);
        QDBusMessage other = QDBusMessage::createSignal(u"/x"_s, u"org.example.Test"_s, u"Other"_s);
        bus.send(other);
        QDBusMessage ping = QDBusMessage::createSignal(u"/x"_s, u"org.example.Test"_s, u"Ping"_s);
        ping << u"arguments are never read"_s;
        bus.send(ping);
        QTRY_COMPARE_WITH_TIMEOUT(hides, 1, 3000);
        QTest::qWait(100);
        QCOMPARE(hides, 1);
    }
};

QTEST_GUILESS_MAIN(TstAutomationSources)
#include "tst_automationsources.moc"
