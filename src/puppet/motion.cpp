#include "puppet/motion.h"

#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

bool finite(double v)
{
    return std::isfinite(v);
}

MotionResult fail(std::string why)
{
    MotionResult r;
    r.error = std::move(why);
    return r;
}

// Cubic Bezier through control values p0..p3 at parameter s.
double bezier(double p0, double p1, double p2, double p3, double s)
{
    const double u = 1 - s;
    return u * u * u * p0 + 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s * p3;
}

} // namespace

MotionResult parseMotion(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text, {kMaxJsonDepth, kMaxMotionJsonValues, kMaxJsonStringBytes});
    if (!parsed.ok)
        return fail("not strict JSON: " + parsed.error);
    const json::Value &root = parsed.value;
    if (!root.isObject())
        return fail("the top level is not an object");
    const json::Value *version = root.find("Version");
    if (!version || !version->isNumber() || version->number != 3)
        return fail("Version is not 3");
    const json::Value *meta = root.find("Meta");
    if (!meta || !meta->isObject())
        return fail("Meta is missing");

    MotionResult result;
    Motion &m = result.motion;
    const json::Value *duration = meta->find("Duration");
    if (!duration || !duration->isNumber() || duration->number < 0 || duration->number > static_cast<double>(kMaxMotionSeconds))
        return fail("Duration is not a time from 0 to kMaxMotionSeconds");
    m.duration = duration->number;
    const json::Value *fps = meta->find("Fps");
    if (!fps || !fps->isNumber() || !(fps->number > 0) || fps->number > 10000)
        return fail("Fps is not a rate");
    m.fps = fps->number;
    if (const json::Value *loop = meta->find("Loop")) {
        if (!loop->isBool())
            return fail("Loop is not a boolean");
        m.loop = loop->boolean;
    }
    for (const auto &[key, slot] : {std::pair<const char *, std::optional<double> *>{"FadeInTime", &m.fadeIn}, {"FadeOutTime", &m.fadeOut}}) {
        if (const json::Value *v = meta->find(key)) {
            if (!v->isNumber() || v->number < 0 || v->number > static_cast<double>(kMaxFadeSeconds))
                return fail(std::string(key) + " is not a time from 0 to kMaxFadeSeconds");
            *slot = v->number;
        }
    }
    const json::Value *curves = root.find("Curves");
    if (!curves || !curves->isArray())
        return fail("Curves is missing");
    if (static_cast<std::int64_t>(curves->size()) > kMaxCurvesPerMotion)
        return fail("too many curves (kMaxCurvesPerMotion)");
    if (!curves->numbers.empty())
        return fail("a curve is not an object");
    std::int64_t totalPoints = 0;
    std::int64_t totalSegments = 0;
    for (const json::Value &c : curves->items) {
        if (!c.isObject())
            return fail("a curve is not an object");
        Curve curve;
        const json::Value *target = c.find("Target");
        if (!target || !target->isString())
            return fail("a curve has no Target");
        if (target->text == "Model")
            curve.target = CurveTarget::Model;
        else if (target->text == "Parameter")
            curve.target = CurveTarget::Parameter;
        else if (target->text == "PartOpacity")
            curve.target = CurveTarget::PartOpacity;
        else
            return fail("a curve has an unknown Target");
        const json::Value *id = c.find("Id");
        if (!id || !id->isString() || id->text.empty() || static_cast<std::int64_t>(id->text.size()) > kMaxIdBytes)
            return fail("a curve has no usable Id");
        curve.id = id->text;
        for (const auto &[key, slot] : {std::pair<const char *, std::optional<double> *>{"FadeInTime", &curve.fadeIn}, {"FadeOutTime", &curve.fadeOut}}) {
            if (const json::Value *v = c.find(key)) {
                if (!v->isNumber() || v->number < 0 || v->number > static_cast<double>(kMaxFadeSeconds))
                    return fail("a curve fade is not a time from 0 to kMaxFadeSeconds");
                *slot = v->number;
            }
        }
        const json::Value *segments = c.find("Segments");
        if (!segments || !segments->isArray() || !segments->items.empty())
            return fail("Segments is not a list of numbers");
        const std::vector<double> &flat = segments->numbers;
        if (flat.size() < 2)
            return fail("a curve is empty");
        // the first point
        for (const double v : flat)
            if (!finite(v))
                return fail("a segment number is not finite");
        if (flat[0] < 0 || flat[0] > static_cast<double>(kMaxMotionSeconds))
            return fail("a point time is out of range");
        curve.points.push_back({flat[0], flat[1]});
        size_t at = 2;
        while (at < flat.size()) {
            const double code = flat[at++];
            if (code != 0 && code != 1 && code != 2 && code != 3)
                return fail("a segment code is not 0 to 3");
            const SegmentKind kind = static_cast<SegmentKind>(static_cast<int>(code));
            const size_t count = kind == SegmentKind::Bezier ? 3 : 1;
            if (at + count * 2 > flat.size())
                return fail("a curve ends inside a segment");
            if (++totalSegments > kMaxSegmentsPerMotion)
                return fail("too many segments (kMaxSegmentsPerMotion)");
            curve.segments.push_back({kind, static_cast<std::uint32_t>(curve.points.size() - 1)});
            for (size_t k = 0; k < count; ++k) {
                const Point p{flat[at], flat[at + 1]};
                at += 2;
                if (p.t < curve.points.back().t || p.t > static_cast<double>(kMaxMotionSeconds))
                    return fail("point times go backwards or are out of range");
                curve.points.push_back(p);
            }
        }
        totalPoints += static_cast<std::int64_t>(curve.points.size());
        if (totalPoints > kMaxPointsPerMotion)
            return fail("too many points (kMaxPointsPerMotion)");
        m.curves.push_back(std::move(curve));
    }
    result.ok = true;
    return result;
}

double evaluateCurve(const Curve &curve, double t)
{
    if (curve.points.empty())
        return 0;
    if (std::isnan(t))
        t = 0;
    if (curve.segments.empty() || t <= curve.points.front().t)
        return curve.points.front().v;
    const auto endIndex = [&](const Segment &s) { return s.first + (s.kind == SegmentKind::Bezier ? 3u : 1u); };
    const Segment &lastSegment = curve.segments.back();
    if (t >= curve.points[endIndex(lastSegment)].t)
        return curve.points[endIndex(lastSegment)].v;
    // the last segment that starts at or before t
    size_t lo = 0;
    size_t hi = curve.segments.size();
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (curve.points[curve.segments[mid].first].t <= t)
            lo = mid;
        else
            hi = mid;
    }
    const Segment &s = curve.segments[lo];
    const Point &p0 = curve.points[s.first];
    const Point &pe = curve.points[endIndex(s)];
    switch (s.kind) {
    case SegmentKind::Linear: {
        const double span = pe.t - p0.t;
        return span > 0 ? p0.v + (pe.v - p0.v) * ((t - p0.t) / span) : pe.v;
    }
    case SegmentKind::Stepped:
        return t < pe.t ? p0.v : pe.v;
    case SegmentKind::InverseStepped:
        return t > p0.t ? pe.v : p0.v;
    case SegmentKind::Bezier: {
        const Point &p1 = curve.points[s.first + 1];
        const Point &p2 = curve.points[s.first + 2];
        double a = 0;
        double b = 1;
        for (int i = 0; i < 60; ++i) { // the control times never decrease, so the time is monotone in s: bisection
            const double mid = 0.5 * (a + b);
            if (bezier(p0.t, p1.t, p2.t, pe.t, mid) < t)
                a = mid;
            else
                b = mid;
        }
        return bezier(p0.v, p1.v, p2.v, pe.v, 0.5 * (a + b));
    }
    }
    return 0;
}

double fadeEase(double x)
{
    if (!finite(x))
        return 0;
    x = std::clamp(x, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * x);
}

MotionPlayer::MotionPlayer(std::shared_ptr<const Motion> motion, double fadeIn, double fadeOut, std::optional<bool> loop)
    : m_motion(std::move(motion))
{
    const Motion &m = *m_motion;
    m_fadeIn = fadeIn >= 0 ? fadeIn : m.fadeIn.value_or(kDefaultFadeSeconds);
    m_fadeOut = fadeOut >= 0 ? fadeOut : m.fadeOut.value_or(kDefaultFadeSeconds);
    m_loop = loop.value_or(m.loop) && m.duration > 0;
    if (m.duration <= 0 && !m_loop)
        m_finished = true; // nothing to play
}

void MotionPlayer::update(double dt)
{
    if (m_finished || !finite(dt) || dt < 0)
        return;
    m_age += dt;
    if (m_stopping) {
        m_stopAge += dt;
        if (m_stopAge >= m_fadeOut)
            m_finished = true;
    }
    m_time += dt;
    const double duration = m_motion->duration;
    if (m_loop) {
        m_time = std::fmod(m_time, duration);
    } else if (m_time >= duration) {
        m_time = duration;
        m_finished = true;
    }
}

void MotionPlayer::stop()
{
    if (m_finished || m_stopping)
        return;
    m_stopping = true;
    m_stopAge = 0;
    if (m_fadeOut <= 0)
        m_finished = true;
}

double MotionPlayer::weight() const
{
    if (m_finished)
        return 0;
    double w = m_fadeIn > 0 ? fadeEase(m_age / m_fadeIn) : 1.0;
    if (m_stopping)
        w *= m_fadeOut > 0 ? fadeEase(1.0 - m_stopAge / m_fadeOut) : 0.0;
    else if (!m_loop && m_fadeOut > 0)
        w *= fadeEase((m_motion->duration - m_time) / m_fadeOut);
    return w;
}

double MotionPlayer::curveWeight(const Curve &curve) const
{
    double w = weight();
    if (curve.fadeIn && *curve.fadeIn > 0)
        w *= fadeEase(m_age / *curve.fadeIn);
    if (curve.fadeOut && *curve.fadeOut > 0 && !m_loop)
        w *= fadeEase((m_motion->duration - m_time) / *curve.fadeOut);
    return w;
}

void MotionPlayer::apply(ParameterSet &parameters, PartSet &parts, ModelValues &model) const
{
    if (m_finished)
        return;
    for (const Curve &curve : m_motion->curves) {
        const double w = curveWeight(curve);
        if (w <= 0)
            continue;
        const double v = evaluateCurve(curve, m_time);
        switch (curve.target) {
        case CurveTarget::Parameter: {
            const int index = parameters.find(curve.id);
            if (index >= 0) {
                const double cur = parameters.value(index);
                parameters.set(index, cur + (v - cur) * w);
            }
            break;
        }
        case CurveTarget::PartOpacity: {
            const int index = parts.find(curve.id);
            if (index >= 0) {
                const double cur = parts.opacity(index);
                parts.set(index, cur + (v - cur) * w);
            }
            break;
        }
        case CurveTarget::Model:
            if (curve.id == "Opacity")
                model.opacity = std::clamp(model.opacity + (v - model.opacity) * w, 0.0, 1.0);
            else if (curve.id == "EyeBlink")
                model.eyeBlink = model.eyeBlink ? *model.eyeBlink + (v - *model.eyeBlink) * w : v * w + (1 - w);
            else if (curve.id == "LipSync")
                model.lipSync = model.lipSync ? *model.lipSync + (v - *model.lipSync) * w : v * w;
            break;
        }
    }
}

// ---- the scheduler ----

const std::string &MotionManager::currentTag() const
{
    static const std::string none;
    return m_current ? m_current->tag : none;
}

void MotionManager::startNow(Request &&r)
{
    m_current.emplace(Active{MotionPlayer(std::move(r.motion), r.fadeIn, r.fadeOut), r.priority, std::move(r.tag)});
}

void MotionManager::retireCurrent()
{
    if (!m_current)
        return;
    m_current->player.stop();
    if (!m_current->player.finished()) {
        if (static_cast<std::int64_t>(m_fading.size()) >= kMaxFadingMotions)
            m_fading.erase(m_fading.begin()); // the oldest is cut short
        m_fading.push_back(std::move(*m_current));
    }
    m_current.reset();
}

bool MotionManager::request(Request r)
{
    if (!r.motion || r.priority == Priority::None)
        return false;
    switch (r.priority) {
    case Priority::Force:
        m_queue.clear();
        retireCurrent();
        startNow(std::move(r));
        return true;
    case Priority::Normal:
        if (!m_current) {
            startNow(std::move(r));
            return true;
        }
        if (m_current->priority == Priority::Idle) {
            retireCurrent();
            startNow(std::move(r));
            return true;
        }
        if (static_cast<std::int64_t>(m_queue.size()) >= kMaxMotionQueue)
            return false;
        m_queue.push_back(std::move(r));
        return true;
    case Priority::Idle:
        if (m_current || !m_queue.empty())
            return false;
        startNow(std::move(r));
        return true;
    case Priority::None:
        break;
    }
    return false;
}

void MotionManager::update(double dt)
{
    if (m_current) {
        m_current->player.update(dt);
        if (m_current->player.finished())
            m_current.reset();
    }
    for (Active &a : m_fading)
        a.player.update(dt);
    m_fading.erase(std::remove_if(m_fading.begin(), m_fading.end(), [](const Active &a) { return a.player.finished(); }), m_fading.end());
    if (!m_current && !m_queue.empty()) {
        Request next = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        startNow(std::move(next));
    }
}

void MotionManager::apply(ParameterSet &parameters, PartSet &parts, ModelValues &model) const
{
    for (const Active &a : m_fading)
        a.player.apply(parameters, parts, model);
    if (m_current)
        m_current->player.apply(parameters, parts, model);
}

void MotionManager::clear()
{
    m_current.reset();
    m_fading.clear();
    m_queue.clear();
}

} // namespace murasaki::puppet
