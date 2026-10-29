#include "puppet/bonephysics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <string>

#include "util/json.h"

namespace murasaki::scene {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kMaxVelocity = 1e5f;       // units/s
constexpr float kMaxAngularVelocity = 1e3f; // rad/s
constexpr float kMaxOffset = 1e4f;          // units a bone may stray from the animated pose

float clampFinite(double v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? static_cast<float>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi))) : fallback;
}

float number(const Json *j, float lo, float hi, float fallback)
{
    return j && j->isNumber() ? clampFinite(j->asNumber(), lo, hi, fallback) : fallback;
}

bool boolean(const Json *j, bool fallback)
{
    return j && j->isBool() ? j->asBool() : fallback;
}

// "x y z" (the editor writes vectors as a string of three numbers, some of them as "-nan(ind)" which makes the whole vector unusable)
std::optional<Vec3> vector3(const Json *j)
{
    if (!j || !j->isString() || j->asString().size() > 256) {
        return std::nullopt;
    }
    const std::string &text = j->asString();
    float v[3];
    const char *p = text.c_str();
    for (float &x : v) {
        char *end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p || !std::isfinite(d) || std::fabs(d) > 1e9) {
            return std::nullopt;
        }
        x = static_cast<float>(d);
        p = end;
    }
    return Vec3{v[0], v[1], v[2]};
}

std::optional<Vec3> unitVector(const Json *j)
{
    const auto v = vector3(j);
    if (!v) {
        return std::nullopt;
    }
    const float len = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len < 1e-6f) {
        return std::nullopt;
    }
    return Vec3{v->x / len, v->y / len, v->z / len};
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}

float component(const Vec3 &v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

float &component(Vec3 &v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

bool finite(const Vec3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// `angle` moved by whole turns to the one nearest `near` (the animation's angles wrap; the simulated ones do not)
float nearestTurn(float angle, float near)
{
    const float turns = std::round((near - angle) / kTwoPi);
    return std::isfinite(turns) ? angle + turns * kTwoPi : angle;
}

// A world vector in the space of a bone's parent. Rotation wants a direction (unit columns), a translation the scaled space.
Vec3 toParentSpace(const Mat4 &world, Vec3 v, bool direction)
{
    Vec3 out;
    for (int c = 0; c < 3; ++c) {
        const Vec3 col{world.at(0, c), world.at(1, c), world.at(2, c)};
        const float len = length(col);
        component(out, c) = len > 1e-6f ? dot(col, v) / (direction ? len : len * len) : 0.0f;
    }
    return out;
}

uint32_t mix(uint32_t h)
{
    h = (h ^ (h >> 16)) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    return h ^ (h >> 16);
}

} // namespace

std::optional<BoneConstraint> parseBoneConstraint(std::string_view text)
{
    if (text.empty() || text.front() != '{') {
        return std::nullopt;
    }
    const auto json = parseJson(text);
    if (!json || !json->isObject()) {
        return std::nullopt;
    }
    BoneConstraint c;
    c.rotation = boolean(json->find("r"), false);
    c.translation = boolean(json->find("t"), false);
    c.rotationStiffness = number(json->find("rs"), 0.0f, kMaxBoneStiffness, c.rotationStiffness);
    c.rotationDamping = number(json->find("rf"), 0.0f, kMaxBoneDamping, c.rotationDamping);
    c.translationStiffness = number(json->find("ts"), 0.0f, kMaxBoneStiffness, c.translationStiffness);
    c.translationDamping = number(json->find("tf"), 0.0f, kMaxBoneDamping, c.translationDamping);
    // the axes are written only by the models of 3D rigs: present, they all count
    if (json->find("rax") || json->find("ray") || json->find("raz")) {
        c.rotationAxes[0] = boolean(json->find("rax"), false);
        c.rotationAxes[1] = boolean(json->find("ray"), false);
        c.rotationAxes[2] = boolean(json->find("raz"), false);
    }
    if (json->find("tax") || json->find("tay") || json->find("taz")) {
        c.translationAxes[0] = boolean(json->find("tax"), false);
        c.translationAxes[1] = boolean(json->find("tay"), false);
        c.translationAxes[2] = boolean(json->find("taz"), false);
    }
    c.gravity = boolean(json->find("ge"), false);
    if (const auto d = unitVector(json->find("gd"))) {
        c.gravityDirection = *d;
    }
    if (const auto a = unitVector(json->find("a"))) {
        c.axis = *a;
    }
    c.limitAngles = boolean(json->find("la"), false);
    const auto lo = vector3(json->find("lamin")), hi = vector3(json->find("lamax"));
    if (c.limitAngles && lo && hi) {
        c.angleMin = {clampFinite(lo->x, -kTwoPi, 0.0f, -kPi), clampFinite(lo->y, -kTwoPi, 0.0f, -kPi), clampFinite(lo->z, -kTwoPi, 0.0f, -kPi)};
        c.angleMax = {clampFinite(hi->x, 0.0f, kTwoPi, kPi), clampFinite(hi->y, 0.0f, kTwoPi, kPi), clampFinite(hi->z, 0.0f, kTwoPi, kPi)};
    } else {
        c.limitAngles = false; // no usable bounds
    }
    c.limitTranslation = boolean(json->find("lt"), false);
    c.translationLimit = number(json->find("ltmax"), 0.0f, kMaxOffset, c.translationLimit);
    c.ik = boolean(json->find("ik"), false);
    c.ikControlPoint = c.ik && boolean(json->find("ikcp"), false);
    if (const Json *depth = json->find("ikd"); c.ik && depth && depth->isNumber()) {
        c.ikDepth = static_cast<int>(clampFinite(depth->asNumber(), 0.0f, 64.0f, 0.0f));
    }
    return c;
}

BoneWorld boneWorldInLayer(const BoneWorld &world, const Mat4 &layerWorld)
{
    BoneWorld out = world;
    out.windDirection = toParentSpace(layerWorld, world.windDirection, true);
    return out;
}

uint32_t bonePhysicsSeed()
{
    if (const char *env = std::getenv("MURASAKI_SCENE_VK_SEED"); env && *env) {
        return static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) + 0x42u * 7919u;
    }
    return std::random_device{}();
}

BonePhysicsState::BonePhysicsState(size_t bones, uint32_t seed) : m_bones(std::min(bones, kMaxPuppetBones)), m_seed(seed) {}

void BonePhysicsState::applyImpulse(size_t bone, Vec3 linear, Vec3 angular)
{
    if (bone >= m_bones.size() || !finite(linear) || !finite(angular)) {
        return;
    }
    BoneSim &s = m_bones[bone];
    const auto add = [](Vec3 &sum, Vec3 v, float limit) {
        sum = {std::clamp(sum.x + v.x, -limit, limit), std::clamp(sum.y + v.y, -limit, limit), std::clamp(sum.z + v.z, -limit, limit)};
    };
    add(s.impulseLinear, linear, kMaxVelocity);
    add(s.impulseAngular, angular, kMaxAngularVelocity);
}

void BonePhysicsState::reset(int bone)
{
    if (bone < 0) {
        std::fill(m_bones.begin(), m_bones.end(), BoneSim{});
    } else if (static_cast<size_t>(bone) < m_bones.size()) {
        m_bones[static_cast<size_t>(bone)] = BoneSim{};
    }
}

void BonePhysicsState::beginFrame(double time)
{
    m_steps = 0;
    if (!std::isfinite(time)) {
        return;
    }
    if (!m_started || time < m_lastTime) {
        m_started = true;
        m_lastTime = time; // the first frame, or a clock that went back: nothing to catch up
        return;
    }
    m_accumulator += std::min(time - m_lastTime, kBonePhysicsCatchUp);
    m_lastTime = time;
    const double h = static_cast<double>(kBonePhysicsStep);
    m_steps = std::min(static_cast<int>(m_accumulator / h), kBonePhysicsMaxSteps);
    m_accumulator = std::fmod(m_accumulator, h);
    // the wind's gusts: two slow waves whose phases come from the seed (the same seed, the same gusts)
    const float phase1 = static_cast<float>(mix(m_seed) & 0xFFFF) / 65536.0f * kTwoPi;
    const float phase2 = static_cast<float>(mix(m_seed ^ 0x9E3779B9u) & 0xFFFF) / 65536.0f * kTwoPi;
    for (int k = 0; k < m_steps; ++k) {
        m_simTime += h;
        const float t = static_cast<float>(m_simTime);
        m_gust[k] = std::max(0.0f, 1.0f + 0.5f * std::sin(kTwoPi * 0.35f * t + phase1) + 0.25f * std::sin(kTwoPi * 0.9f * t + phase2));
    }
}

PuppetFrame BonePhysicsState::step(size_t bone, const BoneConstraint &c, const PuppetFrame &target, const Mat4 &parentWorld, const BoneWorld &world)
{
    PuppetFrame out = target;
    if (bone >= m_bones.size() || !c.simulated()) {
        return out;
    }
    BoneSim &s = m_bones[bone];
    Vec3 targetAngles = target.angles;
    if (!s.valid || !finite(s.position) || !finite(s.angles) || !finite(s.velocity) || !finite(s.angularVelocity)) {
        const Vec3 linear = s.impulseLinear, angular = s.impulseAngular; // a pending impulse survives the start
        s = BoneSim{};
        s.impulseLinear = linear;
        s.impulseAngular = angular;
        s.valid = true;
        s.position = target.position;
        s.angles = target.angles;
    } else {
        targetAngles = {nearestTurn(target.angles.x, s.angles.x), nearestTurn(target.angles.y, s.angles.y), nearestTurn(target.angles.z, s.angles.z)};
    }
    // the bone's lever: its own direction in the parent's space, and how far along it the mass sits
    const Mat4 turn = rotationEuler(targetAngles);
    Vec3 lever{turn.at(0, 0) * c.axis.x + turn.at(0, 1) * c.axis.y + turn.at(0, 2) * c.axis.z,
               turn.at(1, 0) * c.axis.x + turn.at(1, 1) * c.axis.y + turn.at(1, 2) * c.axis.z,
               turn.at(2, 0) * c.axis.x + turn.at(2, 1) * c.axis.y + turn.at(2, 2) * c.axis.z};
    const float reach = std::max(1.0f, c.length);
    // impulses (taken once): a directional one pushes a translating bone, or turns a rotating one about its lever
    if (c.translation) {
        s.velocity = {s.velocity.x + s.impulseLinear.x, s.velocity.y + s.impulseLinear.y, s.velocity.z + s.impulseLinear.z};
    } else {
        const Vec3 torque = cross(lever, s.impulseLinear);
        s.angularVelocity = {s.angularVelocity.x + torque.x / reach, s.angularVelocity.y + torque.y / reach, s.angularVelocity.z + torque.z / reach};
    }
    if (c.rotation) {
        s.angularVelocity = {s.angularVelocity.x + s.impulseAngular.x, s.angularVelocity.y + s.impulseAngular.y, s.angularVelocity.z + s.impulseAngular.z};
    }
    s.impulseLinear = {};
    s.impulseAngular = {};

    // the forces that do not change within the frame, in the parent's space
    Vec3 gravity{0, 0, 0};
    if (c.gravity) {
        const Vec3 g{c.gravityDirection.x * world.gravityStrength * kGravityUnits, c.gravityDirection.y * world.gravityStrength * kGravityUnits,
                     c.gravityDirection.z * world.gravityStrength * kGravityUnits};
        gravity = g;
    }
    Vec3 wind{0, 0, 0};
    if (world.windEnabled) {
        const float len = length(world.windDirection);
        if (len > 1e-6f) {
            const float k = world.windStrength * kWindUnits / len;
            wind = {world.windDirection.x * k, world.windDirection.y * k, world.windDirection.z * k};
        }
    }
    const float h = kBonePhysicsStep;
    for (int k = 0; k < m_steps; ++k) {
        const Vec3 pushed{gravity.x + wind.x * m_gust[k], gravity.y + wind.y * m_gust[k], gravity.z + wind.z * m_gust[k]};
        if (c.translation) {
            const Vec3 a = toParentSpace(parentWorld, pushed, false);
            for (int axis = 0; axis < 3; ++axis) {
                float &x = component(s.position, axis), &v = component(s.velocity, axis);
                if (!c.translationAxes[axis]) {
                    x = component(target.position, axis);
                    v = 0.0f;
                    continue;
                }
                const float acceleration = c.translationStiffness * (component(target.position, axis) - x) + component(a, axis);
                v = std::clamp((v + acceleration * h) / (1.0f + c.translationDamping * h), -kMaxVelocity, kMaxVelocity); // damping implicit
                x += v * h;
            }
            {
                // never farther than the limit from the animated position: `ltmax` when `lt` is set, kMaxOffset always (a spring
                // with no stiffness under gravity would drift without end)
                const float limit = c.limitTranslation ? std::min(c.translationLimit, kMaxOffset) : kMaxOffset;
                const Vec3 offset{s.position.x - target.position.x, s.position.y - target.position.y, s.position.z - target.position.z};
                const float len = length(offset);
                if (len > limit && len > 0.0f) {
                    const float k2 = limit / len;
                    s.position = {target.position.x + offset.x * k2, target.position.y + offset.y * k2, target.position.z + offset.z * k2};
                    const float outward = dot(s.velocity, offset) / len;
                    if (outward > 0.0f) { // the part of the velocity that goes on outward stops (an inelastic stop)
                        s.velocity = {s.velocity.x - offset.x / len * outward, s.velocity.y - offset.y / len * outward, s.velocity.z - offset.z / len * outward};
                    }
                }
            }
        }
        if (c.rotation) {
            // a force turns the bone about its lever by the part of it across the lever
            const Vec3 a = toParentSpace(parentWorld, pushed, true);
            const Vec3 torque = cross(lever, a);
            for (int axis = 0; axis < 3; ++axis) {
                float &x = component(s.angles, axis), &w = component(s.angularVelocity, axis);
                if (!c.rotationAxes[axis]) {
                    x = component(targetAngles, axis);
                    w = 0.0f;
                    continue;
                }
                const float acceleration = c.rotationStiffness * (component(targetAngles, axis) - x) + component(torque, axis) / reach;
                w = std::clamp((w + acceleration * h) / (1.0f + c.rotationDamping * h), -kMaxAngularVelocity, kMaxAngularVelocity);
                x += w * h;
                // never more than half a turn from the animated angle, limits or not
                float lo = -kPi, hi = kPi;
                if (c.limitAngles) {
                    lo = std::max(lo, component(c.angleMin, axis));
                    hi = std::min(hi, component(c.angleMax, axis));
                }
                const float offset = x - component(targetAngles, axis);
                if (offset > hi) {
                    x = component(targetAngles, axis) + hi;
                    w = std::min(w, 0.0f);
                } else if (offset < lo) {
                    x = component(targetAngles, axis) + lo;
                    w = std::max(w, 0.0f);
                }
            }
        }
        if (!finite(s.position) || !finite(s.angles)) {
            s = BoneSim{};
            break;
        }
    }
    if (!s.valid) {
        return out; // a step went non-finite and reset the bone: the animated pose until the next frame
    }
    if (c.translation) {
        out.position = s.position;
    }
    if (c.rotation) {
        out.angles = s.angles;
    }
    return out;
}

} // namespace murasaki::scene
