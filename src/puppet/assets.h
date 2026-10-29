#pragma once

#include "puppet/manifest.h"
#include "puppet/session.h"

namespace murasaki::puppet {

// The motions, expressions, physics and pose a manifest names, read into what a PuppetSession plays (docs/pets/puppets.md, section 21). Every file is opened with readItemFile (the item's own contract) and parsed by
// the bounded readers of tasks 4 and 5. A file that cannot be read or parsed does not refuse the item: the motion, expression, physics or pose is left out and named in `degraded` (puppet: motion skipped: <file>,
// puppet: physics rig skipped: ...), since a puppet that stands still is better than none. A group or expression is kept only when at least one file of it was read.
struct AssetsResult {
    SessionAssets assets;
    std::vector<Problem> degraded;
};

// `budgetBytes`: the most the files may add up to (kMaxTotalAssetBytes; the tests lower it). A file named more than once is read and parsed once. A file that would take the total over is left out and named
// (`puppet: too large: kMaxTotalAssetBytes`, once).
AssetsResult loadSessionAssets(const Manifest &manifest, std::int64_t budgetBytes = -1);

} // namespace murasaki::puppet
