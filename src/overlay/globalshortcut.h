#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace murasaki {

inline constexpr QLatin1String kArrangeShortcutId("toggle-arrange");
inline constexpr QLatin1String kHiddenShortcutId("toggle-hidden");
inline constexpr QLatin1String kRescueShortcutId("rescue");
inline constexpr QLatin1String kInteractShortcutId("toggle-interact");
// Qt::META + Qt::SHIFT + a letter, the key codes kglobalaccel takes.
inline constexpr int kArrangeShortcutKeyCode = 0x10000000 | 0x02000000 | 0x50; // P
inline constexpr int kHiddenShortcutKeyCode = 0x10000000 | 0x02000000 | 0x48;  // H
inline constexpr int kRescueShortcutKeyCode = 0x10000000 | 0x02000000 | 0x52;  // R
inline constexpr int kInteractShortcutKeyCode = 0x10000000 | 0x02000000 | 0x57; // W

// One action a global key can trigger. Anima's own keys are Ctrl+Shift+Space (hide) and Ctrl+Shift+R (rescue); the compositor owns a global key,
// and Ctrl+Shift+R would steal "hard reload" from browsers, so Murasaki suggests Meta ones, and the user changes them in the portal's dialog or
// in System Settings (kglobalaccel keeps what the user chose).
struct ShortcutAction {
    QString id;
    QString description;
    QString portalTrigger; // the portal's syntax ("LOGO+SHIFT+p")
    int keyCode;           // kglobalaccel's
    QString text;          // for people
};

// toggle-arrange (Meta+Shift+P), toggle-hidden (Meta+Shift+H), rescue (Meta+Shift+R) and toggle-interact (Meta+Shift+W).
const QList<ShortcutAction> &defaultShortcutActions();

// A key code of kglobalaccel (modifiers in the high bits) as text: "Meta+Shift+P". Letters, digits, punctuation, Space and F1 to F24; any other key is "Key 0x...".
QString shortcutKeyText(int keyCode);

// A system-wide key binding for the actions above. Optional: when no backend works the GUI, `murasakictl pets ...` and the D-Bus calls still do.
// Starting is asynchronous; exactly one of registered() or failed() follows start(). registered() means at least one action got a key; an action
// whose key was taken (or cleared by the user) has an empty entry in triggers(), which the caller reports.
class GlobalShortcut : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() = 0;
    // Which backend ended up bound ("portal", "kglobalaccel", a fake's name), or empty.
    virtual QString backend() const = 0;
    // The key the compositor actually bound for an action, as it describes it ("Meta+Shift+P"), when it said so; else empty.
    virtual QString trigger(const QString &actionId) const { Q_UNUSED(actionId); return {}; }
    // The actions this binding was asked to register, in order.
    virtual QStringList actionIds() const { return {}; }
    // For the shortcuts of the triggers: they are registered with no suggested key (the user gives them one in the desktop's own dialog), so a binding with
    // no key at all is a success, not a failure. Call before start().
    virtual void setAllowUnbound(bool allow) { m_allowUnbound = allow; }
    bool allowUnbound() const { return m_allowUnbound; }

signals:
    // The key of an action was pressed.
    void activated(const QString &actionId);
    // ... and let go (the portal's Deactivated, kglobalaccel's globalShortcutReleased): a trigger in the mode hold undoes itself then.
    void released(const QString &actionId);
    void registered();
    void failed(const QString &reason);

private:
    bool m_allowUnbound = false;
};

// org.freedesktop.portal.GlobalShortcuts: CreateSession, BindShortcuts, then the Activated signal. Desktop
// neutral; on KDE it needs xdg-desktop-portal-kde. The user is asked by the compositor on the first bind.
class PortalShortcut : public GlobalShortcut
{
    Q_OBJECT

public:
    explicit PortalShortcut(QDBusConnection bus, QString appId, QObject *parent = nullptr);
    PortalShortcut(QDBusConnection bus, QString appId, QList<ShortcutAction> actions, QObject *parent = nullptr);
    ~PortalShortcut() override;

    void start() override;
    QString backend() const override { return m_bound ? QStringLiteral("portal") : QString(); }
    QString trigger(const QString &actionId) const override { return m_triggers.value(actionId); }
    QStringList actionIds() const override;

private slots:
    void onActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp,
                     const QVariantMap &options);
    void onDeactivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong timestamp,
                       const QVariantMap &options);

private:
    // Sends `message` and calls `next` with the Response results of the request it returns (or fails).
    void request(QDBusMessage message, const QString &token, std::function<void(const QVariantMap &)> next);
    void bind();
    void fail(const QString &reason);

    QDBusConnection m_bus;
    QString m_appId;
    QString m_session;
    QList<ShortcutAction> m_actions;
    QMap<QString, QString> m_triggers;
    bool m_bound = false;
    bool m_done = false;
};

// org.kde.kglobalaccel over raw D-Bus: doRegister and setShortcut for the component "murasaki", then the
// component's globalShortcutPressed signal. KDE only; the user can rebind the key in System Settings.
class KGlobalAccelShortcut : public GlobalShortcut
{
    Q_OBJECT

public:
    explicit KGlobalAccelShortcut(QDBusConnection bus, QObject *parent = nullptr);
    KGlobalAccelShortcut(QDBusConnection bus, QList<ShortcutAction> actions, QObject *parent = nullptr);

    void start() override;
    QString backend() const override { return m_bound ? QStringLiteral("kglobalaccel") : QString(); }
    // kglobalaccel answers with the keys it bound: the text is the key as it was bound (the user may have changed it in System Settings).
    QString trigger(const QString &actionId) const override { return m_triggers.value(actionId); }
    QStringList actionIds() const override;

private slots:
    void onPressed(const QString &component, const QString &action, qlonglong timestamp);
    void onReleased(const QString &component, const QString &action, qlonglong timestamp);

private:
    void fail(const QString &reason);
    void registerAction(int index);
    bool hasAnyKey() const;
    void finishRegistering();

    QDBusConnection m_bus;
    QList<ShortcutAction> m_actions;
    QMap<QString, QString> m_triggers;
    bool m_bound = false;
    bool m_done = false;
};

// Tries the candidates in order until one registers; forwards its activations.
class ShortcutChain : public GlobalShortcut
{
    Q_OBJECT

public:
    explicit ShortcutChain(QList<GlobalShortcut *> candidates, QObject *parent = nullptr);

    void start() override;
    QString backend() const override { return m_active ? m_active->backend() : QString(); }
    QString trigger(const QString &actionId) const override { return m_active ? m_active->trigger(actionId) : QString(); }
    QStringList actionIds() const override { return m_active ? m_active->actionIds() : QStringList(); }
    void setAllowUnbound(bool allow) override;

private:
    void tryNext();

    QList<GlobalShortcut *> m_candidates;
    GlobalShortcut *m_active = nullptr;
    int m_index = -1;
    QStringList m_reasons;
};

// The chain the overlay uses: kglobalaccel first when it is on the bus or activatable (KDE Plasma), the portal first elsewhere, so that Plasma never waits
// minutes for a portal approval before kglobalaccel is tried.
std::unique_ptr<GlobalShortcut> makeShortcutChain(QDBusConnection bus, const QString &appId);
// Takes actions out of kglobalaccel's component (the shortcuts of triggers that no longer exist): without it System Settings keeps listing them. Fire and forget.
void forgetKGlobalAccelActions(const QDBusConnection &bus, const QStringList &actionIds);
// The same chain for other actions (the shortcut triggers): `actions` have no suggested key, and a binding that got no key at all still counts as bound.
std::unique_ptr<GlobalShortcut> makeTriggerShortcutChain(QDBusConnection bus, const QString &appId, const QList<ShortcutAction> &actions);

} // namespace murasaki
