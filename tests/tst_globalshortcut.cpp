// The global shortcut backends against fake portal and kglobalaccel services on a private bus, and the chain that
// falls back from one to the other. The services are fakes of the documented protocols; a real kglobalacceld is
// exercised by tools/test-overlay-nested.sh.

#include "globalshortcut.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// One entry of BindShortcuts' a(sa{sv}).
struct FakeShortcutSpec {
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &argument, const FakeShortcutSpec &spec)
{
    argument.beginStructure();
    argument << spec.id << spec.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, FakeShortcutSpec &spec)
{
    argument.beginStructure();
    argument >> spec.id >> spec.properties;
    argument.endStructure();
    return argument;
}

class FakeSession : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")

public:
    int closed = 0;

public slots:
    Q_SCRIPTABLE void Close() { ++closed; }
};

class FakePortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")

public:
    enum class Mode { Ok, Cancel, Silent, NoKey };

    explicit FakePortal(QDBusConnection bus) : m_bus(std::move(bus)) {}

    Mode mode = Mode::Ok;
    QString session = u"/org/freedesktop/portal/desktop/session/fake/s1"_s;
    QString boundId;      // the last one asked for
    QString trigger;
    QString description;
    QStringList boundIds; // every one, in order
    QMap<QString, QString> triggers; // preferred_trigger by id
    QSet<QString> taken;  // ids whose key another application holds: answered with an empty trigger
    int creates = 0;

public slots:
    Q_SCRIPTABLE QDBusObjectPath CreateSession(const QVariantMap &options)
    {
        ++creates;
        return respond(options, {{u"session_handle"_s, session}}, 0);
    }
    Q_SCRIPTABLE QDBusObjectPath BindShortcuts(const QDBusObjectPath &, const QList<FakeShortcutSpec> &shortcuts,
                                               const QString &, const QVariantMap &options)
    {
        QList<FakeShortcutSpec> answer;
        boundIds.clear();
        for (const FakeShortcutSpec &spec : shortcuts) {
            boundId = spec.id;
            boundIds << spec.id;
            trigger = spec.properties.value(u"preferred_trigger"_s).toString();
            triggers.insert(spec.id, trigger);
            description = spec.properties.value(u"description"_s).toString();
            // "LOGO+SHIFT+p" is described as "Meta+Shift+P"; KDE answers success with an empty trigger when the keys are taken or were
            // cleared by the user.
            QString text = trigger;
            text.replace(u"LOGO"_s, u"Meta"_s).replace(u"SHIFT"_s, u"Shift"_s);
            text = text.left(text.size() - 1) + text.right(1).toUpper();
            answer.append({spec.id, {{u"trigger_description"_s, mode == Mode::NoKey || taken.contains(spec.id) ? QString() : text}}});
        }
        return respond(options, {{u"shortcuts"_s, QVariant::fromValue(answer)}}, mode == Mode::Cancel ? 1 : 0);
    }

signals:
    Q_SCRIPTABLE void Activated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp,
                                const QVariantMap &options);
    Q_SCRIPTABLE void Deactivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp,
                                  const QVariantMap &options);

private:
    QDBusObjectPath respond(const QVariantMap &options, const QVariantMap &results, uint code)
    {
        QString sender = message().service();
        sender.remove(u':').replace(u'.', u'_');
        const QString path = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(
            sender, options.value(u"handle_token"_s).toString());
        if (mode != Mode::Silent) {
            // After the method reply, like the real portal; the Request object itself is not needed by the client.
            QTimer::singleShot(10, this, [this, results, code, path] {
                QDBusMessage response = QDBusMessage::createSignal(path, u"org.freedesktop.portal.Request"_s, u"Response"_s);
                response.setArguments({code, results});
                m_bus.send(response);
            });
        }
        return QDBusObjectPath(path);
    }

    QDBusConnection m_bus;
};

class FakeComponent : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kglobalaccel.Component")

signals:
    Q_SCRIPTABLE void globalShortcutPressed(const QString &component, const QString &action, qlonglong timestamp);
    Q_SCRIPTABLE void globalShortcutReleased(const QString &component, const QString &action, qlonglong timestamp);
};

class FakeKGlobalAccel : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KGlobalAccel")

public:
    QStringList registeredId;           // the last
    QList<QStringList> registered;      // all
    QList<uint> flags;
    QList<int> keys;                    // the last
    QMap<QString, QList<int>> keysById;
    QSet<QString> taken;                // actions whose keys another application holds: setShortcut answers none
    QList<QStringList> unregistered;    // actions taken out with unRegister
    QString failRegisterOf;             // doRegister of this action is an error
    QMap<QString, QList<int>> answer;   // keys setShortcut answers instead of the asked ones (the user changed them in System Settings)

public slots:
    Q_SCRIPTABLE void doRegister(const QStringList &actionId)
    {
        if (actionId.value(1) == failRegisterOf && !failRegisterOf.isEmpty()) {
            sendErrorReply(QDBusError::Failed, u"cannot register"_s);
            return;
        }
        registeredId = actionId;
        registered << actionId;
    }
    Q_SCRIPTABLE QList<int> setShortcut(const QStringList &id, const QList<int> &k, uint f)
    {
        keys = k;
        keysById.insert(id.value(1), k);
        flags << f;
        if (answer.contains(id.value(1)))
            return answer.value(id.value(1));
        return taken.contains(id.value(1)) ? QList<int>() : k;
    }
    Q_SCRIPTABLE bool unRegister(const QStringList &actionId)
    {
        unregistered << actionId;
        return true;
    }
    Q_SCRIPTABLE QDBusObjectPath getComponent(const QString &component)
    {
        return QDBusObjectPath(u"/component/"_s + component);
    }
};

} // namespace

class TstGlobalShortcut : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void portalBindsTheShortcutAndReportsActivations();
    void portalCancelledByTheUser();
    void portalThatNeverAnswers();
    void portalMissing();
    void kglobalaccelRegistersAndReportsPresses();
    void kglobalaccelMissing();
    void chainFallsBackToTheNextBackend();
    void chainFailsWhenEveryBackendDoes();
    void portalWithoutKeyFallsBackToKGlobalAccel();
    void anActionWhoseKeyIsTakenIsReportedUnboundAndFiresOnceItHasOne();
    void aPortalWithoutTheArrangeKeyFallsBackEvenIfItBoundTheOthers();
    void kglobalaccelReportsATakenKeyAndStillBindsTheOthers();
    void kglobalaccelShowsTheKeyTheUserChose();
    void kglobalaccelKeepsWhatRegisteredWhenALaterOneFails();
    void keyCodesAreWrittenAsText();
    void aKeyOfZeroIsNoKeyAndNeverPrintsAsOne();
    void onPlasmaKGlobalAccelComesFirstEvenIfThePortalNeverAnswers();
    void withoutKGlobalAccelThePortalIsFirst();
    void kglobalaccelWithNoKeyAtAllFails();
    void theShortcutsOfTriggersNeedNoKeyAndReportReleases();
    void kglobalaccelBindsTriggerActionsWithoutKeysAndReportsReleases();
    void actionsNoLongerWantedAreTakenOutOfKGlobalAccel();

private:
    QDBusConnection server = QDBusConnection::sessionBus();
    QDBusConnection client{u"unused"_s};
    std::unique_ptr<FakePortal> portal;
    std::unique_ptr<FakeKGlobalAccel> kga;
    std::unique_ptr<FakeComponent> component;
    QStringList services;
};

void TstGlobalShortcut::initTestCase()
{
    // These fakes take well-known names that belong to the user's desktop: only ever on a private bus.
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
    QVERIFY(server.isConnected());
    qDBusRegisterMetaType<QList<int>>();
    qDBusRegisterMetaType<FakeShortcutSpec>();
    qDBusRegisterMetaType<QList<FakeShortcutSpec>>();
    client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-shortcut-client"_s);
    QVERIFY(client.isConnected());
}

void TstGlobalShortcut::init()
{
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus");
    portal = std::make_unique<FakePortal>(server);
    kga = std::make_unique<FakeKGlobalAccel>();
    component = std::make_unique<FakeComponent>();
}

void TstGlobalShortcut::cleanup()
{
    for (const QString &name : std::as_const(services))
        server.unregisterService(name);
    services.clear();
    server.unregisterObject(u"/org/freedesktop/portal/desktop"_s);
    server.unregisterObject(u"/kglobalaccel"_s);
    server.unregisterObject(u"/component/murasaki"_s);
    portal.reset();
    kga.reset();
    component.reset();
}

void TstGlobalShortcut::portalBindsTheShortcutAndReportsActivations()
{
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    FakeSession session;
    QVERIFY(server.registerObject(portal->session, &session, QDBusConnection::ExportScriptableSlots));

    {
    PortalShortcut shortcut(client, u"io.github.uriel2121ger_art.Murasaki.Overlay"_s);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(shortcut.backend(), u"portal"_s);
    QCOMPARE(portal->boundIds, (QStringList{u"toggle-arrange"_s, u"toggle-hidden"_s, u"rescue"_s, u"toggle-interact"_s})); // one BindShortcuts, four entries
    QCOMPARE(portal->triggers.value(u"toggle-arrange"_s), u"LOGO+SHIFT+p"_s); // Meta+Shift+P
    QCOMPARE(portal->triggers.value(u"toggle-hidden"_s), u"LOGO+SHIFT+h"_s);
    QCOMPARE(portal->triggers.value(u"rescue"_s), u"LOGO+SHIFT+r"_s);
    QCOMPARE(portal->triggers.value(u"toggle-interact"_s), u"LOGO+SHIFT+w"_s);
    QVERIFY(!portal->description.isEmpty());

    emit portal->Activated(QDBusObjectPath(portal->session), u"toggle-arrange"_s, 1, {});
    QTRY_COMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), u"toggle-arrange"_s);
    emit portal->Activated(QDBusObjectPath(portal->session), u"toggle-hidden"_s, 11, {});
    emit portal->Activated(QDBusObjectPath(portal->session), u"rescue"_s, 12, {});
    QTRY_COMPARE(activated.count(), 3);
    QCOMPARE(activated.at(1).at(0).toString(), u"toggle-hidden"_s);
    QCOMPARE(activated.at(2).at(0).toString(), u"rescue"_s);
    emit portal->Activated(QDBusObjectPath(portal->session), u"something-else"_s, 2, {});
    emit portal->Activated(QDBusObjectPath(u"/other/session"_s), u"toggle-arrange"_s, 3, {});
    QTest::qWait(100);
    QCOMPARE(activated.count(), 3);
    QCOMPARE(shortcut.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s); // what the portal said it bound
    QCOMPARE(shortcut.trigger(u"toggle-hidden"_s), u"Meta+Shift+H"_s);
    QCOMPARE(shortcut.trigger(u"rescue"_s), u"Meta+Shift+R"_s);
    QCOMPARE(shortcut.actionIds(), (QStringList{u"toggle-arrange"_s, u"toggle-hidden"_s, u"rescue"_s, u"toggle-interact"_s}));
    QCOMPARE(session.closed, 0);
    }
    // Leaving closes the session: the portal would keep it (and the binding) for a process that is gone.
    QTRY_COMPARE(session.closed, 1);
    server.unregisterObject(portal->session);
}

void TstGlobalShortcut::portalCancelledByTheUser()
{
    portal->mode = FakePortal::Mode::Cancel;
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    PortalShortcut shortcut(client, QString());
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QVERIFY(failed.at(0).at(0).toString().contains(u"cancel"_s));
    QCOMPARE(registered.count(), 0);
    QCOMPARE(shortcut.backend(), QString());
}

void TstGlobalShortcut::portalThatNeverAnswers()
{
    // The method returns but no Response ever comes: the start must not hang the chain forever. The portal's
    // timeout is long (the user may be answering a dialog), so only check that nothing is reported early.
    portal->mode = FakePortal::Mode::Silent;
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    PortalShortcut shortcut(client, QString());
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(portal->creates, 1, 3000);
    QTest::qWait(200);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(registered.count(), 0);
}

void TstGlobalShortcut::portalMissing()
{
    PortalShortcut shortcut(client, QString());
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(shortcut.backend(), QString());
}

void TstGlobalShortcut::kglobalaccelRegistersAndReportsPresses()
{
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;

    KGlobalAccelShortcut shortcut(client);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(shortcut.backend(), u"kglobalaccel"_s);
    QCOMPARE(kga->registered.size(), 4); // one registration per action
    for (const QStringList &id : std::as_const(kga->registered)) {
        QCOMPARE(id.value(0), u"murasaki"_s);
        QCOMPARE(id.size(), 4);
    }
    QCOMPARE(kga->registered.value(0).value(1), u"toggle-arrange"_s);
    QCOMPARE(kga->registered.value(1).value(1), u"toggle-hidden"_s);
    QCOMPARE(kga->registered.value(2).value(1), u"rescue"_s);
    QCOMPARE(kga->registered.value(3).value(1), u"toggle-interact"_s);
    QCOMPARE(kga->keysById.value(u"toggle-arrange"_s), (QList<int>{kArrangeShortcutKeyCode}));
    QCOMPARE(kga->keysById.value(u"toggle-hidden"_s), (QList<int>{kHiddenShortcutKeyCode}));
    QCOMPARE(kga->keysById.value(u"rescue"_s), (QList<int>{kRescueShortcutKeyCode}));
    QCOMPARE(kga->keysById.value(u"toggle-interact"_s), (QList<int>{kInteractShortcutKeyCode}));
    QCOMPARE(kga->flags, (QList<uint>{8, 2, 8, 2, 8, 2, 8, 2})); // the default, then the active shortcut, for each
    QCOMPARE(kArrangeShortcutKeyCode, QKeyCombination(Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_P).toCombined());
    QCOMPARE(kHiddenShortcutKeyCode, QKeyCombination(Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_H).toCombined());
    QCOMPARE(kRescueShortcutKeyCode, QKeyCombination(Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_R).toCombined());
    QCOMPARE(kInteractShortcutKeyCode, QKeyCombination(Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_W).toCombined());
    QCOMPARE(shortcut.trigger(u"rescue"_s), u"Meta+Shift+R"_s);

    emit component->globalShortcutPressed(u"murasaki"_s, u"toggle-arrange"_s, 5);
    QTRY_COMPARE(activated.count(), 1);
    emit component->globalShortcutPressed(u"murasaki"_s, u"rescue"_s, 8);
    emit component->globalShortcutPressed(u"murasaki"_s, u"toggle-hidden"_s, 9);
    QTRY_COMPARE(activated.count(), 3);
    QCOMPARE(activated.at(1).at(0).toString(), u"rescue"_s);
    QCOMPARE(activated.at(2).at(0).toString(), u"toggle-hidden"_s);
    emit component->globalShortcutPressed(u"murasaki"_s, u"other"_s, 6);
    emit component->globalShortcutPressed(u"kwin"_s, u"toggle-arrange"_s, 7);
    QTest::qWait(100);
    QCOMPARE(activated.count(), 3);
}

void TstGlobalShortcut::kglobalaccelMissing()
{
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
}

void TstGlobalShortcut::chainFallsBackToTheNextBackend()
{
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;

    ShortcutChain chain({new PortalShortcut(client, QString()), new KGlobalAccelShortcut(client)});
    QSignalSpy registered(&chain, &GlobalShortcut::registered);
    QSignalSpy failed(&chain, &GlobalShortcut::failed);
    QSignalSpy activated(&chain, &GlobalShortcut::activated);
    chain.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 8000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(chain.backend(), u"kglobalaccel"_s);
    emit component->globalShortcutPressed(u"murasaki"_s, u"toggle-arrange"_s, 1);
    QTRY_COMPARE(activated.count(), 1);
}

void TstGlobalShortcut::chainFailsWhenEveryBackendDoes()
{
    ShortcutChain chain({new PortalShortcut(client, QString()), new KGlobalAccelShortcut(client)});
    QSignalSpy failed(&chain, &GlobalShortcut::failed);
    QSignalSpy registered(&chain, &GlobalShortcut::registered);
    chain.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 8000);
    QCOMPARE(registered.count(), 0);
    const QString reason = failed.at(0).at(0).toString();
    QVERIFY2(reason.contains(u"PortalShortcut"_s) && reason.contains(u"KGlobalAccelShortcut"_s), qPrintable(reason));
    QCOMPARE(chain.backend(), QString());
}

void TstGlobalShortcut::portalWithoutKeyFallsBackToKGlobalAccel()
{
    portal->mode = FakePortal::Mode::NoKey;
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;

    ShortcutChain chain({new PortalShortcut(client, QString()), new KGlobalAccelShortcut(client)});
    QSignalSpy registered(&chain, &GlobalShortcut::registered);
    chain.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 8000);
    QCOMPARE(portal->boundId, u"toggle-interact"_s); // the portal was asked first (the last of its four)
    QCOMPARE(chain.backend(), u"kglobalaccel"_s);
}

void TstGlobalShortcut::anActionWhoseKeyIsTakenIsReportedUnboundAndFiresOnceItHasOne()
{
    portal->taken << u"rescue"_s; // another application has Meta+Shift+R
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    FakeSession session;
    QVERIFY(server.registerObject(portal->session, &session, QDBusConnection::ExportScriptableSlots));
    const auto cleanup = qScopeGuard([&] { server.unregisterObject(portal->session); });
    {
        PortalShortcut shortcut(client, QString());
        QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
        QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
        QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
        shortcut.start();
        QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000); // arrange and hide are bound: it works
        QCOMPARE(failed.count(), 0);
        QCOMPARE(shortcut.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
        QCOMPARE(shortcut.trigger(u"rescue"_s), QString()); // reported as unbound
        QVERIFY(shortcut.actionIds().contains(u"rescue"_s));
        // the user gives it a key later, in the portal's dialog: the press arrives and counts, with no restart
        emit portal->Activated(QDBusObjectPath(portal->session), u"rescue"_s, 1, {});
        emit portal->Activated(QDBusObjectPath(portal->session), u"toggle-hidden"_s, 2, {});
        emit portal->Activated(QDBusObjectPath(portal->session), u"nonsense"_s, 3, {});
        QTRY_COMPARE(activated.count(), 2);
        QTest::qWait(100);
        QCOMPARE(activated.count(), 2);
        QCOMPARE(activated.at(0).at(0).toString(), u"rescue"_s);
        QCOMPARE(activated.at(1).at(0).toString(), u"toggle-hidden"_s);
    }
    QTRY_COMPARE(session.closed, 1);
}

void TstGlobalShortcut::aPortalWithoutTheArrangeKeyFallsBackEvenIfItBoundTheOthers()
{
    // The old rule: the arrange shortcut is the one that has to have its key. A portal that bound only the newer two must not stop the chain
    // from reaching kglobalaccel, where the user's Meta+Shift+P lives.
    portal->taken << u"toggle-arrange"_s;
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    {
        PortalShortcut alone(client, QString());
        QSignalSpy failed(&alone, &GlobalShortcut::failed);
        alone.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QVERIFY(failed.at(0).at(0).toString().contains(u"toggle-arrange"_s));
    }
    ShortcutChain chain({new PortalShortcut(client, QString()), new KGlobalAccelShortcut(client)});
    QSignalSpy registered(&chain, &GlobalShortcut::registered);
    chain.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 8000);
    QCOMPARE(chain.backend(), u"kglobalaccel"_s);
    QCOMPARE(chain.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
    QCOMPARE(chain.trigger(u"rescue"_s), u"Meta+Shift+R"_s);
}

void TstGlobalShortcut::kglobalaccelReportsATakenKeyAndStillBindsTheOthers()
{
    kga->taken << u"toggle-hidden"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(shortcut.trigger(u"toggle-hidden"_s), QString());
    QCOMPARE(shortcut.trigger(u"rescue"_s), u"Meta+Shift+R"_s);
    // the one without a key fires as soon as the user gives it one in System Settings (no restart): a press of it counts
    emit component->globalShortcutPressed(u"murasaki"_s, u"toggle-hidden"_s, 1);
    emit component->globalShortcutPressed(u"murasaki"_s, u"rescue"_s, 2);
    QTRY_COMPARE(activated.count(), 2);
    QCOMPARE(activated.at(0).at(0).toString(), u"toggle-hidden"_s);
    QCOMPARE(activated.at(1).at(0).toString(), u"rescue"_s);
}

void TstGlobalShortcut::kglobalaccelShowsTheKeyTheUserChose()
{
    // kglobalaccel keeps a binding the user changed and answers with it: Ctrl+Alt+F5 for the hide, Meta+Shift+K for the rescue
    kga->answer.insert(u"toggle-hidden"_s, {0x04000000 | 0x08000000 | 0x01000034});
    kga->answer.insert(u"rescue"_s, {kHiddenShortcutKeyCode - 0x48 + 0x4b});
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(shortcut.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
    QCOMPARE(shortcut.trigger(u"toggle-hidden"_s), u"Ctrl+Alt+F5"_s);
    QCOMPARE(shortcut.trigger(u"rescue"_s), u"Meta+Shift+K"_s);
}

void TstGlobalShortcut::kglobalaccelKeepsWhatRegisteredWhenALaterOneFails()
{
    // doRegister of the third action fails: the first two stay bound and usable
    kga->failRegisterOf = u"rescue"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(shortcut.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
    QCOMPARE(shortcut.trigger(u"toggle-hidden"_s), u"Meta+Shift+H"_s);
    QCOMPARE(shortcut.trigger(u"rescue"_s), QString());
    emit component->globalShortcutPressed(u"murasaki"_s, u"toggle-hidden"_s, 1);
    QTRY_COMPARE(activated.count(), 1);
}

void TstGlobalShortcut::keyCodesAreWrittenAsText()
{
    QCOMPARE(shortcutKeyText(kArrangeShortcutKeyCode), u"Meta+Shift+P"_s);
    QCOMPARE(shortcutKeyText(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Space).toCombined()), u"Ctrl+Shift+Space"_s);
    QCOMPARE(shortcutKeyText(QKeyCombination(Qt::AltModifier, Qt::Key_F12).toCombined()), u"Alt+F12"_s);
    QCOMPARE(shortcutKeyText(QKeyCombination(Qt::MetaModifier, Qt::Key_5).toCombined()), u"Meta+5"_s);
    QCOMPARE(shortcutKeyText(QKeyCombination(Qt::NoModifier, Qt::Key_Escape).toCombined()), u"Key 0x1000000"_s);
}

void TstGlobalShortcut::kglobalaccelWithNoKeyAtAllFails()
{
    kga->taken << u"toggle-arrange"_s << u"toggle-hidden"_s << u"rescue"_s << u"toggle-interact"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy failed(&shortcut, &GlobalShortcut::failed);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QCOMPARE(shortcut.backend(), QString());
}

void TstGlobalShortcut::aKeyOfZeroIsNoKeyAndNeverPrintsAsOne()
{
    // kglobalaccel answers [0] for a shortcut it could not give a key (another application holds Meta+Shift+R, or the user cleared it)
    kga->answer.insert(u"rescue"_s, {0});
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    KGlobalAccelShortcut shortcut(client);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(shortcut.trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
    QCOMPARE(shortcut.trigger(u"toggle-hidden"_s), u"Meta+Shift+H"_s);
    QCOMPARE(shortcut.trigger(u"rescue"_s), QString()); // not "Key 0x0"
    QVERIFY(!shortcut.trigger(u"rescue"_s).contains(u"0x"_s));
    QCOMPARE(shortcutKeyText(0), QString());
    QCOMPARE(shortcutKeyText(0x10000000 | 0x02000000), QString()); // modifiers with no key are no key either
    // all four at zero: nothing bound, the backend fails (so the chain goes on)
    kga->answer.insert(u"toggle-arrange"_s, {0});
    kga->answer.insert(u"toggle-hidden"_s, {0});
    kga->answer.insert(u"toggle-interact"_s, {0});
    KGlobalAccelShortcut none(client);
    QSignalSpy failed(&none, &GlobalShortcut::failed);
    none.start();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
}

void TstGlobalShortcut::onPlasmaKGlobalAccelComesFirstEvenIfThePortalNeverAnswers()
{
    // The portal registers its session but never answers BindShortcuts (it waits for an approval nobody sees): the regression was two minutes of "starting".
    portal->mode = FakePortal::Mode::Silent;
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    const auto chain = makeShortcutChain(client, QString());
    QSignalSpy registered(chain.get(), &GlobalShortcut::registered);
    QElapsedTimer clock;
    clock.start();
    chain->start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 8000);
    QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(chain->backend(), u"kglobalaccel"_s);
    QCOMPARE(portal->creates, 0); // the portal was never asked
    QCOMPARE(chain->trigger(u"toggle-arrange"_s), u"Meta+Shift+P"_s);
}

void TstGlobalShortcut::withoutKGlobalAccelThePortalIsFirst()
{
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                  QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    FakeSession session;
    QVERIFY(server.registerObject(portal->session, &session, QDBusConnection::ExportScriptableSlots));
    const auto cleanup = qScopeGuard([&] { server.unregisterObject(portal->session); });
    {
        const auto chain = makeShortcutChain(client, QString());
        QSignalSpy registered(chain.get(), &GlobalShortcut::registered);
        chain->start();
        QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 8000);
        QCOMPARE(chain->backend(), u"portal"_s);
        QCOMPARE(portal->creates, 1);
    }
    QTRY_COMPARE(session.closed, 1);
}

void TstGlobalShortcut::theShortcutsOfTriggersNeedNoKeyAndReportReleases()
{
    QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(), QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
    services << u"org.freedesktop.portal.Desktop"_s;
    FakeSession session;
    QVERIFY(server.registerObject(portal->session, &session, QDBusConnection::ExportScriptableSlots));
    portal->mode = FakePortal::Mode::NoKey; // the desktop binds nothing until the user chooses a key
    const QList<ShortcutAction> actions{{u"trigger.peek"_s, u"Peek"_s, QString(), 0, QString()}, {u"trigger.wave"_s, u"Wave"_s, QString(), 0, QString()}};
    {
        // as the fixed ones do, a binding with no key at all fails ...
        PortalShortcut strict(client, u"io.github.uriel2121ger_art.Murasaki.Overlay"_s, actions);
        QSignalSpy failed(&strict, &GlobalShortcut::failed);
        strict.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    }
    PortalShortcut shortcut(client, u"io.github.uriel2121ger_art.Murasaki.Overlay"_s, actions);
    shortcut.setAllowUnbound(true); // ... and a shortcut of a trigger does not
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    QSignalSpy released(&shortcut, &GlobalShortcut::released);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QCOMPARE(portal->boundIds, (QStringList{u"trigger.peek"_s, u"trigger.wave"_s}));
    QCOMPARE(portal->triggers.value(u"trigger.peek"_s), QString()); // no suggested key
    QVERIFY(shortcut.trigger(u"trigger.peek"_s).isEmpty());
    // the user gives it a key later: the press and the release come
    emit portal->Activated(QDBusObjectPath(portal->session), u"trigger.peek"_s, 1, {});
    emit portal->Deactivated(QDBusObjectPath(portal->session), u"trigger.peek"_s, 2, {});
    emit portal->Deactivated(QDBusObjectPath(portal->session), u"something-else"_s, 3, {});
    emit portal->Deactivated(QDBusObjectPath(u"/other/session"_s), u"trigger.peek"_s, 4, {});
    QTRY_COMPARE(released.count(), 1);
    QCOMPARE(activated.count(), 1);
    QCOMPARE(released.at(0).at(0).toString(), u"trigger.peek"_s);
    QTest::qWait(50);
    QCOMPARE(released.count(), 1);
}

void TstGlobalShortcut::kglobalaccelBindsTriggerActionsWithoutKeysAndReportsReleases()
{
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerObject(u"/component/murasaki"_s, component.get(), QDBusConnection::ExportScriptableSignals));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    const QList<ShortcutAction> actions{{u"trigger.peek"_s, u"Peek"_s, QString(), 0, QString()}};
    kga->answer.insert(u"trigger.peek"_s, QList<int>{0}); // no key
    {
        KGlobalAccelShortcut strict(client, actions);
        QSignalSpy failed(&strict, &GlobalShortcut::failed);
        strict.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    }
    KGlobalAccelShortcut shortcut(client, actions);
    shortcut.setAllowUnbound(true);
    QSignalSpy registered(&shortcut, &GlobalShortcut::registered);
    QSignalSpy activated(&shortcut, &GlobalShortcut::activated);
    QSignalSpy released(&shortcut, &GlobalShortcut::released);
    shortcut.start();
    QTRY_COMPARE_WITH_TIMEOUT(registered.count(), 1, 5000);
    QVERIFY(shortcut.trigger(u"trigger.peek"_s).isEmpty());
    emit component->globalShortcutPressed(u"murasaki"_s, u"trigger.peek"_s, 5);
    emit component->globalShortcutReleased(u"murasaki"_s, u"trigger.peek"_s, 6);
    emit component->globalShortcutReleased(u"murasaki"_s, u"other"_s, 7);
    QTRY_COMPARE(released.count(), 1);
    QCOMPARE(activated.count(), 1);
}

void TstGlobalShortcut::actionsNoLongerWantedAreTakenOutOfKGlobalAccel()
{
    QVERIFY(server.registerObject(u"/kglobalaccel"_s, kga.get(), QDBusConnection::ExportScriptableSlots));
    QVERIFY(server.registerService(u"org.kde.kglobalaccel"_s));
    services << u"org.kde.kglobalaccel"_s;
    forgetKGlobalAccelActions(client, {u"trigger.peek"_s, u"trigger.wave"_s});
    QTRY_COMPARE_WITH_TIMEOUT(kga->unregistered.size(), 2, 5000);
    QCOMPARE(kga->unregistered.value(0).value(0), u"murasaki"_s);
    QCOMPARE(kga->unregistered.value(0).value(1), u"trigger.peek"_s);
    QCOMPARE(kga->unregistered.value(1).value(1), u"trigger.wave"_s);
    forgetKGlobalAccelActions(client, {}); // nothing: nothing is called
}

QTEST_MAIN(TstGlobalShortcut)
#include "tst_globalshortcut.moc"
