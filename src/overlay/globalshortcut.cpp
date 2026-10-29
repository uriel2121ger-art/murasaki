#include "globalshortcut.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

// One entry of BindShortcuts' a(sa{sv}).
struct PortalShortcutSpec {
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcutSpec &spec)
{
    argument.beginStructure();
    argument << spec.id << spec.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcutSpec &spec)
{
    argument.beginStructure();
    argument >> spec.id >> spec.properties;
    argument.endStructure();
    return argument;
}

// One entry of kglobalaccel's a(ai): a key sequence as a list of key codes.
struct KeySequenceWire {
    QList<int> keys;
};

QDBusArgument &operator<<(QDBusArgument &argument, const KeySequenceWire &sequence)
{
    argument.beginStructure();
    argument << sequence.keys;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, KeySequenceWire &sequence)
{
    argument.beginStructure();
    argument >> sequence.keys;
    argument.endStructure();
    return argument;
}

void registerTypes()
{
    static const bool once = [] {
        qDBusRegisterMetaType<PortalShortcutSpec>();
        qDBusRegisterMetaType<QList<PortalShortcutSpec>>();
        qDBusRegisterMetaType<KeySequenceWire>();
        qDBusRegisterMetaType<QList<KeySequenceWire>>();
        qDBusRegisterMetaType<QList<int>>();
        return true;
    }();
    Q_UNUSED(once);
}

const QString kPortalService = u"org.freedesktop.portal.Desktop"_s;
const QString kPortalPath = u"/org/freedesktop/portal/desktop"_s;
const QString kShortcutsInterface = u"org.freedesktop.portal.GlobalShortcuts"_s;
const QString kRequestInterface = u"org.freedesktop.portal.Request"_s;

// Waits for the Response signal of one portal request.
class ResponseWaiter : public QObject
{
    Q_OBJECT

public:
    ResponseWaiter(QDBusConnection bus, std::function<void(uint, const QVariantMap &)> done, int timeoutMs,
                   QObject *parent)
        : QObject(parent)
        , m_bus(std::move(bus))
        , m_done(std::move(done))
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(timeoutMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { finish(2, {{u"timeout"_s, true}}); });
        m_timer.start();
    }

    void listen(const QString &path)
    {
        if (m_paths.contains(path))
            return;
        m_paths.append(path);
        m_bus.connect(kPortalService, path, kRequestInterface, u"Response"_s, this,
                      SLOT(onResponse(uint,QVariantMap)));
    }

private slots:
    void onResponse(uint code, const QVariantMap &results) { finish(code, results); }

private:
    void finish(uint code, const QVariantMap &results)
    {
        if (!m_done)
            return;
        auto done = std::exchange(m_done, nullptr);
        m_timer.stop();
        done(code, results);
        deleteLater();
    }

    QDBusConnection m_bus;
    std::function<void(uint, const QVariantMap &)> m_done;
    QTimer m_timer;
    QStringList m_paths;
};

QString newToken(const char *what)
{
    return u"murasaki_%1_%2"_s.arg(QString::fromLatin1(what)).arg(QRandomGenerator::global()->generate(), 0, 16);
}

} // namespace

const QList<ShortcutAction> &defaultShortcutActions()
{
    static const QList<ShortcutAction> actions{
        {kArrangeShortcutId, u"Toggle the arrange mode of the desktop pets"_s, u"LOGO+SHIFT+p"_s, kArrangeShortcutKeyCode, u"Meta+Shift+P"_s},
        {kHiddenShortcutId, u"Hide or show all the desktop pets"_s, u"LOGO+SHIFT+h"_s, kHiddenShortcutKeyCode, u"Meta+Shift+H"_s},
        {kRescueShortcutId, u"Rescue the desktop pets: make them visible, solid and grabbable again"_s, u"LOGO+SHIFT+r"_s, kRescueShortcutKeyCode,
         u"Meta+Shift+R"_s},
        {kInteractShortcutId, u"Click on the wallpaper: pass the mouse to interactive wallpapers until Esc"_s, u"LOGO+SHIFT+w"_s,
         kInteractShortcutKeyCode, u"Meta+Shift+W"_s}};
    return actions;
}

QString shortcutKeyText(int keyCode)
{
    // Qt's key codes: the modifiers in the high bits, the key below them.
    QStringList parts;
    if (keyCode & 0x10000000)
        parts << u"Meta"_s;
    if (keyCode & 0x04000000)
        parts << u"Ctrl"_s;
    if (keyCode & 0x08000000)
        parts << u"Alt"_s;
    if (keyCode & 0x02000000)
        parts << u"Shift"_s;
    const int key = keyCode & 0x01ffffff; // (0x01000000 marks the special keys: F-keys, Escape, ...)
    if (key == 0)
        return {}; // no key at all (kglobalaccel's 0): never "Key 0x0"
    if (key >= 0x41 && key <= 0x5a)
        parts << QString(QChar(key));
    else if (key >= 0x20 && key <= 0x7e)
        parts << (key == 0x20 ? u"Space"_s : QString(QChar(key)));
    else if (key >= 0x01000030 && key <= 0x01000047)
        parts << u"F%1"_s.arg(key - 0x01000030 + 1);
    else
        parts << u"Key 0x%1"_s.arg(key, 0, 16);
    return parts.join(u'+');
}

// ---- portal ----------------------------------------------------------------------------------------------

PortalShortcut::PortalShortcut(QDBusConnection bus, QString appId, QObject *parent)
    : PortalShortcut(std::move(bus), std::move(appId), defaultShortcutActions(), parent)
{
}

PortalShortcut::PortalShortcut(QDBusConnection bus, QString appId, QList<ShortcutAction> actions, QObject *parent)
    : GlobalShortcut(parent)
    , m_bus(std::move(bus))
    , m_appId(std::move(appId))
    , m_actions(std::move(actions))
{
    registerTypes();
}

QStringList PortalShortcut::actionIds() const
{
    QStringList ids;
    for (const ShortcutAction &action : m_actions)
        ids << action.id;
    return ids;
}

PortalShortcut::~PortalShortcut()
{
    // The session belongs to the portal until it is closed; a process that just exits leaves it behind.
    if (m_session.isEmpty() || !m_bus.isConnected())
        return;
    QDBusMessage close = QDBusMessage::createMethodCall(kPortalService, m_session,
                                                        u"org.freedesktop.portal.Session"_s, u"Close"_s);
    m_bus.call(close, QDBus::Block, 500);
}

void PortalShortcut::fail(const QString &reason)
{
    if (m_done)
        return;
    m_done = true;
    emit failed(reason);
}

void PortalShortcut::start()
{
    if (!m_bus.isConnected()) {
        fail(u"no D-Bus session bus"_s);
        return;
    }
    // A process that is neither sandboxed nor in an app scope has no identity for the portal; this announces it
    // (xdg-desktop-portal >= 1.19). Older portals lack the call and the error is ignored.
    auto proceed = [this] {
        const QString token = newToken("session");
        QDBusMessage create = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsInterface,
                                                             u"CreateSession"_s);
        create.setArguments({QVariantMap{{u"handle_token"_s, newToken("create")},
                                         {u"session_handle_token"_s, token}}});
        request(create, create.arguments().constFirst().toMap().value(u"handle_token"_s).toString(),
                [this](const QVariantMap &results) {
                    m_session = results.value(u"session_handle"_s).toString();
                    if (m_session.isEmpty()) {
                        fail(u"the portal returned no session"_s);
                        return;
                    }
                    bind();
                });
    };
    if (m_appId.isEmpty()) {
        proceed();
        return;
    }
    QDBusMessage registerApp = QDBusMessage::createMethodCall(
        kPortalService, kPortalPath, u"org.freedesktop.host.portal.Registry"_s, u"Register"_s);
    registerApp.setArguments({m_appId, QVariantMap{}});
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(registerApp, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, proceed] {
        watcher->deleteLater();
        proceed();
    });
}

void PortalShortcut::request(QDBusMessage message, const QString &token,
                             std::function<void(const QVariantMap &)> next)
{
    // The Request object is named after our unique bus name and the handle token; listen before calling, because
    // the answer can beat the method reply.
    QString sender = m_bus.baseService();
    sender.remove(u':').replace(u'.', u'_');
    const QString predicted = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token);
    auto *waiter = new ResponseWaiter(
        m_bus,
        [this, next = std::move(next)](uint code, const QVariantMap &results) {
            if (m_done)
                return;
            if (code != 0) {
                fail(results.contains(u"timeout"_s) ? u"the portal did not answer"_s
                     : code == 1                    ? u"the request was cancelled"_s
                                                    : u"the portal refused the request"_s);
                return;
            }
            next(results);
        },
        message.member() == u"BindShortcuts"_s ? 120000 : 10000, this);
    waiter->listen(predicted);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 10000), waiter);
    connect(watcher, &QDBusPendingCallWatcher::finished, waiter, [this, waiter, watcher](QDBusPendingCallWatcher *) {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            const QString error = reply.error().message();
            waiter->deleteLater();
            fail(error.isEmpty() ? reply.error().name() : error);
            return;
        }
        waiter->listen(reply.value().path()); // portals older than 0.9 do not honor the token
    });
}

void PortalShortcut::bind()
{
    m_bus.connect(kPortalService, kPortalPath, kShortcutsInterface, u"Activated"_s, this,
                  SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    m_bus.connect(kPortalService, kPortalPath, kShortcutsInterface, u"Deactivated"_s, this,
                  SLOT(onDeactivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    QDBusMessage message = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsInterface,
                                                          u"BindShortcuts"_s);
    QList<PortalShortcutSpec> specs;
    for (const ShortcutAction &action : std::as_const(m_actions)) {
        QVariantMap properties{{u"description"_s, action.description}};
        if (!action.portalTrigger.isEmpty())
            properties.insert(u"preferred_trigger"_s, action.portalTrigger); // (a shortcut of a trigger suggests none)
        specs.append({action.id, properties});
    }
    const QString token = newToken("bind");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)),
                          QVariant::fromValue(specs), QString(),
                          QVariantMap{{u"handle_token"_s, token}}});
    request(message, token, [this](const QVariantMap &results) {
        // The portal says which trigger it bound (the user may have chosen another one): a(sa{sv}).
        const QVariant shortcuts = results.value(u"shortcuts"_s);
        if (shortcuts.canConvert<QDBusArgument>()) {
            const QList<PortalShortcutSpec> bound = qdbus_cast<QList<PortalShortcutSpec>>(shortcuts.value<QDBusArgument>());
            for (const PortalShortcutSpec &spec : bound)
                m_triggers.insert(spec.id, spec.properties.value(u"trigger_description"_s).toString());
        }
        // KDE's portal keeps a binding without keys when the trigger was taken (or the user cleared it) and still
        // answers with success; such a shortcut never fires. When none has a key, let the next backend try; when only some
        // have, the others are reported by the caller (trigger() is empty for them).
        // The arrange shortcut is the one that always worked: the portal counts as bound only if it got its key (a portal that bound only the
        // newer ones would stop the chain from reaching kglobalaccel, where the user's Meta+Shift+P lives). The others without a key are reported.
        if (!allowUnbound() && m_triggers.value(QString(kArrangeShortcutId)).isEmpty()) {
            fail(u"the portal bound no key for %1"_s.arg(QString(kArrangeShortcutId)));
            return;
        }
        m_bound = true;
        m_done = true;
        emit registered();
    });
}

void PortalShortcut::onActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong,
                                 const QVariantMap &)
{
    // An action that got no key when it was bound can be given one later (the portal's own dialog), so a press of any of ours counts.
    if (m_bound && session.path() == m_session && actionIds().contains(shortcutId))
        emit activated(shortcutId);
}

void PortalShortcut::onDeactivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong, const QVariantMap &)
{
    if (m_bound && session.path() == m_session && actionIds().contains(shortcutId))
        emit released(shortcutId);
}

// ---- kglobalaccel ------------------------------------------------------------------------------------------

namespace {

const QString kKGlobalAccelService = u"org.kde.kglobalaccel"_s;
const QString kKGlobalAccelPath = u"/kglobalaccel"_s;
const QString kKGlobalAccelInterface = u"org.kde.KGlobalAccel"_s;
const QString kComponentName = u"murasaki"_s;

QStringList actionId(const ShortcutAction &action)
{
    // component unique, action unique, component friendly, action friendly
    return {kComponentName, action.id, u"Murasaki"_s, action.description};
}

// KGlobalAccel's SetShortcutFlag values.
constexpr uint kSetPresent = 2;
constexpr uint kIsDefault = 8;

} // namespace

KGlobalAccelShortcut::KGlobalAccelShortcut(QDBusConnection bus, QObject *parent)
    : KGlobalAccelShortcut(std::move(bus), defaultShortcutActions(), parent)
{
}

KGlobalAccelShortcut::KGlobalAccelShortcut(QDBusConnection bus, QList<ShortcutAction> actions, QObject *parent)
    : GlobalShortcut(parent)
    , m_bus(std::move(bus))
    , m_actions(std::move(actions))
{
    registerTypes();
}

QStringList KGlobalAccelShortcut::actionIds() const
{
    QStringList ids;
    for (const ShortcutAction &action : m_actions)
        ids << action.id;
    return ids;
}

void KGlobalAccelShortcut::fail(const QString &reason)
{
    if (m_done)
        return;
    m_done = true;
    emit failed(reason);
}

void KGlobalAccelShortcut::start()
{
    if (!m_bus.isConnected()) {
        fail(u"no D-Bus session bus"_s);
        return;
    }
    registerAction(0);
}

// One action after the other: doRegister, the default shortcut, then the active one (kglobalaccel keeps a binding the user changed). The answer of
// the last call is the keys it really bound: none when another application has them.
void KGlobalAccelShortcut::registerAction(int index)
{
    if (index >= m_actions.size()) {
        finishRegistering();
        return;
    }
    const ShortcutAction action = m_actions.at(index);
    QDBusMessage doRegister = QDBusMessage::createMethodCall(kKGlobalAccelService, kKGlobalAccelPath,
                                                             kKGlobalAccelInterface, u"doRegister"_s);
    doRegister.setArguments({actionId(action)});
    auto *registerCall = new QDBusPendingCallWatcher(m_bus.asyncCall(doRegister, 5000), this);
    connect(registerCall, &QDBusPendingCallWatcher::finished, this, [this, index, action](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        const QDBusPendingReply<> reply = *done;
        if (reply.isError()) {
            // What registered before stays: a later action that cannot register is left without a key.
            if (hasAnyKey())
                finishRegistering();
            else
                fail(reply.error().message().isEmpty() ? reply.error().name() : reply.error().message());
            return;
        }
        const QList<int> keys{action.keyCode};
        const auto setShortcut = [this, keys, action](uint flags) {
            QDBusMessage message = QDBusMessage::createMethodCall(kKGlobalAccelService, kKGlobalAccelPath,
                                                                  kKGlobalAccelInterface, u"setShortcut"_s);
            message.setArguments({actionId(action), QVariant::fromValue(keys), flags});
            return m_bus.asyncCall(message, 5000);
        };
        auto *defaults = new QDBusPendingCallWatcher(setShortcut(kIsDefault), this);
        connect(defaults, &QDBusPendingCallWatcher::finished, this,
                [this, index, action, keys, setShortcut](QDBusPendingCallWatcher *defaultsDone) {
                    defaultsDone->deleteLater();
                    auto *active = new QDBusPendingCallWatcher(setShortcut(kSetPresent), this);
                    connect(active, &QDBusPendingCallWatcher::finished, this,
                            [this, index, action, keys](QDBusPendingCallWatcher *activeDone) {
                                activeDone->deleteLater();
                                const QDBusPendingReply<QList<int>> reply = *activeDone;
                                if (reply.isError()) {
                                    if (hasAnyKey())
                                        finishRegistering();
                                    else
                                        fail(reply.error().message().isEmpty() ? reply.error().name() : reply.error().message());
                                    return;
                                }
                                // kglobalaccel answers with the keys it bound; a key of 0 is "no key" (what it says for a shortcut another application
                                // holds, or one the user cleared), not a key.
                                QList<int> bound = reply.value();
                                const bool asked = bound == keys;
                                bound.removeAll(0);
                                m_triggers.insert(action.id, bound.isEmpty() ? QString() : asked ? action.text : shortcutKeyText(bound.first()));
                                registerAction(index + 1);
                            });
                });
    });
}

bool KGlobalAccelShortcut::hasAnyKey() const
{
    return std::any_of(m_triggers.cbegin(), m_triggers.cend(), [](const QString &t) { return !t.isEmpty(); });
}

void KGlobalAccelShortcut::finishRegistering()
{
    if (!allowUnbound() && !hasAnyKey()) {
        fail(u"kglobalaccel bound no key"_s);
        return;
    }
    QDBusMessage component = QDBusMessage::createMethodCall(kKGlobalAccelService, kKGlobalAccelPath, kKGlobalAccelInterface,
                                                            u"getComponent"_s);
    component.setArguments({kComponentName});
    auto *lookup = new QDBusPendingCallWatcher(m_bus.asyncCall(component, 5000), this);
    connect(lookup, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *lookupDone) {
        lookupDone->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *lookupDone;
        if (reply.isError()) {
            fail(reply.error().message());
            return;
        }
        m_bus.connect(kKGlobalAccelService, reply.value().path(), u"org.kde.kglobalaccel.Component"_s,
                      u"globalShortcutPressed"_s, this, SLOT(onPressed(QString, QString, qlonglong)));
        m_bus.connect(kKGlobalAccelService, reply.value().path(), u"org.kde.kglobalaccel.Component"_s,
                      u"globalShortcutReleased"_s, this, SLOT(onReleased(QString, QString, qlonglong)));
        m_bound = true;
        m_done = true;
        emit registered();
    });
}

void KGlobalAccelShortcut::onReleased(const QString &component, const QString &action, qlonglong)
{
    if (m_bound && component == kComponentName && actionIds().contains(action))
        emit released(action);
}

void KGlobalAccelShortcut::onPressed(const QString &component, const QString &action, qlonglong)
{
    // An action that got no key when it was registered can be given one later (System Settings, Shortcuts), so a press of any of ours counts.
    if (m_bound && component == kComponentName && actionIds().contains(action))
        emit activated(action);
}

// ---- chain ---------------------------------------------------------------------------------------------

std::unique_ptr<GlobalShortcut> makeShortcutChain(QDBusConnection bus, const QString &appId)
{
    // On KDE Plasma kglobalaccel is the backend that has always worked and answers at once, and it is where the user's shortcuts live (System Settings).
    // The portal on Plasma may wait for an approval nobody sees, up to two minutes, so it comes second there; elsewhere it is the only one that can work.
    bool kde = false;
    if (bus.isConnected() && bus.interface()) {
        kde = bus.interface()->isServiceRegistered(u"org.kde.kglobalaccel"_s)
            || bus.interface()->activatableServiceNames().value().contains(u"org.kde.kglobalaccel"_s);
    }
    QList<GlobalShortcut *> candidates;
    if (kde)
        candidates = {new KGlobalAccelShortcut(bus), new PortalShortcut(bus, appId)};
    else
        candidates = {new PortalShortcut(bus, appId), new KGlobalAccelShortcut(bus)};
    return std::make_unique<ShortcutChain>(candidates);
}

void forgetKGlobalAccelActions(const QDBusConnection &bus, const QStringList &actionIds)
{
    if (!bus.isConnected())
        return;
    for (const QString &id : actionIds) {
        QDBusMessage message = QDBusMessage::createMethodCall(kKGlobalAccelService, kKGlobalAccelPath, kKGlobalAccelInterface, u"unRegister"_s);
        message.setArguments({actionId(ShortcutAction{id, id, QString(), 0, QString()})});
        message.setAutoStartService(false);
        bus.asyncCall(message, 2000);
    }
}

std::unique_ptr<GlobalShortcut> makeTriggerShortcutChain(QDBusConnection bus, const QString &appId, const QList<ShortcutAction> &actions)
{
    bool kde = false;
    if (bus.isConnected() && bus.interface()) {
        kde = bus.interface()->isServiceRegistered(u"org.kde.kglobalaccel"_s)
            || bus.interface()->activatableServiceNames().value().contains(u"org.kde.kglobalaccel"_s);
    }
    QList<GlobalShortcut *> candidates;
    if (kde)
        candidates = {new KGlobalAccelShortcut(bus, actions), new PortalShortcut(bus, appId, actions)};
    else
        candidates = {new PortalShortcut(bus, appId, actions), new KGlobalAccelShortcut(bus, actions)};
    auto chain = std::make_unique<ShortcutChain>(candidates);
    chain->setAllowUnbound(true);
    return chain;
}

void ShortcutChain::setAllowUnbound(bool allow)
{
    GlobalShortcut::setAllowUnbound(allow);
    for (GlobalShortcut *candidate : std::as_const(m_candidates))
        candidate->setAllowUnbound(allow);
}

ShortcutChain::ShortcutChain(QList<GlobalShortcut *> candidates, QObject *parent)
    : GlobalShortcut(parent)
    , m_candidates(std::move(candidates))
{
    for (GlobalShortcut *candidate : std::as_const(m_candidates)) {
        candidate->setParent(this);
        connect(candidate, &GlobalShortcut::released, this, [this, candidate](const QString &actionId) {
            if (candidate == m_active)
                emit released(actionId);
        });
        connect(candidate, &GlobalShortcut::activated, this, [this, candidate](const QString &actionId) {
            if (candidate == m_active)
                emit activated(actionId);
        });
        connect(candidate, &GlobalShortcut::registered, this, [this, candidate] {
            if (candidate == m_active)
                emit registered();
        });
        connect(candidate, &GlobalShortcut::failed, this, [this, candidate](const QString &reason) {
            if (candidate != m_active)
                return;
            m_reasons << u"%1: %2"_s.arg(QString::fromLatin1(candidate->metaObject()->className()), reason);
            tryNext();
        });
    }
}

void ShortcutChain::start()
{
    m_index = -1;
    m_reasons.clear();
    tryNext();
}

void ShortcutChain::tryNext()
{
    m_active = nullptr;
    if (++m_index >= m_candidates.size()) {
        emit failed(m_reasons.join(u"; "_s));
        return;
    }
    m_active = m_candidates.at(m_index);
    m_active->start();
}

} // namespace murasaki

#include "globalshortcut.moc"
