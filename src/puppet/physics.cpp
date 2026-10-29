#include "puppet/physics.h"

#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kDegrees = 180.0 / kPi;

PhysicsResult fail(std::string why)
{
    PhysicsResult r;
    r.error = std::move(why);
    return r;
}

bool finiteNumber(const json::Value *v)
{
    return v && v->isNumber() && std::isfinite(v->number) && std::abs(v->number) <= static_cast<double>(kMaxCoordinate);
}

bool numberIn(const json::Value *v, double lo, double hi, double &out)
{
    if (!finiteNumber(v) || v->number < lo || v->number > hi)
        return false;
    out = v->number;
    return true;
}

bool vec2(const json::Value *v, Vec2 &out)
{
    if (!v || !v->isObject())
        return false;
    const json::Value *x = v->find("X");
    const json::Value *y = v->find("Y");
    if (!finiteNumber(x) || !finiteNumber(y))
        return false;
    out = {x->number, y->number};
    return true;
}

bool normalization(const json::Value *v, Normalization &out)
{
    if (!v || !v->isObject())
        return false;
    const json::Value *lo = v->find("Minimum");
    const json::Value *mid = v->find("Default");
    const json::Value *hi = v->find("Maximum");
    if (!finiteNumber(lo) || !finiteNumber(mid) || !finiteNumber(hi) || !(lo->number <= mid->number) || !(mid->number <= hi->number))
        return false;
    out = {lo->number, mid->number, hi->number};
    return true;
}

bool parameterRef(const json::Value *v, std::string &id)
{
    if (!v || !v->isObject())
        return false;
    const json::Value *target = v->find("Target");
    const json::Value *name = v->find("Id");
    if (!target || !target->isString() || target->text != "Parameter" || !name || !name->isString() || name->text.empty() || static_cast<std::int64_t>(name->text.size()) > kMaxIdBytes)
        return false;
    id = name->text;
    return true;
}

bool ioType(const json::Value *v, IoType &out)
{
    if (!v || !v->isString())
        return false;
    if (v->text == "X")
        out = IoType::X;
    else if (v->text == "Angle")
        out = IoType::Angle;
    else
        return false;
    return true;
}

// -1 .. 1 across the parameter's range, the default being 0 (each side has its own scale, so the default stays at 0 whatever the range).
double sideNormalised(double value, const Parameter &p)
{
    if (value >= p.def)
        return p.max > p.def ? std::clamp((value - p.def) / (p.max - p.def), 0.0, 1.0) : 0.0;
    return p.def > p.min ? -std::clamp((p.def - value) / (p.def - p.min), 0.0, 1.0) : 0.0;
}

// The rig's normalised -1 .. 1 to its own units: the default of the range at 0, the maximum at 1, the minimum at -1.
double toRange(double n, const Normalization &r)
{
    return n >= 0 ? r.def + n * (r.max - r.def) : r.def + n * (r.def - r.min);
}

double fromRange(double value, const Normalization &r)
{
    if (value >= r.def)
        return r.max > r.def ? std::clamp((value - r.def) / (r.max - r.def), 0.0, 1.0) : 0.0;
    return r.def > r.min ? -std::clamp((r.def - value) / (r.def - r.min), 0.0, 1.0) : 0.0;
}

Vec2 rotate(Vec2 v, double degrees)
{
    const double a = degrees / kDegrees;
    const double c = std::cos(a);
    const double s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

Vec2 unit(Vec2 v, Vec2 fallback)
{
    const double length = std::hypot(v.x, v.y);
    return length > 1e-12 ? Vec2{v.x / length, v.y / length} : fallback;
}

} // namespace

PhysicsResult parsePhysics(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text, {kMaxJsonDepth, kMaxJsonValues, kMaxJsonStringBytes});
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
    PhysicsResult result;
    PhysicsSettings &s = result.settings;
    if (const json::Value *fps = meta->find("Fps")) {
        if (!numberIn(fps, 1, 240, s.fps))
            return fail("Fps is not a rate from 1 to 240");
    }
    const json::Value *forces = meta->find("EffectiveForces");
    if (!forces || !forces->isObject() || !vec2(forces->find("Gravity"), s.gravity) || !vec2(forces->find("Wind"), s.wind))
        return fail("EffectiveForces needs a Gravity and a Wind");
    const json::Value *settings = root.find("PhysicsSettings");
    if (!settings || !settings->isArray())
        return fail("PhysicsSettings is missing");
    if (static_cast<std::int64_t>(settings->size()) > kMaxPhysicsRigs)
        return fail("too many physics settings (kMaxPhysicsRigs)");
    if (!settings->numbers.empty())
        return fail("a physics setting is not an object");
    std::int64_t inputs = 0, outputs = 0, vertices = 0;
    for (const json::Value &item : settings->items) {
        if (!item.isObject())
            return fail("a physics setting is not an object");
        PhysicsRig rig;
        const json::Value *id = item.find("Id");
        if (!id || !id->isString() || id->text.empty() || static_cast<std::int64_t>(id->text.size()) > kMaxIdBytes)
            return fail("a physics setting has no usable Id");
        rig.id = id->text;
        const json::Value *in = item.find("Input");
        const json::Value *out = item.find("Output");
        const json::Value *vs = item.find("Vertices");
        if (!in || !in->isArray() || !out || !out->isArray() || !vs || !vs->isArray())
            return fail("a physics setting needs Input, Output and Vertices lists");
        if (static_cast<std::int64_t>(in->size()) > kMaxPhysicsIoPerRig || static_cast<std::int64_t>(out->size()) > kMaxPhysicsIoPerRig
            || static_cast<std::int64_t>(vs->size()) > kMaxPhysicsVerticesPerRig)
            return fail("a physics setting is too large (kMaxPhysicsIoPerRig, kMaxPhysicsVerticesPerRig)");
        inputs += static_cast<std::int64_t>(in->size());
        outputs += static_cast<std::int64_t>(out->size());
        vertices += static_cast<std::int64_t>(vs->size());
        if (inputs > kMaxPhysicsInputs || outputs > kMaxPhysicsOutputs || vertices > kMaxPhysicsVertices)
            return fail("too many inputs, outputs or vertices (kMaxPhysicsInputs, kMaxPhysicsOutputs, kMaxPhysicsVertices)");
        if (!in->numbers.empty() || !out->numbers.empty() || !vs->numbers.empty())
            return fail("a list holds numbers where objects are expected");
        for (const json::Value &i : in->items) {
            if (!i.isObject())
                return fail("an input is not an object");
            PhysicsInput input;
            const json::Value *reflect = i.find("Reflect");
            if (!parameterRef(i.find("Source"), input.id) || !numberIn(i.find("Weight"), 0, 100, input.weight) || !ioType(i.find("Type"), input.type) || !reflect || !reflect->isBool())
                return fail("an input needs a Source, a Weight from 0 to 100, a Type X or Angle and a Reflect");
            input.reflect = reflect->boolean;
            rig.inputs.push_back(std::move(input));
        }
        for (const json::Value &o : out->items) {
            if (!o.isObject())
                return fail("an output is not an object");
            PhysicsOutput output;
            const json::Value *reflect = o.find("Reflect");
            const json::Value *vertex = o.find("VertexIndex");
            if (!parameterRef(o.find("Destination"), output.id) || !numberIn(o.find("Weight"), 0, 100, output.weight) || !ioType(o.find("Type"), output.type) || !reflect || !reflect->isBool()
                || !finiteNumber(o.find("Scale")) || !vertex || !vertex->isNumber() || vertex->number != std::floor(vertex->number) || vertex->number < 0 || vertex->number > 1000)
                return fail("an output needs a Destination, a Scale, a VertexIndex, a Weight from 0 to 100, a Type X or Angle and a Reflect");
            output.reflect = reflect->boolean;
            output.scale = o.find("Scale")->number;
            output.vertex = static_cast<int>(vertex->number);
            rig.outputs.push_back(std::move(output));
        }
        for (const json::Value &v : vs->items) {
            if (!v.isObject())
                return fail("a vertex is not an object");
            PhysicsVertex vertex;
            if (!vec2(v.find("Position"), vertex.position) || !numberIn(v.find("Mobility"), 0, 1, vertex.mobility) || !numberIn(v.find("Delay"), 0, 100, vertex.delay)
                || !numberIn(v.find("Acceleration"), 0, 100, vertex.acceleration) || !numberIn(v.find("Radius"), 0, 1000, vertex.radius))
                return fail("a vertex needs a Position, a Mobility from 0 to 1, a Delay and an Acceleration from 0 to 100 and a Radius from 0 to 1000");
            rig.vertices.push_back(vertex);
        }
        const json::Value *norm = item.find("Normalization");
        if (!norm || !norm->isObject() || !normalization(norm->find("Position"), rig.position) || !normalization(norm->find("Angle"), rig.angle))
            return fail("a physics setting needs a Normalization with a Position and an Angle range (minimum <= default <= maximum)");
        s.rigs.push_back(std::move(rig));
    }
    result.ok = true;
    return result;
}

Physics::Physics(PhysicsSettings settings, const ParameterSet &parameters) : m_settings(std::move(settings)), m_parameterCount(parameters.size())
{
    if (!(m_settings.fps >= 1 && m_settings.fps <= 240))
        m_settings.fps = 60;
    m_rigs.reserve(m_settings.rigs.size());
    for (const PhysicsRig &source : m_settings.rigs) {
        Rig rig;
        rig.source = &source;
        std::string why;
        for (const PhysicsInput &in : source.inputs) {
            const int index = parameters.find(in.id);
            if (index < 0) {
                why = "no input parameter " + in.id;
                break;
            }
            rig.inputs.push_back({index, &in});
        }
        if (why.empty()) {
            for (const PhysicsOutput &out : source.outputs) {
                const int index = parameters.find(out.id);
                if (index < 0) {
                    why = "no output parameter " + out.id;
                    break;
                }
                if (out.vertex < 1 || static_cast<size_t>(out.vertex) >= source.vertices.size()) {
                    why = "output vertex " + std::to_string(out.vertex) + " is not a pendulum particle";
                    break;
                }
                rig.outputs.push_back({index, &out});
            }
        }
        if (why.empty() && source.vertices.size() < 2)
            why = "fewer than two vertices";
        if (!why.empty()) {
            m_skipped.push_back(std::string("puppet: physics rig skipped: ") + source.id + " (" + why + ")");
            continue;
        }
        rig.particles.resize(source.vertices.size());
        rig.lengths.assign(source.vertices.size(), 0.0);
        for (size_t i = 1; i < source.vertices.size(); ++i) {
            const PhysicsVertex &v = source.vertices[i];
            rig.lengths[i] = v.radius > 0 ? v.radius : std::hypot(v.position.x - source.vertices[i - 1].position.x, v.position.y - source.vertices[i - 1].position.y);
        }
        m_rigs.push_back(std::move(rig));
    }
}

void Physics::readInputs(const Rig &rig, const ParameterSet &parameters, double &baseX, double &baseAngle) const
{
    baseX = 0;
    baseAngle = 0;
    for (const BoundInput &b : rig.inputs) {
        const Parameter &p = parameters.at(static_cast<size_t>(b.index));
        const double n = sideNormalised(p.value, p) * (b.input->reflect ? -1.0 : 1.0);
        const double w = b.input->weight / 100.0;
        if (b.input->type == IoType::X)
            baseX += w * toRange(n, rig.source->position);
        else
            baseAngle += w * toRange(n, rig.source->angle);
    }
}

void Physics::initialise(Rig &rig, double baseX, double baseAngle) const
{
    const Vec2 gravity = unit(m_settings.gravity, {0, -1});
    const Vec2 direction = unit(rotate(gravity, baseAngle), gravity);
    rig.particles[0] = {{baseX, 0}, {0, 0}};
    for (size_t i = 1; i < rig.particles.size(); ++i)
        rig.particles[i] = {{rig.particles[i - 1].position.x + direction.x * rig.lengths[i], rig.particles[i - 1].position.y + direction.y * rig.lengths[i]}, {0, 0}};
    rig.baseX = baseX;
    rig.baseAngle = baseAngle;
    rig.started = true;
}

void Physics::step(Rig &rig, double baseX, double baseAngle, double h) const
{
    const Vec2 gravity = unit(m_settings.gravity, {0, -1});
    // the chain's rest direction: gravity turned by the base angle, with the wind pushing it sideways in proportion
    const Vec2 rest = unit({rotate(gravity, baseAngle).x + m_settings.wind.x, rotate(gravity, baseAngle).y + m_settings.wind.y}, gravity);
    Particle &root = rig.particles[0];
    root.velocity = {(baseX - root.position.x) / h, 0};
    root.position = {baseX, 0};
    const double omegaMax = 0.8 / h;
    for (size_t i = 1; i < rig.particles.size(); ++i) {
        const PhysicsVertex &v = rig.source->vertices[i];
        const Particle &parent = rig.particles[i - 1];
        Particle &p = rig.particles[i];
        const Vec2 target{parent.position.x + rest.x * rig.lengths[i], parent.position.y + rest.y * rig.lengths[i]};
        const double omega = std::min(omegaMax, 3.0 * v.acceleration / std::max(v.delay, 0.01));
        const double zeta = 1.0 - 0.8 * v.mobility;
        // semi-implicit Euler on a damped spring toward the target
        const Vec2 a{omega * omega * (target.x - p.position.x) - 2 * zeta * omega * p.velocity.x, omega * omega * (target.y - p.position.y) - 2 * zeta * omega * p.velocity.y};
        p.velocity = {p.velocity.x + a.x * h, p.velocity.y + a.y * h};
        p.position = {p.position.x + p.velocity.x * h, p.position.y + p.velocity.y * h};
        // the segment keeps its length: project onto the circle round the parent and drop the speed along the segment
        Vec2 d{p.position.x - parent.position.x, p.position.y - parent.position.y};
        const Vec2 n = unit(d, rest);
        p.position = {parent.position.x + n.x * rig.lengths[i], parent.position.y + n.y * rig.lengths[i]};
        const double along = p.velocity.x * n.x + p.velocity.y * n.y;
        const double parentAlong = parent.velocity.x * n.x + parent.velocity.y * n.y;
        p.velocity = {p.velocity.x - (along - parentAlong) * n.x, p.velocity.y - (along - parentAlong) * n.y};
    }
}

void Physics::writeOutputs(const Rig &rig, ParameterSet &parameters) const
{
    const Vec2 gravity = unit(m_settings.gravity, {0, -1});
    for (const BoundOutput &b : rig.outputs) {
        const PhysicsOutput &o = *b.output;
        const size_t k = static_cast<size_t>(o.vertex);
        const Vec2 segment{rig.particles[k].position.x - rig.particles[k - 1].position.x, rig.particles[k].position.y - rig.particles[k - 1].position.y};
        double raw = 0;
        if (o.type == IoType::Angle) {
            if (std::hypot(segment.x, segment.y) > 1e-12)
                raw = kDegrees * std::atan2(gravity.x * segment.y - gravity.y * segment.x, gravity.x * segment.x + gravity.y * segment.y);
        } else {
            raw = segment.x;
        }
        const Normalization &range = o.type == IoType::Angle ? rig.source->angle : rig.source->position;
        double n = fromRange(raw, range) * (o.reflect ? -1.0 : 1.0) * o.scale * (o.weight / 100.0);
        const Parameter &p = parameters.at(static_cast<size_t>(b.index));
        const double side = n >= 0 ? p.max - p.def : p.def - p.min;
        parameters.set(b.index, p.value + n * side);
    }
}

void Physics::update(ParameterSet &parameters, double dt)
{
    if (!std::isfinite(dt) || dt < 0 || parameters.size() != m_parameterCount)
        return;
    const double h = 1.0 / m_settings.fps;
    m_accumulator += dt;
    int steps = static_cast<int>(std::min<double>(std::floor(m_accumulator / h), static_cast<double>(kMaxPhysicsStepsPerUpdate)));
    if (m_accumulator / h > static_cast<double>(kMaxPhysicsStepsPerUpdate))
        m_accumulator = std::fmod(m_accumulator, h); // a long pause is dropped, not simulated
    else
        m_accumulator -= steps * h;
    for (Rig &rig : m_rigs) {
        double baseX = 0, baseAngle = 0;
        readInputs(rig, parameters, baseX, baseAngle);
        if (!rig.started)
            initialise(rig, baseX, baseAngle);
        for (int k = 1; k <= steps; ++k) {
            const double f = static_cast<double>(k) / steps;
            step(rig, rig.baseX + (baseX - rig.baseX) * f, rig.baseAngle + (baseAngle - rig.baseAngle) * f, h);
        }
        if (steps > 0) {
            rig.baseX = baseX;
            rig.baseAngle = baseAngle;
        }
        for (const Particle &p : rig.particles) { // a rig that went non-finite starts again from rest instead of staying dead
            if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || !std::isfinite(p.velocity.x) || !std::isfinite(p.velocity.y)) {
                rig.started = false;
                break;
            }
        }
        if (rig.started)
            writeOutputs(rig, parameters);
    }
}

void Physics::reset()
{
    for (Rig &rig : m_rigs)
        rig.started = false;
    m_accumulator = 0;
}

double Physics::energy() const
{
    double sum = 0;
    for (const Rig &rig : m_rigs)
        for (size_t i = 1; i < rig.particles.size(); ++i)
            sum += rig.particles[i].velocity.x * rig.particles[i].velocity.x + rig.particles[i].velocity.y * rig.particles[i].velocity.y;
    return sum;
}

} // namespace murasaki::puppet
