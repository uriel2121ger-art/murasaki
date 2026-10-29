#include "puppet/pose.h"

#include "puppet/motion.h"
#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

PoseResult fail(std::string why)
{
    PoseResult r;
    r.error = std::move(why);
    return r;
}

constexpr double kShownThreshold = 0.001;

} // namespace

PoseResult parsePose(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text, {kMaxJsonDepth, kMaxJsonValues, kMaxJsonStringBytes});
    if (!parsed.ok)
        return fail("not strict JSON: " + parsed.error);
    const json::Value &root = parsed.value;
    if (!root.isObject())
        return fail("the top level is not an object");
    if (const json::Value *version = root.find("Version"); version && (!version->isNumber() || version->number != 3))
        return fail("Version is not 3");
    const json::Value *type = root.find("Type");
    if (!type || !type->isString() || type->text != "Live2D Pose")
        return fail("Type is not Live2D Pose");
    PoseResult result;
    if (const json::Value *fade = root.find("FadeInTime")) {
        if (!fade->isNumber() || fade->number < 0 || fade->number > static_cast<double>(kMaxFadeSeconds))
            return fail("FadeInTime is not a time from 0 to kMaxFadeSeconds");
        result.pose.fadeIn = fade->number;
    }
    const json::Value *groups = root.find("Groups");
    if (!groups || !groups->isArray() || !groups->numbers.empty())
        return fail("Groups is missing");
    if (static_cast<std::int64_t>(groups->size()) > kMaxPoseGroups)
        return fail("too many groups (kMaxPoseGroups)");
    std::int64_t nodes = 0;
    for (const json::Value &group : groups->items) {
        if (!group.isArray() || !group.numbers.empty())
            return fail("a group is not a list of nodes");
        std::vector<PoseNode> out;
        for (const json::Value &node : group.items) {
            if (!node.isObject())
                return fail("a node is not an object");
            if (++nodes > kMaxPoseNodes)
                return fail("too many nodes (kMaxPoseNodes)");
            PoseNode n;
            const json::Value *id = node.find("Id");
            if (!id || !id->isString() || id->text.empty() || static_cast<std::int64_t>(id->text.size()) > kMaxIdBytes)
                return fail("a node has no usable Id");
            n.part = id->text;
            if (const json::Value *link = node.find("Link")) {
                if (!link->isArray() || !link->numbers.empty())
                    return fail("Link is not a list of ids");
                for (const json::Value &l : link->items) {
                    if (!l.isString() || l.text.empty() || static_cast<std::int64_t>(l.text.size()) > kMaxIdBytes)
                        return fail("a link is not an id");
                    if (++nodes > kMaxPoseNodes)
                        return fail("too many nodes (kMaxPoseNodes)");
                    n.links.push_back(l.text);
                }
            }
            out.push_back(std::move(n));
        }
        result.pose.groups.push_back(std::move(out));
    }
    result.ok = true;
    return result;
}

PoseState::PoseState(Pose pose) : m_pose(std::move(pose)), m_groups(m_pose.groups.size()) {}

void PoseState::update(PartSet &parts, double dt)
{
    if (!std::isfinite(dt) || dt < 0)
        dt = 0;
    for (size_t g = 0; g < m_pose.groups.size(); ++g) {
        const std::vector<PoseNode> &nodes = m_pose.groups[g];
        Group &state = m_groups[g];
        if (nodes.empty())
            continue;
        if (state.written.size() != nodes.size())
            state.written.assign(nodes.size(), -1.0);
        // which node is wanted: at the start the first whose part is visible (else the first); later the first other node whose part someone raised above what we left it at
        int wanted = state.shown;
        if (state.shown < 0) {
            wanted = 0;
            for (size_t i = 0; i < nodes.size(); ++i) {
                const int part = parts.find(nodes[i].part);
                if (part >= 0 && parts.opacity(part) > kShownThreshold) {
                    wanted = static_cast<int>(i);
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < nodes.size(); ++i) {
                const int part = parts.find(nodes[i].part);
                if (static_cast<int>(i) != state.shown && part >= 0 && parts.opacity(part) > kShownThreshold && parts.opacity(part) > state.written[i] + 1e-9) {
                    wanted = static_cast<int>(i);
                    break;
                }
            }
        }
        if (state.shown < 0) {
            state.shown = wanted;
            state.progress = m_started ? 0.0 : 1.0; // the first frame shows the starting choice at once
        } else if (wanted != state.shown) {
            state.shown = wanted;
            state.progress = 0.0;
        }
        state.progress = m_pose.fadeIn > 0 ? std::min(1.0, state.progress + dt / m_pose.fadeIn) : 1.0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const bool isShown = static_cast<int>(i) == state.shown;
            const auto write = [&](const std::string &id) {
                const int part = parts.find(id);
                if (part < 0)
                    return;
                parts.set(part, isShown ? state.progress : std::min(parts.opacity(part), 1.0 - state.progress));
            };
            write(nodes[i].part);
            for (const std::string &link : nodes[i].links)
                write(link);
            const int part = parts.find(nodes[i].part);
            state.written[i] = part >= 0 ? parts.opacity(part) : -1.0;
        }
    }
    m_started = true;
}

} // namespace murasaki::puppet
