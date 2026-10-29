#pragma once

// The host's side of the shortcuts of one wallpaper (a SceneItem): the usershortcut properties of its project.json, the applications the
// user assigned to them, the icons sent to the engine, and the decision about a script's request to start one. One per SceneItem.
// docs/design/specs/2026-10-21-user-shortcuts-design.md

#include "shortcut/desktopentry.h"
#include "shortcut/shortcutpolicy.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantMap>

#include <functional>

namespace murasaki {
class BridgeClient;
}

namespace murasaki::shortcut {

// The process-wide part: the user's switch (as the daemon says it; unknown counts as off) and the count of launches over the minute.
class ShortcutHub
{
public:
    static ShortcutHub &shared();
    void setLaunchAllowed(bool on) { m_allowed = on ? Tri::On : Tri::Off; }
    bool launchAllowed() const { return m_allowed == Tri::On; }
    LaunchLimiter &limiter() { return m_limiter; }

private:
    enum class Tri { Unknown, On, Off };
    Tri m_allowed = Tri::Unknown;
    LaunchLimiter m_limiter;
};

struct ShortcutEnvironment {
    QStringList roots;                 // where applications are looked for
    DesktopEnvironment desktop;
    QStringList iconRoots;             // where an Icon= path may point (and a theme icon file may be)
    QString themeName;                 // the icon theme in use (read on the GUI thread)
    QStringList themePaths;            // the directories icon themes are in
    std::function<qint64()> nowMs;     // a monotonic clock
    bool useThreads = true;            // decode icon files on a worker thread
};
ShortcutEnvironment systemShortcutEnvironment();

class ShortcutItem : public QObject
{
    Q_OBJECT

public:
    // Reads the keys from <itemDir>/project.json.
    ShortcutItem(QString itemDir, ShortcutEnvironment env, LaunchLimiter *limiter, QObject *parent = nullptr);

    QString itemDir() const { return m_itemDir; }
    const QStringList &keys() const { return m_keys; }

    // The properties the engine is given: each shortcut property that holds an application becomes that application's display name, any
    // other one the empty string. The id never reaches the engine. Pure: nothing is remembered.
    QVariantMap engineProperties(const QVariantMap &properties) const;

    // The user's current assignments (the properties as the GUI stores them): remembered, and the icons the engine lacks are sent.
    void assignmentsChanged(const QVariantMap &properties);

    // The engine of this wallpaper is connected: it gets the icons and its launch requests are heard.
    void attach(BridgeClient *client);

    // A real button press reached the engine (the host saw the pointer event itself).
    void noteClick();
    // Whether the wallpaper is shown (not paused, not hidden).
    void setVisibleProbe(std::function<bool()> probe) { m_visible = std::move(probe); }

    // What a request for `key` would do now (tests; the request signal of the client calls it).
    Verdict decide(const QString &key);

signals:
    // The gate passed: the daemon is asked to start the application of `key` (it judges again from its own data).
    void launchApproved(const QString &itemDir, const QString &key);
    // A request was refused, with the reason (logged, once per reason).
    void launchRefused(const QString &key, const QString &reason);

private:
    void request(const QByteArray &name);
    void refreshEntries();
    void pushIcons();
    void sendIcon(const QString &key);
    void onIcon(const QString &key, const QString &id, uint32_t generation, quint64 epoch, std::optional<QByteArray> rgba);

    QString m_itemDir;
    ShortcutEnvironment m_env;
    QStringList m_keys;
    LaunchGate m_gate;
    std::function<bool()> m_visible;
    QPointer<BridgeClient> m_client;
    QHash<QString, QString> m_assigned;             // key -> desktop id, as the user stored it (empty: none)
    QHash<QString, DesktopEntry> m_entries;         // key -> the entry the id resolves to now
    QHash<QString, uint32_t> m_generation;          // key -> counter of what was asked to be sent
    QHash<QString, QString> m_sentId;               // key -> the id whose icon the engine has ("" : none sent)
    QHash<QString, QElapsedTimer> m_lastSent;       // key -> when an icon was last sent
    QHash<QString, QByteArray> m_icons;             // desktop id -> rendered icon (at most kMaxSlots)
    QSet<QString> m_pending;                        // keys with a send waiting for its second
    QSet<QString> m_told;                           // refusal reasons already logged
    quint64 m_epoch = 0;                            // changes with every set of assignments: a late icon of an older one is dropped
};

} // namespace murasaki::shortcut
