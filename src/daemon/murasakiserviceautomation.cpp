// The automation calls of MurasakiService (revision 18, docs/pets/automation.md): profiles, triggers, timelines and the runtime layer of the pets.

#include "murasakiservice.h"

#include "automationmanager.h"
#include "automationstore.h"
#include "constants.h"
#include "packautomation.h"
#include "screenrefstore.h"
#include "screenwatch.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

QString text(const QJsonValue &value)
{
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> parseChanges(const QString &json)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

// The overlay plays the sounds; it is never started for this (a pet without an overlay has no sound to play).
void callOverlay(const QString &method, const QVariantList &args)
{
    QDBusMessage message = QDBusMessage::createMethodCall(QString(kOverlayDBusService), QString(kOverlayDBusPath), QString(kOverlayDBusInterface), method);
    message.setArguments(args);
    message.setAutoStartService(false);
    QDBusConnection::sessionBus().asyncCall(message, 2000);
}

// The timeline an earlier import of the same package made (same origin, the name or "name (n)"): it is replaced; another one's is never touched.
std::optional<Timeline> madeBefore(const TimelineStore &store, const QString &origin, const QString &name)
{
    for (const Timeline &t : store.timelines())
        if (t.origin == origin && (t.name == name || t.name.startsWith(name + u" ("_s)))
            return t;
    return std::nullopt;
}

// A name no other timeline has: `name`, or the first "name (n)" that is free.
QString freeTimelineName(const TimelineStore &store, QString name)
{
    const QString base = name.left(kMaxAutomationNameLength - 8);
    for (int n = 2; store.timelineByName(name) && n < 1000; ++n)
        name = u"%1 (%2)"_s.arg(base).arg(n);
    return name;
}

} // namespace

void MurasakiService::attachAutomation(ProfileStore *profiles, TriggerStore *triggers, TimelineStore *timelines, const QString &statePath)
{
    m_profileStore = profiles;
    m_triggerStore = triggers;
    m_timelineStore = timelines;
    AutomationHooks hooks;
    hooks.setPetsHidden = [this](bool hidden) { SetPetsHidden(hidden); };
    hooks.petsHidden = [this] { return m_petsHidden; };
    hooks.playSound = [](const QString &petId) { callOverlay(u"PlayPetSound"_s, {petId}); };
    hooks.stopSounds = [] { callOverlay(u"StopOneShotPetSounds"_s, {}); }; // (not a pet in the mode continuous: use mute for that)
    hooks.startScene = [this](const QString &petId, int index, QString *problem) { return startScene(petId, index, problem); };
    hooks.setMuted = [this](bool muted) { SetPetsMuted(muted); }; // a state of the session, like hiding: nothing is written to settings.json
    hooks.muted = [this] { return m_petsMuted; };
    hooks.checkItem = [this](const QString &itemId, const QString &variant) -> QString {
        const auto item = m_library->item(itemId);
        if (!item || item->kind != ItemKind::Pet)
            return QString(kProfileItemMissing);
        for (const PetVariant &v : item->variants)
            if (v.name == variant)
                return {};
        return QString(kProfileVariantMissing);
    };
    hooks.petSize = [this](const QString &petId, const QString &variant) -> QSizeF {
        const auto pet = m_pets->pet(petId);
        const auto item = pet ? m_library->item(pet->itemId) : std::nullopt;
        if (!item || item->variants.isEmpty())
            return {};
        const PetVariant *chosen = &item->variants.first();
        for (const PetVariant &v : item->variants)
            if (v.name == (variant.isEmpty() ? pet->variant : variant))
                chosen = &v;
        if (chosen->width <= 0 || chosen->height <= 0)
            return {};
        return QSizeF(pet->width, pet->width * chosen->height / chosen->width);
    };
    m_automation = new AutomationManager(m_pets, profiles, triggers, timelines, m_settings, hooks, statePath, this);
    connect(m_automation, &AutomationManager::runtimeChanged, this, &MurasakiService::PetRuntimeChanged);
    connect(m_automation, &AutomationManager::activeProfileChanged, this, &MurasakiService::ProfilesChanged);
    connect(m_automation, &AutomationManager::triggerFired, this, &MurasakiService::TriggerFired);
    connect(m_automation, &AutomationManager::timelinesChanged, this, &MurasakiService::TimelinesChanged);
    connect(m_automation, &AutomationManager::suspendedChanged, this, &MurasakiService::TriggersChanged);
    connect(profiles, &ProfileStore::changed, this, &MurasakiService::ProfilesChanged);
    connect(triggers, &TriggerStore::changed, this, &MurasakiService::TriggersChanged);
    connect(timelines, &TimelineStore::changed, this, &MurasakiService::TimelinesChanged);
    m_automation->load();
}

QString MurasakiService::activeProfile() const
{
    return m_automation ? m_automation->activeProfile() : QString();
}

bool MurasakiService::profileModified() const
{
    return m_automation && m_automation->profileModified();
}

QString MurasakiService::petRuntime() const
{
    return m_automation ? m_automation->layerJson() : u"{}"_s;
}

QString MurasakiService::shortcutTriggerIds() const
{
    QJsonArray array;
    if (m_triggerStore && m_settings->automationSettings().enabled) {
        QSet<QString> seen;
        for (const Trigger &t : m_triggerStore->triggers()) {
            if (!t.enabled || t.source.kind != TriggerKind::Shortcut || seen.contains(t.source.id) || array.size() >= kMaxShortcutTriggers)
                continue;
            seen.insert(t.source.id);
            array.append(QJsonObject{{u"id"_s, t.source.id}, {u"description"_s, t.source.description.isEmpty() ? t.name : t.source.description}});
        }
    }
    return text(array);
}

// ---------------------------------------------------------------- profiles

QString MurasakiService::ListProfiles() const
{
    QJsonArray array;
    if (m_profileStore)
        for (const Profile &p : m_profileStore->profiles())
            array.append(p.toJson());
    return text(array);
}

QString MurasakiService::SaveProfile(const QString &name)
{
    if (!m_automation) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    QString problem;
    const QString id = m_automation->saveProfile(name, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

QString MurasakiService::SaveExampleProfile(const QString &name)
{
    if (!m_automation) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    QString problem;
    const QString id = m_automation->saveExampleProfile(name, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

QString MurasakiService::SwitchProfile(const QString &idOrName)
{
    if (!m_automation) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    QString problem;
    QJsonObject report;
    if (!m_automation->switchProfile(idOrName, &problem, &report)) {
        rejectCall(problem);
        return {};
    }
    return text(report);
}

void MurasakiService::RenameProfile(const QString &idOrName, const QString &newName)
{
    QString problem;
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else if (!m_automation->renameProfile(idOrName, newName, &problem))
        rejectCall(problem);
}

void MurasakiService::SetProfileOrigin(const QString &idOrName, const QString &origin)
{
    QString problem;
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else if (!m_automation->setProfileOrigin(idOrName, origin, &problem))
        rejectCall(problem);
}

void MurasakiService::DeleteProfile(const QString &idOrName)
{
    QString problem;
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else if (!m_automation->deleteProfile(idOrName, &problem))
        rejectCall(problem);
}

// ---------------------------------------------------------------- triggers

QString MurasakiService::ListTriggers() const
{
    QJsonArray array;
    if (m_triggerStore)
        for (const Trigger &t : m_triggerStore->triggers())
            array.append(t.toJson());
    return text(array);
}

QString MurasakiService::SetTrigger(const QString &triggerId, const QString &changesJson)
{
    if (!m_triggerStore) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    const auto changes = parseChanges(changesJson);
    if (!changes) {
        rejectCall(u"changes must be a JSON object"_s);
        return {};
    }
    QString problem;
    if (triggerId.isEmpty()) {
        Trigger base;
        base.cooldownMs = m_settings->automationSettings().triggerCooldownMs;
        const auto created = applyTriggerChanges(base, *changes, &problem);
        if (!created) {
            rejectCall(problem);
            return {};
        }
        const QString id = m_triggerStore->add(*created, &problem);
        if (id.isEmpty())
            rejectCall(problem);
        return id;
    }
    const auto current = m_triggerStore->trigger(triggerId);
    if (!current) {
        rejectCall(u"Unknown trigger: "_s + triggerId.left(200));
        return {};
    }
    const auto updated = applyTriggerChanges(*current, *changes, &problem);
    if (!updated) {
        rejectCall(problem);
        return {};
    }
    if (!m_triggerStore->update(*updated, &problem) && !problem.isEmpty())
        rejectCall(problem);
    return triggerId;
}

void MurasakiService::DeleteTrigger(const QString &triggerId)
{
    if (!m_triggerStore) {
        rejectCall(u"automation is not available"_s);
        return;
    }
    QString problem;
    if (!m_triggerStore->remove(triggerId, &problem))
        rejectCall(problem.isEmpty() ? u"Unknown trigger: "_s + triggerId.left(200) : problem);
}

void MurasakiService::SetTriggerEnabled(const QString &triggerId, bool enabled)
{
    if (!m_triggerStore) {
        rejectCall(u"automation is not available"_s);
        return;
    }
    auto trigger = m_triggerStore->trigger(triggerId);
    if (!trigger) {
        rejectCall(u"Unknown trigger: "_s + triggerId.left(200));
        return;
    }
    trigger->enabled = enabled;
    QString problem;
    if (!m_triggerStore->update(*trigger, &problem) && !problem.isEmpty())
        rejectCall(problem);
}

namespace {

// An action a trigger runs inside an incoming call (FireTrigger, a key) does not fail that call: its failures are warnings.
struct QuietScope {
    explicit QuietScope(bool *flag) : flag(flag), was(*flag) { *flag = true; }
    ~QuietScope() { *flag = was; }
    bool *flag;
    bool was;
};

} // namespace

void MurasakiService::ResumeAutomation()
{
    if (m_automation)
        m_automation->resumeAutomation();
}

bool MurasakiService::FireTrigger(const QString &name)
{
    if (!m_automation) {
        rejectCall(u"automation is not available"_s);
        return false;
    }
    QuietScope quiet(&m_quiet);
    return m_automation->fireTrigger(name);
}

bool MurasakiService::TestTrigger(const QString &triggerId)
{
    if (!m_automation || !m_triggerStore) {
        rejectCall(u"automation is not available"_s);
        return false;
    }
    if (!m_triggerStore->trigger(triggerId)) {
        rejectCall(u"Unknown trigger: "_s + triggerId.left(200));
        return false;
    }
    QuietScope quiet(&m_quiet);
    return m_automation->testTrigger(triggerId);
}

QString MurasakiService::TriggerState() const
{
    if (!m_automation)
        return u"{\"states\":[],\"log\":[]}"_s;
    QJsonArray states = m_automation->triggerStates();
    if (m_screenRefs) {
        // Whether watching or not, a screen trigger says how many reference pictures each of its regions has (the editor shows it).
        for (qsizetype i = 0; i < states.size(); ++i) {
            QJsonObject o = states.at(i).toObject();
            const auto trigger = m_triggerStore->trigger(o.value(u"id"_s).toString());
            if (!trigger || trigger->source.kind != TriggerKind::Screen)
                continue;
            QJsonArray counts;
            const int regions = int(std::min<qsizetype>(trigger->source.screen.value(u"regions"_s).toArray().size(), 8));
            for (int r = 0; r < regions; ++r)
                counts.append(int(m_screenRefs->steps(trigger->id, r).size()));
            o.insert(u"refs"_s, counts);
            states[i] = o;
        }
    }
    if (m_screenWatch && m_screenWatch->state() == ScreenWatch::State::Watching) {
        // While watching a screen trigger is live: no "not active" note, and its scores (in memory only) come with it.
        for (qsizetype i = 0; i < states.size(); ++i) {
            QJsonObject o = states.at(i).toObject();
            const QJsonObject live = m_screenWatch->triggerJson(o.value(u"id"_s).toString());
            if (live.isEmpty())
                continue;
            o.remove(u"note"_s);
            o.insert(u"screen"_s, live);
            states[i] = o;
        }
    }
    return text(QJsonObject{{u"states"_s, states}, {u"log"_s, m_automation->triggerLog()}, {u"suspendedMs"_s, double(m_automation->suspendedMs())}});
}

void MurasakiService::TriggerShortcut(const QString &shortcutId, bool pressed)
{
    if (m_automation) {
        QuietScope quiet(&m_quiet);
        m_automation->shortcut(shortcutId, pressed);
    }
}

// ---------------------------------------------------------------- timelines

QString MurasakiService::ListTimelines() const
{
    QJsonArray array;
    if (m_timelineStore)
        for (const Timeline &t : m_timelineStore->timelines())
            array.append(t.toJson());
    return text(array);
}

QString MurasakiService::SetTimeline(const QString &timelineId, const QString &changesJson)
{
    if (!m_timelineStore) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    const auto changes = parseChanges(changesJson);
    if (!changes) {
        rejectCall(u"changes must be a JSON object"_s);
        return {};
    }
    QString problem;
    if (timelineId.isEmpty()) {
        const auto created = applyTimelineChanges(Timeline{}, *changes, &problem);
        if (!created) {
            rejectCall(problem);
            return {};
        }
        const QString id = m_timelineStore->add(*created, &problem);
        if (id.isEmpty())
            rejectCall(problem);
        return id;
    }
    const auto current = m_timelineStore->timeline(timelineId);
    if (!current) {
        rejectCall(u"Unknown timeline: "_s + timelineId.left(200));
        return {};
    }
    const auto updated = applyTimelineChanges(*current, *changes, &problem);
    if (!updated) {
        rejectCall(problem);
        return {};
    }
    if (!m_timelineStore->update(*updated, &problem) && !problem.isEmpty())
        rejectCall(problem);
    return timelineId;
}

void MurasakiService::DeleteTimeline(const QString &timelineId)
{
    if (!m_timelineStore || !m_automation) {
        rejectCall(u"automation is not available"_s);
        return;
    }
    m_automation->stopTimeline(timelineId); // (what runs is undone first)
    QString id = timelineId;
    if (!m_timelineStore->timeline(id))
        if (const auto byName = m_timelineStore->timelineByName(timelineId))
            id = byName->id; // an id or a name
    QString problem;
    if (!m_timelineStore->remove(id, &problem))
        rejectCall(problem.isEmpty() ? u"Unknown timeline: "_s + timelineId.left(200) : problem);
}

void MurasakiService::RunTimeline(const QString &idOrName)
{
    QString problem;
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else if (!m_automation->runTimeline(idOrName, &problem))
        rejectCall(problem);
}

void MurasakiService::StopTimeline(const QString &idOrName)
{
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else
        m_automation->stopTimeline(idOrName);
}

QStringList MurasakiService::RunningTimelines() const
{
    return m_automation ? m_automation->runningTimelines() : QStringList();
}

// ---------------------------------------------------------------- packs

QString MurasakiService::ImportPackAutomation(const QString &itemId)
{
    if (!m_timelineStore || !m_triggerStore) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    const auto item = m_library->item(itemId);
    if (!item || !isGenerated(*item)) {
        rejectCall(u"Not a package imported by this daemon: "_s + itemId.left(200));
        return {};
    }
    const QJsonObject pack = packageRecord(*item).value(u"pack"_s).toObject();
    if (pack.isEmpty()) {
        rejectCall(u"The package has no triggers pack (or its pack.json was refused at the import)"_s);
        return {};
    }
    PackAutomation made = automationFromPack(pack, itemId, item->title);
    if (made.timelines.isEmpty()) {
        rejectCall(u"Nothing could be made from the pack: "_s + made.notes.join(u"; "_s).left(300));
        return {};
    }
    // Made again, it replaces what the same package made before (same name and origin) and never touches what the user made.
    Timeline timeline = made.timelines.first();
    QString problem;
    QString timelineId;
    if (const auto before = madeBefore(*m_timelineStore, timeline.origin, timeline.name)) {
        timeline.id = before->id;
        timeline.name = before->name;
        timelineId = timeline.id;
        if (!m_timelineStore->update(timeline, &problem) && !problem.isEmpty()) {
            rejectCall(problem);
            return {};
        }
    } else {
        timeline.name = freeTimelineName(*m_timelineStore, timeline.name);
        timelineId = m_timelineStore->add(timeline, &problem);
        if (timelineId.isEmpty()) {
            rejectCall(problem);
            return {};
        }
    }
    QJsonArray triggerIds;
    for (Trigger trigger : std::as_const(made.triggers)) {
        trigger.action.id = timelineId;
        trigger.enabled = false;
        QString id;
        for (const Trigger &existing : m_triggerStore->triggers())
            if (existing.name == trigger.name && existing.origin == trigger.origin)
                id = existing.id;
        if (!id.isEmpty()) {
            Trigger previous = *m_triggerStore->trigger(id);
            trigger.id = id;
            trigger.enabled = previous.enabled; // a trigger the user switched on stays on
            m_triggerStore->update(trigger, &problem);
        } else {
            id = m_triggerStore->add(trigger, &problem);
        }
        if (!id.isEmpty() && trigger.source.kind == TriggerKind::Screen && m_screenRefs) {
            // The pictures the import kept (screenrefs/ in the package's folder) go to the trigger's own folder, all or nothing; the reason of a refusal is a note.
            const QJsonObject refs = packageRecord(*item).value(u"screenRefs"_s).toObject();
            if (!refs.isEmpty()) {
                QString why;
                const int kept = m_screenRefs->adoptImported(id, item->dir, refs, &why);
                made.notes.append(kept > 0 ? u"screen: ready (%1 reference pictures kept)"_s.arg(kept) : u"screen: references not kept: %1"_s.arg(why.isEmpty() ? refs.value(u"status"_s).toString().left(120) : why.left(120)));
            }
        }
        if (!id.isEmpty())
            triggerIds.append(id);
        else
            made.notes.append(u"the trigger \"%1\" was not made: %2"_s.arg(trigger.name.left(60), problem.left(120)));
    }
    return text(QJsonObject{{u"timeline"_s, timelineId}, {u"triggers"_s, triggerIds}, {u"notes"_s, QJsonArray::fromStringList(made.notes)}});
}

QString MurasakiService::TimelineFromScene(const QString &itemId, int index)
{
    if (!m_timelineStore) {
        rejectCall(u"automation is not available"_s);
        return {};
    }
    const auto item = m_library->item(itemId);
    if (!item || !isGenerated(*item)) {
        rejectCall(u"Not a package imported by this daemon: "_s + itemId.left(200));
        return {};
    }
    const QJsonArray scenes = packageRecord(*item).value(u"scenes"_s).toArray();
    if (index < 0 || index >= scenes.size()) {
        rejectCall(u"The item has no such scene: "_s + QString::number(index));
        return {};
    }
    QString problem;
    auto timeline = timelineFromScene(scenes.at(index).toObject(), index, itemId, item->title, &problem);
    if (!timeline) {
        rejectCall(problem);
        return {};
    }
    if (const auto before = madeBefore(*m_timelineStore, timeline->origin, timeline->name)) {
        timeline->id = before->id;
        timeline->name = before->name;
        if (!m_timelineStore->update(*timeline, &problem) && !problem.isEmpty()) {
            rejectCall(problem);
            return {};
        }
        return before->id;
    }
    timeline->name = freeTimelineName(*m_timelineStore, timeline->name);
    const QString id = m_timelineStore->add(*timeline, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

void MurasakiService::ClearPetRuntime(const QString &petId)
{
    if (!m_automation)
        rejectCall(u"automation is not available"_s);
    else
        m_automation->clearRuntime(petId);
}

} // namespace murasaki
