#include "puppet/session.h"

#include "puppet/strictjson.h"
#include "puppetlimits.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

constexpr double kTailSeconds = 2.0; // a fade or a swing outlasts the event that began it by this much

const std::vector<std::string> kEyeIds = {"ParamEyeLOpen", "ParamEyeROpen"};

} // namespace

PuppetSession::PuppetSession(std::unique_ptr<ModelBackend> model, std::uint64_t seed, SessionAssets assets)
    : m_model(std::move(model)), m_host(m_model->parameters()), m_hostParts(m_model->parts()), m_frame(m_model->parameters()), m_blink(seed), m_assets(std::move(assets)),
      m_rng(seed ^ 0xA5A5A5A5DEADBEEFull), m_frameParts(m_model->parts())
{
    if (m_assets.physics) {
        m_physics = std::make_unique<Physics>(*m_assets.physics, m_model->parameters());
        m_assets.physics.reset(); // (the rigs were bound; the settings live in the Physics now)
    }
    if (m_assets.pose && !m_assets.pose->groups.empty())
        m_pose = std::make_unique<PoseState>(*m_assets.pose);
    m_hasBreath = m_host.find("ParamBreath") >= 0;
    m_hasBlink = m_host.find("ParamEyeLOpen") >= 0 || m_host.find("ParamEyeROpen") >= 0;
}

bool PuppetSession::command(std::string_view text, std::string &error)
{
    const json::ParseResult parsed = json::parse(text, {4, 4096, 256});
    if (!parsed.ok || !parsed.value.isObject()) {
        error = parsed.ok ? "the command is not an object" : "the command is not strict JSON";
        return false;
    }
    const json::Value &root = parsed.value;
    // validate everything first: a refused command changes nothing
    std::vector<std::pair<int, double>> values;
    std::vector<std::pair<int, double>> opacities;
    if (const json::Value *set = root.find("set")) {
        if (!set->isObject()) {
            error = "set is not an object";
            return false;
        }
        for (const auto &[id, v] : set->members) {
            const int index = m_host.find(id);
            if (!v.isNumber() || index < 0) {
                error = "set names a parameter the model does not have, or a value that is not a number";
                return false;
            }
            values.emplace_back(index, v.number);
        }
    }
    if (const json::Value *part = root.find("part")) {
        if (!part->isObject()) {
            error = "part is not an object";
            return false;
        }
        for (const auto &[id, v] : part->members) {
            const int index = m_hostParts.find(id);
            if (!v.isNumber() || index < 0) {
                error = "part names a part the model does not have, or a value that is not a number";
                return false;
            }
            opacities.emplace_back(index, v.number);
        }
    }
    // play / stop / expression: validated here, applied below
    std::optional<std::pair<const std::vector<MotionAsset> *, size_t>> playing;
    std::string playGroup;
    Priority playPriority = Priority::Normal;
    std::shared_ptr<const Expression> expression;
    bool setExpression = false;
    if (const json::Value *play = root.find("play")) {
        const json::Value *group = play->isObject() ? play->find("group") : nullptr;
        if (!group || !group->isString()) {
            error = "play needs a group";
            return false;
        }
        const auto it = m_assets.motionGroups.find(group->text);
        if (it == m_assets.motionGroups.end() || it->second.empty()) {
            error = "play names a motion group the model does not have";
            return false;
        }
        size_t index = 0;
        if (const json::Value *i = play->find("index")) {
            if (!i->isNumber() || !(i->number >= 0) || i->number != std::floor(i->number) || i->number >= static_cast<double>(it->second.size())) {
                error = "play names a motion the group does not have";
                return false;
            }
            index = static_cast<size_t>(i->number);
        } else {
            index = static_cast<size_t>(m_rng.uniform() * static_cast<double>(it->second.size())) % it->second.size();
        }
        if (const json::Value *p = play->find("priority")) {
            if (!p->isString() || (p->text != "idle" && p->text != "normal" && p->text != "force")) {
                error = "priority is idle, normal or force";
                return false;
            }
            playPriority = p->text == "idle" ? Priority::Idle : p->text == "force" ? Priority::Force : Priority::Normal;
        }
        playGroup = group->text;
        playing = std::make_pair(&it->second, index);
    }
    if (const json::Value *e = root.find("expression")) {
        if (e->isString()) {
            const auto it = m_assets.expressions.find(e->text);
            if (it == m_assets.expressions.end()) {
                error = "expression names an expression the model does not have";
                return false;
            }
            expression = it->second;
        } else if (e->kind != json::Kind::Null) {
            error = "expression is a name or null";
            return false;
        }
        setExpression = true;
    }
    std::optional<ClickRequest> click;
    if (const json::Value *c = root.find("click")) {
        const json::Value *u = c->isObject() ? c->find("u") : nullptr;
        const json::Value *v = c->isObject() ? c->find("v") : nullptr;
        if (!u || !v || !u->isNumber() || !v->isNumber() || !(u->number >= 0 && u->number <= 1) || !(v->number >= 0 && v->number <= 1)) {
            error = "click needs u and v, numbers from 0 to 1";
            return false;
        }
        ClickRequest request;
        request.u = u->number;
        request.v = v->number;
        if (const json::Value *fb = c->find("fallback")) {
            if (!fb->isArray() || fb->size() > 4 || !fb->numbers.empty()) { // (an array of numbers is kept packed: it is not a list of names)
                error = "fallback is a list of at most four group names";
                return false;
            }
            for (const json::Value &name : fb->items) {
                if (!name.isString()) {
                    error = "fallback is a list of group names";
                    return false;
                }
                request.fallback.push_back(name.text);
            }
        }
        click = std::move(request);
    }
    for (const char *flag : {"reset", "look", "breath", "blink", "stop"}) {
        if (const json::Value *v = root.find(flag); v && !v->isBool()) {
            error = std::string(flag) + " is not a boolean";
            return false;
        }
    }
    if (const json::Value *v = root.find("reset"); v && v->boolean) {
        m_host.resetToDefaults();
        m_hostParts.resetToOpaque();
        m_frameParts.resetToOpaque();
    }
    for (const auto &[index, value] : values)
        m_host.set(index, value);
    for (const auto &[index, value] : opacities) {
        m_hostParts.set(index, value);
        m_frameParts.set(index, value);
    }
    if (click)
        m_click = std::move(click);
    if (const json::Value *v = root.find("stop"); v && v->boolean)
        m_motions.clear();
    if (playing) {
        const MotionAsset &asset = (*playing->first)[playing->second];
        MotionManager::Request request;
        request.motion = asset.motion;
        request.priority = playPriority;
        request.fadeIn = asset.fadeIn;
        request.fadeOut = asset.fadeOut;
        request.tag = playGroup + "/" + std::to_string(playing->second);
        m_motions.request(std::move(request)); // (a request the scheduler refuses - an idle behind a playing motion - is not an error)
        m_tail = kTailSeconds;
    }
    if (setExpression) {
        m_expressions.set(expression);
        m_tail = kTailSeconds;
    }
    if (const json::Value *v = root.find("look"))
        setLook(v->boolean);
    if (const json::Value *v = root.find("breath"))
        m_breathOn = v->boolean;
    if (const json::Value *v = root.find("blink"))
        m_blinkOn = v->boolean;
    return true;
}

void PuppetSession::setLook(bool on)
{
    m_look = on;
    if (!on)
        m_lookAt.setTarget({});
}

void PuppetSession::pointer(double x, double y, uint32_t width, uint32_t height)
{
    if (!m_look)
        return;
    m_lookAt.setTarget(m_lookAt.targetFor(x, y, Rect{0, 0, static_cast<double>(width), static_cast<double>(height)}));
}

void PuppetSession::step(double dt)
{
    if (!std::isfinite(dt) || dt <= 0)
        return;
    dt = std::min(dt, kMaxStep);
    if (m_breathOn)
        m_breath.update(dt);
    if (m_blinkOn)
        m_blink.update(dt);
    m_lookAt.update(dt);
    m_motions.update(dt);
    m_expressions.update(dt);
    m_pendingDt = std::min(m_pendingDt + dt, 1.0); // (a pause with the clock running must not hand the rigs and the pose one huge step)
    if (m_motions.playing() || m_motions.fadingOut() > 0 || m_motions.queued() > 0)
        m_tail = kTailSeconds;
    else
        m_tail = std::max(0.0, m_tail - dt);
    m_physicsAwake = std::max(0.0, m_physicsAwake - dt);
}

const std::vector<std::string> &PuppetSession::physicsSkipped() const
{
    static const std::vector<std::string> none;
    return m_physics ? m_physics->skipped() : none;
}

bool PuppetSession::animated() const
{
    if (m_breathOn && m_hasBreath)
        return true;
    if (m_blinkOn && m_hasBlink)
        return true;
    if (m_motions.playing() || m_motions.fadingOut() > 0 || m_motions.queued() > 0 || m_tail > 0)
        return true;
    if (m_physics && (m_physicsAwake > 0 || m_physics->energy() > 1e-9))
        return true;
    const Vec2 c = m_lookAt.current();
    return m_look && (std::abs(c.x) > 1e-4 || std::abs(c.y) > 1e-4);
}

bool PuppetSession::evaluate(Frame &out, bool &changed)
{
    for (size_t i = 0; i < m_host.size(); ++i)
        m_frame.set(static_cast<int>(i), m_host.value(static_cast<int>(i)));
    if (m_breathOn)
        m_breath.apply(m_frame);
    if (m_blinkOn)
        m_blink.apply(m_frame, kEyeIds);
    if (m_look)
        m_lookAt.apply(m_frame);
    // the model's own motion and expressions act on what the host and the life set; then the rigs swing and the pose chooses between parts
    if (!m_pose)
        m_frameParts = m_hostParts; // (with a pose the parts persist: it fades one of a group in and the others out over time, and judges 'raised by someone else' against what it left)
    ModelValues modelValues;
    m_motions.apply(m_frame, m_frameParts, modelValues);
    m_expressions.apply(m_frame);
    if (m_physics) {
        const std::vector<double> before = m_frame.snapshot();
        m_physics->update(m_frame, m_pendingDt);
        const std::vector<double> after = m_frame.snapshot();
        for (size_t i = 0; i < after.size(); ++i) {
            if (std::abs(after[i] - before[i]) > 1e-5) { // (a swing that is still visible)
                m_physicsAwake = kTailSeconds;
                break;
            }
        }
    }
    if (m_pose)
        m_pose->update(m_frameParts, m_pendingDt);
    m_pendingDt = 0;
    std::vector<double> now = m_frame.snapshot();
    std::vector<double> parts;
    for (size_t i = 0; i < m_frameParts.size(); ++i)
        parts.push_back(m_frameParts.opacity(static_cast<int>(i)));
    changed = !m_evaluated || now != m_last || parts != m_lastParts;
    if (!changed)
        return true;
    if (!m_model->evaluate(m_frame, m_frameParts, out))
        return false;
    m_last = std::move(now);
    m_lastParts = std::move(parts);
    m_evaluated = true;
    return true;
}

} // namespace murasaki::puppet

namespace murasaki::puppet {

bool PuppetSession::playRandom(const std::string &group)
{
    const auto it = m_assets.motionGroups.find(group);
    if (it == m_assets.motionGroups.end() || it->second.empty())
        return false;
    const size_t index = static_cast<size_t>(m_rng.uniform() * static_cast<double>(it->second.size())) % it->second.size();
    MotionManager::Request request;
    request.motion = it->second[index].motion;
    request.priority = Priority::Normal;
    request.fadeIn = it->second[index].fadeIn;
    request.fadeOut = it->second[index].fadeOut;
    request.tag = group + "/" + std::to_string(index);
    m_motions.request(std::move(request));
    m_tail = kTailSeconds;
    return true;
}

std::optional<PuppetSession::ClickRequest> PuppetSession::takeClick()
{
    std::optional<ClickRequest> out = std::move(m_click);
    m_click.reset();
    return out;
}

namespace {

bool inTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c)
{
    const auto side = [](Vec2 p1, Vec2 p2, Vec2 p3) { return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y); };
    const double area = side(a, b, c);
    if (!std::isfinite(area) || area == 0 || !std::isfinite(p.x + p.y)) // a collapsed or non-finite triangle covers nothing
        return false;
    const double d1 = side(p, a, b), d2 = side(p, b, c), d3 = side(p, c, a);
    if (!std::isfinite(d1 + d2 + d3))
        return false;
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0, positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

} // namespace

std::string PuppetSession::applyClick(const ClickRequest &click, const Frame &shown, const ViewTransform &view, uint32_t width, uint32_t height)
{
    m_lastClick.clear();
    // the click in the model's units: the picture's pixel, to normalised device coordinates, undone by the view
    const double ndcX = click.u * 2.0 - 1.0, ndcY = click.v * 2.0 - 1.0;
    if (width > 0 && height > 0 && view.scaleX != 0 && view.scaleY != 0 && std::isfinite(view.scaleX + view.scaleY + view.offsetX + view.offsetY)) {
        const Vec2 p{(ndcX - view.offsetX) / view.scaleX, (ndcY - view.offsetY) / view.scaleY};
        const auto play = [&](const std::string &group) { return playRandom(group); };
        for (const HitAreaAsset &area : m_assets.hitAreas) {
            if (area.drawableId.empty()) // (an area with no id would match every drawable that has none)
                continue;
            for (const DrawItem &item : shown.items) {
                if (item.kind != DrawKind::Mesh || !(item.opacity > 0) || item.drawable < 0 || m_model->drawableId(item.drawable) != area.drawableId)
                    continue;
                const Shape &shape = m_model->shape(item.drawable);
                for (size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
                    const size_t a = shape.indices[t], b = shape.indices[t + 1], c = shape.indices[t + 2];
                    if (a >= item.positions.size() || b >= item.positions.size() || c >= item.positions.size())
                        continue;
                    if (inTriangle(p, item.positions[a], item.positions[b], item.positions[c]) && play(area.name)) {
                        m_lastClick = "area:" + area.name;
                        return m_lastClick;
                    }
                }
            }
        }
    }
    for (const std::string &group : click.fallback) {
        if (playRandom(group)) {
            m_lastClick = "fallback:" + group;
            return m_lastClick;
        }
    }
    return m_lastClick;
}

} // namespace murasaki::puppet
