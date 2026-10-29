// The shortcut triggers in the overlay: registered only while the daemon lists enabled ones, press and release told to the daemon, keys reported.

#include "dbusbackend.h"
#include "triggershortcuts.h"

#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class FakeDaemon : public DBusBackend
{
public:
    using DBusBackend::DBusBackend;
    void start() override {}
    void call(const QString &method, const QVariantList &args, Callback callback) override
    {
        calls.append({method, args});
        Reply reply;
        reply.ok = true;
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void property(const QString &name, Callback callback) override
    {
        Reply reply;
        reply.ok = name != u"ShortcutTriggerIds"_s || readable;
        reply.value = ids;
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void emitSignal(const QString &name) { emit signalReceived(name, {}); }
    QString ids = u"[]"_s;
    bool readable = true;
    QList<QPair<QString, QVariantList>> calls;
};

class FakeBinding : public GlobalShortcut
{
public:
    explicit FakeBinding(QList<ShortcutAction> actions) : actions(std::move(actions)) {}
    void start() override
    {
        started = true;
        QTimer::singleShot(0, this, [this] { emit registered(); });
    }
    QString backend() const override { return u"fake"_s; }
    QString trigger(const QString &id) const override { return keys.value(id); }
    QStringList actionIds() const override
    {
        QStringList ids;
        for (const ShortcutAction &a : actions)
            ids << a.id;
        return ids;
    }
    QList<ShortcutAction> actions;
    QMap<QString, QString> keys;
    bool started = false;
};

}

class TstTriggerShortcuts : public QObject
{
    Q_OBJECT

private slots:
    void nothingIsRegisteredWithoutShortcutTriggers()
    {
        FakeDaemon daemon;
        int made = 0;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &) {
            ++made;
            return std::unique_ptr<GlobalShortcut>();
        });
        emit daemon.availabilityChanged(true);
        QTest::qWait(30);
        QCOMPARE(made, 0);
        QVERIFY(shortcuts.ids().isEmpty());
        QVERIFY(shortcuts.keys().isEmpty());
    }
    void theDaemonsListIsRegisteredWithoutSuggestedKeysAndFollowsChanges()
    {
        FakeDaemon daemon;
        QList<FakeBinding *> made;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &actions) {
            auto binding = std::make_unique<FakeBinding>(actions);
            made.append(binding.get());
            return std::unique_ptr<GlobalShortcut>(std::move(binding));
        });
        daemon.ids = u"[{\"id\":\"peek\",\"description\":\"Peek\"},{\"id\":\"wave\"},{\"id\":\"../bad\"},{\"id\":\"has space\"}]"_s;
        emit daemon.availabilityChanged(true);
        QTRY_COMPARE(made.size(), 1);
        QCOMPARE(shortcuts.ids(), (QStringList{u"peek"_s, u"wave"_s})); // a bad name is not asked for
        const QList<ShortcutAction> actions = made[0]->actions;
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions[0].id, u"trigger.peek"_s);
        QCOMPARE(actions[0].description, u"Peek"_s);
        QVERIFY(actions[0].portalTrigger.isEmpty() && actions[0].keyCode == 0); // the user chooses the key in the desktop's dialog
        QVERIFY(made[0]->started);
        made[0]->keys.insert(u"trigger.peek"_s, u"Meta+Shift+K"_s);
        QCOMPARE(shortcuts.keys().value(u"peek"_s).toString(), u"Meta+Shift+K"_s);
        QCOMPARE(shortcuts.keys().value(u"wave"_s).toString(), QString()); // no key yet
        // press and release are told to the daemon
        emit made[0]->activated(u"trigger.peek"_s);
        emit made[0]->released(u"trigger.peek"_s);
        emit made[0]->activated(u"toggle-hidden"_s); // not ours
        QTRY_COMPARE(daemon.calls.size(), 2);
        QCOMPARE(daemon.calls[0].first, u"TriggerShortcut"_s);
        QCOMPARE(daemon.calls[0].second, (QVariantList{u"peek"_s, true}));
        QCOMPARE(daemon.calls[1].second, (QVariantList{u"peek"_s, false}));
        // the same list again: nothing is rebuilt; a different list is
        emit daemon.signalReceived(u"TriggersChanged"_s, {});
        QTest::qWait(30);
        QCOMPARE(made.size(), 1);
        daemon.ids = u"[{\"id\":\"peek\"}]"_s;
        daemon.emitSignal(u"TriggersChanged"_s);
        QTRY_COMPARE(made.size(), 2);
        QCOMPARE(shortcuts.ids(), QStringList{u"peek"_s});
        // and an empty one registers nothing and drops the binding
        daemon.ids = u"[]"_s;
        daemon.emitSignal(u"TriggersChanged"_s);
        QTRY_VERIFY(shortcuts.ids().isEmpty());
        QCOMPARE(made.size(), 2);
        QVERIFY(shortcuts.keys().isEmpty());
    }
    void aShortcutThatIsNoLongerWantedIsForgotten()
    {
        FakeDaemon daemon;
        QStringList forgotten;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &actions) { return std::unique_ptr<GlobalShortcut>(std::make_unique<FakeBinding>(actions)); }, nullptr,
                                   [&](const QStringList &gone) { forgotten << gone; });
        daemon.ids = u"[{\"id\":\"peek\"},{\"id\":\"wave\"}]"_s;
        emit daemon.availabilityChanged(true);
        QTRY_COMPARE(shortcuts.ids().size(), 2);
        QVERIFY(forgotten.isEmpty());
        daemon.ids = u"[{\"id\":\"wave\"}]"_s;
        daemon.emitSignal(u"TriggersChanged"_s);
        QTRY_COMPARE(forgotten, QStringList{u"trigger.peek"_s});
        daemon.ids = u"[]"_s;
        daemon.emitSignal(u"TriggersChanged"_s);
        QTRY_COMPARE(forgotten, (QStringList{u"trigger.peek"_s, u"trigger.wave"_s}));
    }
    void aDaemonOfAnOlderRevisionHasNone()
    {
        FakeDaemon daemon;
        daemon.readable = false;
        int made = 0;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &) {
            ++made;
            return std::unique_ptr<GlobalShortcut>();
        });
        emit daemon.availabilityChanged(true);
        QTest::qWait(30);
        QCOMPARE(made, 0);
    }
    void theKeysGoWithTheDaemon()
    {
        FakeDaemon daemon;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &actions) { return std::unique_ptr<GlobalShortcut>(std::make_unique<FakeBinding>(actions)); });
        daemon.ids = u"[{\"id\":\"peek\"}]"_s;
        emit daemon.availabilityChanged(true);
        QTRY_COMPARE(shortcuts.ids().size(), 1);
        QSignalSpy changed(&shortcuts, &TriggerShortcuts::changed);
        emit daemon.availabilityChanged(false);
        QVERIFY(shortcuts.ids().isEmpty());
        QCOMPARE(changed.size(), 1);
    }
    void atMostEightAreAsked()
    {
        FakeDaemon daemon;
        QString list = u"["_s;
        for (int i = 0; i < 20; ++i)
            list += (i ? u","_s : QString()) + u"{\"id\":\"k%1\"}"_s.arg(i);
        daemon.ids = list + u"]"_s;
        TriggerShortcuts shortcuts(&daemon, [&](const QList<ShortcutAction> &actions) { return std::unique_ptr<GlobalShortcut>(std::make_unique<FakeBinding>(actions)); });
        emit daemon.availabilityChanged(true);
        QTRY_COMPARE(shortcuts.ids().size(), 8);
    }
};

QTEST_GUILESS_MAIN(TstTriggerShortcuts)
#include "tst_triggershortcuts.moc"
