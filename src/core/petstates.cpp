#include "petstates.h"

#include <QJsonArray>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr std::array<PetState, kPetStateCount> kStates = {PetState::Idle, PetState::Grabbed, PetState::Moving, PetState::Released};

std::optional<PetStates> failStates(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

std::optional<PetTracking> failTracking(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

bool validVariantName(const QJsonValue &value)
{
    return value.isString() && !value.toString().isEmpty() && value.toString().size() <= 200;
}

} // namespace

const char *petStateName(PetState state)
{
    switch (state) {
    case PetState::Idle:
        return "idle";
    case PetState::Grabbed:
        return "grabbed";
    case PetState::Moving:
        return "moving";
    case PetState::Released:
        return "released";
    }
    return "idle";
}

std::optional<PetState> petStateFromName(const QString &name)
{
    for (const PetState state : kStates) {
        if (name == QLatin1StringView(petStateName(state)))
            return state;
    }
    return std::nullopt;
}

const char *petSequenceOrderName(PetSequenceOrder order)
{
    return order == PetSequenceOrder::Random ? "random" : "linear";
}

std::optional<PetSequenceOrder> petSequenceOrderFromName(const QString &name)
{
    for (const PetSequenceOrder order : {PetSequenceOrder::Linear, PetSequenceOrder::Random}) {
        if (name == QLatin1StringView(petSequenceOrderName(order)))
            return order;
    }
    return std::nullopt;
}

const char *petSequenceAdvanceName(PetSequenceAdvance advance)
{
    return advance == PetSequenceAdvance::Click ? "click" : "auto";
}

std::optional<PetSequenceAdvance> petSequenceAdvanceFromName(const QString &name)
{
    for (const PetSequenceAdvance advance : {PetSequenceAdvance::Auto, PetSequenceAdvance::Click}) {
        if (name == QLatin1StringView(petSequenceAdvanceName(advance)))
            return advance;
    }
    return std::nullopt;
}

QJsonObject PetStateAnimation::toJson() const
{
    if (!isSequence())
        return QJsonObject{{u"variant"_s, variant}};
    return QJsonObject{{u"sequence"_s,
                        QJsonObject{{u"items"_s, QJsonArray::fromStringList(sequence.items)},
                                    {u"order"_s, QLatin1StringView(petSequenceOrderName(sequence.order))},
                                    {u"advance"_s, QLatin1StringView(petSequenceAdvanceName(sequence.advance))},
                                    {u"once"_s, sequence.once},
                                    {u"holdMs"_s, sequence.holdMs}}}};
}

QStringList PetStates::variants() const
{
    QStringList names;
    const auto add = [&names](const QString &name) {
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    };
    for (const auto &animation : byState) {
        if (!animation)
            continue;
        add(animation->variant);
        for (const QString &item : animation->sequence.items)
            add(item);
    }
    return names;
}

QJsonObject PetStates::toJson() const
{
    QJsonObject o;
    for (const PetState state : kStates) {
        if (const auto &animation = of(state))
            o.insert(QLatin1StringView(petStateName(state)), animation->toJson());
    }
    return o;
}

namespace {

std::optional<PetStateAnimation> parseAnimation(const QString &stateName, const QJsonObject &o, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<PetStateAnimation> {
        if (error)
            *error = u"states."_s + stateName + u": "_s + message;
        return std::nullopt;
    };
    const bool hasVariant = o.contains(u"variant"_s);
    const bool hasSequence = o.contains(u"sequence"_s);
    if (hasVariant == hasSequence)
        return fail(u"give either variant or sequence"_s);
    PetStateAnimation animation;
    if (hasVariant) {
        if (!validVariantName(o.value(u"variant"_s)))
            return fail(u"variant must be 1-200 characters"_s);
        animation.variant = o.value(u"variant"_s).toString();
        return animation;
    }
    if (!o.value(u"sequence"_s).isObject())
        return fail(u"sequence must be an object"_s);
    const QJsonObject s = o.value(u"sequence"_s).toObject();
    const QJsonValue items = s.value(u"items"_s);
    if (!items.isArray() || items.toArray().isEmpty() || items.toArray().size() > kMaxStateSequenceItems)
        return fail(u"sequence.items must be a list of 1 to %1 variants"_s.arg(kMaxStateSequenceItems));
    for (const QJsonValue &item : items.toArray()) {
        if (!validVariantName(item))
            return fail(u"sequence.items must be variant names of 1-200 characters"_s);
        animation.sequence.items.append(item.toString());
    }
    if (const auto it = s.constFind(u"order"_s); it != s.constEnd()) {
        const auto order = it->isString() ? petSequenceOrderFromName(it->toString()) : std::nullopt;
        if (!order)
            return fail(u"sequence.order must be linear or random"_s);
        animation.sequence.order = *order;
    }
    if (const auto it = s.constFind(u"advance"_s); it != s.constEnd()) {
        const auto advance = it->isString() ? petSequenceAdvanceFromName(it->toString()) : std::nullopt;
        if (!advance)
            return fail(u"sequence.advance must be auto or click"_s);
        animation.sequence.advance = *advance;
    }
    if (const auto it = s.constFind(u"once"_s); it != s.constEnd()) {
        if (!it->isBool())
            return fail(u"sequence.once must be a boolean"_s);
        animation.sequence.once = it->toBool();
    }
    if (const auto it = s.constFind(u"holdMs"_s); it != s.constEnd()) {
        const double value = it->toDouble();
        if (!it->isDouble() || !std::isfinite(value) || value != std::floor(value) || value < kMinStateHoldMs || value > kMaxStateHoldMs)
            return fail(u"sequence.holdMs must be a whole number from %1 to %2"_s.arg(kMinStateHoldMs).arg(kMaxStateHoldMs));
        animation.sequence.holdMs = int(value);
    }
    return animation;
}

} // namespace

std::optional<PetStates> applyPetStatesChanges(const PetStates &base, const QJsonValue &change, QString *error)
{
    if (change.isNull())
        return PetStates{};
    if (!change.isObject())
        return failStates(error, u"states must be an object, or null for no states"_s);
    const QJsonObject o = change.toObject();
    PetStates states = base;
    for (const PetState state : kStates) {
        const QString name = QLatin1StringView(petStateName(state));
        const auto it = o.constFind(name);
        if (it == o.constEnd())
            continue;
        if (it->isNull()) {
            states.of(state).reset();
            continue;
        }
        if (!it->isObject())
            return failStates(error, u"states."_s + name + u" must be an object, or null to clear it"_s);
        const auto animation = parseAnimation(name, it->toObject(), error);
        if (!animation)
            return std::nullopt;
        states.of(state) = *animation;
    }
    if (states.variants().size() > kMaxStateVariants)
        return failStates(error, u"states name at most %1 different animations in all"_s.arg(kMaxStateVariants));
    return states;
}

const char *petTrackingModeName(PetTrackingMode mode)
{
    switch (mode) {
    case PetTrackingMode::Off:
        return "off";
    case PetTrackingMode::Look:
        return "look";
    case PetTrackingMode::Follow:
        return "follow";
    }
    return "off";
}

std::optional<PetTrackingMode> petTrackingModeFromName(const QString &name)
{
    for (const PetTrackingMode mode : {PetTrackingMode::Off, PetTrackingMode::Look, PetTrackingMode::Follow}) {
        if (name == QLatin1StringView(petTrackingModeName(mode)))
            return mode;
    }
    return std::nullopt;
}

QJsonObject PetTracking::toJson() const
{
    return QJsonObject{{u"mode"_s, QLatin1StringView(petTrackingModeName(mode))},
                       {u"distance"_s, distance},
                       {u"speed"_s, speed},
                       {u"returnHome"_s, returnHome}};
}

std::optional<PetTracking> applyPetTrackingChanges(const PetTracking &base, const QJsonValue &change, QString *error)
{
    if (change.isNull())
        return PetTracking{};
    if (!change.isObject())
        return failTracking(error, u"tracking must be an object, or null for no tracking"_s);
    const QJsonObject o = change.toObject();
    PetTracking t = base;
    if (const auto it = o.constFind(u"mode"_s); it != o.constEnd()) {
        const auto mode = it->isString() ? petTrackingModeFromName(it->toString()) : std::nullopt;
        if (!mode)
            return failTracking(error, u"tracking.mode must be off, look or follow"_s);
        t.mode = *mode;
    }
    if (const auto it = o.constFind(u"distance"_s); it != o.constEnd()) {
        const double value = it->toDouble();
        if (!it->isDouble() || !std::isfinite(value) || value < 0 || value > kMaxTrackingDistance)
            return failTracking(error, u"tracking.distance must be a number from 0 to %1"_s.arg(kMaxTrackingDistance));
        t.distance = value;
    }
    if (const auto it = o.constFind(u"speed"_s); it != o.constEnd()) {
        const double value = it->toDouble();
        if (!it->isDouble() || !std::isfinite(value) || value < kMinTrackingSpeed || value > kMaxTrackingSpeed)
            return failTracking(error, u"tracking.speed must be a number from %1 to %2"_s.arg(kMinTrackingSpeed).arg(kMaxTrackingSpeed));
        t.speed = value;
    }
    if (const auto it = o.constFind(u"returnHome"_s); it != o.constEnd()) {
        if (!it->isBool())
            return failTracking(error, u"tracking.returnHome must be a boolean"_s);
        t.returnHome = it->toBool();
    }
    return t;
}

} // namespace murasaki
