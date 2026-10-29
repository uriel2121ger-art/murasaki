#include "automationmanager.h"

#include "demos.h"
#include "jsonfile.h"
#include "profileswitch.h"

#include <QJsonDocument>

#include <algorithm>
#include <climits>
#include <ctime>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr qint64 kMaxTimerMs = 3'600'000; // a long wait is taken in steps of an hour, so a clock that jumped is noticed

}

AutomationHooks systemClockHooks(AutomationHooks hooks)
{
    if (!hooks.nowMs)
        hooks.nowMs = [] {
            timespec ts{};
            clock_gettime(CLOCK_BOOTTIME, &ts); // (counts a suspend: an idle that slept through one lasted that long)
            return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
        };
    if (!hooks.nowLocal)
        hooks.nowLocal = [] { return QDateTime::currentDateTime(); };
    return hooks;
}

AutomationManager::AutomationManager(PetStore *pets, ProfileStore *profiles, TriggerStore *triggers, TimelineStore *timelines, SettingsStore *settings,
                                     AutomationHooks hooks, QString statePath, QObject *parent)
    : QObject(parent)
    , m_pets(pets)
    , m_profiles(profiles)
    , m_triggers(triggers)
    , m_timelines(timelines)
    , m_settings(settings)
    , m_hooks(systemClockHooks(std::move(hooks)))
    , m_statePath(std::move(statePath))
{
    m_timelineTimer.setSingleShot(true);
    m_engineTimer.setSingleShot(true);
    m_suspendTimer.setSingleShot(true);
    connect(&m_suspendTimer, &QTimer::timeout, this, &AutomationManager::resumeAutomation);
    connect(&m_timelineTimer, &QTimer::timeout, this, &AutomationManager::onTimelineTimer);
    connect(&m_engineTimer, &QTimer::timeout, this, &AutomationManager::onEngineTimer);
    connect(m_pets, &PetStore::changed, this, &AutomationManager::onPetsChanged);
    connect(m_profiles, &ProfileStore::changed, this, [this] {
        if (!m_activeProfile.isEmpty() && !m_profiles->profile(m_activeProfile)) {
            m_activeProfile.clear();
            saveState();
        }
        emit activeProfileChanged();
    });
    connect(m_triggers, &TriggerStore::changed, this, &AutomationManager::onTriggersChanged);
    connect(m_timelines, &TimelineStore::changed, this, &AutomationManager::timelinesChanged);
    connect(m_settings, &SettingsStore::changed, this, &AutomationManager::onSettingsChanged);
}

void AutomationManager::load()
{
    if (!m_statePath.isEmpty()) {
        const JsonFileRead read = readJsonObjectFile(m_statePath);
        if (read.exists && read.problem.isEmpty()) {
            const QString id = read.root.value(u"activeProfile"_s).toString();
            if (m_profiles->profile(id))
                m_activeProfile = id;
        }
    }
    for (const PetPlacement &p : m_pets->pets())
        m_knownPets.append(p.petId);
    m_loaded = true;
    m_engine.setAutomationEnabled(engineWanted());
    m_engine.setTriggers(m_triggers->triggers(), now(), m_hooks.nowLocal());
    m_engine.setFacts(m_facts, now()); // the starting point of every edge: nothing known, not locked
    scheduleEngine();
    publishNeeds();
}

void AutomationManager::saveState() const
{
    if (m_statePath.isEmpty())
        return;
    writeJsonObjectFile(m_statePath, QJsonObject{{u"activeProfile"_s, m_activeProfile}});
}

// ---------------------------------------------------------------- profiles

QString AutomationManager::saveProfile(const QString &name, QString *error)
{
    if (error)
        error->clear();
    const QList<PetPlacement> pets = m_pets->pets();
    if (pets.size() > kMaxProfilePets) {
        if (error)
            *error = QString(kProfileTooManyPets);
        return {};
    }
    Profile profile;
    QString id;
    if (const auto existing = m_profiles->profileByName(name.trimmed())) {
        profile = *existing;
        profile.pets = entriesFromPets(pets);
        id = profile.id;
        QString problem;
        if (!m_profiles->update(profile, &problem) && !problem.isEmpty()) {
            if (error)
                *error = problem;
            return {};
        }
    } else {
        profile.name = name.trimmed();
        profile.pets = entriesFromPets(pets);
        id = m_profiles->add(profile, error);
        if (id.isEmpty())
            return {};
    }
    m_slotPets.clear();
    for (int i = 0; i < pets.size(); ++i)
        m_slotPets.insert(i, pets[i].petId);
    m_activeProfile = id;
    saveState();
    emit activeProfileChanged();
    return id;
}

QString AutomationManager::saveExampleProfile(const QString &name, QString *error)
{
    if (error)
        error->clear();
    const QList<PetPlacement> pets = m_pets->pets();
    if (pets.size() > kMaxProfilePets) {
        if (error)
            *error = QString(kProfileTooManyPets);
        return {};
    }
    auto profile = m_profiles->profileByName(name.trimmed());
    if (profile && profile->origin != kDemoOrigin) {
        if (error)
            *error = u"a profile called that exists and is not an example"_s;
        return {};
    }
    QString problem;
    if (profile) {
        profile->pets = entriesFromPets(pets);
        if (!m_profiles->update(*profile, &problem) && !problem.isEmpty()) {
            if (error)
                *error = problem;
            return {};
        }
        return profile->id;
    }
    Profile fresh;
    fresh.name = name.trimmed();
    fresh.origin = QString(kDemoOrigin);
    fresh.pets = entriesFromPets(pets);
    return m_profiles->add(fresh, error);
}

bool AutomationManager::switchProfile(const QString &idOrName, QString *error, QJsonObject *report)
{
    if (error)
        error->clear();
    auto profile = m_profiles->profile(idOrName);
    if (!profile)
        profile = m_profiles->profileByName(idOrName);
    if (!profile) {
        if (error)
            *error = u"Unknown profile: "_s + idOrName.left(200);
        return false;
    }
    const SwitchPlan plan = planProfileSwitch(m_pets->pets(), *profile, m_hooks.checkItem);
    QJsonArray skipped;
    for (const SkippedEntry &s : plan.skipped)
        skipped.append(QJsonObject{{u"slot"_s, s.slot}, {u"itemId"_s, s.itemId}, {u"reason"_s, s.reason}});
    m_lastReport = QJsonObject{{u"profile"_s, profile->name}, {u"kept"_s, plan.kept}, {u"updated"_s, plan.updated}, {u"added"_s, plan.added}, {u"removed"_s, plan.removed}, {u"skipped"_s, skipped}};
    if (report)
        *report = m_lastReport;
    if (!plan.isNoop()) {
        QString problem;
        if (!m_pets->replaceAll(plan.result, &problem) && !problem.isEmpty()) {
            if (error)
                *error = problem;
            return false;
        }
    }
    m_slotPets.clear();
    const QList<PetPlacement> now = m_pets->pets();
    // The pets of the plan are in the profile's order; the store keeps its own order when nothing was written (a no-op switch), so the ids come from the plan when it has them.
    for (int i = 0; i < plan.entrySlots.size(); ++i) {
        QString petId = plan.result[i].petId;
        if (petId.isEmpty() && i < now.size())
            petId = now[i].petId; // (a pet that was added: the store gave it its id, in the order of the plan)
        if (!petId.isEmpty())
            m_slotPets.insert(plan.entrySlots[i], petId);
    }
    if (m_activeProfile != profile->id) {
        // Going to an example from one of the user's: remember where to come back when the example is removed.
        if (profile->origin == kDemoOrigin) {
            const auto before = m_profiles->profile(m_activeProfile);
            if (before && before->origin != kDemoOrigin)
                m_profileBeforeExample = before->id;
        }
        m_activeProfile = profile->id;
        saveState();
    }
    emit activeProfileChanged();
    return true;
}

bool AutomationManager::renameProfile(const QString &idOrName, const QString &newName, QString *error)
{
    auto profile = m_profiles->profile(idOrName);
    if (!profile)
        profile = m_profiles->profileByName(idOrName);
    if (!profile) {
        if (error)
            *error = u"Unknown profile: "_s + idOrName.left(200);
        return false;
    }
    profile->name = newName.trimmed();
    if (profile->origin == kDemoOrigin)
        profile->origin.clear(); // renamed by the user: it is theirs now, "Remove examples" leaves it
    QString problem;
    const bool done = m_profiles->update(*profile, &problem);
    if (!done && !problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    return true;
}

bool AutomationManager::setProfileOrigin(const QString &idOrName, const QString &origin, QString *error)
{
    if (!origin.isEmpty() && origin != kDemoOrigin) {
        if (error)
            *error = u"origin is empty or demo (a package's comes with its import)"_s;
        return false;
    }
    auto profile = m_profiles->profile(idOrName);
    if (!profile)
        profile = m_profiles->profileByName(idOrName);
    if (!profile) {
        if (error)
            *error = u"Unknown profile: "_s + idOrName.left(200);
        return false;
    }
    profile->origin = origin;
    QString problem;
    const bool done = m_profiles->update(*profile, &problem);
    if (!done && !problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    return true;
}

bool AutomationManager::deleteProfile(const QString &idOrName, QString *error)
{
    auto profile = m_profiles->profile(idOrName);
    if (!profile)
        profile = m_profiles->profileByName(idOrName);
    if (!profile) {
        if (error)
            *error = u"Unknown profile: "_s + idOrName.left(200);
        return false;
    }
    // An example that is the active profile: back to the profile that was active before it (the pets follow it, as in any switch).
    if (profile->origin == kDemoOrigin && profile->id == m_activeProfile && !m_profileBeforeExample.isEmpty() && m_profiles->profile(m_profileBeforeExample))
        switchProfile(m_profileBeforeExample);
    QString problem;
    if (!m_profiles->remove(profile->id, &problem)) {
        if (error)
            *error = problem.isEmpty() ? u"could not delete the profile"_s : problem;
        return false;
    }
    return true;
}

bool AutomationManager::profileModified() const
{
    const auto profile = m_profiles->profile(m_activeProfile);
    return profile && profileDiffers(m_pets->pets(), *profile, m_hooks.checkItem);
}

// ---------------------------------------------------------------- the runtime layer

QString AutomationManager::layerJson() const
{
    return QString::fromUtf8(QJsonDocument(runtimeLayerToJson(m_layer)).toJson(QJsonDocument::Compact));
}

void AutomationManager::setLayer(const QString &petId, const PetRuntime &runtime)
{
    const bool had = m_layer.contains(petId);
    if (runtime.isDefault()) {
        if (!had)
            return;
        m_layer.remove(petId);
    } else {
        if (had && m_layer.value(petId) == runtime)
            return;
        m_layer.insert(petId, runtime);
    }
    emit runtimeChanged(petId);
}

bool AutomationManager::setPetHidden(const QString &petId, bool hidden)
{
    if (!m_pets->pet(petId))
        return false;
    PetRuntime r = m_layer.value(petId);
    r.hidden = hidden;
    setLayer(petId, r);
    return true;
}

void AutomationManager::clearRuntime(const QString &petId)
{
    if (!petId.isEmpty()) {
        setLayer(petId, PetRuntime{});
        return;
    }
    const QStringList ids = m_layer.keys();
    m_layer.clear();
    for (const QString &id : ids)
        emit runtimeChanged(id);
}

bool AutomationManager::engineWanted() const
{
    return m_settings->automationSettings().enabled && m_suspendedUntilMs <= now();
}

qint64 AutomationManager::suspendedMs() const
{
    return std::max<qint64>(0, m_suspendedUntilMs - now());
}

void AutomationManager::resumeAutomation()
{
    m_suspendTimer.stop();
    if (m_suspendedUntilMs == 0)
        return;
    m_suspendedUntilMs = 0;
    m_engine.setAutomationEnabled(engineWanted());
    scheduleEngine();
    publishNeeds();
    emit suspendedChanged();
}

void AutomationManager::rescue()
{
    emit rescued();
    m_runner.cancelAll();
    applyEffects(m_runner.takeEffects());
    scheduleTimeline();
    clearRuntime();
    emit timelinesChanged();
    if (m_rescueSuspendMs > 0 && m_settings->automationSettings().enabled) {
        m_suspendedUntilMs = now() + m_rescueSuspendMs;
        m_engine.setAutomationEnabled(false);
        processFires();
        m_suspendTimer.start(m_rescueSuspendMs);
        scheduleEngine();
        publishNeeds();
        emit suspendedChanged();
    }
}

void AutomationManager::onPetsChanged()
{
    QStringList current;
    for (const PetPlacement &p : m_pets->pets())
        current.append(p.petId);
    for (const QString &id : std::as_const(m_knownPets)) {
        if (current.contains(id))
            continue;
        m_runner.petRemoved(id);
        setLayer(id, PetRuntime{});
        for (auto it = m_slotPets.begin(); it != m_slotPets.end();)
            it = it.value() == id ? m_slotPets.erase(it) : std::next(it);
    }
    m_knownPets = current;
    applyEffects(m_runner.takeEffects());
    scheduleTimeline();
    emit activeProfileChanged(); // (the "modified" mark may have changed)
}

// ---------------------------------------------------------------- timelines

QList<QString> AutomationManager::petsOfItem(const QString &item) const
{
    QList<QString> ids;
    for (const PetPlacement &p : m_pets->pets())
        if (p.itemId == item)
            ids.append(p.petId);
    return ids;
}

ResolvedPet AutomationManager::resolve(const PetRef &ref) const
{
    if (!ref.petId.isEmpty())
        return {m_pets->pet(ref.petId) ? ref.petId : QString(), false};
    if (ref.slot >= 0) {
        const QString id = m_slotPets.value(ref.slot);
        return {!id.isEmpty() && m_pets->pet(id) ? id : QString(), false};
    }
    // An item alone is its first pet; with a variant it is the pet that shows that animation (a Triggers pack's tracks each drive their own).
    for (const PetPlacement &p : m_pets->pets())
        if (p.itemId == ref.item && (ref.variant.isEmpty() || p.variant == ref.variant))
            return {p.petId, false};
    // (a pet made for the run is not part of this version: a timeline that names an item with no pet skips that track)
    return {};
}

// Where the top-left corner goes for an action that names the centre (a Triggers pack's positions): half the pet's size back, with the size it has now.
PetPosition AutomationManager::shifted(const TimelineEffect &e, const PetRuntime &layer, const QString &storedVariant) const
{
    PetPosition at{e.x, e.y};
    if (e.centre && m_hooks.petSize) {
        const QSizeF size = m_hooks.petSize(e.petId, layer.variant.isEmpty() ? storedVariant : layer.variant);
        at.x -= size.width() / 2;
        at.y -= size.height() / 2;
    }
    return at;
}

void AutomationManager::applyEffects(const QList<TimelineEffect> &effects)
{
    using Kind = TimelineEffect::Kind;
    for (const TimelineEffect &e : effects) {
        const auto pet = m_pets->pet(e.petId);
        PetRuntime r = m_layer.value(e.petId);
        switch (e.kind) {
        case Kind::SetHidden:
            if (!pet)
                continue;
            r.hidden = e.hidden;
            break;
        case Kind::SetVariant:
            if (!pet)
                continue;
            r.variant = e.variant;
            break;
        case Kind::Place:
            if (!pet)
                continue;
            r.position = shifted(e, r, pet->variant);
            r.motion.reset();
            if (!e.output.isEmpty())
                r.output = e.output;
            break;
        case Kind::Move: {
            if (!pet)
                continue;
            const PetPosition from = effectivePosition(r, PetPosition{pet->x, pet->y}, e.atMs);
            const PetPosition to = shifted(e, r, pet->variant);
            r.motion = PetMotion{from.x, from.y, to.x, to.y, e.atMs, e.durationMs};
            r.position = to;
            if (!e.output.isEmpty())
                r.output = e.output;
            break;
        }
        case Kind::SetTracking:
            if (!pet)
                continue;
            r.tracking = e.tracking;
            break;
        case Kind::Sound:
            if (pet && m_hooks.playSound)
                m_hooks.playSound(e.petId);
            continue;
        case Kind::StopSounds:
            if (m_hooks.stopSounds)
                m_hooks.stopSounds();
            continue;
        case Kind::Restore:
            r = PetRuntime{};
            break;
        case Kind::Release:
            continue;
        }
        QString problem;
        if (!validatePetRuntime(r, &problem)) {
            qWarning("murasakid: a timeline step was refused: %s", qPrintable(problem));
            continue;
        }
        setLayer(e.petId, r);
    }
}

void AutomationManager::scheduleTimeline()
{
    const qint64 next = m_runner.nextDueMs();
    if (next < 0) {
        m_timelineTimer.stop();
        return;
    }
    m_timelineTimer.start(int(std::clamp<qint64>(next - now(), 0, kMaxTimerMs)));
}

void AutomationManager::onTimelineTimer()
{
    m_runner.advance(now());
    applyEffects(m_runner.takeEffects());
    scheduleTimeline();
    if (m_runner.running() == 0)
        emit timelinesChanged();
}

bool AutomationManager::runTimeline(const QString &idOrName, QString *error)
{
    if (error)
        error->clear();
    auto timeline = m_timelines->timeline(idOrName);
    if (!timeline)
        timeline = m_timelines->timelineByName(idOrName);
    if (!timeline) {
        if (error)
            *error = u"Unknown timeline: "_s + idOrName.left(200);
        return false;
    }
    const TimelineRunner::Started started = m_runner.start(*timeline, now(), [this](const PetRef &ref) { return resolve(ref); });
    if (!started.ok) {
        if (error)
            *error = started.error;
        return false;
    }
    applyEffects(m_runner.takeEffects());
    scheduleTimeline();
    emit timelinesChanged();
    if (!started.notes.isEmpty() && !m_runner.isRunning(timeline->id) && started.notes.size() == timeline->tracks.size()) {
        if (error)
            *error = QString(kTimelinePetNotFound);
        return false;
    }
    return true;
}

bool AutomationManager::stopTimeline(const QString &idOrName)
{
    auto timeline = m_timelines->timeline(idOrName);
    if (!timeline)
        timeline = m_timelines->timelineByName(idOrName);
    const QString id = timeline ? timeline->id : idOrName;
    const bool done = m_runner.cancel(id);
    applyEffects(m_runner.takeEffects());
    scheduleTimeline();
    if (done)
        emit timelinesChanged();
    return done;
}

// ---------------------------------------------------------------- triggers

void AutomationManager::onSettingsChanged()
{
    const bool enabled = engineWanted();
    const bool was = m_engine.automationEnabled();
    m_engine.setAutomationEnabled(enabled);
    processFires(); // (what a hold held is undone when the switch goes off)
    if (was && !enabled) {
        m_runner.cancelAll();
        applyEffects(m_runner.takeEffects());
        scheduleTimeline();
    }
    scheduleEngine();
    publishNeeds();
}

void AutomationManager::onTriggersChanged()
{
    m_engine.setTriggers(m_triggers->triggers(), now(), m_hooks.nowLocal());
    scheduleEngine();
    publishNeeds();
}

void AutomationManager::publishNeeds()
{
    const EngineNeeds needs = m_engine.needs();
    if (needs.windows == m_lastNeeds.windows && needs.power == m_lastNeeds.power && needs.game == m_lastNeeds.game && needs.session == m_lastNeeds.session
        && needs.idle == m_lastNeeds.idle && needs.outputs == m_lastNeeds.outputs && needs.timer == m_lastNeeds.timer && needs.signalFiles == m_lastNeeds.signalFiles
        && needs.screen == m_lastNeeds.screen && needs.shortcutIds == m_lastNeeds.shortcutIds && needs.dbusRules == m_lastNeeds.dbusRules)
        return;
    m_lastNeeds = needs;
    emit needsChanged();
}

void AutomationManager::scheduleEngine()
{
    const qint64 next = m_engine.automationEnabled() ? m_engine.nextWakeMs(now(), m_hooks.nowLocal()) : -1;
    if (next < 0) {
        m_engineTimer.stop();
        return;
    }
    m_engineTimer.start(int(std::clamp<qint64>(next, 0, kMaxTimerMs)));
}

void AutomationManager::onEngineTimer()
{
    m_engine.tick(now(), m_hooks.nowLocal());
    processFires();
    scheduleEngine();
}

void AutomationManager::setSessionFacts(const QJsonObject &json)
{
    AutomationFacts f = factsFromSessionJson(json, m_facts);
    f.outputsKnown = f.known;
    f.outputs = f.known ? json.value(u"outputs"_s).toObject().keys() : QStringList();
    if (f == m_facts)
        return;
    const bool battery = onBattery();
    m_facts = f;
    if (battery != onBattery())
        emit batteryChanged(onBattery());
    m_engine.setFacts(m_facts, now());
    processFires();
    scheduleEngine();
}

void AutomationManager::screenActive(const QString &triggerId, bool active)
{
    m_engine.screenActive(triggerId, active, now());
    processFires();
}

void AutomationManager::setLocked(bool locked)
{
    if (m_facts.locked == locked)
        return;
    m_facts.locked = locked;
    emit lockedChanged(locked);
    m_engine.setFacts(m_facts, now());
    processFires();
}

void AutomationManager::setIdle(bool known, bool idle)
{
    // logind's hint: the fallback for a session whose surface does not know the idle (the facts keep both and use the surface's when it has it)
    AutomationFacts f = m_facts;
    f.setLogindIdle(known, idle);
    if (f == m_facts)
        return;
    m_facts = f;
    m_engine.setFacts(m_facts, now());
    processFires();
    scheduleEngine();
}

bool AutomationManager::fireTrigger(const QString &name)
{
    int count = m_engine.fire(name, now());
    processFires();
    if (count == 0 && m_settings->automationSettings().fireByTimelineName && m_engine.automationEnabled()) {
        if (runTimeline(name))
            count = 1;
    }
    scheduleEngine();
    return count > 0;
}

void AutomationManager::signalFile(const QString &name)
{
    m_engine.signalFile(name, now());
    processFires();
}

void AutomationManager::dbusSignal(const QString &interface, const QString &member, const QStringList &senderNames)
{
    m_engine.dbusSignal(interface, member, senderNames, now());
    processFires();
}

void AutomationManager::shortcut(const QString &id, bool pressed)
{
    m_engine.shortcut(id, pressed, now());
    processFires();
}

bool AutomationManager::testTrigger(const QString &triggerId)
{
    const bool done = m_engine.test(triggerId, now());
    processFires();
    return done;
}

void AutomationManager::processFires()
{
    const QList<Fired> fires = m_engine.takeFires();
    for (const Fired &fired : fires) {
        emit triggerFired(fired.triggerId, fired.release ? u"released"_s : u"ran"_s);
        const bool hides = fired.action.type == TriggerActionType::HideAll || fired.action.type == TriggerActionType::ShowAll || fired.action.type == TriggerActionType::ToggleHidden;
        const bool mutes = fired.action.type == TriggerActionType::Mute;
        if (!fired.release) {
            // (a press only matters when a release will come: the snapshot of a trigger that is not a hold is overwritten by its next press and is never read)
            if (hides && m_hooks.petsHidden)
                m_heldHidden.insert(fired.triggerId, m_hooks.petsHidden());
            if (mutes && m_hooks.muted)
                m_heldMuted.insert(fired.triggerId, m_hooks.muted());
        } else if (hides && m_heldHidden.contains(fired.triggerId)) {
            if (m_hooks.setPetsHidden)
                m_hooks.setPetsHidden(m_heldHidden.take(fired.triggerId));
            continue;
        } else if (mutes && m_heldMuted.contains(fired.triggerId)) {
            if (m_hooks.setMuted)
                m_hooks.setMuted(m_heldMuted.take(fired.triggerId));
            continue;
        }
        runAction(fired.action, fired.release);
    }
}

void AutomationManager::runAction(const TriggerAction &action, bool release)
{
    // The closed list of docs/pets/automation.md. A new kind of action has to be added here on purpose: the compiler warns about a missing case, and
    // tst_automationclosed lists them.
    switch (action.type) {
    case TriggerActionType::Timeline:
        if (release) {
            stopTimeline(action.id);
        } else {
            QString problem;
            if (!runTimeline(action.id, &problem))
                qWarning("murasakid: trigger: timeline %s: %s", qPrintable(action.id.left(80)), qPrintable(problem));
        }
        break;
    case TriggerActionType::Scene: {
        const QStringList ids = action.petId.isEmpty() ? petsOfItem(action.item) : QStringList{action.petId};
        for (const QString &id : ids) {
            QString problem;
            if (m_hooks.startScene && !m_hooks.startScene(id, action.index, &problem))
                qWarning("murasakid: trigger: scene %d: %s", action.index, qPrintable(problem));
        }
        break;
    }
    case TriggerActionType::Profile: {
        QString problem;
        if (!switchProfile(action.id, &problem))
            qWarning("murasakid: trigger: profile: %s", qPrintable(problem));
        break;
    }
    case TriggerActionType::HideAll:
        if (m_hooks.setPetsHidden)
            m_hooks.setPetsHidden(true);
        break;
    case TriggerActionType::ShowAll:
        if (m_hooks.setPetsHidden)
            m_hooks.setPetsHidden(false);
        break;
    case TriggerActionType::ToggleHidden:
        if (m_hooks.setPetsHidden && m_hooks.petsHidden)
            m_hooks.setPetsHidden(!m_hooks.petsHidden());
        break;
    case TriggerActionType::StopSounds:
        if (m_hooks.stopSounds)
            m_hooks.stopSounds();
        break;
    case TriggerActionType::Mute:
        if (m_hooks.setMuted && m_hooks.muted)
            m_hooks.setMuted(action.mute == u"on" ? true : action.mute == u"off" ? false : !m_hooks.muted());
        break;
    case TriggerActionType::PlaySound: {
        const QStringList ids = action.petId.isEmpty() ? petsOfItem(action.item) : QStringList{action.petId};
        for (const QString &id : ids)
            if (m_hooks.playSound)
                m_hooks.playSound(id);
        break;
    }
    }
}

QJsonArray AutomationManager::triggerStates() const
{
    QJsonArray array;
    const qint64 nowMs = now();
    for (const Trigger &t : m_triggers->triggers()) {
        const TriggerState s = m_engine.state(t.id);
        QJsonObject o{{u"id"_s, t.id}, {u"enabled"_s, s.enabled}, {u"available"_s, s.available}, {u"rateLimited"_s, s.rateLimited}};
        o.insert(u"outcome"_s, s.hasOutcome ? fireOutcomeName(s.lastOutcome) : QString());
        if (s.lastFireMs >= 0)
            o.insert(u"agoMs"_s, double(nowMs - s.lastFireMs));
        if (t.source.kind == TriggerKind::Screen)
            o.insert(u"note"_s, QString(kAutomationScreenInactive));
        else if (!s.available)
            o.insert(u"note"_s, QString(kTriggerSourceUnavailable));
        array.append(o);
    }
    return array;
}

QJsonArray AutomationManager::triggerLog() const
{
    QJsonArray array;
    const qint64 nowMs = now();
    for (const TriggerLogEntry &e : m_engine.log())
        array.append(QJsonObject{{u"id"_s, e.triggerId}, {u"agoMs"_s, double(nowMs - e.atMs)}, {u"outcome"_s, fireOutcomeName(e.outcome)}});
    return array;
}

} // namespace murasaki
