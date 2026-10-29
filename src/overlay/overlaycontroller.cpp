#include "overlaycontroller.h"

#include "petaudiocontroller.h"

#include "constants.h"
#include "crashledger.h"
#include "puppetnames.h"
#include "spansync.h"

#include <QDBusArgument>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <ctime>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kMaxFailures = 3;
constexpr int kRetryMs = 1500;
constexpr int kItemRetryMs = 2000;
constexpr int kOpenTimeoutMs = 10000;
// A paused pet (the session is locked or the screens are off) renders no frame, so mpv never reports that the file
// started: waiting the full time for it would only hold the other pets back, 10 s each. The file has been handed to
// libmpv, whose decoder runs on its own thread, and a file that crashes it does so within this time.
constexpr int kPausedOpenMs = 1000;
constexpr int kStageResumeSyncMs = 400; // after a window of a desktop stage comes back, the time mpv needs to play again before its position is compared
constexpr int kStageSyncMs = 1000;
constexpr double kStageLoopEndMargin = 0.3;   // seconds before the end of the loop in which a window is not compared
constexpr double kStageSeekLatencyGuess = 0.1; // seconds a seek takes to land until the window has measured one
constexpr double kStageMaxSeekLatency = 1.0;   // and the most it is allowed to aim ahead
constexpr qint64 kStageSeekCooldownMs = 3000; // after a seek a window is not seeked again for this long
constexpr double kStageSyncToleranceSeconds = 0.15; // looser than the wallpapers' 50 ms: a seek is itself a visible jump, and a stricter rule can chase its own corrections // the windows of a desktop stage are compared this often (a correction is a seek, so at most once a second)
qint64 monotonicMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

constexpr qint64 kStateSwitchSlowMs = 120; // a change of state that takes longer than this to show is named (states: switching slow)
constexpr double kTranscodeHalveFps = 50; // the daemon halves sources of this rate or more when it does not retime

// The frames are decoded with the file's own delays whatever the pet's timing is (a change of timing is applied to
// the frames that are on screen, without decoding again), so the key is the file and the size.
QString spriteKeyFor(const QString &file, QSize size, bool masks = false)
{
    // (a decode that also makes the hit masks is another decode: turning a behaviour on for a pet that needs masks asks for the frames again)
    return file + u'@' + QString::number(size.width()) + u'x' + QString::number(size.height()) + (masks ? u"+m"_s : QString());
}

QString degradeText(std::string_view name)
{
    return QString::fromUtf8(name.data(), qsizetype(name.size()));
}

QStringList toStringList(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return qdbus_cast<QStringList>(value.value<QDBusArgument>());
    return value.toStringList();
}

PetPlacement parsePlacement(const QJsonObject &object)
{
    PetPlacement placement;
    placement.petId = object.value(u"petId"_s).toString();
    placement.itemId = object.value(u"itemId"_s).toString();
    placement.variant = object.value(u"variant"_s).toString();
    placement.output = object.value(u"output"_s).toString(u"*"_s);
    placement.x = object.value(u"x"_s).toDouble();
    placement.y = object.value(u"y"_s).toDouble();
    placement.width = object.value(u"width"_s).toDouble(256);
    placement.opacity = object.value(u"opacity"_s).toDouble(1);
    placement.playOriginal = object.value(u"playOriginal"_s).toBool();
    placement.paused = object.value(u"paused"_s).toBool();
    placement.fps = object.value(u"fps"_s).toInt(0);
    placement.speed = object.value(u"speed"_s).toDouble(1); // absent from a daemon older than the field: the file's own speed
    // The behaviour fields (revision 12); a daemon without them leaves the defaults, which are what every pet did before.
    placement.clickThrough = object.value(u"clickThrough"_s).toBool(true);
    placement.locked = object.value(u"locked"_s).toBool(false);
    placement.keepAbove = object.value(u"keepAbove"_s).toBool(true);
    placement.hoverHide = object.value(u"hoverHide"_s).toBool(false);
    placement.focusFade = object.value(u"focusFade"_s).toBool(false);
    placement.smartDodge = object.value(u"smartDodge"_s).toBool(false);
    const double angle = object.value(u"rotation"_s).toDouble(0);
    placement.rotation = std::abs(angle) <= 1e9 ? normalizedPetRotation(angle) : 0.0; // (as the store: not a sane angle is 0)
    placement.flipH = object.value(u"flipH"_s).toBool(false);
    placement.flipV = object.value(u"flipV"_s).toBool(false);
    // The sound (revision 15); absent or not valid is no sound.
    placement.audio = applyPetAudioChanges(PetAudio{}, object.value(u"audio"_s)).value_or(PetAudio{});
    // The fullscreen mode (revision 17); absent or not valid is an ordinary pet.
    placement.stage = applyPetStageChanges(PetStage{}, object.value(u"stage"_s)).value_or(PetStage{});
    // The states and Mouse Tracking (revision 19); absent or not valid is neither.
    placement.states = applyPetStatesChanges(PetStates{}, object.value(u"states"_s)).value_or(PetStates{});
    placement.tracking = applyPetTrackingChanges(PetTracking{}, object.value(u"tracking"_s)).value_or(PetTracking{});
    return placement;
}

QString timingModeName(PetTimingMode mode)
{
    switch (mode) {
    case PetTimingMode::Fixed:
        return u"fixed"_s;
    case PetTimingMode::Speed:
        return u"speed"_s;
    case PetTimingMode::File:
        break;
    }
    return u"file"_s;
}

QJsonObject parseObject(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}

} // namespace

OverlayController::OverlayController(DBusBackend *daemon, SurfaceFactory factory, QObject *parent)
    : QObject(parent)
    , m_daemon(daemon)
    , m_factory(std::move(factory))
{
    connect(m_daemon, &DBusBackend::availabilityChanged, this, &OverlayController::onAvailability);
    connect(m_daemon, &DBusBackend::signalReceived, this, &OverlayController::onSignal);
    m_stateTimer.setSingleShot(true);
    connect(&m_stateTimer, &QTimer::timeout, this, &OverlayController::tickStates);
    m_motionTimer.setInterval(33);
    connect(&m_motionTimer, &QTimer::timeout, this, &OverlayController::onMotionTick);
    m_stageSync.setInterval(kStageSyncMs);
    connect(&m_stageSync, &QTimer::timeout, this, &OverlayController::syncStageVideo);
    m_openTimer.setSingleShot(true);
    m_openTimer.setInterval(kOpenTimeoutMs);
    // A window that says nothing for this long is no longer waited for. Its marker goes, so it is not blamed for
    // a crash that happens much later.
    connect(&m_openTimer, &QTimer::timeout, this, [this] {
        const QString petId = m_openingPet;
        const auto it = m_entries.find(petId);
        if (it == m_entries.end() || !it->second.lastView.paused)
            qWarning("murasaki-overlay: pet %s is slow to open its file; opening the others", qPrintable(petId));
        finishOpening(petId);
    });
}

OverlayController::~OverlayController()
{
    *m_alive = false; // (an answer of the daemon that is still on its way finds nobody)
    // A clean exit leaves no marker behind, so only a crash counts against a file.
    for (auto &[petId, entry] : m_entries)
        dropSurface(petId, entry);
}

void OverlayController::dropSurface(const QString &petId, Entry &entry)
{
    if (m_maskLoader && !entry.passFile.isEmpty())
        m_maskLoader->cancel(petId);
    entry.passFile.clear();
    entry.passMasks.reset();
    entry.passPending = false;
    entry.masksWanted = false;
    entry.surface.reset();
    // A drag that was under way ends with its window: no Release will come (an output went, the file was blocked), and a pet must never stay grabbed.
    if (entry.states)
        entry.states->reset(stateNow());
    if (entry.tracker)
        entry.tracker->hold(false, stateNow());
    dropStageWindows(entry);
    entry.stageOutputs.clear();
    entry.stageHidden.clear();
    pushAudio(entry, false);
    entry.startedFile.clear();
    entry.spriteKey.clear();
    entry.spriteBytes = 0;
    entry.spriteAdmitted = false;
    entry.preloadAdmitted = false;
    entry.preloadFailedKey.clear(); // (a new window may do what the old one could not)
    entry.preloadHandedKey.clear();
    entry.preloadReadyKey.clear();
    entry.preloadBytes = 0;
    finishOpening(petId);
    updatePointerWanted(); // the last pet that needed the cursor may be the one that went
}

void OverlayController::dropStageWindows(Entry &entry)
{
    entry.stageWindows.clear();
}

void OverlayController::finishOpening(const QString &petId)
{
    if (!m_ledger)
        return;
    m_ledger->clearOpening(petId);
    if (m_openingPet != petId)
        return;
    m_openingPet.clear();
    m_openTimer.stop();
    // The pets that waited get their turn, outside of whatever callback got us here.
    QTimer::singleShot(0, this, [this] {
        if (m_openingPet.isEmpty())
            reconcile();
    });
}

// Every answer of the daemon comes back through here: one that arrives after the controller is gone is dropped instead of calling a destroyed object.
void OverlayController::daemonCall(const QString &method, const QVariantList &args, DBusBackend::Callback callback)
{
    m_daemon->call(method, args, [alive = m_alive, callback = std::move(callback)](const DBusBackend::Reply &reply) {
        if (*alive)
            callback(reply);
    });
}

void OverlayController::daemonProperty(const QString &name, DBusBackend::Callback callback)
{
    m_daemon->property(name, [alive = m_alive, callback = std::move(callback)](const DBusBackend::Reply &reply) {
        if (*alive)
            callback(reply);
    });
}

void OverlayController::start()
{
    m_daemon->start();
}

int OverlayController::surfaceCount() const
{
    int count = 0;
    for (const auto &[petId, entry] : m_entries)
        count += entry.surface ? 1 : 0;
    return count;
}

void OverlayController::onAvailability(bool available)
{
    m_available = available;
    if (!available) {
        // The pets stay on screen without their daemon, but nobody can end the arrange mode any more: it is a
        // session state of the daemon, which starts again with it off. The real value is read when it returns.
        // (A hidden state is the daemon's too: nobody could show the pets again, and a daemon that comes back starts with them shown.)
        if (m_arrange || m_petsHidden || m_sessionMuted || !m_runtime.isEmpty()) {
            m_arrange = false;
            m_petsHidden = false;
            m_sessionMuted = false;
            applyMute();
            m_runtime.clear(); // the layer is the daemon's too: a daemon that returns starts with none
            m_motionTimer.stop();
            for (auto &[petId, entry] : m_entries)
                refreshPlacement(entry);
            reconcile();
        }
        return;
    }
    daemonProperty(u"ApiVersion"_s, [this](const DBusBackend::Reply &reply) {
        const quint32 version = reply.ok ? reply.value.toUInt() : 0;
        m_apiVersion = version;
        if (version < 5) {
            qWarning("murasaki-overlay: the daemon speaks API %u; pets need 5", version);
            m_compatible = false;
            m_order.clear();
            reconcile();
            emit incompatibleDaemon(version);
            return;
        }
        m_compatible = true;
        sendOutputScales();
        loadSettings();
        daemonProperty(u"PetsArrange"_s, [this](const DBusBackend::Reply &arrange) {
            if (arrange.ok && arrange.value.toBool() != m_arrange) {
                m_arrange = arrange.value.toBool();
                reconcile();
            }
        });
        // Both session states are read again on every connection: a daemon that restarted has them off, and a property has no signal for that.
        daemonProperty(u"PetsHidden"_s, [this](const DBusBackend::Reply &hidden) {
            const bool value = hidden.ok && hidden.value.toBool(); // a daemon older than revision 12 has none: shown
            if (value != m_petsHidden) {
                m_petsHidden = value;
                reconcile();
            }
        });
        loadRuntime();
        daemonProperty(u"PetsMuted"_s, [this](const DBusBackend::Reply &muted) { // (a daemon older than revision 18 has none: not muted)
            m_sessionMuted = muted.ok && muted.value.toBool();
            applyMute();
        });
        requestList();
        // Variants may have changed while the daemon was away; what is on screen stays until the answer is in.
        const QStringList known = m_items.keys();
        for (const QString &id : known)
            fetchItem(id);
    });
}

void OverlayController::onSignal(const QString &name, const QVariantList &args)
{
    if (!m_compatible)
        return;
    if (name == u"PetsChanged"_s) {
        requestList();
    } else if (name == u"PetsArrangeChanged"_s && !args.isEmpty()) {
        const bool arrange = args.constFirst().toBool();
        if (arrange != m_arrange) {
            m_arrange = arrange;
            reconcile();
        }
    } else if (name == u"PetsHiddenChanged"_s && !args.isEmpty()) {
        const bool hidden = args.constFirst().toBool();
        if (hidden != m_petsHidden) {
            m_petsHidden = hidden;
            reconcile();
        }
    } else if (name == u"PetsMutedChanged"_s && !args.isEmpty()) {
        m_sessionMuted = args.constFirst().toBool();
        applyMute();
    } else if (name == u"PetRuntimeChanged"_s) {
        loadRuntime();
    } else if (name == u"SettingsChanged"_s) {
        loadSettings();
    } else if (name == u"TranscodeChanged"_s && !args.isEmpty()) {
        const QString itemId = args.constFirst().toString();
        if (m_items.contains(itemId))
            fetchItem(itemId);
    } else if (name == u"ItemsChanged"_s && args.size() >= 3) {
        QSet<QString> touched;
        for (int i = 0; i < 3; ++i) {
            const QStringList ids = toStringList(args.at(i));
            touched.unite(QSet<QString>(ids.begin(), ids.end()));
        }
        for (const QString &id : std::as_const(touched)) {
            if (m_items.contains(id))
                fetchItem(id);
        }
    }
}

int OverlayController::currentFpsCap() const
{
    return petFpsCap({m_petMaxFps, m_petSaveEnergy, m_onBattery, m_powerSaver});
}

void OverlayController::setPowerState(bool onBattery, bool powerSaver)
{
    if (onBattery == m_onBattery && powerSaver == m_powerSaver)
        return;
    const int before = currentFpsCap();
    m_onBattery = onBattery;
    m_powerSaver = powerSaver;
    if (currentFpsCap() != before)
        reconcile();
}

void OverlayController::setPetLimits(int maxFps, bool saveEnergy)
{
    maxFps = std::clamp(maxFps, 0, kMaxPetFps);
    if (maxFps == m_petMaxFps && saveEnergy == m_petSaveEnergy)
        return;
    const int before = currentFpsCap();
    m_petMaxFps = maxFps;
    m_petSaveEnergy = saveEnergy;
    if (currentFpsCap() != before)
        reconcile();
}

// The daemon's settings petMaxFps and petSaveEnergy. A daemon without them (or without Settings at all) leaves the
// defaults: no limit of our own, saving energy.
void OverlayController::loadSettings()
{
    daemonCall(u"Settings"_s, {}, [this](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            return;
        const QJsonObject values = parseObject(reply.value);
        setPetLimits(values.value(u"petMaxFps"_s).toInt(kDefaultPetMaxFps),
                     values.value(u"petSaveEnergy"_s).toBool(kDefaultPetSaveEnergy));
        const bool hide = values.value(u"stageHideOverFullscreen"_s).toBool(kDefaultStageHideOverFullscreen);
        const double pixels = values.value(u"stageMaxPixels"_s).toDouble(double(kDefaultStageMaxPixels));
        const long long maxPixels = pixels >= kMinStageMaxPixels && pixels <= kMaxStageMaxPixels ? static_cast<long long>(pixels) : kDefaultStageMaxPixels;
        if (hide != m_stageHideOverFullscreen || maxPixels != m_stageMaxPixels) {
            m_stageHideOverFullscreen = hide;
            m_stageMaxPixels = maxPixels;
            if (anyStage())
                reconcile();
        }
        const bool hideOnGame = values.value(u"petsHideOnGame"_s).toBool(kDefaultPetsHideOnGame);
        if (hideOnGame != m_petsHideOnGame) {
            m_petsHideOnGame = hideOnGame;
            if (!m_gameScreens.isEmpty())
                reconcile();
        }
        if (m_audio) {
            m_audio->setVolume(values.value(u"petsVolume"_s).toInt(kDefaultPetsVolume));
            m_settingsMuted = values.value(u"petsMuted"_s).toBool(kDefaultPetsMuted);
            applyMute();
            m_audio->setMuteWhenFullscreen(values.value(u"petsMuteWhenFullscreen"_s).toBool(kDefaultPetsMuteWhenFullscreen));
            m_audio->setRequireSandbox(values.value(u"importRequireSandbox"_s).toBool(true)); // (the player is confined as the imports are)
        }
        const bool requirePuppetSandbox = values.value(u"importRequireSandbox"_s).toBool(true); // (and so is the puppet engine)
        const qint64 puppetPixels = qint64(std::clamp(values.value(u"puppetMaxPixels"_s).toDouble(4194304.0), 1000000.0, 16000000.0));
        if (requirePuppetSandbox != m_requirePuppetSandbox || puppetPixels != m_puppetMaxPixels) {
            m_requirePuppetSandbox = requirePuppetSandbox;
            m_puppetMaxPixels = puppetPixels;
            for (auto &[petId, entry] : m_entries) { // the engines that run are started again under the new rule
                if (entry.surface && isPuppet(entry.placement.itemId))
                    applyEntry(entry);
            }
        }
    });
}

void OverlayController::requestList()
{
    const quint64 seq = ++m_listSeq;
    daemonCall(u"ListPets"_s, {}, [this, seq](const DBusBackend::Reply &reply) {
        if (seq != m_listSeq || !reply.ok)
            return;
        const QJsonArray array = QJsonDocument::fromJson(reply.value.toString().toUtf8()).array();
        QStringList order;
        QHash<QString, PetPlacement> placements;
        for (const QJsonValue &value : array) {
            const PetPlacement placement = parsePlacement(value.toObject());
            if (placement.petId.isEmpty() || placement.itemId.isEmpty())
                continue;
            order << placement.petId;
            placements.insert(placement.petId, placement);
        }
        // New and changed placements; the surfaces of the removed ones go in reconcile().
        for (auto it = placements.cbegin(); it != placements.cend(); ++it) {
            Entry &entry = m_entries[it.key()];
            entry.stored = it.value();
            refreshPlacement(entry);
            if (!m_items.contains(it->itemId) && !m_fetching.contains(it->itemId))
                fetchItem(it->itemId);
        }
        m_order = order;
        reconcile();
    });
}

void OverlayController::fetchItem(const QString &itemId)
{
    if (m_fetching.contains(itemId)) {
        m_refetch.insert(itemId); // what the answer in flight says may already be stale
        return;
    }
    m_fetching.insert(itemId);
    daemonCall(u"GetItem"_s, {itemId}, [this, itemId](const DBusBackend::Reply &reply) {
        m_fetching.remove(itemId);
        if (!reply.ok && !reply.isInvalidArgs()) {
            // A busy or restarting daemon is not "no such item": keep what is known and ask again.
            m_refetch.remove(itemId);
            QTimer::singleShot(kItemRetryMs, this, [this, itemId] {
                if (m_available && itemUsed(itemId))
                    fetchItem(itemId);
            });
            return;
        }
        ItemInfo info;
        if (reply.ok) {
            const QJsonObject item = parseObject(reply.value);
            info.known = !item.isEmpty();
            if (item.value(u"format"_s).toString() == u"puppet"_s) {
                info.puppet = true;
                info.puppetDir = item.value(u"dir"_s).toString();
                const QString entryFile = item.value(u"entryFile"_s).toString();
                info.puppetFile = entryFile.startsWith(info.puppetDir + u'/') ? entryFile.mid(info.puppetDir.size() + 1) : QString();
                const QJsonObject puppet = item.value(u"puppet"_s).toObject();
                for (const QJsonValue &group : puppet.value(u"motionGroups"_s).toArray())
                    info.motionGroups << group.toObject().value(u"name"_s).toString();
                for (const QJsonValue &name : puppet.value(u"expressions"_s).toArray())
                    info.expressions << name.toString();
            }
            for (const QJsonValue &value : item.value(u"variants"_s).toArray()) {
                const QJsonObject object = value.toObject();
                Variant variant;
                variant.name = object.value(u"name"_s).toString();
                variant.file = object.value(u"file"_s).toString();
                variant.size = QSize(object.value(u"width"_s).toInt(), object.value(u"height"_s).toInt());
                variant.fps = object.value(u"fps"_s).toDouble();
                variant.frames = object.value(u"frames"_s).toInt();
                if (object.contains(u"buckets"_s)) {
                    for (const QJsonValue &entry : object.value(u"buckets"_s).toArray()) {
                        const QJsonObject bucket = entry.toObject();
                        const int side = bucket.value(u"bucket"_s).toInt();
                        const QString file = bucket.value(u"file"_s).toString();
                        const QString state = bucket.value(u"state"_s).toString();
                        if (state == u"done"_s && isTranscodeBucket(side) && !file.isEmpty())
                            variant.doneFiles.append({side, bucket.value(u"fps"_s).toInt(-1), file});
                        else if (state == u"queued"_s || state == u"running"_s)
                            variant.copyPending = true;
                    }
                } else {
                    // A revision 5 daemon has one copy per variant, converted to the biggest size.
                    const QJsonObject transcode = object.value(u"transcode"_s).toObject();
                    if (transcode.value(u"state"_s).toString() == u"done"_s && !transcode.value(u"file"_s).toString().isEmpty())
                        variant.doneFiles.append({kTranscodeMaxSide, -1, transcode.value(u"file"_s).toString()});
                }
                if (!variant.name.isEmpty() && !variant.file.isEmpty())
                    info.variants.append(variant);
            }
        } // else: the daemon says the item does not exist (known stays false)
        m_items.insert(itemId, info);
        ++m_itemsRevision;
        reconcile();
        if (m_refetch.remove(itemId))
            fetchItem(itemId);
    });
}

bool OverlayController::isPuppet(const QString &itemId) const
{
    const auto it = m_items.constFind(itemId);
    return it != m_items.cend() && it->known && it->puppet;
}

bool OverlayController::itemUsed(const QString &itemId) const
{
    for (const auto &[petId, entry] : m_entries) {
        if (entry.placement.itemId == itemId)
            return true;
    }
    return false;
}

QString OverlayController::effectiveOutput(const PetPlacement &placement) const
{
    return placement.output == u"*"_s ? m_primary : placement.output;
}

const OverlayController::Variant *OverlayController::variantOf(const PetPlacement &placement) const
{
    const auto it = m_items.constFind(placement.itemId);
    if (it == m_items.cend() || !it->known || it->variants.isEmpty())
        return nullptr;
    for (const Variant &variant : it->variants) {
        if (variant.name == placement.variant)
            return &variant;
    }
    return &it->variants.first();
}

bool OverlayController::resolveView(const PetPlacement &placement, const QString &badTranscode, PetView *view) const
{
    const Variant *chosen = variantOf(placement);
    if (!chosen)
        return false;
    view->placement = placement;
    view->native = chosen->size;
    view->file = chosen->file;
    view->original = chosen->file;
    view->bucket = 0;
    view->speed = 1;
    // The copy that plays, if any: its rate when it was retimed to a uniform step (> 0), 0 at the file's own timing, -1 from a daemon that does not say.
    int copyFps = 0;
    bool haveCopy = false;
    if (!placement.playOriginal) {
        // The smallest finished copy that is big enough, else the biggest there is, else the original. A copy that failed to play (or is
        // gone) is not a candidate. A copy retimed to a uniform rate (made by earlier versions) serves a pet at any fixed rate, exactly (mpv's
        // speed is the ratio of the two rates); the one of exactly the pet's rate is preferred, then a copy at the file's timing, then another
        // rate. A pet at the file's timing or at a speed cannot use a retimed copy (the file's delays are gone from it).
        const int sourceLongest = std::max(chosen->size.width(), chosen->size.height());
        const int stageNeed = placement.stage.active() ? stageNeedFor(placement, chosen->size, sourceLongest) : 0;
        const int need = stageNeed > 0 ? stageNeed
                                       : transcodeNeed(displayedLongestSide(placement.width, chosen->size), scaleOf(placement), sourceLongest);
        const auto rank = [&](const Variant::DoneFile &done) { // higher is better
            if (done.fps > 0 && done.fps == placement.fps)
                return 3;
            return done.fps > 0 ? 1 : 2; // (the daemon removes a retimed copy once one at the file's timing is as big)
        };
        QMap<int, Variant::DoneFile> usable;
        for (const Variant::DoneFile &done : chosen->doneFiles) {
            if (done.file == badTranscode || !QFileInfo::exists(done.file))
                continue;
            if (done.fps > 0 && placement.fps == 0)
                continue;
            if (!usable.contains(done.bucket) || rank(done) > rank(usable.value(done.bucket)))
                usable.insert(done.bucket, done);
        }
        // A stage plays a copy only when it is as big as the stage needs (a smaller one would be stretched over the screen); until then the original plays.
        std::optional<int> picked = pickDoneBucket(usable.keys(), need);
        if (stageNeed > 0 && picked && *picked < need)
            picked.reset();
        if (const auto bucket = picked) {
            view->file = usable.value(*bucket).file;
            view->bucket = *bucket;
            copyFps = usable.value(*bucket).fps;
            haveCopy = true;
        }
    }
    // The rate the frames of the playing file really step at on average: a retimed copy's, else the source's (a copy at the file's timing
    // keeps the wall time of the source, with the frames above 30 fps dropped, but it is the source's pace that a fixed rate means).
    const double sourceRate = chosen->fps;
    const bool halved = haveCopy && copyFps <= 0 && sourceRate >= kTranscodeHalveFps;
    const double copyRate = haveCopy && copyFps > 0 ? copyFps : (halved ? 30 : sourceRate);
    if (placement.fps > 0) {
        // Every source frame is to show for 1/fps. A copy retimed to a uniform rate has all of them, 1/copyFps apart; the original has them
        // at their own delays, on average sourceRate; a halved copy has every other one, so its pace is the source's.
        const double reference = haveCopy && copyFps > 0 ? double(copyFps) : sourceRate;
        view->speed = std::clamp(playbackSpeed(placement.fps, reference), 0.05, 50.0);
        if (haveCopy && copyFps == placement.fps)
            view->speed = 1;
    } else if (placement.speed != 1) {
        // The file's delays, faster or slower: libmpv's speed does that on the video path.
        view->speed = std::clamp(placement.speed, kMinPetSpeed, kMaxPetSpeed);
    }
    view->timing = placement.timing();
    if (halved && placement.fps > 0) {
        // The frame cache plays the copy's frames: with every other source frame gone, a fixed step per frame would run at twice the pace, so the
        // same ratio as libmpv's speed is applied to the copy's own delays.
        view->timing = {0, std::clamp(view->speed, kMinPetSpeed, kMaxPetSpeed)};
    }
    view->fpsCap = currentFpsCap();
    // libmpv's cap is a resampling filter: it drops frames above the rate but also repeats frames of a slower animation, which would
    // judder it and cost more. It only gets the cap when the animation really steps faster than that.
    {
        const double playedRate = copyRate * view->speed;
        view->videoFpsCap = view->fpsCap > 0 && playedRate > view->fpsCap ? view->fpsCap : 0;
    }
    // Hidden pets are not arranged: the mode is the daemon's (PetsArrange stays on) and comes back with the pets.
    // all the pets, this one by a timeline or a trigger, or the pets on a game's screen
    const bool hidden = m_petsHidden || runtimeHidden(placement.petId) || gameHidden(placement);
    view->arrange = m_arrange && !hidden;
    view->paused = placement.paused || m_sessionPaused || hidden;
    view->hidden = hidden;
    return true;
}

void OverlayController::wire(Entry &entry)
{
    const QString petId = entry.placement.petId;
    PetSurface::Callbacks callbacks;
    callbacks.edited = [this, petId](const QJsonObject &changes) {
        if (const auto found = m_entries.find(petId); found != m_entries.end() && found->second.tracker) {
            // The user placed the pet: where it was dropped is its place, and a follower starts from there.
            found->second.tracker->adopt();
            if (changes.contains(u"x"_s) && changes.contains(u"y"_s))
                found->second.pendingHome = PetPoint{std::round(changes.value(u"x"_s).toDouble()), std::round(changes.value(u"y"_s).toDouble())};
        }
        // The user's hand wins over a timeline: the override of the pet is dropped (here at once, and in the daemon), and the edit is stored.
        if (const auto runtime = m_runtime.constFind(petId); runtime != m_runtime.constEnd() && (runtime->position || runtime->motion)) {
            m_runtime.remove(petId);
            daemonCall(u"ClearPetRuntime"_s, {petId}, [](const DBusBackend::Reply &) {});
            updateMotionTimer();
        }
        const bool placed = changes.contains(u"x"_s) && changes.contains(u"y"_s);
        daemonCall(u"UpdatePet"_s, {petId, QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact))},
                       [this, petId, placed](const DBusBackend::Reply &reply) {
                           if (!reply.ok)
                               qWarning("murasaki-overlay: UpdatePet %s: %s", qPrintable(petId), qPrintable(reply.message));
                           if (placed)
                               homeAnswered(petId, reply.ok);
                       });
    };
    callbacks.removeRequested = [this, petId] {
        daemonCall(u"RemovePet"_s, {petId}, [petId](const DBusBackend::Reply &reply) {
            if (!reply.ok)
                qWarning("murasaki-overlay: RemovePet %s: %s", qPrintable(petId), qPrintable(reply.message));
        });
    };
    callbacks.failed = [this, petId](const QString &reason) { onFailed(petId, reason); };
    callbacks.started = [this, petId] { onStarted(petId); };
    callbacks.spriteDone = [this, petId](bool ok, qint64 bytes, const QString &reason) {
        onSpriteDone(petId, ok, bytes, reason);
    };
    callbacks.preloadDone = [this, petId](bool ok, qint64 bytes, const QString &key, const QString &) { onPreloadDone(petId, ok, bytes, key); };
    callbacks.pointerWantChanged = [this] { updatePointerWanted(); };
    callbacks.wrapped = [this, petId](qint64 monotonicNs, qint64 loop) {
        if (m_audio)
            m_audio->onWrap(petId, monotonicNs, loop);
    };
    callbacks.wrapCancelled = [this, petId] {
        if (m_audio)
            m_audio->onWrapCancelled(petId);
    };
    callbacks.clicked = [this, petId] {
        if (m_audio)
            m_audio->onClick(petId);
        if (const auto it = m_entries.find(petId); it != m_entries.end() && it->second.states) {
            it->second.states->click(stateNow());
            deferStatesMoved(petId); // (never from inside the window's own callback: the picture may mean dropping that window)
        }
    };
    callbacks.pointerEvent = [this, petId](const PetPointerEvent &event) { onPointerEvent(petId, event); };
    callbacks.puppetCrashed = [this, petId](bool afterLongRun) { onPuppetCrashed(petId, afterLongRun); };
    callbacks.puppetRefused = [this, petId](const QString &name) { onPuppetRefused(petId, name); };
    entry.surface->setCallbacks(std::move(callbacks));
}

void OverlayController::onStarted(const QString &petId)
{
    // The file plays: whatever failed before is behind us.
    const auto it = m_entries.find(petId);
    if (it == m_entries.end())
        return;
    it->second.failures = 0;
    it->second.lastError.clear();
    it->second.startedFile = it->second.file;
    if (it->second.puppetState == u"starting"_s || it->second.puppetState == u"restarting"_s || it->second.puppetState == u"running"_s) {
        it->second.puppetState = u"running"_s; // (the engine drew its first picture, after a restart too)
        it->second.puppetDegraded.clear();
    }
    finishSwitch(petId, it->second);
    requestMaskPass(it->second, it->second.lastView); // a pet that plays through the video path gets its masks now that its file plays
    finishOpening(petId);
    if (m_spriteBudget > 0) {
        // The file opens: its frames may be decoded now (outside of the window's callback).
        QTimer::singleShot(0, this, [this] { reconcile(); });
    }
    // (A puppet's engine crashes are counted by recordCrash and are not forgiven by a first picture: a model that draws and then kills the engine is the model's - see the rules of the ledger.)
    if (m_ledger && !isPuppet(it->second.placement.itemId)) {
        m_ledger->forget(it->second.file); // it opens after all: earlier crashes were not its fault
    }
}

void OverlayController::onSpriteDone(const QString &petId, bool ok, qint64 bytes, const QString &reason)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || !it->second.surface)
        return;
    Entry &entry = it->second;
    entry.spriteDecoding = false;
    if (ok) {
        entry.spriteBytes = bytes;
        entry.spriteCost = bytes;
        if (bytes > 0 && !entry.spriteKey.isEmpty()) {
            if (m_croppedBytes.size() >= 512)
                m_croppedBytes.clear(); // (a bound, not a policy: it is learned again by the next decode)
            m_croppedBytes.insert(entry.spriteKey, bytes);
        }
        entry.startedFile = entry.file;
        finishSwitch(petId, entry);
        entry.failures = 0;
        entry.lastError.clear();
        if (m_ledger)
            m_ledger->forget(entry.file); // it decodes: earlier crashes were not its fault
        finishOpening(petId);
    } else {
        qWarning("murasaki-overlay: pet %s: cannot cache the frames of %s (%s); playing the video", qPrintable(petId),
                 qPrintable(entry.file), qPrintable(reason));
        entry.spriteFailedKey = entry.spriteKey;
        m_croppedBytes.remove(entry.spriteKey); // (what was known of it may have been wrong: the file changed under the same path)
        entry.spriteKey.clear();
        entry.spriteBytes = 0;
        // The decode was the opening of the file only when no video had played it yet; the video is loading now
        // and ends the opening itself.
        if (entry.startedFile == entry.file)
            finishOpening(petId);
    }
    QTimer::singleShot(0, this, [this] { reconcile(); });
}

void OverlayController::onPreloadDone(const QString &petId, bool ok, qint64 bytes, const QString &key)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || !it->second.surface || key != it->second.preloadHandedKey)
        return; // (an answer for a picture that is no longer the one asked for)
    Entry &entry = it->second;
    if (ok) {
        entry.preloadReadyKey = key;
        entry.preloadBytes = bytes;
    } else {
        entry.preloadFailedKey = key;
        entry.preloadReadyKey.clear();
        entry.preloadBytes = 0;
    }
    QTimer::singleShot(0, this, [this] { reconcile(); });
}

void OverlayController::onFailed(const QString &petId, const QString &reason)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || !it->second.surface)
        return;
    Entry &entry = it->second;
    finishOpening(petId); // it failed politely, not with a crash; the failure counter below handles it
    if (entry.usesTranscode) {
        // The converted copy is broken: play the original, which is what the user has anyway.
        qWarning("murasaki-overlay: pet %s cannot play its transcoded copy %s (%s); using the original",
                 qPrintable(petId), qPrintable(entry.file), qPrintable(reason));
        entry.badTranscode = entry.file;
    } else {
        entry.lastError = reason;
        ++entry.failures;
        if (entry.failures == 1 || entry.failures >= kMaxFailures) // once, then when it is given up
            qWarning("murasaki-overlay: pet %s cannot play %s (%s), failure %d of %d", qPrintable(petId),
                     qPrintable(entry.file), qPrintable(reason), entry.failures, kMaxFailures);
        if (entry.failures >= kMaxFailures) {
            entry.blocked = true;
            entry.blockedFile = entry.file;
        }
    }
    // Never destroy a surface from inside its own callback.
    QTimer::singleShot(entry.blocked || entry.usesTranscode ? 0 : kRetryMs, this, [this, petId] {
        const auto found = m_entries.find(petId);
        if (found == m_entries.end())
            return;
        dropSurface(petId, found->second);
        found->second.file.clear();
        reconcile();
    });
}

void OverlayController::applyEntry(Entry &entry)
{
    PetView view;
    const QString petId = entry.placement.petId;
    const PetPlacement basis = effectivePlacement(entry.placement);
    if (!resolveView(basis, entry.badTranscode, &view)) {
        dropSurface(petId, entry);
        return;
    }
    // A transcoded copy that took the process down before is skipped like one that failed to play.
    if (m_ledger && view.file != view.original && m_ledger->blocked(view.file)) {
        entry.badTranscode = view.file;
        resolveView(basis, entry.badTranscode, &view);
    }
    if (entry.states && entry.states->holding())
        view.paused = true; // `once`: the pass is over and the last frame stays (about: the pass is counted from the moment the window was handed the file)
    if (entry.blocked && view.file != entry.blockedFile) {
        entry.blocked = false;
        entry.failures = 0;
    }
    if (!entry.blocked && m_ledger && m_ledger->blocked(view.file)) {
        qWarning("murasaki-overlay: pet %s: %s crashed the overlay %d times; not opening it", qPrintable(petId),
                 qPrintable(view.file), CrashLedger::kMaxCrashes);
        entry.blocked = true;
        entry.blockedFile = view.file;
        entry.failures = kMaxFailures;
        entry.lastError = u"it crashed the overlay"_s;
    }
    if (isPuppet(basis.itemId)) {
        applyPuppetEntry(entry, view);
        return;
    }
    QString output = effectiveOutput(entry.placement);
    // The fullscreen mode: the layout on the outputs there are. A stage the session can not draw is an ordinary pet with a name for it; one whose outputs
    // another stage has is not shown.
    StageLayout stage;
    bool isStage = false;
    entry.stageDegraded.clear();
    entry.stageOutputs.clear();
    entry.stageHidden.clear();
    if (entry.placement.stage.active()) {
        if (stageTaken(entry)) {
            entry.stageDegraded.append(degradeText(stagedegrade::kOutputTaken));
            dropSurface(petId, entry);
            return;
        }
        isStage = stageLayoutFor(entry, view, &stage);
        if (isStage)
            output = stage.slices.first().output;
    }
    if (!isStage)
        dropStageWindows(entry);
    if (entry.blocked || output.isEmpty() || !m_outputs.contains(output)) {
        dropSurface(petId, entry);
        return;
    }
    // What each window of a stage is shown: its own rectangle, and hidden while a fullscreen window is on its output.
    const auto stageView = [&](PetView window, const StageSlice &slice) {
        const bool fullscreen = hiddenByFullscreen(entry, slice.output);
        const QRect outputRect = m_geometry.value(slice.output);
        window.stage.active = true;
        window.stage.petRect = stagePetRect(stage, slice, QSizeF(view.native), entry.placement.rotation, outputRect);
        if (&slice == &stage.slices.first() && (entry.trackOffset.x != 0 || entry.trackOffset.y != 0))
            window.stage.petRect.translate(entry.trackOffset.x, entry.trackOffset.y); // a follower's picture walks inside its screen
        window.arrange = false;
        window.hidden = window.hidden || fullscreen;
        window.paused = window.paused || fullscreen;
        window.spriteSize = QSize();
        window.spriteKey.clear();
        window.masks.reset();
        return window;
    };
    const PetView baseView = view; // (each window of a stage starts from this, not from the first one's)
    if (isStage) {
        view = stageView(baseView, stage.slices.first());
        for (const StageSlice &slice : std::as_const(stage.slices))
            entry.stageOutputs.append(slice.output);
        bool all = true;
        QString app;
        QSet<QString> hiddenNow;
        for (const StageSlice &slice : std::as_const(stage.slices)) {
            const bool hidden = hiddenByFullscreen(entry, slice.output);
            all = all && hidden;
            if (hidden) {
                hiddenNow.insert(slice.output);
                if (app.isEmpty())
                    app = m_fullscreen.value(slice.output, m_fullscreen.value(u"*"_s));
            }
        }
        // A window that comes back after a fullscreen window left has lost the time it was paused: the next span sync, soon, brings it to the others' clock.
        for (const QString &output : std::as_const(entry.stageHiddenOutputs)) {
            if (!hiddenNow.contains(output)) {
                QTimer::singleShot(kStageResumeSyncMs, this, &OverlayController::syncStageVideo);
                break;
            }
        }
        entry.stageHiddenOutputs = hiddenNow;
        if (m_petsHidden || runtimeHidden(entry.placement.petId))
            entry.stageHidden = u"hidden"_s;
        else if (all && !m_fullscreenAnswered)
            entry.stageHidden = u"starting"_s; // (the surface has not answered yet)
        else if (all)
            entry.stageHidden = u"fullscreen"_s;
        else if (m_sessionPaused)
            entry.stageHidden = u"session"_s;
        // Drawn below full resolution only where something is: what plays is compared with what it would take without the cap (the longest side of the copy, of the
        // frames, or of the original that plays as it is). A copy rounded up to the next bucket is not "reduced" by the cap.
        entry.stageScale = 1.0;
        if (m_stageFactor < 1) {
            const Variant *variant = variantOf(entry.placement);
            const int source = variant ? std::max(variant->size.width(), variant->size.height()) : 0;
            const int full = variant ? stageRequestNeed(stageOutputList(), stageRequestOf(entry.placement, variant->size), 1.0, source) : 0;
            const int played = view.bucket > 0 ? view.bucket : (entry.spriteAdmitted ? std::max(entry.spriteSize.width(), entry.spriteSize.height()) : source);
            if (full > 0 && played > 0 && played < full)
                entry.stageScale = std::max(0.1, double(played) / full);
        }
        if (entry.stageScale < 1)
            entry.stageDegraded.append(degradeText(stagedegrade::kResolutionReduced));
        if (const Variant *variant = variantOf(entry.placement); variant && view.file == view.original && !entry.placement.playOriginal) {
            if (variant->copyPending)
                entry.stageDegraded.append(degradeText(stagedegrade::kTranscodePending));
            else if (!variant->doneFiles.isEmpty())
                entry.stageDegraded.append(degradeText(stagedegrade::kCopyTooSmall)); // copies exist, none as big as the stage needs: the original plays
        }
        if (all && !m_petsHidden && m_fullscreenAnswered)
            entry.stageDegraded.append(app.isEmpty() ? degradeText(stagedegrade::kHiddenByFullscreen)
                                                      : degradeText(stagedegrade::kHiddenByFullscreen) + u": "_s + app);
    }
    const bool needsSurface = !entry.surface || entry.output != output;
    const bool opening = needsSurface || view.file != entry.file;
    if (opening && m_ledger && !m_openingPet.isEmpty() && m_openingPet != petId)
        return; // another file is being opened; finishOpening() applies this pet afterwards
    if (needsSurface) {
        if (entry.surface && entry.states)
            entry.states->reset(stateNow()); // (the old window goes with whatever it was holding)
        if (entry.surface && entry.tracker)
            entry.tracker->hold(false, stateNow());
        entry.surface = m_factory(output, petId);
        if (!entry.surface)
            return;
        entry.output = output;
        wire(entry);
    }
    if (opening && m_ledger) {
        m_ledger->markOpening(petId, view.file); // before the window sees the file: a crash must leave this behind
        m_openingPet = petId;
        m_openTimer.start(view.paused ? kPausedOpenMs : kOpenTimeoutMs);
    }
    // The GPU frame cache: the window decodes the frames when it is handed a new key, one decode at a time.
    view.spriteSize = entry.spriteAdmitted ? entry.spriteSize : QSize();
    view.spriteBudget = entry.spriteAllowance;
    if (!entry.spriteAdmitted) {
        // The window drops its frames and stops the decode without saying so: the decode's turn ends here.
        if (entry.spriteDecoding) {
            entry.spriteDecoding = false;
            if (entry.startedFile == view.file)
                finishOpening(petId);
        }
        entry.spriteKey.clear();
        entry.spriteBytes = 0;
    } else {
        const QString wanted = spriteKeyFor(view.file, entry.spriteSize, entry.masksWanted);
        const bool playable = entry.startedFile == view.file || entry.spriteBytes > 0;
        if (wanted != entry.spriteKey && playable) {
            if (m_ledger && !m_openingPet.isEmpty() && m_openingPet != petId) {
                // another file is being opened; finishOpening() applies this pet afterwards
            } else {
                if (m_ledger && !opening) {
                    m_ledger->markOpening(petId, view.file);
                    m_openingPet = petId;
                    m_openTimer.start();
                }
                entry.spriteKey = wanted;
                entry.spriteDecoding = true;
            }
        }
    }
    view.spriteKey = entry.spriteKey;
    view.restartToken = entry.restartToken;
    // The picture prepared for the next state: once the pet's own frames are in, never while they are being made (a decode and a preload would hold three sets).
    if (entry.preloadAdmitted && entry.spriteAdmitted && !entry.spriteDecoding && entry.spriteBytes > 0 && entry.spriteKey == spriteKeyFor(view.file, entry.spriteSize, entry.masksWanted)
        && !entry.preloadKey.isEmpty()) {
        view.preloadFile = entry.preloadFile;
        view.preloadSize = entry.preloadSize;
        view.preloadKey = entry.preloadKey;
        view.preloadBudget = entry.preloadAllowance;
        entry.preloadHandedKey = entry.preloadKey;
        if (entry.preloadReadyKey != entry.preloadKey) {
            entry.preloadReadyKey.clear();
            entry.preloadBytes = 0;
        }
    } else {
        entry.preloadHandedKey.clear();
        entry.preloadReadyKey.clear();
        entry.preloadBytes = 0;
    }
    entry.file = view.file;
    entry.bucket = view.bucket;
    entry.usesTranscode = view.file != view.original;
    // The pointer input: the level, the masks' budget and the masks of a pet that plays through the video path.
    if (needsMasks(basis))
        entry.masksWanted = true;
    view.pointer = m_pointerLevel;
    view.transformApplied = true; // the windows draw the rotation and the flips: the masks turn with them
    view.maskBudget = entry.masksWanted ? maskBudgetFor() : 0; // (a pet with no use for masks makes none)
    requestMaskPass(entry, view);
    view.masks = entry.passFile == view.file ? entry.passMasks : nullptr;
    entry.lastView = view;
    entry.surface->apply(view);
    if (isStage) {
        // The windows of the other outputs of a `desktop` stage: made and dropped with the outputs, each with its part of the one picture.
        std::vector<Entry::StageWindow> windows;
        for (qsizetype i = 1; i < stage.slices.size(); ++i) {
            const StageSlice &slice = stage.slices.at(i);
            Entry::StageWindow window;
            for (Entry::StageWindow &old : entry.stageWindows) {
                if (old.output == slice.output && old.surface) {
                    window = std::move(old);
                    break;
                }
            }
            if (!window.surface) {
                window.output = slice.output;
                window.surface = m_factory(slice.output, petId);
                if (!window.surface)
                    continue;
                PetSurface::Callbacks callbacks;
                callbacks.failed = [this, petId](const QString &reason) { onFailed(petId, reason); };
                window.surface->setCallbacks(std::move(callbacks));
            }
            window.surface->apply(stageView(baseView, slice));
            windows.push_back(std::move(window));
        }
        entry.stageWindows = std::move(windows);
    }
    pushAudio(entry, true);
    updatePointerWanted();
}

// A puppet pet (docs/pets/puppets.md section 22). Its picture comes from an engine process that the window starts: no frame cache, no transcode, no mask pass, no preload. What it shares with every pet is the
// opening marker of the crash ledger (the model file is the file), the failure counter (the engine could not start), the states (a variant named "motion:<Group>" plays that group, "expression:<Name>" sets that
// expression, the base "model" plays nothing and shows no expression) and the pointer (Mouse Tracking `look` is the gaze, not a flip).
void OverlayController::applyPuppetEntry(Entry &entry, PetView view)
{
    const QString petId = entry.placement.petId;
    const PetPlacement basis = effectivePlacement(entry.placement);
    const ItemInfo &info = m_items[basis.itemId];
    const QString output = effectiveOutput(entry.placement);
    if (info.puppetFile.isEmpty()) { // the daemon's item names a model file outside its folder: nothing to start, and the pet says why
        if (entry.puppetState != u"refused"_s) {
            qWarning("murasaki-overlay: pet %s: the puppet item %s names no model file inside its folder", qPrintable(petId), qPrintable(basis.itemId));
            entry.puppetState = u"refused"_s;
            entry.puppetDegraded = QStringList{QString::fromUtf8(puppet::kManifestInvalid.data(), qsizetype(puppet::kManifestInvalid.size())) + u": model file"_s};
            emit stateChanged();
        }
        dropSurface(petId, entry);
        return;
    }
    if (entry.blocked || output.isEmpty() || !m_outputs.contains(output)) {
        dropSurface(petId, entry);
        return;
    }
    const bool needsSurface = !entry.surface || entry.output != output;
    const bool opening = needsSurface || view.file != entry.file;
    if (opening && m_ledger && !m_openingPet.isEmpty() && m_openingPet != petId)
        return; // another file is being opened; finishOpening() applies this pet afterwards
    if (needsSurface) {
        if (entry.surface && entry.states)
            entry.states->reset(stateNow());
        if (entry.surface && entry.tracker)
            entry.tracker->hold(false, stateNow());
        entry.surface = m_factory(output, petId);
        if (!entry.surface)
            return;
        entry.output = output;
        wire(entry);
        entry.puppetState = u"starting"_s; // (running once the engine has drawn its first picture: onStarted)
        entry.puppetGroup.clear();
        entry.puppetExpression.clear();
    }
    if (opening && m_ledger) {
        m_ledger->markOpening(petId, view.file); // before the window starts the engine: a crash of the overlay must leave this behind
        m_openingPet = petId;
        m_openTimer.start(view.paused ? kPausedOpenMs : kOpenTimeoutMs);
    }
    entry.file = view.file;
    entry.bucket = 0;
    entry.usesTranscode = false;
    // What the pet's variant (its state's, or the placement's) asks of the model.
    QString group, expression;
    if (basis.variant.startsWith(u"motion:"_s) && info.motionGroups.contains(basis.variant.mid(7)))
        group = basis.variant.mid(7);
    else if (basis.variant.startsWith(u"expression:"_s) && info.expressions.contains(basis.variant.mid(11)))
        expression = basis.variant.mid(11);
    if (group != entry.puppetStateGroup) { // the state moved to another group (or to none): that is what plays now - a click's tap motion is the group until the state changes again
        entry.puppetStateGroup = group;
        entry.puppetGroup = group;
        ++entry.puppetSerial;
    }
    entry.puppetExpression = expression;
    int fps = basis.fps > 0 ? basis.fps : 60;
    if (const int cap = currentFpsCap(); cap > 0)
        fps = std::min(fps, cap);
    view.puppet.active = true;
    view.puppet.dir = info.puppetDir;
    view.puppet.file = info.puppetFile;
    view.puppet.sandbox = true;
    view.puppet.requireSandbox = m_requirePuppetSandbox;
    view.puppet.blocked = entry.blocked;
    view.puppet.look = entry.puppetLook;
    view.puppet.expression = expression;
    view.puppet.group = entry.puppetGroup;
    view.puppet.playSerial = entry.puppetSerial;
    view.puppet.fps = std::clamp(fps, 1, 120);
    view.puppet.maxPixels = m_puppetMaxPixels;
    for (const QString &group : {u"TapBody"_s, u"Tap"_s}) { // (a click on no hit area: the window sends these to the engine)
        if (info.motionGroups.contains(group))
            view.puppet.tapGroups << group;
    }
    // None of the picture machinery: the engine draws at the size of the window.
    view.spriteSize = QSize();
    view.spriteKey.clear();
    view.preloadKey.clear();
    view.masks.reset();
    view.maskBudget = 0;
    view.pointer = m_pointerLevel;
    view.transformApplied = true;
    entry.spriteAdmitted = false;
    entry.spriteKey.clear();
    entry.spriteBytes = 0;
    entry.lastView = view;
    entry.surface->apply(view);
    pushAudio(entry, true);
    updatePointerWanted();
}

void OverlayController::onPuppetCrashed(const QString &petId, bool afterLongRun)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || !it->second.surface)
        return;
    Entry &entry = it->second;
    finishOpening(petId); // (a crash of the engine is not a crash of the overlay: the opening marker goes before the ledger counts it)
    if (m_ledger)
        m_ledger->recordCrash(entry.file, afterLongRun);
    if (m_ledger && m_ledger->blocked(entry.file)) {
        entry.blocked = true;
        entry.blockedFile = entry.file;
        entry.failures = kMaxFailures;
        entry.lastError = u"puppet: blocked after crashes"_s;
        entry.puppetState = u"blocked"_s;
        entry.puppetDegraded = QStringList{QString::fromUtf8(puppet::kBlockedAfterCrashes.data(), qsizetype(puppet::kBlockedAfterCrashes.size()))};
        qWarning("murasaki-overlay: pet %s: the puppet engine crashed on %s %d times; not starting it again", qPrintable(petId), qPrintable(entry.file), CrashLedger::kMaxCrashes);
        QTimer::singleShot(0, this, [this, petId] { // (never destroy a surface from inside its own callback)
            if (const auto found = m_entries.find(petId); found != m_entries.end()) {
                dropSurface(petId, found->second);
                found->second.file.clear();
                emit stateChanged();
            }
        });
    } else {
        entry.puppetState = u"restarting"_s;
        entry.puppetDegraded = QStringList{QString::fromUtf8(puppet::kEngineRestarted.data(), qsizetype(puppet::kEngineRestarted.size()))};
        if (m_ledger) { // the engine starts again by itself: the opening marker for the new run
            m_ledger->markOpening(petId, entry.file);
            m_openingPet = petId;
            m_openTimer.start(kOpenTimeoutMs);
        }
    }
    emit stateChanged();
}

void OverlayController::onPuppetRefused(const QString &petId, const QString &name)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || !it->second.surface)
        return;
    Entry &entry = it->second;
    finishOpening(petId);
    entry.puppetState = name == QString::fromUtf8(puppet::kNoGpu.data(), qsizetype(puppet::kNoGpu.size())) ? u"nogpu"_s : u"refused"_s;
    entry.puppetDegraded = QStringList{name};
    emit stateChanged();
}

// ---------------------------------------------------------------- the states of a pet

qint64 OverlayController::stateNow() const
{
    return m_stateClock ? m_stateClock() : monotonicMs();
}

bool OverlayController::inactive(const Entry &entry) const
{
    return entry.placement.paused || m_sessionPaused || m_petsHidden || runtimeHidden(entry.stored.petId) || gameHidden(entry.placement) || m_arrange;
}

bool OverlayController::trackerRuns(const Entry &entry) const
{
    // (a follower parked over a fullscreen window does not need the cursor either: nothing is watched while the user plays)
    return entry.tracker && entry.surface && !inactive(entry) && !entry.tracker->parked();
}

void OverlayController::refreshPlacement(Entry &entry)
{
    entry.placement = withRuntime(entry.stored);
    refreshStates(entry);
    refreshTracking(entry);
}

void OverlayController::refreshStates(Entry &entry)
{
    if (entry.stored.states.isDefault()) {
        // No states: no machine, no timer, no events; the pet shows its variant as it always did.
        entry.states.reset();
        entry.stateGeneration = 0;
        entry.stateHolding = false;
        entry.switchStartedAt = -1;
        entry.switchMs = -1;
        return;
    }
    const QString petId = entry.stored.petId;
    const qint64 now = stateNow();
    if (!entry.states) {
        entry.states = std::make_unique<PetStateMachine>(PetStateTuning{}, quint32(qHash(petId)));
        entry.stateGeneration = 0;
    }
    PetStateMachine &machine = *entry.states;
    if (entry.surface && entry.output != effectiveOutput(entry.stored))
        machine.reset(now); // the pet goes to another output: the window it was held in is replaced, and the new one starts at rest
    const QString itemId = entry.placement.itemId;
    if (entry.stateItemsRevision != m_itemsRevision) {
        entry.stateItemsRevision = m_itemsRevision;
        machine.setVariantExists(
            [this, itemId](const QString &name) {
                const auto item = m_items.constFind(itemId);
                if (item == m_items.cend() || !item->known)
                    return true; // not told yet: the pet shows nothing until it is
                return std::any_of(item->variants.cbegin(), item->variants.cend(), [&](const Variant &variant) { return variant.name == name; });
            },
            now);
    }
    // A variant that a timeline or a trigger set (the runtime layer) is the pet's base: what states name still wins over it, except that the layer's
    // own choice is shown as it is while the layer holds it.
    const bool layerVariant = entry.placement.variant != entry.stored.variant;
    machine.setBaseVariant(entry.placement.variant, now);
    machine.setStates(entry.stored.states, now);
    if (inactive(entry)) {
        machine.reset(now); // arranging is editing, and a pet nobody sees has nothing to react to
        machine.setSuspended(true, now); // and nothing is waited for: no sequence runs, no timer wakes
    } else {
        machine.setSuspended(false, now);
    }
    // How long one pass of the animation on screen is (the pet's rate over the file's frames). What the machine shows can change as it learns it (a pass that
    // was over), so it is told again until it settles.
    for (int round = 0; round < 3; ++round) {
        const QString shownName = machine.variant();
        int passMs = 0;
        if (const auto item = m_items.constFind(itemId); item != m_items.cend()) {
            for (const Variant &variant : std::as_const(item->variants)) {
                if (variant.name == shownName && variant.frames > 1) {
                    const double rate = entry.placement.fps > 0 ? double(entry.placement.fps) : variant.fps * std::max(entry.placement.speed, 0.01);
                    if (rate > 0)
                        passMs = int(std::min<double>(3'600'000.0, variant.frames * 1000.0 / rate));
                }
            }
        }
        machine.setAnimation(passMs, true, now); // (every file plays in a loop here, whatever its own loop count says)
        if (machine.variant() == shownName)
            break;
    }
    if (machine.restarts() != entry.stateRestarts) {
        entry.stateRestarts = machine.restarts();
        ++entry.restartToken;
    }
    if (!layerVariant)
        entry.placement.variant = machine.variant();
    entry.stateGeneration = machine.generation();
    entry.stateHolding = machine.holding();
}

PetPoint OverlayController::cursorInOutput(const Entry &entry, QPointF global) const
{
    return {global.x() - entry.trackOrigin.x(), global.y() - entry.trackOrigin.y()};
}

bool OverlayController::fullscreenOn(const Entry &entry) const
{
    return m_fullscreenKnown && (m_fullscreen.contains(effectiveOutput(entry.stored)) || m_fullscreen.contains(u"*"_s));
}

void OverlayController::refreshTracking(Entry &entry)
{
    const PetTracking tracking = trackingOf(entry.stored);
    // A follow the runtime layer asked for is never stored: it glides home when it ends, whatever the pet's own returnHome says (also after the layer is cleared).
    if (const auto layer = m_runtime.constFind(entry.stored.petId); layer != m_runtime.constEnd() && layer->tracking && *layer->tracking == PetTrackingMode::Follow)
        entry.trackForceHome = true;
    else if (!(entry.tracker && entry.tracker->busy()))
        entry.trackForceHome = false;
    // Both modes need the cursor wherever it is: the global pointer level.
    // A stage (docs/pets/states.md, "In a stage") has no window to move: its picture moves inside the screen it covers (follow, `output` mode) or turns (look, both
    // modes); a stage the session can not draw is an ordinary pet.
    // (only a pet whose placement is a stage asks: a follower steps thirty times a second and a pet that is not a stage needs no copy of its placement for that)
    const bool stage = entry.stored.stage.active() && effectivePlacement(entry.placement).stage.active();
    const bool stageAcrossAll = stage && entry.stored.stage.mode == StageMode::Desktop;
    const bool possible = tracking.active() && m_pointer && m_pointerLevel == PointerSource::Level::Global;
    // A puppet's `look` is its gaze (the window gives the engine the pointer), not a mirror of the picture.
    entry.puppetLook = possible && tracking.mode == PetTrackingMode::Look && isPuppet(entry.stored.itemId);
    // The place and the flip the layers below give (the stored placement and the runtime layer); the tracking goes on top of them, so this can run on its own.
    const PetPlacement below = withRuntime(entry.stored);
    entry.placement.x = below.x;
    entry.placement.y = below.y;
    entry.placement.flipH = below.flipH;
    // Where the daemon says the pet is: once it agrees with a drop or a hand-over, the wait is over.
    if (entry.pendingHome && std::abs(entry.pendingHome->x - entry.stored.x) < 0.5 && std::abs(entry.pendingHome->y - entry.stored.y) < 0.5)
        entry.pendingHome.reset();
    // A pet that was following and is told to stop is kept until it is home (or has handed over where it is).
    if (!possible && !(entry.tracker && entry.tracker->busy())) {
        const bool had = entry.tracker != nullptr;
        entry.tracker.reset();
        entry.trackMirror.reset();
        entry.trackOffset = {};
        entry.trackMoving = false;
        entry.trackParked = false;
        if (entry.pendingHome) {
            entry.placement.x = entry.pendingHome->x;
            entry.placement.y = entry.pendingHome->y;
        }
        if (had)
            updatePointerWanted(); // the last pet that needed the cursor may be this one
        return;
    }
    const qint64 now = stateNow();
    if (!entry.tracker)
        entry.tracker = std::make_unique<PetTracker>();
    PetTracker &tracker = *entry.tracker;
    PetTrackerConfig config;
    config.mode = possible ? tracking.mode : PetTrackingMode::Off;
    if (entry.puppetLook)
        config.mode = PetTrackingMode::Off; // (a puppet's `look` is its gaze)
    if (config.mode == PetTrackingMode::Follow && entry.stored.locked && !stage)
        config.mode = PetTrackingMode::Off; // a locked pet does not move (look still works; a stage is locked by its nature)
    if (config.mode == PetTrackingMode::Follow && stageAcrossAll)
        config.mode = PetTrackingMode::Off; // one picture across all the screens has nowhere to walk
    config.distance = tracking.distance;
    config.speed = tracking.speed;
    config.returnHome = tracking.returnHome || stage || entry.trackForceHome; // (a stage's picture has no stored place to stay at: it always goes back)
    tracker.setConfig(config, now);
    // The pet's rectangle at its stored place (the height follows the animation that plays), and its output; the cursor comes in the output's space.
    // (the placement's own variant, not the state's: a pet whose states differ in height would otherwise move its centre with every change of state and chase itself)
    const Variant *shown = variantOf(entry.stored);
    const QSize size = petDisplaySize(entry.stored.width, shown ? shown->size : QSize());
    const QRect output = m_geometry.value(effectiveOutput(entry.stored));
    const PetPoint home = entry.pendingHome ? *entry.pendingHome : PetPoint{entry.stored.x, entry.stored.y};
    entry.trackOrigin = output.topLeft();
    PetRect homeRect{home.x, home.y, double(size.width()), double(size.height())};
    PetRect outputRect = output.isEmpty() ? PetRect{0, 0, 100000, 100000} : PetRect{0, 0, double(output.width()), double(output.height())};
    if (stage && shown) {
        // the picture's own rectangle inside the screen it covers (the window of a stage is its output)
        StageLayout layout;
        if (buildStageLayout(entry.placement, shown->size, &layout)) {
            const StageSlice &slice = layout.slices.first();
            const QRect sliceOutput = m_geometry.value(slice.output);
            const QRectF rect = stagePetRect(layout, slice, QSizeF(shown->size), entry.placement.rotation, sliceOutput);
            homeRect = {rect.x(), rect.y(), rect.width(), rect.height()};
            outputRect = {0, 0, double(sliceOutput.width()), double(sliceOutput.height())};
            entry.trackOrigin = sliceOutput.topLeft(); // (the cursor comes in the space of the picture's own screen, which is not always the main one)
        }
    }
    tracker.setGeometry(homeRect, outputRect);
    tracker.park(config.mode == PetTrackingMode::Follow && fullscreenOn(entry), now);
    // Nothing is seen or arranged, or the session lost the global cursor: it ends at once instead of walking home.
    if (inactive(entry) || !entry.surface || m_pointerLevel != PointerSource::Level::Global)
        tracker.halt(now);
    if (inactive(entry))
        tracker.reset(now); // (the cursor is forgotten too)
    if (const auto commit = tracker.takeCommit()) {
        // Following ended where the pet was and returnHome is off: that is its place now, stored once.
        const PetPoint place{std::round(home.x + commit->x), std::round(home.y + commit->y)};
        entry.pendingHome = place;
        sendHome(entry.stored.petId, place);
    }
    entry.trackMirror = tracker.mirror();
    if (entry.trackMirror)
        entry.placement.flipH = entry.placement.flipH != *entry.trackMirror;
    entry.trackOffset = tracker.offset();
    const PetPoint base = entry.pendingHome ? *entry.pendingHome : PetPoint{entry.stored.x, entry.stored.y};
    if (!stage && (entry.pendingHome || entry.trackOffset.x != 0 || entry.trackOffset.y != 0)) {
        // (the runtime layer's own position, when there is one, is the base the displacement is added to)
        const bool layerPlace = below.x != entry.stored.x || below.y != entry.stored.y;
        if (!layerPlace) {
            entry.placement.x = base.x;
            entry.placement.y = base.y;
        }
        entry.placement.x += entry.trackOffset.x;
        entry.placement.y += entry.trackOffset.y;
    }
    entry.trackMoving = tracker.moving();
    entry.trackParked = tracker.parked();
    if (!possible && !tracker.busy()) {
        entry.tracker.reset();
        entry.trackMirror.reset();
        entry.trackOffset = {};
        entry.trackMoving = false;
        entry.trackParked = false;
        updatePointerWanted();
    }
}

void OverlayController::sendHome(const QString &petId, PetPoint place)
{
    daemonCall(u"UpdatePet"_s, {petId, QString::fromUtf8(QJsonDocument(QJsonObject{{u"x"_s, place.x}, {u"y"_s, place.y}}).toJson(QJsonDocument::Compact))},
                   [this, petId](const DBusBackend::Reply &reply) {
                       if (!reply.ok)
                           qWarning("murasaki-overlay: UpdatePet %s: %s", qPrintable(petId), qPrintable(reply.message));
                       homeAnswered(petId, reply.ok);
                   });
}

// The daemon answered the store of a place. Refused: the stored place is the pet's again at once. Accepted: the list that carries it is on its way (or came); a
// place that the daemon changed (a clamp) or that someone changed after is not held for ever: the stored one rules after a few seconds.
void OverlayController::homeAnswered(const QString &petId, bool ok)
{
    const auto drop = [this, petId] {
        const auto it = m_entries.find(petId);
        if (it == m_entries.end() || !it->second.pendingHome)
            return;
        it->second.pendingHome.reset();
        refreshPlacement(it->second);
        applyEntry(it->second);
    };
    if (!ok)
        drop();
    else
        QTimer::singleShot(3000, this, drop);
}

// The cursor moved (or time passed): when that changes what the pet shows, the window is handed the new picture.
void OverlayController::trackerMoved(Entry &entry)
{
    if (!entry.tracker)
        return;
    const bool moving = entry.tracker->moving();
    const PetPoint offset = entry.tracker->offset();
    const bool offsetChanged = offset.x != entry.trackOffset.x || offset.y != entry.trackOffset.y;
    if (moving && offsetChanged && entry.states) {
        entry.states->dodgeStep(stateNow(), 120); // a follower that walks is in the moving state, and idle when it stops
        statesMoved(entry);
        if (!entry.tracker)
            return;
    }
    const bool mirrorChanged = entry.tracker->mirror() != entry.trackMirror;
    if (mirrorChanged || offsetChanged) {
        // Only the tracking is applied again (a step is not a change of the pet's states or its plan), and Windows() is told only when something else than the
        // place changed: a follower must not make the overlay publish its list thirty times a second.
        const bool wasMoving = entry.trackMoving;
        const bool wasParked = entry.trackParked;
        refreshTracking(entry); // (it ends the tracker when the pet is home and tracking is off)
        applyEntry(entry);
        if (mirrorChanged || !entry.tracker || wasMoving != moving || wasParked != entry.tracker->parked())
            emit stateChanged();
    }
    if (entry.tracker) {
        entry.trackMoving = moving;
        entry.trackParked = entry.tracker->parked();
    } else {
        updatePointerWanted();
    }
    scheduleStates();
}

bool OverlayController::previewState(const QString &petId, const QString &stateName)
{
    const auto it = m_entries.find(petId);
    const auto state = petStateFromName(stateName);
    if (it == m_entries.end() || !it->second.states || !state || !it->second.surface || inactive(it->second))
        return false;
    Entry &entry = it->second;
    const qint64 now = stateNow();
    // The GUI is not a way to keep the overlay busy: one look at a time, a quarter of a second apart, and never over a pet that is being held.
    if (now - entry.lastPreviewAt < 250 || (entry.states->previewing() && entry.states->state() == *state))
        return false;
    if (!entry.states->preview(*state, now, 3000))
        return false;
    entry.lastPreviewAt = now;
    statesMoved(entry);
    return true;
}

// The window has the first frame of the file a change of state handed it (libmpv started, or the frames are on the GPU).
void OverlayController::finishSwitch(const QString &petId, Entry &entry)
{
    if (entry.switchStartedAt < 0)
        return;
    entry.switchMs = stateNow() - entry.switchStartedAt;
    entry.switchStartedAt = -1;
    if (entry.states) {
        entry.states->animationStarted(stateNow()); // released, a hold or a pass are counted from the first frame, not from the order to load the file
        scheduleStates();
    }
    // MURASAKI_DEBUG_STATES=1 logs every change of picture and how long it took (tools/bench/state_switch.sh reads this).
    static const bool debug = qEnvironmentVariableIsSet("MURASAKI_DEBUG_STATES");
    if (debug)
        qInfo("murasaki-overlay: states: pet %s switched to %s in %lld ms", qPrintable(petId), qPrintable(entry.placement.variant), static_cast<long long>(entry.switchMs));
}

void OverlayController::onPointerEvent(const QString &petId, const PetPointerEvent &event)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end())
        return;
    Entry &entry = it->second;
    const qint64 now = stateNow();
    // A pet that follows the cursor stands still while it is held; the drop is its place (the window's edit adopts it).
    if (entry.tracker && event.kind == PetPointerEvent::Kind::Grab)
        entry.tracker->hold(true, now);
    else if (entry.tracker && event.kind == PetPointerEvent::Kind::Release)
        entry.tracker->hold(false, now);
    if (!entry.states)
        return;
    switch (event.kind) {
    case PetPointerEvent::Kind::Grab:
        entry.dragPos = {};
        entry.states->press(now, entry.dragPos, true);
        break;
    case PetPointerEvent::Kind::DragMove:
        entry.dragPos.x += event.dx;
        entry.dragPos.y += event.dy;
        entry.states->motion(now, entry.dragPos);
        break;
    case PetPointerEvent::Kind::Release:
        entry.states->release(now);
        break;
    case PetPointerEvent::Kind::Dodge:
        entry.states->dodgeStep(now, event.durationMs);
        break;
    }
    deferStatesMoved(petId); // (the machine is fed now; what it shows is applied on the next turn, outside the window's callback)
}

void OverlayController::deferStatesMoved(const QString &petId)
{
    // A change of picture can end in dropping the pet's window (its file is blocked, its output is gone): that must not happen inside the window's own
    // callback. One queued turn is nothing against the 0.3 s a file takes to load.
    QTimer::singleShot(0, this, [this, petId] {
        if (const auto it = m_entries.find(petId); it != m_entries.end())
            statesMoved(it->second);
    });
}

void OverlayController::statesMoved(Entry &entry)
{
    if (!entry.states)
        return;
    if (entry.states->generation() != entry.stateGeneration) {
        const QString variantBefore = entry.placement.variant;
        const QString fileBefore = entry.file;
        const bool holdingBefore = entry.stateHolding;
        const int tokenBefore = entry.restartToken;
        refreshPlacement(entry);
        if (entry.placement.variant != variantBefore || entry.stateHolding != holdingBefore || entry.restartToken != tokenBefore) {
            reconcile(); // the window is handed the picture of the new state (it keeps showing the old one until the new one has its first frame)
            if (entry.file != fileBefore)
                entry.switchStartedAt = stateNow();
        } else {
            emit stateChanged(); // another state, the same picture: only Windows() changes
        }
    }
    scheduleStates();
}

void OverlayController::scheduleStates()
{
    std::optional<qint64> next;
    for (const auto &[petId, entry] : m_entries) {
        if (entry.states) {
            if (const auto wake = entry.states->nextWakeMs(); wake && (!next || *wake < *next))
                next = wake;
        }
        if (entry.tracker) {
            if (const auto wake = entry.tracker->nextWakeMs(); wake && (!next || *wake < *next))
                next = wake;
        }
    }
    if (!next) {
        m_stateTimer.stop();
        m_stateWake.reset();
        return;
    }
    if (m_stateTimer.isActive() && m_stateWake == next)
        return; // (a drag asks at every step: the timer is left alone when nothing changed)
    m_stateWake = next;
    m_stateTimer.start(int(std::clamp<qint64>(*next - stateNow(), 5, 3'600'000)));
}

void OverlayController::tickStates()
{
    const qint64 now = stateNow();
    QStringList ids;
    for (const auto &[petId, entry] : m_entries) {
        if (entry.states || entry.tracker)
            ids << petId;
    }
    for (const QString &petId : std::as_const(ids)) {
        const auto it = m_entries.find(petId);
        if (it == m_entries.end())
            continue;
        if (it->second.states) {
            it->second.states->tick(now);
            statesMoved(it->second);
        }
        if (const auto again = m_entries.find(petId); again != m_entries.end() && again->second.tracker) {
            again->second.tracker->tick(now);
            trackerMoved(again->second);
        }
    }
    scheduleStates();
}

// ---------------------------------------------------------------- the runtime layer

bool OverlayController::runtimeHidden(const QString &petId) const
{
    const auto it = m_runtime.constFind(petId);
    return it != m_runtime.constEnd() && it->hidden;
}

bool OverlayController::gameHidden(const PetPlacement &placement) const
{
    if (!m_petsHideOnGame || m_gameScreens.isEmpty() || placement.stage.active())
        return false;
    return m_gameScreens.contains(u"*"_s) || m_gameScreens.contains(effectiveOutput(placement));
}

PetPlacement OverlayController::withRuntime(const PetPlacement &stored) const
{
    const auto it = m_runtime.constFind(stored.petId);
    if (it == m_runtime.constEnd())
        return stored;
    PetPlacement shown = stored;
    if (!it->variant.isEmpty()) {
        // A variant the item does not have is not shown (the stored one stays).
        const auto item = m_items.constFind(stored.itemId);
        if (item != m_items.cend() && item->known) {
            for (const Variant &variant : item->variants) {
                if (variant.name == it->variant) {
                    shown.variant = it->variant;
                    break;
                }
            }
        }
    }
    if (it->opacity)
        shown.opacity = *it->opacity;
    // A pet in the fullscreen mode (a stage) fills its screens: where a timeline would put it means nothing, and the move is ignored (Windows() says so).
    if (stored.stage.active())
        return shown;
    if (!it->output.isEmpty() && (it->position || it->motion))
        shown.output = it->output;
    const PetPosition at = effectivePosition(*it, PetPosition{stored.x, stored.y}, monotonicMs());
    shown.x = at.x;
    shown.y = at.y;
    return shown;
}

// The pet's mouse tracking: the stored one, with the mode a timeline's `track` action put in the runtime layer on top of it.
PetTracking OverlayController::trackingOf(const PetPlacement &stored) const
{
    PetTracking tracking = stored.tracking;
    if (const auto it = m_runtime.constFind(stored.petId); it != m_runtime.constEnd() && it->tracking)
        tracking.mode = *it->tracking;
    return tracking;
}

void OverlayController::applyMute()
{
    if (m_audio)
        m_audio->setMuted(m_settingsMuted || m_sessionMuted);
}

void OverlayController::loadRuntime()
{
    const quint64 seq = ++m_runtimeSeq;
    daemonProperty(u"PetRuntime"_s, [this, seq](const DBusBackend::Reply &reply) {
        if (seq != m_runtimeSeq)
            return;
        // A daemon older than revision 18 has no such property: no layer.
        const QHash<QString, PetRuntime> next = reply.ok ? runtimeLayerFromJson(QJsonDocument::fromJson(reply.value.toString().toUtf8()).object()) : QHash<QString, PetRuntime>();
        if (next == m_runtime)
            return;
        QSet<QString> touched;
        for (auto it = next.cbegin(); it != next.cend(); ++it)
            if (m_runtime.value(it.key()) != it.value())
                touched.insert(it.key());
        for (auto it = m_runtime.cbegin(); it != m_runtime.cend(); ++it)
            if (!next.contains(it.key()))
                touched.insert(it.key());
        m_runtime = next;
        for (const QString &petId : std::as_const(touched)) {
            const auto entry = m_entries.find(petId);
            if (entry == m_entries.end())
                continue;
            refreshPlacement(entry->second);
            applyEntry(entry->second);
        }
        updateMotionTimer();
        emit stateChanged();
    });
}

void OverlayController::updateMotionTimer()
{
    const qint64 now = monotonicMs();
    bool moving = false;
    for (auto it = m_runtime.cbegin(); it != m_runtime.cend() && !moving; ++it)
        moving = it->motion && !motionDone(*it->motion, now);
    if (moving && !m_motionTimer.isActive())
        m_motionTimer.start();
    else if (!moving && m_motionTimer.isActive())
        m_motionTimer.stop();
}

// A motion is drawn by moving the pet's window every 33 ms while it lasts; the daemon sent it once and hears nothing back.
void OverlayController::onMotionTick()
{
    for (auto it = m_runtime.cbegin(); it != m_runtime.cend(); ++it) {
        if (!it->motion)
            continue;
        const auto entry = m_entries.find(it.key());
        if (entry == m_entries.end())
            continue;
        refreshPlacement(entry->second);
        applyEntry(entry->second);
    }
    updateMotionTimer();
    // (a motion that ended was applied at its end position by the tick above)
}

void OverlayController::pushAudio(const Entry &entry, bool shown)
{
    if (!m_audio)
        return;
    PetAudioController::Pet pet;
    pet.audio = entry.placement.audio;
    pet.clickThrough = entry.placement.clickThrough || entry.placement.stage.active(); // (a stage takes no click)
    pet.shown = shown && !m_petsHidden && !runtimeHidden(entry.placement.petId) && !gameHidden(entry.placement);
    pet.paused = entry.placement.paused || m_sessionPaused;
    pet.output = entry.output;
    if (const Variant *variant = variantOf(entry.placement))
        pet.frames = variant->frames;
    m_audio->setPet(entry.placement.petId, pet);
}

qint64 OverlayController::maskBudgetFor() const
{
    // An equal share of the budget for every pet that has a window to show (not the blocked ones, nor those whose output is gone); a pet
    // always keeps its union (about 2 KiB), which the builder adds beyond the share.
    qint64 pets = 0;
    for (const auto &[petId, entry] : m_entries) {
        if (!entry.blocked && m_outputs.contains(effectiveOutput(entry.placement)))
            ++pets;
    }
    pets = std::max<qint64>(1, pets);
    constexpr qint64 kUnion = 2048 + qint64(kMaskOverheadBytes);
    return std::max<qint64>(kUnion, m_maskBudget / pets);
}

void OverlayController::setMaskBudget(qint64 bytes)
{
    bytes = std::max<qint64>(0, bytes);
    if (bytes == m_maskBudget)
        return;
    m_maskBudget = bytes; // masks already made keep their size; the next decode or pass takes the new share
    reapplyAll();
}

// A pet that does not play from the frame cache has no pixels in the GUI: its masks come from one pass over the file it plays, in the
// background. Until they arrive the window takes the pet's whole rectangle (`hit mask: none`).
void OverlayController::requestMaskPass(Entry &entry, const PetView &view)
{
    const QString petId = entry.placement.petId;
    if (!m_maskLoader)
        return;
    const auto drop = [&] {
        if (!entry.passFile.isEmpty())
            m_maskLoader->cancel(petId);
        entry.passFile.clear();
        entry.passMasks.reset();
        entry.passPending = false;
    };
    // The masks are made only for a pet that has a use for them, and only for as long as it has: the default setup (click-through, no behaviour)
    // never starts a pass, never writes a mask file.
    if (!needsMasks(effectivePlacement(entry.placement)) || entry.spriteAdmitted) { // (the frames of a cached pet bring their own masks)
        drop();
        return;
    }
    if (entry.passFile == view.file)
        return; // already asked (or answered)
    if (!entry.passFile.isEmpty())
        drop(); // it was for another file: its job and its masks are of no use
    if (entry.startedFile != view.file)
        return; // the file does not play yet
    entry.passFile = view.file;
    entry.passPending = true;
    m_maskLoader->load(petId, entry.placement.itemId, entry.placement.variant, view.file, std::size_t(maskBudgetFor()),
                       [this, petId](const QString &file, std::shared_ptr<const MaskSet> masks) { onMaskPass(petId, file, std::move(masks)); });
}

void OverlayController::onMaskPass(const QString &petId, const QString &file, std::shared_ptr<const MaskSet> masks)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end() || it->second.passFile != file)
        return;
    Entry &entry = it->second;
    entry.passPending = false;
    entry.passMasks = std::move(masks);
    applyEntry(entry);
    emit stateChanged();
}

void OverlayController::reapplyAll()
{
    for (auto &[petId, entry] : m_entries) {
        if (entry.states || entry.tracker || trackingOf(entry.stored).active())
            refreshPlacement(entry); // (the pointer level decides whether a pet can track)
        if (entry.surface)
            applyEntry(entry);
    }
}

// ---- the pointer ------------------------------------------------------------------------------------------------------------

void OverlayController::setPointerSource(PointerSource *source)
{
    if (m_pointer)
        m_pointer->disconnect(this);
    m_pointer = source;
    m_pointerLevel = source ? source->level() : PointerSource::Level::Local;
    if (source) {
        connect(source, &PointerSource::moved, this, &OverlayController::onPointerMoved);
        connect(source, &PointerSource::unavailable, this, &OverlayController::onPointerUnavailable);
    }
    reapplyAll();
}

void OverlayController::updatePointerWanted()
{
    if (!m_pointer || m_pointerLevel != PointerSource::Level::Global)
        return;
    bool any = false;
    for (const auto &[petId, entry] : m_entries) {
        if ((entry.surface && entry.surface->wantsGlobalPointer()) || trackerRuns(entry)) {
            any = true;
            break;
        }
    }
    m_pointer->wanted(any); // (nothing runs in the compositor while no pet hides, fades, dodges or tracks)
}

void OverlayController::onPointerMoved(QPointF global)
{
    QStringList trackers;
    for (auto &[petId, entry] : m_entries) {
        if (entry.surface && entry.surface->wantsGlobalPointer())
            entry.surface->globalPointer(global);
        if (trackerRuns(entry))
            trackers << petId;
    }
    for (const QString &petId : std::as_const(trackers)) {
        if (const auto it = m_entries.find(petId); it != m_entries.end() && it->second.tracker) {
            it->second.tracker->cursor(stateNow(), cursorInOutput(it->second, global));
            trackerMoved(it->second);
        }
    }
}

void OverlayController::onPointerUnavailable(const QString &reason)
{
    qWarning("murasaki-overlay: the global pointer is unavailable (%s); the pets work at the local level", qPrintable(reason));
    if (m_pointer)
        m_pointer->wanted(false);
    m_pointerLevel = PointerSource::Level::Local;
    reapplyAll();
    emit stateChanged();
}

void OverlayController::setFrameCacheBudget(qint64 bytes)
{
    bytes = std::max<qint64>(0, bytes);
    if (bytes == m_spriteBudget)
        return;
    m_spriteBudget = bytes;
    reconcile();
}

// Decides which pets keep their frames in GPU memory. A pet's cost is what its frames take (frames x pixels x 4,
// from the item's header; the real size once they are in) and what it saves is the work the video path does for
// it: decoding and uploading `fps` frames of that many pixels a second. The pets that save most are admitted first,
// while the budget lasts; a pet that does not fit leaves room for smaller ones.
void OverlayController::planSprites()
{
    struct Candidate {
        QString petId;
        double saving;
        qint64 cost;
    };
    QList<Candidate> candidates;
    for (auto &[petId, entry] : m_entries) {
        entry.spriteAdmitted = false;
        entry.preloadAdmitted = false;
        entry.preloadKey.clear();
    }
    if (m_spriteBudget <= 0)
        return;
    // The size the frames of `variant` are decoded at for a placement, and the rate and number of frames that play; false when it can not be told (a stage that has no layout).
    const auto sizeOf = [this](const PetPlacement &placement, const PetPlacement &eff, const Variant *variant, const PetView &view, QSize *outSize, double *outFps, int *outFrames) -> bool {
        // Pixels the pet covers on its screen, never more than the file has.
        QSize shown = petDisplaySize(placement.width, variant->size);
        double scale = scaleOf(placement);
        if (eff.stage.active()) {
            // A stage draws the pet in its rectangle on the output (at the cap's fraction of the resolution).
            StageLayout layout;
            if (!buildStageLayout(placement, variant->size, &layout))
                return false;
            const StageSlice &slice = layout.slices.first();
            const QRectF rect = stagePetRect(layout, slice, QSizeF(variant->size), placement.rotation, m_geometry.value(slice.output));
            if (rect.isEmpty())
                return false;
            shown = QSize(std::max(1, int(std::lround(rect.width()))), std::max(1, int(std::lround(rect.height()))));
            scale = slice.scale * m_stageFactor;
        }
        QSizeF px(std::ceil(shown.width() * scale), std::ceil(shown.height() * scale));
        QSizeF file = variant->size;
        double fps = placement.fps > 0 ? placement.fps : variant->fps;
        int frames = variant->frames;
        if (view.bucket > 0 && placement.fps == 0) {
            const double side = std::min<double>(view.bucket, std::max(file.width(), file.height()));
            file *= side / std::max(file.width(), file.height());
            if (fps >= 50) { // the transcoder halves the rate of heavy sources to 30
                frames = std::max(1, int(std::ceil(frames * 30.0 / fps)));
                fps = 30;
            }
        }
        *outSize = QSize(std::max(1, int(std::lround(std::min(px.width(), file.width())))),
                         std::max(1, int(std::lround(std::min(px.height(), file.height())))));
        *outFps = fps;
        *outFrames = frames;
        return true;
    };
    // The picture of the state a pet with states is likely to enter next, when it is another one: what would be decoded ahead (the plan only; the admission is below).
    const auto planPreload = [&](Entry &entry, const PetPlacement &eff, const Variant *current, const auto &sizeFor) {
        entry.preloadFile.clear();
        entry.preloadEstimate = 0;
        if (!entry.states || eff.stage.active())
            return;
        const auto item = m_items.constFind(entry.placement.itemId);
        if (item == m_items.cend())
            return;
        for (const PetState state : entry.states->likelyNext()) {
            const std::optional<QString> name = entry.states->peekVariant(state);
            if (!name || *name == current->name)
                continue;
            const Variant *next = nullptr;
            for (const Variant &candidate : std::as_const(item->variants)) {
                if (candidate.name == *name)
                    next = &candidate;
            }
            if (!next || next->frames < 2 || next->fps <= 0 || !next->size.isValid())
                continue;
            PetPlacement placement = entry.placement;
            placement.variant = *name;
            const PetPlacement nextEff = effectivePlacement(placement);
            PetView nextView;
            if (!resolveView(nextEff, entry.badTranscode, &nextView))
                continue;
            QSize size;
            double fps = 0;
            int frames = 0;
            if (!sizeFor(placement, nextEff, next, nextView, &size, &fps, &frames))
                continue;
            const QString key = spriteKeyFor(nextView.file, size, entry.masksWanted);
            if (key == entry.preloadFailedKey || key == entry.spritePlanKey)
                continue;
            entry.preloadFile = nextView.file;
            entry.preloadSize = size;
            entry.preloadKey = key;
            entry.preloadEstimate = qint64(frames) * size.width() * size.height() * 4;
            return;
        }
    };
    for (const QString &petId : std::as_const(m_order)) {
        const auto it = m_entries.find(petId);
        if (it == m_entries.end())
            continue;
        Entry &entry = it->second;
        PetView view;
        const Variant *variant = variantOf(entry.placement);
        const PetPlacement eff = effectivePlacement(entry.placement);
        if (!variant || entry.blocked || entry.placement.playOriginal || eff.stage.mode == StageMode::Desktop
            || !resolveView(eff, entry.badTranscode, &view))
            continue; // (the windows of a `desktop` stage play through libmpv and are kept together by their position: docs/pets/stage.md)
        if (variant->frames < 2 || variant->fps <= 0 || !variant->size.isValid())
            continue; // a still image has nothing to cache
        if (!m_outputs.contains(effectiveOutput(entry.placement)))
            continue;
        QSize size;
        double fps = 0;
        int frames = 0;
        if (!sizeOf(entry.placement, eff, variant, view, &size, &fps, &frames))
            continue;
        if (needsMasks(effectivePlacement(entry.placement)))
            entry.masksWanted = true;
        const QString key = spriteKeyFor(view.file, size, entry.masksWanted);
        if (key == entry.spriteFailedKey)
            continue;
        entry.spriteSize = size;
        entry.spritePlanKey = key;
        // The whole frames, or what the same frames took once cut when this overlay has decoded them before (never more than the whole ones).
        qint64 estimate = qint64(frames) * size.width() * size.height() * 4;
        if (const qint64 known = m_croppedBytes.value(key, 0); known > 0)
            estimate = std::min(estimate, known);
        // Frames that are on screen at this size cost what they really take.
        const bool built = entry.spriteBytes > 0 && entry.spriteKey == key;
        // While frames of another size are being replaced both sets are in memory.
        entry.spriteCost = built ? entry.spriteBytes : estimate + entry.spriteBytes;
        entry.spriteOwn = built ? entry.spriteBytes : estimate;
        candidates.append({petId, fps * size.width() * size.height(), entry.spriteCost});
        planPreload(entry, eff, variant, sizeOf);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.saving > b.saving; });
    qint64 used = 0;
    QList<Candidate> admitted;
    for (const Candidate &candidate : std::as_const(candidates)) {
        if (used + candidate.cost > m_spriteBudget)
            continue;
        used += candidate.cost;
        admitted.append(candidate);
    }
    for (const Candidate &candidate : std::as_const(admitted)) {
        Entry &entry = m_entries.at(candidate.petId);
        entry.spriteAdmitted = true;
        // A decode may take a quarter more than the header promised, and never more than what is left once the
        // others (and the frames this pet still shows) have theirs.
        const qint64 left = m_spriteBudget - (used - candidate.cost) - (candidate.cost - entry.spriteOwn);
        entry.spriteAllowance = std::min(left, entry.spriteOwn + entry.spriteOwn / 4);
    }
    // The pictures prepared for the next state come after every current picture has its place, in the same order: one that does not fit is not prepared.
    for (const Candidate &candidate : std::as_const(admitted)) {
        Entry &entry = m_entries.at(candidate.petId);
        if (entry.preloadKey.isEmpty())
            continue;
        const bool ready = entry.preloadBytes > 0 && entry.preloadReadyKey == entry.preloadKey;
        const qint64 cost = ready ? entry.preloadBytes : entry.preloadEstimate;
        if (used + cost > m_spriteBudget)
            continue;
        used += cost;
        entry.preloadAdmitted = true;
        entry.preloadAllowance = std::min(m_spriteBudget - (used - cost), cost + cost / 4);
    }
}

void OverlayController::reconcile()
{
    if (!m_compatible)
        m_order.clear();
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (m_order.contains(it->first)) {
            ++it;
            continue;
        }
        // The placement is gone: so is what is known against its file, unless another pet still plays it.
        const QString petId = it->first;
        const QString file = it->second.file;
        const QString blockedFile = it->second.blockedFile;
        dropSurface(petId, it->second);
        if (m_audio)
            m_audio->removePet(petId);
        it = m_entries.erase(it);
        const bool shared = std::any_of(m_entries.cbegin(), m_entries.cend(),
                                        [&file](const auto &other) { return other.second.file == file; });
        if (m_ledger && !shared) {
            // A pet blocked at first sight never opened a window, so only blockedFile names its file.
            if (!file.isEmpty())
                m_ledger->forget(file);
            if (!blockedFile.isEmpty())
                m_ledger->forget(blockedFile);
        }
    }
    m_stageFactor = computeStageFactor();
    for (auto &[petId, entry] : m_entries) {
        if (entry.states || entry.tracker || trackingOf(entry.stored).active())
            refreshPlacement(entry); // the arrange mode, a pause or a hide put the pet back to idle; an item that arrived says which variants exist
    }
    planSprites();
    for (const QString &petId : std::as_const(m_order)) {
        if (const auto it = m_entries.find(petId); it != m_entries.end())
            applyEntry(it->second);
    }
    // The windows of a `desktop` stage are kept together once a second, only while there is such a stage.
    const bool desktop = std::any_of(m_entries.cbegin(), m_entries.cend(),
                                     [](const auto &e) { return e.second.placement.stage.mode == StageMode::Desktop && !e.second.stageWindows.empty(); });
    if (desktop && !m_stageSync.isActive())
        m_stageSync.start();
    else if (!desktop)
        m_stageSync.stop();
    updatePointerWanted();
    scheduleStates();
    emit stateChanged();
}

void OverlayController::setGameScreens(const QSet<QString> &outputs)
{
    if (outputs == m_gameScreens)
        return;
    m_gameScreens = outputs;
    if (m_petsHideOnGame)
        reconcile();
}

void OverlayController::setOutputFullscreen(const QHash<QString, QString> &apps, bool known)
{
    const bool first = !m_fullscreenAnswered;
    m_fullscreenAnswered = true;
    if (apps == m_fullscreen && known == m_fullscreenKnown && !first)
        return;
    m_fullscreen = apps;
    m_fullscreenKnown = known;
    if (anyStage()) {
        reconcile();
    } else {
        for (auto &[petId, entry] : m_entries) {
            if (entry.tracker) // a follower parks over a fullscreen window and walks again when it is gone
                refreshPlacement(entry);
        }
        updatePointerWanted(); // (a follower that parks stops asking for the cursor)
        scheduleStates();
        emit stateChanged();
    }
}

void OverlayController::setStageCompositing(bool available)
{
    if (available == m_stageCompositing)
        return;
    m_stageCompositing = available;
    if (anyStage())
        reconcile();
}

bool OverlayController::anyStage() const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [](const auto &entry) { return entry.second.placement.stage.active(); });
}

QStringList OverlayController::stageOutputsOf(const Entry &entry) const
{
    if (entry.placement.stage.mode == StageMode::Desktop)
        return m_outputs;
    return {effectiveOutput(entry.placement)};
}

// One stage per output, and `desktop` takes them all: the first in the daemon's order keeps its outputs (a "*" and the primary's own name can only be told
// apart here, the store compares what the placements say).
bool OverlayController::stageTaken(const Entry &entry) const
{
    const QStringList mine = stageOutputsOf(entry);
    for (const QString &otherId : std::as_const(m_order)) {
        if (otherId == entry.placement.petId)
            return false;
        const auto other = m_entries.find(otherId);
        if (other == m_entries.end() || !other->second.placement.stage.active())
            continue;
        for (const QString &name : stageOutputsOf(other->second)) {
            if (mine.contains(name))
                return true;
        }
    }
    return false;
}

PetPlacement OverlayController::effectivePlacement(const PetPlacement &placement) const
{
    if (!placement.stage.active())
        return placement;
    const Variant *variant = variantOf(placement);
    StageLayout layout;
    if (variant && !buildStageLayout(placement, variant->size, &layout)) {
        PetPlacement pet = placement;
        pet.stage = PetStage{};
        return pet;
    }
    return placement; // (an unknown item shows nothing anyway)
}

QList<StageOutput> OverlayController::stageOutputList() const
{
    QList<StageOutput> outputs;
    for (const QString &name : std::as_const(m_outputs)) {
        if (const auto geometry = m_geometry.constFind(name); geometry != m_geometry.cend())
            outputs.append({name, *geometry, m_scales.value(name, 1.0), name == m_primary});
    }
    return outputs;
}

StageRequest OverlayController::stageRequestOf(const PetPlacement &placement, QSize native) const
{
    StageRequest request;
    request.mode = placement.stage.mode;
    request.fit = placement.stage.fit;
    request.output = placement.output; // ("*" is resolved by the shared function, with the primary the outputs say)
    request.rotation = placement.rotation;
    request.native = native;
    return request;
}

bool OverlayController::buildStageLayout(const PetPlacement &placement, QSize native, StageLayout *layout) const
{
    if (!m_stageCompositing) {
        *layout = StageLayout{};
        return false; // (the pet is drawn as a pet: stageLayoutFor says why)
    }
    *layout = stageLayoutOf(stageOutputList(), stageRequestOf(placement, native));
    return !layout->isEmpty();
}

int OverlayController::stageNeedFor(const PetPlacement &placement, QSize native, int sourceLongest) const
{
    return stageRequestNeed(stageOutputList(), stageRequestOf(placement, native), m_stageFactor, sourceLongest);
}

// All the stages together draw at most stageMaxPixels device pixels: the one factor that brings their sum within it (the daemon computes the same one to size the copies).
double OverlayController::computeStageFactor() const
{
    QList<StageRequest> requests;
    for (const QString &petId : std::as_const(m_order)) {
        const auto it = m_entries.find(petId);
        if (it == m_entries.end() || !it->second.placement.stage.active() || it->second.blocked || stageTaken(it->second))
            continue;
        if (const Variant *variant = variantOf(it->second.placement))
            requests.append(stageRequestOf(it->second.placement, variant->size));
    }
    return stageResolutionFactor(stageRequestPixels(stageOutputList(), requests), m_stageMaxPixels);
}

void OverlayController::syncStageVideo()
{
    // The record of the seeks only has to outlive the cooldown: what is older (and the windows of pets that are gone) is forgotten.
    if (m_syncClock.isValid()) {
        for (auto it = m_lastSeek.begin(); it != m_lastSeek.end();) {
            if (m_syncClock.elapsed() - it.value() > 10 * kStageSeekCooldownMs)
                it = m_lastSeek.erase(it);
            else
                ++it;
        }
    }
    for (auto &[petId, entry] : m_entries) {
        if (entry.placement.stage.mode != StageMode::Desktop || !entry.surface || entry.stageWindows.empty())
            continue;
        struct Window {
            QString output;
            PetSurface *surface;
        };
        QList<Window> windows{{entry.output, entry.surface.get()}};
        for (Entry::StageWindow &window : entry.stageWindows)
            windows.append({window.output, window.surface.get()});
        // The reference is the first by name among the windows that were playing at the last round: one that comes back from a fullscreen window has lost the time it was
        // paused and must follow the others, not lead them.
        std::sort(windows.begin(), windows.end(), [&entry](const Window &a, const Window &b) {
            const bool pa = entry.syncPlaying.contains(a.output);
            const bool pb = entry.syncPlaying.contains(b.output);
            return pa != pb ? pa : a.output < b.output;
        });
        // Positions on the circle of the loop: two windows on either side of the wrap (0.02 s and 3.98 s of a 4 s loop) are 0.04 s apart, not 3.96. A window about to
        // wrap (within kStageLoopEndMargin) is left out of the round: a seek there lands on the wrap and the next round would chase it.
        QList<SpanPlayer> players;
        QSet<QString> playing;
        double reference = -1;
        double loop = 0;
        for (const Window &window : std::as_const(windows)) {
            double position = window.surface->videoPosition();
            const double duration = window.surface->videoDuration();
            if (position >= 0 && duration > 0 && position > duration - std::min(kStageLoopEndMargin, duration * 0.1))
                position = -1;
            const bool has = position >= 0;
            if (has) {
                if (reference < 0) {
                    reference = position;
                } else if (duration > 0) {
                    double delta = position - reference;
                    delta -= duration * std::round(delta / duration);
                    position = reference + delta;
                }
            }
            // (a position put on the circle can fall below 0: every position is shifted by one loop, which a negative number could not say, and the shift is taken off the target)
            players.append({window.output, has ? position + (duration > 0 ? duration : 0) : -1});
            if (has)
                playing.insert(window.output);
            if (duration > 0)
                loop = std::max(loop, duration);
        }
        entry.syncPlaying = playing;
        for (const SpanCorrection &fix : spanVideoCorrections(players, kStageSyncToleranceSeconds)) {
            for (const Window &window : std::as_const(windows)) {
                if (window.output != fix.output)
                    continue;
                // A seek takes a moment to settle: the window that was seeked lately is not judged again until it has (corrections computed on a position in the middle of a seek
                // are what makes a rule chase itself). The seek is exact: the copies have sparse keyframes, a fast seek would land seconds away.
                const QString key = petId + u'/' + fix.output;
                if (m_lastSeek.contains(key) && m_syncClock.isValid() && m_syncClock.elapsed() - m_lastSeek.value(key) < kStageSeekCooldownMs)
                    continue;
                if (!m_syncClock.isValid())
                    m_syncClock.start();
                m_lastSeek.insert(key, m_syncClock.elapsed());
                double target = fix.seconds;
                if (loop > 0)
                    target = std::fmod(target - loop + 2 * loop, loop); // (back on the line of the file, 0 up to its length)
                // The file goes on playing while an exact seek decodes from the last keyframe (a tenth of a second or more on a 4K copy, measured with
                // tools/stage-probe/seeklatency.cpp): it lands that late, so it is aimed that far ahead, with what the window measured of its last seek.
                const double measured = window.surface->videoSeekLatency();
                const double latency = measured >= 0 ? std::min(measured, kStageMaxSeekLatency) : kStageSeekLatencyGuess;
                target += latency;
                if (loop > 0)
                    target = std::fmod(target, loop);
                window.surface->seekVideo(target);
            }
        }
    }
}

bool OverlayController::stageLayoutFor(Entry &entry, const PetView &view, StageLayout *layout)
{
    const bool built = buildStageLayout(entry.placement, view.native, layout);
    entry.stageDegraded = layout->degraded;
    if (!built) {
        // The session did not tell where the outputs are (or the output is gone): an ordinary pet, with the name of what it is not.
        // Which reason: the output is not there, it is too large, or the session did not say where the outputs are.
        std::string_view reason = stagedegrade::kNotFullscreen;
        const QString wanted = effectiveOutput(entry.placement);
        const QRect wantedRect = m_geometry.value(wanted);
        const bool single = entry.placement.stage.mode == StageMode::Output;
        if (!m_stageCompositing)
            reason = stagedegrade::kNeedsCompositor;
        else if (single && !m_outputs.contains(wanted))
            reason = stagedegrade::kOutputMissing;
        else if (single && (wantedRect.width() > kMaxStageSide || wantedRect.height() > kMaxStageSide))
            reason = stagedegrade::kOutputTooLarge;
        const QString name = degradeText(reason);
        if (!entry.stageDegraded.contains(name))
            entry.stageDegraded.append(name);
        return false;
    }
    // Whether the session can say that a window is fullscreen (when the stage would hide for it): otherwise it is shown, and says so.
    const PetStage &stage = entry.placement.stage;
    const bool hides = stage.overFullscreen == StageOverFullscreen::Hide || (stage.overFullscreen == StageOverFullscreen::Inherit && m_stageHideOverFullscreen);
    if (hides && m_fullscreenAnswered && !m_fullscreenKnown)
        entry.stageDegraded.append(degradeText(stagedegrade::kFullscreenUnknown));
    return true;
}

bool OverlayController::hiddenByFullscreen(const Entry &entry, const QString &output) const
{
    const PetStage &stage = entry.placement.stage;
    const bool hides = stage.overFullscreen == StageOverFullscreen::Hide || (stage.overFullscreen == StageOverFullscreen::Inherit && m_stageHideOverFullscreen);
    // Until the surface has said anything (or is known to be absent) a stage that would hide waits: it must not show over a game for the length of the first read.
    return hides && (!m_fullscreenAnswered || m_fullscreen.contains(output) || m_fullscreen.contains(u"*"_s));
}

void OverlayController::setOutputs(const QStringList &names, const QString &primary, const QHash<QString, double> &scales, const QHash<QString, QRect> &geometry)
{
    QHash<QString, double> clamped;
    for (auto it = scales.cbegin(); it != scales.cend(); ++it) {
        if (std::isfinite(it.value()))
            clamped.insert(it.key(), std::clamp(it.value(), kMinOutputScale, kMaxOutputScale));
    }
    if (names == m_outputs && primary == m_primary && clamped == m_scales && geometry == m_geometry)
        return;
    const bool scalesChanged = clamped != m_scales || names != m_outputs || geometry != m_geometry || primary != m_primary;
    m_geometry = geometry;
    m_outputs = names;
    m_primary = primary;
    m_scales = clamped;
    if (scalesChanged && m_available && m_compatible)
        sendOutputScales();
    reconcile();
}

double OverlayController::scaleOf(const PetPlacement &placement) const
{
    return m_scales.value(effectiveOutput(placement), 1.0);
}

void OverlayController::sendOutputScales()
{
    if (m_apiVersion < kOutputScalesDBusApiVersion)
        return; // a revision 5 daemon sizes every transcode for the biggest pet
    // An output is its scale; from revision 17 also where it is, which sizes the copy of a stage (the daemon does not know the layout).
    const auto outputInfo = [this](const QString &name) -> QJsonValue {
        const double scale = m_scales.value(name, 1.0);
        const QRect rect = m_geometry.value(name);
        if (m_apiVersion < kPetStageDBusApiVersion || rect.isEmpty() || rect.width() > kMaxStageSide || rect.height() > kMaxStageSide)
            return scale;
        return QJsonObject{{u"scale"_s, scale}, {u"primary"_s, name == m_primary}, {u"x"_s, rect.x()}, {u"y"_s, rect.y()}, {u"width"_s, rect.width()}, {u"height"_s, rect.height()}};
    };
    QJsonObject scales;
    for (const QString &name : std::as_const(m_outputs)) {
        if (scales.size() >= kMaxScaledOutputs)
            break;
        if (!name.isEmpty() && name.size() <= 200)
            scales.insert(name, outputInfo(name));
    }
    daemonCall(u"SetOutputScales"_s, {QString::fromUtf8(QJsonDocument(scales).toJson(QJsonDocument::Compact))},
                   [](const DBusBackend::Reply &reply) {
                       if (!reply.ok)
                           qWarning("murasaki-overlay: SetOutputScales: %s", qPrintable(reply.message));
                   });
}

void OverlayController::setSessionPaused(bool paused)
{
    if (paused == m_sessionPaused)
        return;
    m_sessionPaused = paused;
    reconcile();
}

bool OverlayController::resetFailures(const QString &petId)
{
    const auto it = m_entries.find(petId);
    if (it == m_entries.end())
        return false;
    Entry &entry = it->second;
    if (m_ledger) {
        m_ledger->forget(entry.blockedFile);
        m_ledger->forget(entry.file);
    }
    entry.failures = 0;
    entry.blocked = false;
    entry.blockedFile.clear();
    entry.badTranscode.clear();
    entry.spriteFailedKey.clear();
    entry.lastError.clear();
    reconcile();
    return true;
}

void OverlayController::toggleArrange()
{
    if (!m_available || !m_compatible)
        return;
    daemonCall(u"SetPetsArrange"_s, {!m_arrange}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki-overlay: SetPetsArrange: %s", qPrintable(reply.message));
    });
}

void OverlayController::toggleHidden()
{
    if (!m_available || !m_compatible) {
        qWarning("murasaki-overlay: hide/show all: the daemon is not there (or too old), nothing was done");
        return;
    }
    daemonCall(u"SetPetsHidden"_s, {!m_petsHidden}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki-overlay: SetPetsHidden: %s", qPrintable(reply.message));
    });
}

void OverlayController::rescue()
{
    if (!m_available || !m_compatible) {
        qWarning("murasaki-overlay: rescue: the daemon is not there (or too old), nothing was done; "
                 "start murasakid, or set \"clickThrough\": false in pets.json by hand");
        return;
    }
    daemonCall(u"RescuePets"_s, {}, [](const DBusBackend::Reply &reply) {
        if (!reply.ok)
            qWarning("murasaki-overlay: RescuePets: %s", qPrintable(reply.message));
    });
}

void OverlayController::onShortcut(const QString &actionId)
{
    if (actionId == u"toggle-arrange"_s)
        toggleArrange();
    else if (actionId == u"toggle-hidden"_s)
        toggleHidden();
    else if (actionId == u"rescue"_s)
        rescue();
}

bool OverlayController::simulateEdit(const QString &petId, const QString &kind, double a, double b)
{
    const auto it = m_entries.find(petId);
    return it != m_entries.end() && it->second.surface && it->second.surface->simulateEdit(kind, a, b);
}

QJsonArray OverlayController::describe() const
{
    QJsonArray out;
    for (const QString &petId : m_order) {
        const auto it = m_entries.find(petId);
        if (it == m_entries.cend())
            continue;
        const Entry &entry = it->second;
        QJsonObject object{{u"petId"_s, petId},
                           {u"itemId"_s, entry.placement.itemId},
                           {u"output"_s, effectiveOutput(entry.placement)},
                           {u"shown"_s, entry.surface != nullptr},
                           {u"paused"_s, entry.lastView.paused},
                           {u"file"_s, entry.file},
                           {u"bucket"_s, entry.bucket},
                           {u"fps"_s, entry.placement.fps},
                           {u"speed"_s, entry.lastView.speed},
                           {u"timing"_s, timingModeName(entry.placement.timing().mode())},
                           {u"timingSpeed"_s, entry.placement.speed},
                           {u"fpsCap"_s, entry.lastView.fpsCap},
                           {u"mode"_s, entry.spriteAdmitted && entry.spriteBytes > 0 ? u"sprite"_s : u"video"_s},
                           {u"cacheBytes"_s, entry.spriteAdmitted ? entry.spriteBytes : 0},
                           {u"failures"_s, entry.failures},
                           {u"blocked"_s, entry.blocked}};
        if (entry.blocked)
            object.insert(u"error"_s, entry.lastError);
        if (isPuppet(entry.placement.itemId)) {
            // (only a puppet pet has this key: Windows() of any other pet is what it was)
            object.insert(u"puppet"_s, QJsonObject{{u"state"_s, entry.puppetState.isEmpty() ? u"idle"_s : entry.puppetState},
                                                   {u"fps"_s, entry.lastView.puppet.fps},
                                                   {u"crashes"_s, m_ledger ? m_ledger->crashes(entry.file.isEmpty() ? entry.blockedFile : entry.file) : 0}, // (a blocked pet has no file shown: the one it was blocked on)
                                                   {u"group"_s, entry.puppetGroup},
                                                   {u"expression"_s, entry.puppetExpression},
                                                   {u"look"_s, entry.puppetLook}});
        }
        if (entry.stored.stage.active() && m_runtime.contains(petId) && (m_runtime.value(petId).position || m_runtime.value(petId).motion))
            object.insert(u"runtimeMoveIgnored"_s, true); // a stage fills its screens: a timeline can hide it, not move it
        if (runtimeHidden(petId))
            object.insert(u"runtimeHidden"_s, true); // hidden by a timeline or a trigger (ClearPetRuntime or the rescue brings it back)
        if (gameHidden(entry.placement))
            object.insert(u"gameHidden"_s, true); // on a game's screen while it plays (the setting petsHideOnGame)
        // What the window does less than asked, by the names of docs/pets/behaviours.md (keep above off on layer-shell and GNOME).
        // and what the pointer input does less (pointer: local, hit mask: coarse, ...): one list.
        QStringList degraded;
        const PetInputStatus input = entry.surface ? entry.surface->inputStatus() : PetInputStatus();
        if (entry.surface) {
            degraded = entry.surface->degraded();
            for (const std::string_view name : input.degraded) {
                const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
                if (!degraded.contains(text))
                    degraded.append(text);
            }
        }
        for (const QString &name : entry.puppetDegraded) {
            if (!degraded.contains(name))
                degraded.append(name);
        }
        if (entry.placement.stage.active()) {
            // (only a pet that is drawn as a stage has these keys: Windows() of any other pet, a stage that is shown as a pet included, is what it was)
            if (entry.surface && entry.lastView.stage.active) {
                object.insert(u"stage"_s, QLatin1StringView(stageModeName(entry.placement.stage.mode)));
                object.insert(u"stageOutputs"_s, QJsonArray::fromStringList(entry.stageOutputs));
                object.insert(u"stageHidden"_s, entry.stageHidden);
                object.insert(u"stageScale"_s, entry.stageScale);
            }
            for (const QString &name : std::as_const(entry.stageDegraded)) {
                if (!degraded.contains(name))
                    degraded.append(name);
            }
        }
        if (entry.states) {
            // (only a pet that has states has these keys: Windows() of any other pet is what it was)
            object.insert(u"state"_s, QLatin1StringView(petStateName(entry.states->state())));
            // (a variant of the runtime layer is shown instead of the state's: the pet says so, not the state's own source)
            object.insert(u"stateSource"_s, entry.placement.variant != entry.states->variant() ? u"layer"_s : entry.states->source());
            object.insert(u"preloadBytes"_s, entry.preloadAdmitted ? entry.preloadBytes : 0); // the picture prepared for the next state, on the frame cache
            const auto add = [&degraded](std::string_view name) {
                const QString text = degradeText(name);
                if (!degraded.contains(text))
                    degraded.append(text);
            };
            if (entry.states->missingVariant())
                add(statesdegrade::kMissingVariant);
            if (const auto item = m_items.constFind(entry.placement.itemId); item != m_items.cend()) {
                for (const Variant &variant : std::as_const(item->variants)) {
                    if (variant.copyPending && variant.doneFiles.isEmpty() && entry.stored.usesVariant(variant.name))
                        add(statesdegrade::kTranscodePending);
                }
            }
            if (entry.switchMs > kStateSwitchSlowMs)
                add(statesdegrade::kSwitchingSlow);
            if (m_spriteBudget > 0 && !entry.spriteAdmitted && entry.surface && !entry.spritePlanKey.isEmpty()) { // (a plan has run and the pet lost)
                if (const Variant *shown = variantOf(entry.placement); shown && shown->frames >= 2)
                    add(statesdegrade::kOverBudget);
            }
        }
        if (!trackingOf(entry.stored).isDefault()) {
            // (only a pet that tracks, or did and was told to stop, has these keys)
            object.insert(u"tracking"_s, QLatin1StringView(petTrackingModeName(trackingOf(entry.stored).mode)));
            object.insert(u"trackingActive"_s, trackerRuns(entry));
            const auto add = [&degraded](std::string_view name) {
                const QString text = degradeText(name);
                if (!degraded.contains(text))
                    degraded.append(text);
            };
            if (trackingOf(entry.stored).active() && !entry.tracker && !entry.stored.stage.active())
                add(statesdegrade::kNeedsGlobalPointer);
            if (entry.tracker && entry.tracker->parked())
                add(statesdegrade::kParkedOverFullscreen);
            if (entry.tracker && trackingOf(entry.stored).mode == PetTrackingMode::Follow && entry.tracker->moving())
                add(statesdegrade::kMovesLimited);
            if (entry.tracker && trackingOf(entry.stored).mode == PetTrackingMode::Follow && entry.lastView.stage.active && entry.placement.stage.mode == StageMode::Desktop)
                add(statesdegrade::kStageFollowOneScreen);
        }
        object.insert(u"degraded"_s, QJsonArray::fromStringList(degraded));
        if (m_audio) {
            const QJsonObject audio = m_audio->status(entry.placement.petId);
            if (!audio.isEmpty())
                object.insert(u"audio"_s, audio);
        }
        if (entry.surface) {
            const QRectF geometry = entry.surface->geometry();
            object.insert(u"x"_s, geometry.x());
            object.insert(u"y"_s, geometry.y());
            object.insert(u"width"_s, geometry.width());
            object.insert(u"height"_s, geometry.height());
            object.insert(u"interactive"_s, entry.surface->inputEnabled());
            if (!input.pointer.isEmpty()) {
                object.insert(u"pointer"_s, input.pointer);
                object.insert(u"hitMask"_s, input.hitMask);
                object.insert(u"maskBytes"_s, input.maskBytes);
                object.insert(u"hidden"_s, input.hidden);
            }
        }
        out.append(object);
    }
    return out;
}

} // namespace murasaki
