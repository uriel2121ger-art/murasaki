#include "petstage.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

const char *stageModeName(StageMode mode)
{
    switch (mode) {
    case StageMode::Off:
        return "off";
    case StageMode::Output:
        return "output";
    case StageMode::Desktop:
        return "desktop";
    }
    return "off";
}

std::optional<StageMode> stageModeFromName(const QString &name)
{
    for (const StageMode mode : {StageMode::Off, StageMode::Output, StageMode::Desktop}) {
        if (name == QLatin1StringView(stageModeName(mode)))
            return mode;
    }
    return std::nullopt;
}

const char *stageFitName(StageFit fit)
{
    switch (fit) {
    case StageFit::Contain:
        return "contain";
    case StageFit::Cover:
        return "cover";
    case StageFit::Stretch:
        return "stretch";
    }
    return "contain";
}

std::optional<StageFit> stageFitFromName(const QString &name)
{
    for (const StageFit fit : {StageFit::Contain, StageFit::Cover, StageFit::Stretch}) {
        if (name == QLatin1StringView(stageFitName(fit)))
            return fit;
    }
    return std::nullopt;
}

const char *stageOverFullscreenName(StageOverFullscreen value)
{
    switch (value) {
    case StageOverFullscreen::Inherit:
        return "inherit";
    case StageOverFullscreen::Show:
        return "show";
    case StageOverFullscreen::Hide:
        return "hide";
    }
    return "inherit";
}

std::optional<StageOverFullscreen> stageOverFullscreenFromName(const QString &name)
{
    for (const StageOverFullscreen value : {StageOverFullscreen::Inherit, StageOverFullscreen::Show, StageOverFullscreen::Hide}) {
        if (name == QLatin1StringView(stageOverFullscreenName(value)))
            return value;
    }
    return std::nullopt;
}

QJsonObject PetStage::toJson() const
{
    return QJsonObject{{u"mode"_s, QLatin1StringView(stageModeName(mode))},
                       {u"fit"_s, QLatin1StringView(stageFitName(fit))},
                       {u"overFullscreen"_s, QLatin1StringView(stageOverFullscreenName(overFullscreen))}};
}

std::optional<PetStage> applyPetStageChanges(const PetStage &base, const QJsonValue &change, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<PetStage> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (change.isNull())
        return PetStage{};
    if (!change.isObject())
        return fail(u"stage must be an object, or null for no stage"_s);
    const QJsonObject o = change.toObject();
    PetStage s = base;
    if (const auto it = o.constFind(u"mode"_s); it != o.constEnd()) {
        const auto mode = it->isString() ? stageModeFromName(it->toString()) : std::nullopt;
        if (!mode)
            return fail(u"stage.mode must be off, output or desktop"_s);
        s.mode = *mode;
    }
    if (const auto it = o.constFind(u"fit"_s); it != o.constEnd()) {
        const auto fit = it->isString() ? stageFitFromName(it->toString()) : std::nullopt;
        if (!fit)
            return fail(u"stage.fit must be contain, cover or stretch"_s);
        s.fit = *fit;
    }
    if (const auto it = o.constFind(u"overFullscreen"_s); it != o.constEnd()) {
        const auto value = it->isString() ? stageOverFullscreenFromName(it->toString()) : std::nullopt;
        if (!value)
            return fail(u"stage.overFullscreen must be inherit, show or hide"_s);
        s.overFullscreen = *value;
    }
    return s;
}

namespace {
bool usable(QSizeF size)
{
    return std::isfinite(size.width()) && std::isfinite(size.height()) && size.width() > 0 && size.height() > 0;
}
} // namespace

QSizeF stageFittedSize(QSizeF target, QSizeF content, StageFit fit)
{
    if (!usable(target) || !usable(content))
        return {};
    if (fit == StageFit::Stretch)
        return target;
    const double sx = target.width() / content.width();
    const double sy = target.height() / content.height();
    const double scale = fit == StageFit::Contain ? std::min(sx, sy) : std::max(sx, sy);
    const QSizeF size(content.width() * scale, content.height() * scale);
    return usable(size) ? size : QSizeF();
}

QSizeF stageRotatedFootprint(QSizeF native, double rotation)
{
    if (!usable(native) || !std::isfinite(rotation))
        return {};
    double r = std::fmod(rotation, 360.0);
    if (r < 0)
        r += 360.0;
    if (r == 0.0 || r == 180.0)
        return native;
    if (r == 90.0 || r == 270.0)
        return {native.height(), native.width()};
    const double a = r * 3.14159265358979323846 / 180.0;
    const double c = std::fabs(std::cos(a));
    const double s = std::fabs(std::sin(a));
    return {native.width() * c + native.height() * s, native.width() * s + native.height() * c};
}

bool stagesConflict(StageMode modeA, const QString &outputA, StageMode modeB, const QString &outputB)
{
    if (modeA == StageMode::Off || modeB == StageMode::Off)
        return false;
    if (modeA == StageMode::Desktop || modeB == StageMode::Desktop)
        return true;
    return outputA == outputB;
}

} // namespace murasaki
