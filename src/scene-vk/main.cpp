#include "engine/check.h"
#include "engine/engine.h"
#include "engine/options.h"
#include "util/heap.h"
#include "util/log.h"

#include <cstdio>

int main(int argc, char **argv)
{
    using namespace murasaki::scene;
    configureHeap();
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        logError("{}", error);
        std::fputs(usageText(), stderr);
        return 2;
    }
    if (options.showVersion) {
        std::printf("murasaki-scene-vk %s\n", kEngineVersion);
        return 0;
    }
    if (options.showHelp) {
        std::fputs(usageText(), stdout);
        return 0;
    }
    if (!options.checkDir.empty()) {
        return runCheck(options.checkDir, options.assetsDir);
    }
    if (options.bridgeFd < 0) {
        logError("--murasaki-bridge-fd is required");
        std::fputs(usageText(), stderr);
        return 2;
    }
    return runEngine(options);
}
