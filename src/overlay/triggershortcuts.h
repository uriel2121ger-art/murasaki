#pragma once

#include "globalshortcut.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

namespace murasaki {

class DBusBackend;

// The keys of the shortcut triggers (docs/pets/automation.md, "Global shortcuts as triggers"): the daemon lists the enabled ones (ShortcutTriggerIds) and this registers
// them with the desktop next to the fixed actions, without a suggested key. The compositor owns the keys; the press and the release are told to the daemon
// (TriggerShortcut), which runs the trigger. Nothing is registered while there is no shortcut trigger.
class TriggerShortcuts : public QObject
{
    Q_OBJECT

public:
    // Makes the binding for the given actions; the overlay's chain, a fake in the tests.
    using Factory = std::function<std::unique_ptr<GlobalShortcut>(const QList<ShortcutAction> &)>;

    // Called with the action ids that were registered and are not wanted any more (kglobalaccel: unRegister).
    using Forget = std::function<void(const QStringList &)>;

    TriggerShortcuts(DBusBackend *daemon, Factory factory, QObject *parent = nullptr, Forget forget = {});

    // The ids now registered (or being registered), without the prefix the desktop sees.
    QStringList ids() const { return m_ids; }
    // The key the desktop bound for each, "" for one that has none: {id: key}.
    QJsonObject keys() const;
    // Prefix of the action ids the desktop sees ("trigger.<id>"), so they never clash with the fixed ones.
    static QString actionId(const QString &id);

signals:
    // The set or the keys changed (the overlay says ShortcutsChanged).
    void changed();

private:
    void refresh();
    void rebuild(const QList<QPair<QString, QString>> &wanted);

    DBusBackend *m_daemon;
    Factory m_factory;
    Forget m_forget;
    std::unique_ptr<GlobalShortcut> m_binding;
    QStringList m_ids;
    QList<QPair<QString, QString>> m_wanted;
    quint64 m_seq = 0;
};

} // namespace murasaki
