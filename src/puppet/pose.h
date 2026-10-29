#pragma once

#include "puppet/params.h"

#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// Pose (docs/pets/puppets.md, section 3.4): groups of parts of which only one is shown (the alternatives of an arm or an outfit). The specification gives no rule for the switch or
// the fade (Q-F8): ours is that the shown node of a group is the first whose part opacity is above 0.001 at the start, and afterwards the first other node whose part is raised above what the pose left it at (by a motion), that it fades in over FadeInTime (0.5 s when absent)
// while the others fade out at the same pace, and that the parts a node links to follow it.
struct PoseNode {
    std::string part;
    std::vector<std::string> links;
};

struct Pose {
    double fadeIn = 0.5;
    std::vector<std::vector<PoseNode>> groups;
};

struct PoseResult {
    bool ok = false;
    Pose pose;
    std::string error;
};

// Reads a `*.pose3.json`: Type "Live2D Pose", Version 3 or absent, at most kMaxPoseGroups groups and kMaxPoseNodes nodes.
PoseResult parsePose(std::string_view text);

class PoseState
{
public:
    explicit PoseState(Pose pose);
    // Call once per frame after motions have written the part opacities.
    void update(PartSet &parts, double dt);

private:
    struct Group {
        int shown = -1;
        double progress = 1.0;
        std::vector<double> written; // what the pose left in each node's part last frame: a node whose part is above it has been raised by someone else
    };
    Pose m_pose;
    std::vector<Group> m_groups;
    bool m_started = false;
};

} // namespace murasaki::puppet
