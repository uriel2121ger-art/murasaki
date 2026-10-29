#pragma once

// Command line of murasaki-scene-vk. It keeps the common scene engine contract so Murasaki's
// SceneEngine can launch the binary with these arguments:
//
//   --murasaki-bridge-fd N --murasaki-size WxH     host mode: the engine end of the bridge socketpair
//   --screen-root NAME --bg DIR --scaling MODE     the viewport name, the item directory and the fill mode
//   --assets-dir DIR                               Wallpaper Engine "assets" (found automatically when omitted)
//   --fps N  --volume N  --silent                  frame rate cap; volume 0..128 (the scene's sounds), --silent is accepted and ignored: Murasaki never passes it
//   --set-property KEY=VALUE                       initial user property (repeatable)
//   --disable-mouse  --disable-parallax            ignore the pointer / never move the camera with it
//
// and adds `--check DIR`, which parses an item, prints {"ok":bool,"reasons":[...]} and exits.

#include <string>
#include <utility>
#include <vector>

namespace murasaki::scene {

struct Options {
    int bridgeFd = -1;
    unsigned width = 1280;
    unsigned height = 720;
    std::string viewportName = "murasaki";
    std::string itemDir;
    std::string assetsDir;
    std::string scaling = "fill"; // fill | fit | stretch
    int fps = 30;
    int volume = 0;
    std::vector<std::pair<std::string, std::string>> properties;
    std::string checkDir; // --check
    bool disableMouse = false;
    float renderScale = 0.0f;   // --render-scale: target pixels per output pixel, fixed; 0 (or auto): the engine adapts it
    bool disableParallax = false;
    bool showVersion = false;
    bool showHelp = false;
};

// Returns false and fills `error` on a malformed command line. Unknown options are logged and skipped, which keeps
// the engine launchable by a newer Murasaki.
bool parseOptions(int argc, const char *const *argv, Options &options, std::string &error);

const char *usageText();

} // namespace murasaki::scene
