#include "plan.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki::screenmatch {
namespace {

double number(const QJsonValue &v, double fallback, double low, double high)
{
    const double d = v.isDouble() ? v.toDouble() : fallback;
    return std::isfinite(d) ? std::clamp(d, low, high) : fallback;
}

} // namespace

TriggerSpec specOf(const Trigger &trigger)
{
    TriggerSpec spec;
    spec.id = trigger.id;
    const QJsonObject screen = trigger.source.screen;
    const QJsonObject match = screen.value(u"match"_s).toObject();
    bool featureMode = false;
    const QString modeName = match.value(u"mode"_s).toString();
    spec.mode = screenMatchModeFromPack(modeName, &featureMode);
    if (featureMode)
        spec.notes.append(u"screen: matched by structure, not SIFT; check the threshold"_s);
    spec.match.agg = screenAggFromName(match.value(u"agg"_s).toString());
    spec.match.threshold = number(match.value(u"threshold"_s), kScreenDefaultThreshold, 0.0, 1.0);
    spec.match.startFrames = int(number(match.value(u"startFramesToCheck"_s), kDefaultStartFrames, 1, 64));
    spec.match.graceMs = qint64(number(match.value(u"stopGraceMs"_s), double(kDefaultGraceMs), 0, double(kScreenMaxDurationMs)));
    spec.match.cooldownMs = qint64(number(match.value(u"cooldownMs"_s), double(kDefaultCooldownMs), 0, double(kScreenMaxDurationMs)));
    spec.match.cooldownFrom = match.value(u"cooldownFrom"_s).toString() == u"deactivate" ? ScreenCooldownFrom::Deactivate : ScreenCooldownFrom::Activate;
    spec.match.stopOnMismatch = match.value(u"stopOnMismatch"_s).isBool() ? match.value(u"stopOnMismatch"_s).toBool() : true;
    spec.match.sequencer = match.value(u"refMode"_s).toString() == u"sequencer";
    spec.match.seqLen = spec.match.sequencer ? int(number(match.value(u"seqLen"_s), 8, 1, kMaxSteps)) : 1;
    spec.match.loopDelayMs = qint64(number(match.value(u"loopDelayMs"_s), 0, 0, double(kScreenMaxDurationMs)));

    const QJsonArray regions = screen.value(u"regions"_s).toArray();
    spec.regionCount = int(std::min<qsizetype>(regions.size(), kMaxRegionsPerTrigger));
    for (int i = 0; i < spec.regionCount; ++i) {
        const QJsonObject r = regions.at(i).toObject();
        RegionSpec region;
        region.index = i;
        region.x = number(r.value(u"x"_s), 0, -kMaxCoordinate, kMaxCoordinate);
        region.y = number(r.value(u"y"_s), 0, -kMaxCoordinate, kMaxCoordinate);
        region.w = number(r.value(u"w"_s), 0, 0, kMaxCoordinate);
        region.h = number(r.value(u"h"_s), 0, 0, kMaxCoordinate);
        spec.regions.append(region);
    }
    if (regions.size() > kMaxRegionsPerTrigger)
        spec.notes.append(u"screen: more than 8 regions in a trigger; the rest are not watched"_s);
    return spec;
}

Mapped mapRectangle(double x, double y, double w, double h, const QList<StreamGeometry> &streams)
{
    Mapped out;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0)
        return out;
    const double cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < streams.size(); ++i) {
        const StreamGeometry &s = streams.at(i);
        if (s.width <= 0 || s.height <= 0 || !(s.scale > 0) || cx < s.x || cx >= s.x + s.width || cy < s.y || cy >= s.y + s.height)
            continue;
        if (!s.scaleKnown) {
            out.scaleKnown = false; // (the right stream, but its pixels can not be worked out)
            return out;
        }
        const double pw = s.width * s.scale, ph = s.height * s.scale;
        const double left = std::clamp((x - s.x) * s.scale, 0.0, pw), top = std::clamp((y - s.y) * s.scale, 0.0, ph);
        const double right = std::clamp((x + w - s.x) * s.scale, 0.0, pw), bottom = std::clamp((y + h - s.y) * s.scale, 0.0, ph);
        const int ix = int(std::floor(left)), iy = int(std::floor(top));
        const int iw = int(std::ceil(right)) - ix, ih = int(std::ceil(bottom)) - iy;
        if (iw < kMinRegionSide || ih < kMinRegionSide)
            return out;
        out = {true, true, i, ix, iy, iw, ih};
        return out;
    }
    return out;
}

QList<PetRect> mapPets(const QList<LogicalRect> &pets, const QList<StreamGeometry> &streams)
{
    QList<PetRect> out;
    for (const LogicalRect &p : pets) {
        const Mapped m = mapRectangle(p.x, p.y, p.w, p.h, streams);
        if (m.onScreen && out.size() < kMaxPets)
            out.append({m.stream, m.x, m.y, m.w, m.h});
    }
    return out;
}

Plan makePlan(const PlanInput &input)
{
    Plan plan;
    plan.commands.append({.type = CommandType::Clear});
    int totalRegions = 0;
    for (const Trigger &trigger : input.triggers) {
        if (trigger.source.kind != TriggerKind::Screen)
            continue;
        if (plan.triggers.size() >= kMaxEnabledScreenTriggers) {
            if (!plan.notes.contains(QString(kTooManyTriggers)))
                plan.notes.append(QString(kTooManyTriggers));
            continue;
        }
        if (!validTriggerId(trigger.id))
            continue;
        PlannedTrigger planned;
        planned.spec = specOf(trigger);
        planned.steps = planned.spec.match.sequencer ? planned.spec.match.seqLen : 1;
        for (const QString &note : std::as_const(planned.spec.notes))
            plan.notes.append(note);
        plan.commands.append({.type = CommandType::Trigger, .trigger = trigger.id, .mode = planned.spec.mode});
        for (int i = 0; i < planned.spec.regionCount; ++i) {
            PlannedRegion region;
            region.index = i;
            const RegionSpec &spec = planned.spec.regions.at(i);
            const Mapped mapped = mapRectangle(spec.x, spec.y, spec.w, spec.h, input.streams);
            const QList<int> steps = input.steps ? input.steps(trigger.id, i) : QList<int>();
            if (!mapped.onScreen) {
                region.problem = QString(mapped.scaleKnown ? kRegionOutside : kScaleUnknown);
            } else if (steps.isEmpty()) {
                region.problem = QString(kRegionNoReference);
            } else if (totalRegions >= kMaxRegionsTotal) {
                if (!plan.notes.contains(QString(kTooManyRegions)))
                    plan.notes.append(QString(kTooManyRegions));
            } else {
                region.declared = true;
                ++totalRegions;
                ++plan.declaredRegions;
                plan.commands.append({.type = CommandType::Region, .trigger = trigger.id, .region = i, .stream = mapped.stream, .x = mapped.x, .y = mapped.y, .w = mapped.w, .h = mapped.h});
                int sent = 0;
                for (const int step : steps) {
                    if (step < 0 || step >= kMaxSteps || sent >= planned.steps)
                        continue;
                    const QByteArray grid = input.grid ? input.grid(trigger.id, i, step) : QByteArray();
                    if (grid.size() != kGridBytes)
                        continue;
                    plan.commands.append({.type = CommandType::Ref, .trigger = trigger.id, .region = i, .step = step, .grid = grid});
                    ++sent;
                }
                if (sent == 0) {
                    region.declared = false; // (its references could not be read: the region stays declared in the helper but scores nothing; counted as without reference)
                    region.problem = QString(kRegionNoReference);
                    --plan.declaredRegions;
                }
            }
            if (!region.problem.isEmpty())
                plan.notes.append(u"%1 (%2, region %3)"_s.arg(region.problem, trigger.id.left(8)).arg(i + 1));
            planned.regions.append(region);
        }
        plan.triggers.append(planned);
    }
    Command rate;
    rate.type = CommandType::Rate;
    rate.rate = std::clamp(input.ticksPerSecond, kMinRate, kMaxRate);
    plan.commands.append(rate);
    return plan;
}

} // namespace murasaki::screenmatch
