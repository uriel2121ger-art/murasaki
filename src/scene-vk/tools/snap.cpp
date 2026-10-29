// scene-vk-snap ITEMDIR OUT.pam WxH [--pointer X,Y] [--frames N] [--step SECONDS] [--at SECONDS] [--no-parallax] [--no-defaults]: renders an item offscreen and writes the
// last frame as a PAM file. The pointer is in output pixels, from the top left, and is set before the last frame.
// Frame f is drawn at scene time f * step; with --at the last frame is drawn at exactly SECONDS (the others evenly from 0, a single
// frame at SECONDS), so a snapshot of any moment of the scene is one command.
// Development tool: checks what the engine draws for a real item without a bridge.
#include "assets/texcache.h"
#include "assets/vfs.h"
#include "core/chunkedsubmit.h"
#include "core/context.h"
#include "core/framebuffers.h"
#include "render/scenerenderer.h"
#include "scene/scene.h"
#include "util/json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace murasaki::scene {
extern uint64_t g_particleSteps;
}
using namespace murasaki::scene;

int main(int argc, char **argv)
{
    const auto started = std::chrono::steady_clock::now();
    if (argc < 4) {
        std::fprintf(stderr, "usage: scene-vk-snap ITEMDIR OUT.pam WxH [--pointer X,Y] [--frames N] [--step SECONDS] [--at SECONDS] [--no-parallax]\n");
        return 2;
    }
    unsigned width = 0;
    unsigned height = 0;
    if (std::sscanf(argv[3], "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
        std::fprintf(stderr, "bad size\n");
        return 2;
    }
    int pointerX = -1;
    int pointerY = -1;
    int frames = 2;
    double step = 0.25;
    double at = -1.0;
    bool parallax = true;
    bool noDefaults = false;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pointer") && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d", &pointerX, &pointerY);
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = std::max(1, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--no-defaults")) {
            noDefaults = true;
        } else if (!std::strcmp(argv[i], "--no-parallax")) {
            parallax = false;
        } else if (!std::strcmp(argv[i], "--step") && i + 1 < argc) {
            step = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--at") && i + 1 < argc) {
            at = std::max(0.0, std::atof(argv[++i]));
        }
    }

    std::string error;
    const Vfs vfs = Vfs::forItem(argv[1], findAssetsDir({}, argv[1]));
    // the scene file project.json names ("gifscene.json" in some items), as the engine and --check read it
    auto scene = loadScene(vfs, sceneFileName(vfs), &error);
    if (!scene) {
        std::fprintf(stderr, "scene: %s\n", error.c_str());
        return 1;
    }
    auto ctx = VkContext::create(contextOptionsFromEnvironment(), error);
    if (!ctx) {
        std::fprintf(stderr, "vulkan: %s\n", error.c_str());
        return 1;
    }
    try {
        PropertyStore properties;
        // what the Murasaki GUI sends at launch: the project's own defaults (a scene's bindings fall back to their own values otherwise)
        if (!noDefaults) {
            applyProjectDefaults(vfs, properties);
        }
        SceneRenderer renderer(*ctx, std::move(*scene), vfs, properties);
        renderer.setParallaxEnabled(parallax);
        renderer.setScaling(murasaki::bridge::msg::ScalingMode::Fill);
        auto set = FrameBufferSet::create(*ctx, width, height, Transport::Shm, error);
        if (!set) {
            std::fprintf(stderr, "buffers: %s\n", error.c_str());
            return 1;
        }
        double gpuMsSum = 0.0;
        int gpuMsCount = 0;
        // the engine's way of sending a frame: in pieces (core/chunkedsubmit.h), one fence for the lot
        ChunkedSubmit submit(*ctx);
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCheck(vkCreateFence(ctx->device(), &fci, nullptr, &fence), "vkCreateFence");
        size_t piecesSum = 0;
        for (int f = 0; f < frames; ++f) {
            if (pointerX >= 0 && f == frames - 1) {
                renderer.setPointer(pointerX, pointerY, 0, set->extent());
            }
            VkCommandBuffer cb = submit.begin();
            set->cmdAcquire(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            FrameParams params;
            params.cb = cb;
            params.split = [&](VkCommandBuffer full) { return cb = submit.split(full); };
            params.image = set->image(0);
            params.view = set->view(0);
            params.extent = set->extent();
            params.timeSeconds = at >= 0.0 ? (frames == 1 ? at : at * f / (frames - 1)) : f * step;
            renderer.render(params);
            set->cmdRelease(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            submit.finish(cb, fence);
            vkCheck(vkWaitForFences(ctx->device(), 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences");
            vkCheck(vkResetFences(ctx->device(), 1, &fence), "vkResetFences");
            piecesSum += submit.pieces();
            set->finishFrame(0);
            renderer.frameCompleted();
            if (const double ms = renderer.gpuFrameMs(); ms >= 0.0 && f >= frames / 2) {
                gpuMsSum += ms;
                ++gpuMsCount;
            }
        }
        vkDestroyFence(ctx->device(), fence, nullptr);
        std::fprintf(stderr, "gpu-ms-per-frame %.3f over %d frames\n", gpuMsCount ? gpuMsSum / gpuMsCount : -1.0, gpuMsCount);
        std::fprintf(stderr, "pieces-per-frame %.2f\n", frames > 0 ? static_cast<double>(piecesSum) / frames : 0.0);
        std::fprintf(stderr, "counters: draws %llu particle-steps %llu\n", static_cast<unsigned long long>(renderer.draws()), static_cast<unsigned long long>(g_particleSteps));
        const auto passOpt = renderer.passOptStats();
        std::fprintf(stderr, "pass-opt: skipped %llu reduced %llu partial %llu probes %llu\n", static_cast<unsigned long long>(passOpt.skipped),
                     static_cast<unsigned long long>(passOpt.reduced), static_cast<unsigned long long>(passOpt.partial), static_cast<unsigned long long>(passOpt.probes));
        std::fprintf(stderr, "scene passes %llu, animated %d, uses pointer %d\n", static_cast<unsigned long long>(renderer.scenePasses()), renderer.animated(),
                     renderer.usesPointer());
        for (const std::string &name : renderer.unknownUniforms()) {
            std::fprintf(stderr, "unknown uniform %s\n", name.c_str());
        }
        const auto bytes = set->ring().frame(0);
        std::ofstream out(argv[2], std::ios::binary);
        out << "P7\nWIDTH " << width << "\nHEIGHT " << height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "render: %s\n", e.what());
        return 1;
    }
    // how long the load and the frame took (what a user waits for), then the decoded-picture cache's background writes finish
    std::fprintf(stderr, "snap-seconds %.3f\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    flushCachedImageStores();
    return 0;
}
