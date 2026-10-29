#pragma once

// `murasaki-scene-vk --check DIR`: feature detection. Murasaki runs it before it picks an engine for an item and
// caches the answer; it only parses, nothing is drawn and no bridge is opened.
//
// Output (one line on stdout, exit status 0 whenever a verdict was printed):
//   {"ok":true,"reasons":[],"engine":"murasaki-scene-vk","version":"0.1.0"}
//   {"ok":false,"reasons":["effects: M2"],...}
// "degraded" lists the features an accepted scene is drawn without; when it is not empty a `degraded: a; b` line also
// goes to stderr (stdout stays one JSON line).
// A reason is a short human sentence ending in the milestone that will lift it, or a plain failure ("cannot read
// scene.json"). The version lets the caller drop cached verdicts when the engine changes.

#include <string>
#include <vector>

namespace murasaki::scene {

struct CheckResult {
    bool ok = false;
    std::vector<std::string> reasons;
    std::vector<std::string> degraded;
};

// Inspects an item directory: type in project.json, the scene and everything it references, and (unless
// MURASAKI_SCENE_VK_CHECK_DEVICE=0) that a Vulkan device is usable at all.
CheckResult checkItem(const std::string &itemDir, const std::string &assetsDir);

std::string checkResultJson(const CheckResult &result);

// Runs --check: prints the verdict and returns the exit status.
int runCheck(const std::string &itemDir, const std::string &assetsDir);

inline constexpr const char *kEngineVersion = "0.1.0";

} // namespace murasaki::scene
