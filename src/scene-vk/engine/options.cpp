#include "engine/options.h"

#include "util/log.h"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace murasaki::scene {

namespace {

bool parseInt(std::string_view text, int &out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool parseSize(std::string_view text, unsigned &width, unsigned &height)
{
    const size_t x = text.find('x');
    if (x == std::string_view::npos) {
        return false;
    }
    int w = 0;
    int h = 0;
    if (!parseInt(text.substr(0, x), w) || !parseInt(text.substr(x + 1), h) || w < 1 || h < 1 || w > 16384 || h > 16384) {
        return false;
    }
    width = static_cast<unsigned>(w);
    height = static_cast<unsigned>(h);
    return true;
}

} // namespace

const char *usageText()
{
    return "usage: murasaki-scene-vk --murasaki-bridge-fd N --murasaki-size WxH --screen-root NAME --bg DIR [--assets-dir DIR]\n"
           "                         [--scaling fill|fit|stretch] [--fps N] [--render-scale auto|F] [--volume N] [--silent] [--set-property K=V]...\n           [--disable-mouse] [--disable-parallax]\n"
           "       murasaki-scene-vk --check DIR [--assets-dir DIR]\n"
           "       murasaki-scene-vk --version\n";
}

bool parseOptions(int argc, const char *const *argv, Options &options, std::string &error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        // Options that take a value.
        const auto value = [&](std::string &out) -> bool {
            if (i + 1 >= argc) {
                error = std::string(arg) + " needs a value";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string text;
        if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--silent") {
            // accepted for compatibility with the engines' common command line
        } else if (arg == "--murasaki-bridge-fd") {
            if (!value(text)) {
                return false;
            }
            if (!parseInt(text, options.bridgeFd) || options.bridgeFd < 0) {
                error = "--murasaki-bridge-fd must be a descriptor number";
                return false;
            }
        } else if (arg == "--murasaki-size") {
            if (!value(text)) {
                return false;
            }
            if (!parseSize(text, options.width, options.height)) {
                error = "--murasaki-size must be WxH with sizes between 1 and 16384";
                return false;
            }
        } else if (arg == "--screen-root") {
            if (!value(options.viewportName)) {
                return false;
            }
        } else if (arg == "--bg") {
            if (!value(options.itemDir)) {
                return false;
            }
        } else if (arg == "--assets-dir") {
            if (!value(options.assetsDir)) {
                return false;
            }
        } else if (arg == "--scaling") {
            if (!value(options.scaling)) {
                return false;
            }
            if (options.scaling != "fill" && options.scaling != "fit" && options.scaling != "stretch") {
                error = "--scaling must be fill, fit or stretch";
                return false;
            }
        } else if (arg == "--fps") {
            if (!value(text)) {
                return false;
            }
            if (!parseInt(text, options.fps) || options.fps < 1 || options.fps > 1000) {
                error = "--fps must be between 1 and 1000";
                return false;
            }
        } else if (arg == "--volume") {
            if (!value(text)) {
                return false;
            }
            if (!parseInt(text, options.volume)) {
                error = "--volume must be a number";
                return false;
            }
        } else if (arg == "--set-property") {
            if (!value(text)) {
                return false;
            }
            const size_t eq = text.find('=');
            if (eq == std::string::npos || eq == 0) {
                error = "--set-property wants KEY=VALUE";
                return false;
            }
            options.properties.emplace_back(text.substr(0, eq), text.substr(eq + 1));
        } else if (arg == "--render-scale") {
            if (!value(text)) {
                return false;
            }
            char *end = nullptr;
            const float scale = text == "auto" ? 0.0f : std::strtof(text.c_str(), &end);
            if (text != "auto" && (end == text.c_str() || *end != '\0' || !(scale >= 0.25f && scale <= 2.0f))) {
                error = "--render-scale must be auto or a number from 0.25 to 2";
                return false;
            }
            options.renderScale = scale;
        } else if (arg == "--disable-mouse") {
            options.disableMouse = true;
        } else if (arg == "--disable-parallax") {
            options.disableParallax = true;
        } else if (arg == "--check") {
            if (!value(options.checkDir)) {
                return false;
            }
        } else if (arg.starts_with("--")) {
            logInfo("ignoring unknown option {}", arg);
        } else {
            error = "unexpected argument " + std::string(arg);
            return false;
        }
    }
    return true;
}

} // namespace murasaki::scene
