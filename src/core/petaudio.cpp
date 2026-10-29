#include "petaudio.h"

#include <cmath>

namespace murasaki {

using namespace Qt::StringLiterals;

const char *petAudioModeName(PetAudioMode mode)
{
    switch (mode) {
    case PetAudioMode::None:
        return "none";
    case PetAudioMode::Once:
        return "once";
    case PetAudioMode::AnimationLoop:
        return "animation_loop";
    case PetAudioMode::Continuous:
        return "continuous";
    case PetAudioMode::Click:
        return "click";
    }
    return "none";
}

std::optional<PetAudioMode> petAudioModeFromName(const QString &name)
{
    for (const PetAudioMode mode : {PetAudioMode::None, PetAudioMode::Once, PetAudioMode::AnimationLoop, PetAudioMode::Continuous, PetAudioMode::Click})
        if (name == QLatin1String(petAudioModeName(mode)))
            return mode;
    return std::nullopt;
}

QJsonObject PetAudio::toJson() const
{
    QJsonObject o{{u"mode"_s, QLatin1String(petAudioModeName(mode))}, {u"volume"_s, volume}, {u"offsetMs"_s, offsetMs}};
    if (!asset.isEmpty())
        o.insert(u"asset"_s, asset);
    return o;
}

bool isPetAudioAssetId(const QString &id)
{
    if (id.size() != kPetAudioAssetIdLength)
        return false;
    for (const QChar c : id)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    return true;
}

QString validatePetAudio(const PetAudio &a)
{
    if (!a.asset.isEmpty() && !isPetAudioAssetId(a.asset))
        return u"audio.asset must be 64 lowercase hex characters"_s;
    if (a.volume < 0 || a.volume > 100)
        return u"audio.volume must be between 0 and 100"_s;
    if (a.offsetMs < -kMaxPetAudioOffsetMs || a.offsetMs > kMaxPetAudioOffsetMs)
        return u"audio.offsetMs must be between -5000 and 5000"_s;
    return {};
}

std::optional<PetAudio> applyPetAudioChanges(const PetAudio &base, const QJsonValue &change, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<PetAudio> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (change.isNull())
        return PetAudio{};
    if (!change.isObject())
        return fail(u"audio must be an object, or null for no sound"_s);
    const QJsonObject o = change.toObject();
    PetAudio a = base;
    if (const auto it = o.constFind(u"asset"_s); it != o.constEnd()) {
        if (it->isNull())
            a.asset.clear();
        else if (it->isString())
            a.asset = it->toString();
        else
            return fail(u"audio.asset must be a string"_s);
    }
    if (const auto it = o.constFind(u"mode"_s); it != o.constEnd()) {
        const auto mode = it->isString() ? petAudioModeFromName(it->toString()) : std::nullopt;
        if (!mode)
            return fail(u"audio.mode must be none, once, animation_loop, continuous or click"_s);
        a.mode = *mode;
    }
    for (const auto &[key, target] : {std::pair{u"volume"_s, &a.volume}, std::pair{u"offsetMs"_s, &a.offsetMs}}) {
        const auto it = o.constFind(key);
        if (it == o.constEnd())
            continue;
        const double value = it->toDouble();
        if (!it->isDouble() || !std::isfinite(value) || value != std::floor(value) || std::abs(value) > 1e6)
            return fail(u"audio."_s + key + u" must be an integer"_s);
        *target = int(value);
    }
    if (const QString problem = validatePetAudio(a); !problem.isEmpty())
        return fail(problem);
    return a;
}

} // namespace murasaki
