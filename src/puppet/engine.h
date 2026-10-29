#pragma once

#include "puppet/exitcodes.h"
#include "puppet/model.h"
#include "puppet/render.h"
#include "puppet/session.h"
#include "puppetlimits.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::puppet {

// The puppet engine process (docs/pets/puppets.md, section 19): loads a model, draws it on the GPU at the rate the host asks for, and hands the pictures to the overlay over the bridge. Nothing is
// drawn while the host says it is paused, and a picture is not redrawn when nothing that shows has changed.
//
struct EngineOptions {
    int bridgeFd = -1;
    uint32_t width = 0;  // the picture, pixels
    uint32_t height = 0;
    std::string itemDir; // the puppet folder
    std::string file;    // the `.inp` in it
    int fps = 60;
    std::uint64_t seed = 1;
    std::string device;  // Vulkan device choice (empty: automatic; the same strings as MURASAKI_VK_DEVICE)
    uint32_t maxLongSide = 2048; // kMaxPuppetPixels: the largest side of the picture
    // The budget of one frame (the limits table; the tests lower them). A frame whose triangles are estimated to cover more than `maxOverdraw` pictures is not drawn, and `maxHeavyFrames` of them in a row end the
    // engine with kExitModelRefused ("puppet: too heavy"). Only that estimate can give a model up. `maxSlowFrames` frames in a row whose GPU work took over `slowFrameMs` make the engine halve its own frame
    // rate (to a floor of kMinSlowFps) with a warning; on a software device the time is not watched.
    double maxOverdraw = static_cast<double>(kMaxOverdrawFactor);
    unsigned maxHeavyFrames = static_cast<unsigned>(kMaxHeavyFrames);
    int slowFrameMs = static_cast<int>(kMaxSlowFrameMs);
    unsigned maxSlowFrames = static_cast<unsigned>(kMaxSlowFrames);
};

// Whether a file name is an Inochi2D puppet's (`.inp`, in any case): the one format this build shows.
bool hasInpExtension(const std::string &name);

// Loads the model from the item folder (through openInItem) and runs. Returns an exit status above.
int runEngine(const EngineOptions &options);

// Runs a model the caller built (for the tests: a hand-made model and its textures). `pointSampling` as in PuppetRenderer::create.
int runEngineWithModel(const EngineOptions &options, std::unique_ptr<ModelBackend> model, std::vector<TextureImage> textures, bool pointSampling, SessionAssets assets = {});

} // namespace murasaki::puppet
