// The demonstrations of DaemonClient (API revision 22, src/core/demos.h): small examples made out of the user's pets with the calls that exist, all marked
// origin "demo" so that removeDemos deletes exactly them. Nothing here writes pets.json.

#include "daemonclient.h"
#include "demos.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QString compact(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonArray parseList(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8()).array();
}

// A name no trigger has (the example of an earlier "Try it" counts: the guided walk never takes one over, its region and pictures may be somebody's work by now).
QString freshName(const QJsonArray &list, const QString &name)
{
    const auto taken = [&list](const QString &candidate) {
        for (const QJsonValue &value : list)
            if (value.toObject().value(u"name"_s).toString().compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    if (!taken(name))
        return name;
    for (int n = 2; n < 1000; ++n)
        if (const QString candidate = u"%1 (%2)"_s.arg(name).arg(n); !taken(candidate))
            return candidate;
    return name;
}

DemoNames namesFrom(const QVariantMap &options)
{
    const QVariantMap given = options.value(u"names"_s).toMap();
    DemoNames names = DemoNames::english();
    if (const QString s = given.value(u"timeline"_s).toString(); !s.isEmpty())
        names.timeline = s;
    if (const QString s = given.value(u"trigger"_s).toString(); !s.isEmpty())
        names.trigger = s;
    if (const QString s = given.value(u"profile"_s).toString(); !s.isEmpty())
        names.profile = s;
    if (const QString s = given.value(u"tracking"_s).toString(); !s.isEmpty())
        names.tracking = s;
    if (const QString s = given.value(u"screen"_s).toString(); !s.isEmpty())
        names.screen = s;
    return names;
}

} // namespace

bool DaemonClient::hasDemos() const
{
    for (const QVariantList *list : {&m_profiles, &m_triggers, &m_timelines})
        for (const QVariant &entry : *list)
            if (entry.toMap().value(u"origin"_s).toString() == kDemoOrigin)
                return true;
    return false;
}

bool DaemonClient::requireDemos()
{
    if (!requireAutomation())
        return false;
    if (demosSupported())
        return true;
    emit error(tr("The examples need a newer Murasaki service; update and restart it."));
    return false;
}

// Makes the timeline (or replaces the one an earlier "Try it" made with the same name: the pets may have changed) and answers with its id. The list is asked again, not
// taken from the cache, so that a timeline the user just made is never taken or replaced.
void DaemonClient::saveDemoTimeline(const QJsonObject &spec, const std::function<void(const QString &, const QJsonObject &)> &done)
{
    call(
        u"ListTimelines"_s, {},
        [this, spec, done](const QVariant &list) {
            const QJsonArray known = parseList(list);
            QJsonObject named = spec;
            named.insert(u"name"_s, demoFreeName(known, spec.value(u"name"_s).toString()));
            call(
                u"SetTimeline"_s, {demoIdByName(known, named.value(u"name"_s).toString()), compact(named)},
                [this, named, done](const QVariant &id) {
                    QJsonObject saved = named;
                    saved.insert(u"id"_s, id.toString());
                    emit timelineSaved(id.toString());
                    done(id.toString(), saved);
                },
                {}, Ungated);
        },
        {}, Ungated);
}

void DaemonClient::runDemo(const QString &kind, const QVariantMap &options)
{
    if (!requireDemos())
        return;
    const DemoNames names = namesFrom(options);
    const QJsonArray pets = QJsonArray::fromVariantList(m_pets);
    const QSize screen(options.value(u"screenWidth"_s).toInt(), options.value(u"screenHeight"_s).toInt());

    if (kind == u"timelines" || kind == u"triggers") {
        const auto spec = demoTimeline(pets, screen, names.timeline);
        if (!spec) {
            emit demoNeedsPet(kind);
            return;
        }
        saveDemoTimeline(*spec, [this, kind, names](const QString &id, const QJsonObject &timeline) {
            if (kind == u"timelines") {
                emit demoDone(kind, {{u"timeline"_s, timeline.toVariantMap()}});
                return;
            }
            call(
                u"ListTriggers"_s, {},
                [this, kind, id, names](const QVariant &list) {
                    const QJsonArray known = parseList(list);
                    const QString name = demoFreeName(known, names.trigger);
                    call(
                        u"SetTrigger"_s, {demoIdByName(known, name), compact(demoTrigger(id, name))},
                        [this, kind, id](const QVariant &trigger) {
                            emit triggerSaved(trigger.toString());
                            emit demoDone(kind, {{u"trigger"_s, trigger.toString()}, {u"timeline"_s, id}, {u"shortcutId"_s, QString(kDemoShortcutId)}});
                        },
                        {}, Ungated);
                },
                {}, Ungated);
        });
    } else if (kind == u"profiles") {
        if (pets.isEmpty()) {
            emit demoNeedsPet(kind);
            return;
        }
        // The desktop as it is, as an example profile: one call that marks it and leaves the active profile and the slots of the timelines alone. Switching to it is the
        // user's choice (the page offers it).
        call(
            u"ListProfiles"_s, {},
            [this, names](const QVariant &list) {
                const QString profileName = demoFreeName(parseList(list), names.profile);
                call(
                    u"SaveExampleProfile"_s, {profileName},
                    [this, profileName](const QVariant &saved) {
                        emit profileSaved(profileName, saved.toString());
                        emit demoDone(u"profiles"_s, {{u"profile"_s, saved.toString()}, {u"name"_s, profileName}});
                    },
                    {}, Ungated);
            },
            {}, Ungated);
    } else if (kind == u"screen") {
        // The screen example: a NEW screen trigger that is OFF, with a region to move and no reference picture (the user captures their own). It is never one an earlier
        // "Try it" made: that one may have been kept and shaped since, and the walk would overwrite its region and pictures.
        if (!screenSupported()) {
            emit error(tr("Watching the screen needs a newer Murasaki service; update and restart it."));
            return;
        }
        if (pets.isEmpty()) {
            emit demoNeedsPet(kind);
            return;
        }
        call(
            u"ListTriggers"_s, {},
            [this, kind, names](const QVariant &list) {
                const QString name = freshName(parseList(list), names.screen);
                call(
                    u"SetTrigger"_s, {QString(), compact(demoScreenTrigger(name))},
                    [this, kind](const QVariant &trigger) {
                        emit triggerSaved(trigger.toString());
                        emit demoDone(kind, {{u"trigger"_s, trigger.toString()}});
                    },
                    {}, Ungated);
            },
            {}, Ungated);
    } else if (kind == u"tracking") {
        const QString wanted = options.value(u"petId"_s).toString();
        QJsonObject pet;
        for (const QJsonValue &value : pets)
            if (value.toObject().value(u"petId"_s).toString() == wanted)
                pet = value.toObject();
        if (pet.isEmpty())
            pet = demoTrackingPet(pets);
        if (pet.isEmpty()) {
            emit demoNeedsPet(kind);
            return;
        }
        saveDemoTimeline(demoTrackingTimeline(pet, names.tracking), [this, pet](const QString &id, const QJsonObject &) {
            call(
                u"RunTimeline"_s, {id},
                [this, id, pet](const QVariant &) {
                    emit demoDone(u"tracking"_s, {{u"timeline"_s, id}, {u"petId"_s, pet.value(u"petId"_s).toString()}, {u"seconds"_s, kDemoTrackingMs / 1000}, {u"mode"_s, demoTrackingMode(pet)}});
                },
                {}, Ungated);
        });
    } else {
        emit error(tr("Unknown example: %1").arg(kind.left(40)));
    }
}

// Closes a first-visit card for good: the list in the settings is read again first, so two cards closed in a row both stay closed.
void DaemonClient::dismissDemoCard(const QString &kind)
{
    call(
        u"Settings"_s, {},
        [this, kind](const QVariant &value) {
            QStringList pages = QJsonDocument::fromJson(value.toString().toUtf8()).object().value(u"demoCardsDismissed"_s).toVariant().toStringList();
            if (!pages.contains(kind))
                pages.append(kind);
            // (A daemon that does not know a page name, "screen" before this revision of the list, refuses it: the card is hidden for this session by the page, quietly.)
            call(u"SetSettings"_s, {QString::fromUtf8(QJsonDocument(QJsonObject{{u"demoCardsDismissed"_s, QJsonArray::fromStringList(pages)}}).toJson(QJsonDocument::Compact))}, {}, {}, Ungated | Silent);
        },
        {}, Ungated);
}

void DaemonClient::setScreenDemoJournalPath(const QString &path)
{
    m_screenDemoJournalPath = path;
}

QVariantMap DaemonClient::readScreenDemoJournal() const
{
    const QString path = m_screenDemoJournalPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + u"/screen-demo.ini"_s : m_screenDemoJournalPath;
    QSettings file(path, QSettings::IniFormat);
    if (!file.contains(u"trigger"_s))
        return {};
    return {{u"trigger"_s, file.value(u"trigger"_s).toString()},
            {u"settingTurnedOn"_s, file.value(u"settingTurnedOn"_s).toBool()},
            {u"watchStarted"_s, file.value(u"watchStarted"_s).toBool()}};
}

void DaemonClient::writeScreenDemoJournal(const QVariantMap &journal) const
{
    const QString path = m_screenDemoJournalPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + u"/screen-demo.ini"_s : m_screenDemoJournalPath;
    QSettings file(path, QSettings::IniFormat);
    file.clear();
    if (!journal.isEmpty()) {
        file.setValue(u"trigger"_s, journal.value(u"trigger"_s).toString());
        file.setValue(u"settingTurnedOn"_s, journal.value(u"settingTurnedOn"_s).toBool());
        file.setValue(u"watchStarted"_s, journal.value(u"watchStarted"_s).toBool());
    }
    file.sync(); // (on disk before the change it names is sent)
}

void DaemonClient::noteScreenDemo(const QVariantMap &journal)
{
    m_screenDemoLive = true;
    writeScreenDemoJournal(journal);
}

void DaemonClient::endScreenDemo()
{
    m_screenDemoLive = false;
    writeScreenDemoJournal({});
}

void DaemonClient::undoScreenDemo()
{
    m_screenDemoLive = false;
    if (!screenSupported())
        return; // (the journal stays: the next connection does it)
    const QVariantMap journal = readScreenDemoJournal();
    if (journal.isEmpty())
        return;
    // Taken at once: a walk that starts while the answers are on their way writes a journal of its own, which is not ours to clear.
    writeScreenDemoJournal({});
    if (journal.value(u"watchStarted"_s).toBool())
        call(u"StopScreenWatch"_s, {}, [this](const QVariant &) { refreshScreenWatch(); }, {}, Ungated | Silent);
    const QString id = journal.value(u"trigger"_s).toString();
    call(
        u"ListTriggers"_s, {},
        [this, journal, id](const QVariant &list) {
            bool otherScreenTriggerOn = false;
            for (const QJsonValue &value : parseList(list)) {
                const QJsonObject trigger = value.toObject();
                if (trigger.value(u"id"_s).toString() == id) {
                    // Still the example the walk made (the marker is cleared when it is kept or edited: then it is the user's and stays, on).
                    if (trigger.value(u"origin"_s).toString() == QString(kDemoOrigin))
                        call(u"DeleteTrigger"_s, {id}, {}, {}, Ungated);
                    else if (trigger.value(u"enabled"_s).toBool())
                        otherScreenTriggerOn = true;
                } else if (trigger.value(u"enabled"_s).toBool() && trigger.value(u"source"_s).toObject().value(u"kind"_s).toString() == u"screen"_s) {
                    otherScreenTriggerOn = true;
                }
            }
            if (journal.value(u"settingTurnedOn"_s).toBool() && !otherScreenTriggerOn)
                call(u"SetSettings"_s, {compact(QJsonObject{{u"screenTriggersEnabled"_s, false}})}, {}, {}, Ungated);
        },
        [this, journal](const DBusBackend::Reply &) {
            if (readScreenDemoJournal().isEmpty() && !m_screenDemoLive)
                writeScreenDemoJournal(journal); // the daemon went away before it answered: tried again at the next connection
        },
        Ungated);
}

void DaemonClient::removeDemos()
{
    if (!requireDemos())
        return;
    // Triggers first (they name a timeline), then the timelines (stopped first: a stop undoes what a run changed), then the profiles. The lists are asked again, not
    // taken from the caches, so that nothing is deleted by a stale id; only what carries the marker goes.
    call(
        u"ListTriggers"_s, {},
        [this](const QVariant &triggers) {
            const QStringList triggerIds = demoIds(parseList(triggers));
            for (const QString &id : triggerIds)
                call(u"DeleteTrigger"_s, {id}, {}, {}, Ungated);
            call(
                u"ListTimelines"_s, {},
                [this, triggerIds](const QVariant &timelines) {
                    const QStringList timelineIds = demoIds(parseList(timelines));
                    for (const QString &id : timelineIds) {
                        call(u"StopTimeline"_s, {id}, {}, {}, Ungated | Silent);
                        call(u"DeleteTimeline"_s, {id}, {}, {}, Ungated);
                    }
                    call(
                        u"ListProfiles"_s, {},
                        [this, triggerIds, timelineIds](const QVariant &profiles) {
                            const QStringList profileIds = demoIds(parseList(profiles));
                            for (const QString &id : profileIds)
                                call(u"DeleteProfile"_s, {id}, {}, {}, Ungated);
                            emit demosRemoved({{u"triggers"_s, int(triggerIds.size())}, {u"timelines"_s, int(timelineIds.size())}, {u"profiles"_s, int(profileIds.size())}});
                        },
                        {}, Ungated);
                },
                {}, Ungated);
        },
        {}, Ungated);
}

} // namespace murasaki
