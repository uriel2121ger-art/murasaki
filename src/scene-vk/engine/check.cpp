#include "engine/check.h"

#include "assets/vfs.h"
#include "core/context.h"
#include "render/analysis.h"
#include "scene/scene.h"
#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace murasaki::scene {

namespace {

std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += raw;
            }
        }
    }
    return out + "\"";
}

bool deviceCheckEnabled()
{
    const char *v = std::getenv("MURASAKI_SCENE_VK_CHECK_DEVICE");
    return !(v && std::strcmp(v, "0") == 0);
}

} // namespace

CheckResult checkItem(const std::string &itemDir, const std::string &assetsDir)
{
    CheckResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(itemDir, ec)) {
        result.reasons.push_back("item directory '" + itemDir + "' does not exist");
        return result;
    }
    const std::string assets = findAssetsDir(assetsDir, itemDir);
    std::string packageError;
    const Vfs vfs = Vfs::forItem(itemDir, assets, &packageError);

    // project.json names the type; an item without one is judged by its scene file alone. It is read once.
    std::string sceneFile = "scene.json";
    if (const auto project = readProject(vfs)) {
        if (!project->file.empty()) {
            sceneFile = project->file;
        }
        if (!project->type.empty()) {
            std::string folded = project->type;
            for (char &c : folded) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (folded != "scene") {
                result.reasons.push_back("type '" + project->type + "' is not a scene");
                return result;
            }
        }
        if (!project->dependency.empty()) {
            result.reasons.push_back("preset of workshop item " + project->dependency + ": M6");
            return result;
        }
    }
    if (!packageError.empty()) {
        result.reasons.push_back("package: " + packageError);
        return result;
    }

    std::string error;
    const auto scene = loadScene(vfs, sceneFile, &error);
    if (!scene) {
        result.reasons.push_back(error);
        return result;
    }
    result.reasons = scene->unsupported;
    result.degraded = scene->degraded;
    // The shaders, textures and limits of the pass machinery: only when the scene itself parsed cleanly, so the reasons
    // stay about the first thing in the way.
    if (result.reasons.empty()) {
        std::vector<std::string> analysed;
        result.reasons = renderabilityReasons(*scene, vfs, &analysed);
        for (const std::string &d : analysed) {
            if (std::find(result.degraded.begin(), result.degraded.end(), d) == result.degraded.end()) {
                result.degraded.push_back(d);
            }
        }
    }
    if (result.reasons.empty() && deviceCheckEnabled()) {
        std::string deviceError;
        if (!VkContext::create(contextOptionsFromEnvironment(), deviceError)) {
            result.reasons.push_back("no usable Vulkan 1.3 device: " + deviceError);
        }
    }
    result.ok = result.reasons.empty();
    return result;
}

std::string checkResultJson(const CheckResult &result)
{
    std::string out = std::string("{\"ok\":") + (result.ok ? "true" : "false") + ",\"reasons\":[";
    for (size_t i = 0; i < result.reasons.size(); ++i) {
        out += (i ? "," : "") + quoted(result.reasons[i]);
    }
    out += "],\"degraded\":[";
    for (size_t i = 0; i < result.degraded.size(); ++i) {
        out += (i ? "," : "") + quoted(result.degraded[i]);
    }
    out += std::string("],\"engine\":\"murasaki-scene-vk\",\"version\":\"") + kEngineVersion + "\"}";
    return out;
}

int runCheck(const std::string &itemDir, const std::string &assetsDir)
{
    const CheckResult result = checkItem(itemDir, assetsDir);
    std::puts(checkResultJson(result).c_str());
    if (!result.degraded.empty()) {
        std::string line = "degraded: ";
        for (size_t i = 0; i < result.degraded.size(); ++i) {
            line += (i ? "; " : "") + result.degraded[i];
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    return 0;
}

} // namespace murasaki::scene
