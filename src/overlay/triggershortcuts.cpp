#include "triggershortcuts.h"

#include "automation.h"
#include "dbusbackend.h"
#include "trigger.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace Qt::StringLiterals;

namespace murasaki {

QString TriggerShortcuts::actionId(const QString &id)
{
    return u"trigger."_s + id;
}

TriggerShortcuts::TriggerShortcuts(DBusBackend *daemon, Factory factory, QObject *parent, Forget forget)
    : QObject(parent)
    , m_daemon(daemon)
    , m_factory(std::move(factory))
    , m_forget(std::move(forget))
{
    connect(m_daemon, &DBusBackend::availabilityChanged, this, [this](bool available) {
        if (available) {
            refresh();
        } else if (m_binding) { // nobody to tell: the keys go with the daemon
            m_binding.reset();
            m_ids.clear();
            m_wanted.clear();
            emit changed();
        }
    });
    connect(m_daemon, &DBusBackend::signalReceived, this, [this](const QString &name, const QVariantList &) {
        if (name == u"TriggersChanged")
            refresh();
    });
}

void TriggerShortcuts::refresh()
{
    const quint64 seq = ++m_seq;
    m_daemon->property(u"ShortcutTriggerIds"_s, [this, seq](const DBusBackend::Reply &reply) {
        if (seq != m_seq)
            return;
        QList<QPair<QString, QString>> wanted; // (id, description)
        if (reply.ok) { // a daemon older than revision 18 has none
            for (const QJsonValue &value : QJsonDocument::fromJson(reply.value.toString().toUtf8()).array()) {
                const QJsonObject o = value.toObject();
                const QString id = o.value(u"id"_s).toString();
                if (validateName(id, nullptr) && wanted.size() < kMaxShortcutTriggers)
                    wanted.append({id, o.value(u"description"_s).toString().left(200)});
            }
        }
        if (wanted == m_wanted)
            return;
        rebuild(wanted);
    });
}

void TriggerShortcuts::rebuild(const QList<QPair<QString, QString>> &wanted)
{
    m_binding.reset(); // (the old session is closed)
    const QStringList before = m_ids;
    m_wanted = wanted;
    m_ids.clear();
    for (const auto &[id, description] : wanted)
        m_ids.append(id);
    if (m_forget) {
        QStringList gone;
        for (const QString &id : before)
            if (!m_ids.contains(id))
                gone.append(actionId(id));
        if (!gone.isEmpty())
            m_forget(gone);
    }
    if (!wanted.isEmpty() && m_factory) {
        QList<ShortcutAction> actions;
        for (const auto &[id, description] : wanted)
            actions.append({actionId(id), description.isEmpty() ? id : description, QString(), 0, QString()});
        m_binding = m_factory(actions);
        if (m_binding) {
            connect(m_binding.get(), &GlobalShortcut::activated, this, [this](const QString &action) {
                if (action.startsWith(u"trigger."))
                    m_daemon->call(u"TriggerShortcut"_s, {action.mid(8), true}, [](const DBusBackend::Reply &) {});
            });
            connect(m_binding.get(), &GlobalShortcut::released, this, [this](const QString &action) {
                if (action.startsWith(u"trigger."))
                    m_daemon->call(u"TriggerShortcut"_s, {action.mid(8), false}, [](const DBusBackend::Reply &) {});
            });
            connect(m_binding.get(), &GlobalShortcut::registered, this, &TriggerShortcuts::changed);
            connect(m_binding.get(), &GlobalShortcut::failed, this, [](const QString &reason) {
                qInfo("murasaki-overlay: no keys for the shortcut triggers (%s); they can still be fired with murasakictl", qPrintable(reason));
            });
            m_binding->start();
        }
    }
    emit changed();
}

QJsonObject TriggerShortcuts::keys() const
{
    QJsonObject keys;
    for (const QString &id : m_ids)
        keys.insert(id, m_binding ? m_binding->trigger(actionId(id)) : QString());
    return keys;
}

} // namespace murasaki
