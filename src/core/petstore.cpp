#include "petstore.h"

#include "assignment.h"
#include "jsonfile.h"
#include "petlimits.h"

#include <QJsonArray>
#include <QSet>
#include <QUuid>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

QJsonObject PetPlacement::toJson() const
{
    QJsonObject o{
        {u"petId"_s, petId},
        {u"itemId"_s, itemId},
        {u"variant"_s, variant},
        {u"output"_s, output},
        {u"x"_s, x},
        {u"y"_s, y},
        {u"width"_s, width},
        {u"opacity"_s, opacity},
        {u"playOriginal"_s, playOriginal},
        {u"paused"_s, paused},
        {u"fps"_s, fps},
        {u"speed"_s, speed},
        {u"clickThrough"_s, clickThrough},
        {u"locked"_s, locked},
        {u"keepAbove"_s, keepAbove},
        {u"hoverHide"_s, hoverHide},
        {u"focusFade"_s, focusFade},
        {u"smartDodge"_s, smartDodge},
        {u"rotation"_s, rotation},
        {u"flipH"_s, flipH},
        {u"flipV"_s, flipV},
    };
    if (!audio.isDefault())
        o.insert(u"audio"_s, audio.toJson());
    if (!stage.isDefault())
        o.insert(u"stage"_s, stage.toJson());
    if (!states.isDefault())
        o.insert(u"states"_s, states.toJson());
    if (!tracking.isDefault())
        o.insert(u"tracking"_s, tracking.toJson());
    return o;
}

QStringList PetPlacement::variantsUsed() const
{
    QStringList names{variant};
    for (const QString &name : states.variants()) {
        if (!names.contains(name))
            names.append(name);
    }
    return names;
}

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool inRange(double value, double low, double high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

} // namespace

double normalizedPetRotation(double degrees)
{
    if (!std::isfinite(degrees))
        return 0;
    double r = std::fmod(degrees, kPetRotationTurn);
    if (r < 0)
        r += kPetRotationTurn;
    r = std::round(r * 100.0) / 100.0;
    return r >= kPetRotationTurn ? 0.0 : r;
}

int defaultPetFps(const QString &)
{
    return kDefaultPetFps;
}

double playbackSpeed(int fps, double nativeFps)
{
    if (fps <= 0 || !std::isfinite(nativeFps) || nativeFps <= 0)
        return 1;
    return fps / nativeFps;
}

bool validatePlacement(const PetPlacement &p, QString *error)
{
    if (p.itemId.isEmpty() || p.itemId.size() > 200)
        return fail(error, u"itemId must be 1-200 characters"_s);
    if (p.variant.isEmpty() || p.variant.size() > 200)
        return fail(error, u"variant must be 1-200 characters"_s);
    if (!isValidKeyPart(p.output))
        return fail(error, u"output must be 1-200 characters"_s);
    if (!inRange(p.x, -kMaxPetCoordinate, kMaxPetCoordinate) || !inRange(p.y, -kMaxPetCoordinate, kMaxPetCoordinate))
        return fail(error, u"x and y must be within +-100000"_s);
    if (!inRange(p.width, kMinPetWidth, kMaxPetWidth))
        return fail(error, u"width must be between 16 and 16384"_s);
    if (!inRange(p.opacity, kMinPetOpacity, 1.0))
        return fail(error, u"opacity must be between 0.1 and 1"_s);
    if (p.fps < 0 || p.fps > kMaxPetFps)
        return fail(error, u"fps must be 0 (not a fixed rate) or between 1 and %1"_s.arg(kMaxPetFps));
    if (!inRange(p.speed, kMinPetSpeed, kMaxPetSpeed))
        return fail(error, u"speed must be between %1 and %2"_s.arg(kMinPetSpeed).arg(kMaxPetSpeed));
    if (p.hoverHide && p.focusFade)
        return fail(error, u"hoverHide and focusFade exclude each other"_s);
    if (!std::isfinite(p.rotation) || p.rotation < 0 || p.rotation >= kPetRotationTurn)
        return fail(error, u"rotation must be a number of degrees from 0 up to (not including) 360; a request is wrapped first, and a number beyond 1e9 in size is refused"_s);
    if (const QString problem = validatePetAudio(p.audio); !problem.isEmpty())
        return fail(error, problem);
    if (p.fps > 0 && p.speed != 1)
        return fail(error, u"a fixed fps and a speed other than 1 can not be used together"_s);
    if (p.tracking.mode == PetTrackingMode::Follow && (p.smartDodge || p.hoverHide || p.focusFade))
        return fail(error, u"tracking follow excludes smartDodge, hoverHide and focusFade (a pet that follows the cursor can not also run from it or hide under it)"_s);
    return true;
}

std::optional<PetPlacement> applyPlacementChanges(const PetPlacement &base, const QJsonObject &changes,
                                                  QString *error)
{
    PetPlacement p = base;
    const auto number = [&](const QString &key, double *target) {
        const auto it = changes.constFind(key);
        if (it == changes.constEnd())
            return true;
        if (!it->isDouble())
            return fail(error, key + u" must be a number"_s);
        *target = it->toDouble();
        return true;
    };
    const auto boolean = [&](const QString &key, bool *target) {
        const auto it = changes.constFind(key);
        if (it == changes.constEnd())
            return true;
        if (!it->isBool())
            return fail(error, key + u" must be a boolean"_s);
        *target = it->toBool();
        return true;
    };
    const auto text = [&](const QString &key, QString *target) {
        const auto it = changes.constFind(key);
        if (it == changes.constEnd())
            return true;
        if (!it->isString())
            return fail(error, key + u" must be a string"_s);
        *target = it->toString();
        return true;
    };
    const auto integer = [&](const QString &key, int *target) {
        const auto it = changes.constFind(key);
        if (it == changes.constEnd())
            return true;
        const double value = it->toDouble();
        if (!it->isDouble() || !std::isfinite(value) || value != std::floor(value) || std::abs(value) > 1e6)
            return fail(error, key + u" must be an integer"_s);
        *target = int(value);
        return true;
    };
    if (!text(u"variant"_s, &p.variant) || !text(u"output"_s, &p.output) || !number(u"x"_s, &p.x)
        || !number(u"y"_s, &p.y) || !number(u"width"_s, &p.width) || !number(u"opacity"_s, &p.opacity)
        || !boolean(u"playOriginal"_s, &p.playOriginal) || !boolean(u"paused"_s, &p.paused)
        || !integer(u"fps"_s, &p.fps) || !number(u"speed"_s, &p.speed) || !boolean(u"clickThrough"_s, &p.clickThrough)
        || !boolean(u"locked"_s, &p.locked) || !boolean(u"keepAbove"_s, &p.keepAbove) || !boolean(u"hoverHide"_s, &p.hoverHide)
        || !boolean(u"focusFade"_s, &p.focusFade) || !boolean(u"smartDodge"_s, &p.smartDodge) || !number(u"rotation"_s, &p.rotation)
        || !boolean(u"flipH"_s, &p.flipH) || !boolean(u"flipV"_s, &p.flipV))
        return std::nullopt;
    if (const auto it = changes.constFind(u"audio"_s); it != changes.constEnd()) {
        const auto audio = applyPetAudioChanges(p.audio, *it, error);
        if (!audio)
            return std::nullopt;
        p.audio = *audio;
    }
    if (const auto it = changes.constFind(u"stage"_s); it != changes.constEnd()) {
        const auto stage = applyPetStageChanges(p.stage, *it, error);
        if (!stage)
            return std::nullopt;
        p.stage = *stage;
    }
    if (const auto it = changes.constFind(u"states"_s); it != changes.constEnd()) {
        const auto states = applyPetStatesChanges(p.states, *it, error);
        if (!states)
            return std::nullopt;
        p.states = *states;
    }
    if (const auto it = changes.constFind(u"tracking"_s); it != changes.constEnd()) {
        const auto tracking = applyPetTrackingChanges(p.tracking, *it, error);
        if (!tracking)
            return std::nullopt;
        p.tracking = *tracking;
    }
    if (changes.contains(u"rotation"_s) && std::isfinite(p.rotation) && std::abs(p.rotation) <= 1e9)
        p.rotation = normalizedPetRotation(p.rotation); // wrapped: 370 is 10, -90 is 270
    // Choosing one mode by its own key leaves the other: {"fps": 15} on a pet that plays at 2x makes it a 15 fps pet.
    // hoverHide and focusFade: choosing one switches the other off, as in Anima; both in one call is a contradiction
    if (changes.contains(u"hoverHide"_s) && !changes.contains(u"focusFade"_s) && p.hoverHide)
        p.focusFade = false;
    if (changes.contains(u"focusFade"_s) && !changes.contains(u"hoverHide"_s) && p.focusFade)
        p.hoverHide = false;
    if (changes.contains(u"fps"_s) && !changes.contains(u"speed"_s) && p.fps > 0)
        p.speed = 1;
    if (changes.contains(u"speed"_s) && !changes.contains(u"fps"_s) && p.speed != 1)
        p.fps = 0;
    if (!validatePlacement(p, error))
        return std::nullopt;
    return p;
}

PetStore::PetStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void PetStore::load()
{
    m_pets.clear();
    m_saveBlocked = false;
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;

    QString problem = read.problem;
    if (problem.isEmpty()) {
        if (read.root.value(u"version"_s).toInt() != 1 || !read.root.value(u"pets"_s).isArray()) {
            problem = u"unsupported format"_s;
        } else {
            const QJsonArray entries = read.root.value(u"pets"_s).toArray();
            if (entries.size() > kMaxPets)
                problem = u"too many pets"_s;
            for (const QJsonValue &value : entries) {
                if (!problem.isEmpty())
                    break;
                QJsonObject entry = value.toObject();
                // Two things a file can hold that must not cost the user every pet: both exclusive flags on (hand edit, or a newer version) clears
                // focusFade, and an angle that is not a sane number is 0.
                if (entry.value(u"hoverHide"_s).toBool() && entry.value(u"focusFade"_s).toBool())
                    entry.insert(u"focusFade"_s, false);
                if (const QJsonValue rotation = entry.value(u"rotation"_s); rotation.isDouble() && !(std::abs(rotation.toDouble()) <= 1e9))
                    entry.insert(u"rotation"_s, 0);
                // A stage that is not valid costs the pet its stage (it is a pet again), not the user every pet.
                if (const auto stage = entry.constFind(u"stage"_s); stage != entry.constEnd() && !applyPetStageChanges(PetStage{}, *stage))
                    entry.remove(u"stage"_s);
                // A sound that is not valid costs the pet its sound, not the user every pet.
                if (const auto audio = entry.constFind(u"audio"_s); audio != entry.constEnd() && !applyPetAudioChanges(PetAudio{}, *audio))
                    entry.remove(u"audio"_s);
                // States that are not valid cost the pet its states; tracking that is not valid, or that follows a pet that also dodges or hides (a hand edit), costs
                // the pet its tracking: never every pet.
                if (const auto states = entry.constFind(u"states"_s); states != entry.constEnd() && !applyPetStatesChanges(PetStates{}, *states))
                    entry.remove(u"states"_s);
                if (const auto tracking = entry.constFind(u"tracking"_s); tracking != entry.constEnd()) {
                    const auto parsed = applyPetTrackingChanges(PetTracking{}, *tracking);
                    if (!parsed || (parsed->mode == PetTrackingMode::Follow
                                    && (entry.value(u"smartDodge"_s).toBool() || entry.value(u"hoverHide"_s).toBool() || entry.value(u"focusFade"_s).toBool())))
                        entry.remove(u"tracking"_s);
                }
                PetPlacement placement;
                placement.petId = entry.value(u"petId"_s).toString();
                placement.itemId = entry.value(u"itemId"_s).toString();
                placement.fps = defaultPetFps(placement.itemId); // pets stored before the field existed
                QString error;
                const auto parsed = applyPlacementChanges(placement, entry, &error);
                if (!parsed) {
                    problem = u"invalid entry: "_s + error;
                } else if (QUuid::fromString(parsed->petId).isNull()) {
                    problem = u"invalid entry: petId must be a uuid"_s;
                } else {
                    bool duplicate = false;
                    for (const PetPlacement &other : std::as_const(m_pets))
                        duplicate = duplicate || other.petId == parsed->petId;
                    if (duplicate)
                        problem = u"invalid entry: duplicate petId"_s;
                    else
                        m_pets.append(*parsed);
                }
            }
        }
    }
    if (problem.isEmpty())
        return;

    m_pets.clear();
    if (!quarantineBadFile(m_path, problem, u"no pets"_s, u"pets"_s))
        m_saveBlocked = true;
}

std::optional<PetPlacement> PetStore::pet(const QString &petId) const
{
    for (const PetPlacement &p : m_pets) {
        if (p.petId == petId)
            return p;
    }
    return std::nullopt;
}

bool PetStore::referencesItem(const QString &itemId) const
{
    for (const PetPlacement &p : m_pets) {
        if (p.itemId == itemId)
            return true;
    }
    return false;
}

QString PetStore::add(PetPlacement placement, QString *error)
{
    if (m_pets.size() >= kMaxPets) {
        fail(error, u"at most 64 pets"_s);
        return {};
    }
    placement.petId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!validatePlacement(placement, error) || !checkStageFree(placement, error))
        return {};
    m_pets.append(placement);
    if (!save()) {
        m_pets.removeLast();
        fail(error, u"could not save the pets"_s);
        return {};
    }
    emit changed();
    return placement.petId;
}

bool PetStore::checkStageFree(const PetPlacement &placement, QString *error) const
{
    if (!placement.stage.active())
        return true;
    for (const PetPlacement &other : m_pets) {
        if (other.petId != placement.petId && stagesConflict(placement.stage.mode, placement.output, other.stage.mode, other.output))
            return fail(error, u"another pet is already a stage on that output (a pet that fills the desktop leaves no room for another)"_s);
    }
    return true;
}

bool PetStore::update(const PetPlacement &placement, QString *error)
{
    if (error)
        error->clear();
    if (!validatePlacement(placement, error))
        return false;
    for (PetPlacement &p : m_pets) {
        if (p.petId != placement.petId)
            continue;
        if (p == placement)
            return false;
        // Only a change of what the stage covers asks whether it is free: a pet whose stage a hand-edited file made conflict can still be edited.
        if ((p.stage.mode != placement.stage.mode || p.output != placement.output) && !checkStageFree(placement, error))
            return false;
        const PetPlacement previous = p;
        p = placement;
        if (!save()) {
            p = previous;
            fail(error, u"could not save the pets"_s);
            return false;
        }
        emit changed();
        return true;
    }
    return false;
}

bool PetStore::updateAll(const std::function<void(PetPlacement &)> &change, QString *error)
{
    if (error)
        error->clear();
    QList<PetPlacement> updated = m_pets;
    bool any = false;
    for (PetPlacement &p : updated) {
        const PetPlacement before = p;
        change(p);
        p.rotation = normalizedPetRotation(p.rotation);
        if (!validatePlacement(p, error))
            return false;
        any = any || !(p == before);
    }
    if (!any)
        return false;
    const QList<PetPlacement> previous = m_pets;
    m_pets = updated;
    if (!save()) {
        m_pets = previous;
        fail(error, u"could not save the pets"_s);
        return false;
    }
    emit changed();
    return true;
}

bool PetStore::replaceAll(QList<PetPlacement> pets, QString *error)
{
    if (error)
        error->clear();
    if (pets.size() > kMaxPets)
        return fail(error, u"at most %1 pets"_s.arg(kMaxPets));
    QSet<QString> known, seen;
    for (const PetPlacement &p : std::as_const(m_pets))
        known.insert(p.petId);
    for (PetPlacement &p : pets) {
        p.rotation = normalizedPetRotation(p.rotation);
        if (p.petId.isEmpty())
            p.petId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        else if (!known.contains(p.petId))
            return fail(error, u"unknown petId"_s);
        if (seen.contains(p.petId))
            return fail(error, u"petId repeated"_s);
        seen.insert(p.petId);
        if (!validatePlacement(p, error))
            return false;
    }
    if (pets == m_pets)
        return false;
    const QList<PetPlacement> previous = m_pets;
    m_pets = pets;
    if (!save()) {
        m_pets = previous;
        return fail(error, u"could not save the pets"_s);
    }
    emit changed();
    return true;
}

bool PetStore::remove(const QString &petId, QString *error)
{
    if (error)
        error->clear();
    for (qsizetype i = 0; i < m_pets.size(); ++i) {
        if (m_pets.at(i).petId == petId) {
            const PetPlacement removed = m_pets.takeAt(i);
            if (!save()) {
                m_pets.insert(i, removed);
                fail(error, u"could not save the pets"_s);
                return false;
            }
            emit changed();
            return true;
        }
    }
    return false;
}

bool PetStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving pets over the unreadable %s", qPrintable(m_path));
        return false;
    }
    QJsonArray array;
    for (const PetPlacement &p : m_pets)
        array.append(p.toJson());
    return writeJsonObjectFile(m_path, QJsonObject{{u"version"_s, 1}, {u"pets"_s, array}});
}

} // namespace murasaki
