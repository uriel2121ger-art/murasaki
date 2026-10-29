#include "profile.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

QJsonObject ProfileEntry::toJson() const
{
    QJsonObject o = pet.toJson();
    o.remove(u"petId"_s);
    o.insert(u"slot"_s, slot);
    return o;
}

QJsonObject Profile::toJson() const
{
    QJsonArray array;
    for (const ProfileEntry &entry : pets)
        array.append(entry.toJson());
    QJsonObject o{{u"id"_s, id}, {u"name"_s, name}, {u"pets"_s, array}};
    if (!origin.isEmpty())
        o.insert(u"origin"_s, origin);
    return o;
}

bool validateProfile(const Profile &p, QString *error)
{
    if (p.name.trimmed().isEmpty() || p.name.size() > kMaxAutomationNameLength)
        return fail(error, u"name must be 1-%1 characters"_s.arg(kMaxAutomationNameLength));
    if (p.origin.size() > 200)
        return fail(error, u"origin is at most 200 characters"_s);
    if (p.pets.size() > kMaxProfilePets)
        return fail(error, u"at most %1 pets (%2)"_s.arg(kMaxProfilePets).arg(kProfileTooManyPets));
    QSet<int> seen;
    for (int i = 0; i < p.pets.size(); ++i) {
        const ProfileEntry &entry = p.pets.at(i);
        if (entry.slot < 0 || entry.slot > 1000 || seen.contains(entry.slot))
            return fail(error, u"pet %1: slot must be a number from 0 to 1000, once"_s.arg(i));
        seen.insert(entry.slot);
        if (!entry.pet.petId.isEmpty())
            return fail(error, u"pet %1: an entry has no petId"_s.arg(i));
        QString problem;
        PetPlacement withId = entry.pet;
        withId.petId = u"x"_s; // (validatePlacement does not look at it)
        if (!validatePlacement(withId, &problem))
            return fail(error, u"pet %1: %2"_s.arg(i).arg(problem));
    }
    return true;
}

std::optional<Profile> applyProfileChanges(const Profile &base, const QJsonObject &changes, QString *error)
{
    Profile p = base;
    const auto bad = [&](const QString &message) -> std::optional<Profile> {
        fail(error, message);
        return std::nullopt;
    };
    if (const auto it = changes.constFind(u"name"_s); it != changes.constEnd()) {
        if (!it->isString())
            return bad(u"name must be a string"_s);
        p.name = it->toString();
    }
    if (const auto it = changes.constFind(u"origin"_s); it != changes.constEnd()) {
        if (!it->isString())
            return bad(u"origin must be a string"_s);
        p.origin = it->toString();
    }
    if (const auto it = changes.constFind(u"pets"_s); it != changes.constEnd()) {
        if (!it->isArray())
            return bad(u"pets must be an array"_s);
        const QJsonArray array = it->toArray();
        if (array.size() > kMaxProfilePets)
            return bad(u"at most %1 pets (%2)"_s.arg(kMaxProfilePets).arg(kProfileTooManyPets));
        QList<ProfileEntry> entries;
        QSet<int> used;
        for (const QJsonValue &value : array) // the slots that were written down first, so a missing one takes a free number
            if (value.isObject() && value.toObject().value(u"slot"_s).isDouble())
                used.insert(int(value.toObject().value(u"slot"_s).toDouble(-1)));
        int next = 0;
        for (int i = 0; i < array.size(); ++i) {
            if (!array.at(i).isObject())
                return bad(u"pet %1 must be an object"_s.arg(i));
            const QJsonObject o = array.at(i).toObject();
            ProfileEntry entry;
            if (const auto slot = o.constFind(u"slot"_s); slot != o.constEnd()) {
                if (!slot->isDouble() || slot->toDouble() != std::floor(slot->toDouble()) || slot->toDouble() < 0 || slot->toDouble() > 1000)
                    return bad(u"pet %1: slot must be a whole number from 0 to 1000"_s.arg(i));
                entry.slot = int(slot->toDouble());
            } else {
                while (used.contains(next))
                    ++next;
                entry.slot = next;
                used.insert(next);
            }
            // The placement fields are the ones of the store, parsed by its own function (so every field a pet has is carried and checked the same way).
            PetPlacement base;
            base.itemId = o.value(u"itemId"_s).toString();
            base.fps = defaultPetFps(base.itemId); // as the store reads pets written before the field existed
            base.variant = o.value(u"variant"_s).toString();
            QJsonObject fields = o;
            fields.remove(u"petId"_s);
            fields.remove(u"itemId"_s);
            QString problem;
            const auto placement = applyPlacementChanges(base, fields, &problem);
            if (!placement)
                return bad(u"pet %1: %2"_s.arg(i).arg(problem));
            entry.pet = *placement;
            entries.append(entry);
        }
        p.pets = entries;
    }
    if (!validateProfile(p, error))
        return std::nullopt;
    return p;
}

QList<ProfileEntry> entriesFromPets(const QList<PetPlacement> &pets)
{
    QList<ProfileEntry> entries;
    for (int i = 0; i < pets.size(); ++i) {
        ProfileEntry entry;
        entry.slot = i;
        entry.pet = pets.at(i);
        entry.pet.petId.clear();
        entries.append(entry);
    }
    return entries;
}

} // namespace murasaki
