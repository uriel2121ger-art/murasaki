#include "puppet/assets.h"

#include "puppetlimits.h"

#include <map>
#include <set>

namespace murasaki::puppet {

namespace {

bool readFile(const Manifest &manifest, const std::string &relative, std::int64_t maxBytes, std::string &out)
{
    return readItemFile(manifest.folder, relative, static_cast<size_t>(maxBytes), out) && static_cast<std::int64_t>(out.size()) <= maxBytes;
}

} // namespace

AssetsResult loadSessionAssets(const Manifest &manifest, std::int64_t budgetBytes)
{
    if (budgetBytes < 0)
        budgetBytes = kMaxTotalAssetBytes;
    AssetsResult result;
    for (const HitArea &area : manifest.hitAreas)
        result.assets.hitAreas.push_back({area.id, area.name});
    std::int64_t spent = 0;
    bool overBudget = false;
    std::string text;
    // One read per file: a path named twice is the same parsed object (a manifest that names one file five hundred times is read once).
    std::map<std::string, std::shared_ptr<const Motion>> motions;
    std::set<std::string> refused; // files that failed (or were over budget): named once, not read again
    const auto fetch = [&](const std::string &relative, std::int64_t maxBytes) -> bool {
        if (!readFile(manifest, relative, maxBytes, text))
            return false;
        spent += static_cast<std::int64_t>(text.size());
        return true;
    };
    const auto afford = [&](const std::string &relative) {
        if (spent < budgetBytes)
            return true;
        if (!overBudget) {
            overBudget = true;
            result.degraded.push_back({kTooLarge, "kMaxTotalAssetBytes"});
        }
        refused.insert(relative);
        return false;
    };
    for (const MotionGroup &group : manifest.motionGroups) {
        std::vector<MotionAsset> kept;
        for (const MotionRef &ref : group.motions) {
            std::shared_ptr<const Motion> motion;
            if (const auto known = motions.find(ref.file); known != motions.end()) {
                motion = known->second;
            } else if (!refused.count(ref.file) && afford(ref.file)) {
                const MotionResult parsed = fetch(ref.file, kMaxMotionFileBytes) ? parseMotion(text) : MotionResult{};
                if (parsed.ok)
                    motion = std::make_shared<const Motion>(parsed.motion);
                motions[ref.file] = motion; // (null for a file that did not read: remembered, so the next mention does not read it again)
            }
            if (!motion) {
                if (!refused.count(ref.file)) // (one over the budget is named by the budget's entry)
                    result.degraded.push_back({kMotionSkipped, ref.file});
                continue;
            }
            MotionAsset asset;
            asset.motion = motion;
            asset.fadeIn = ref.fadeIn.value_or(-1);
            asset.fadeOut = ref.fadeOut.value_or(-1);
            kept.push_back(std::move(asset));
        }
        if (!kept.empty())
            result.assets.motionGroups[group.name] = std::move(kept);
    }
    std::map<std::string, std::shared_ptr<const Expression>> expressions;
    for (const ExpressionRef &ref : manifest.expressions) {
        std::shared_ptr<const Expression> expression;
        if (const auto known = expressions.find(ref.file); known != expressions.end()) {
            expression = known->second;
        } else if (afford(ref.file)) {
            const ExpressionResult parsed = fetch(ref.file, kMaxExpressionFileBytes) ? parseExpression(text) : ExpressionResult{};
            if (parsed.ok)
                expression = std::make_shared<const Expression>(parsed.expression);
            expressions[ref.file] = expression;
        }
        if (!expression) {
            if (!refused.count(ref.file))
                result.degraded.push_back({kMotionSkipped, ref.file});
            continue;
        }
        result.assets.expressions[ref.name] = expression;
    }
    if (!manifest.physics.empty()) {
        const PhysicsResult parsed = afford(manifest.physics) && fetch(manifest.physics, kMaxPhysicsFileBytes) ? parsePhysics(text) : PhysicsResult{};
        if (parsed.ok)
            result.assets.physics = parsed.settings;
        else
            result.degraded.push_back({kPhysicsRigSkipped, manifest.physics});
    }
    if (!manifest.pose.empty()) {
        const PoseResult parsed = afford(manifest.pose) && fetch(manifest.pose, kMaxPoseFileBytes) ? parsePose(text) : PoseResult{};
        if (parsed.ok)
            result.assets.pose = parsed.pose;
        else
            result.degraded.push_back({kMotionSkipped, manifest.pose});
    }
    return result;
}

} // namespace murasaki::puppet
