#include "engine/engine.h"
#include "engine/fpscontrol.h"

#include "core/bridgesink.h"
#include "core/chunkedsubmit.h"
#include "core/context.h"
#include "assets/vfs.h"
#include "render/renderer.h"
#include "render/scenerenderer.h"
#include "scene/properties.h"
#include "scene/scene.h"
#include "util/heap.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace murasaki::scene {

namespace msg = bridge::msg;
using Clock = std::chrono::steady_clock;
constexpr Clock::duration kKeepAlive = std::chrono::seconds(1);

namespace {

// The fixed resolution the user asked for: --render-scale, else MURASAKI_SCENE_VK_RENDER_SCALE; 0: automatic.
double fixedScale(const Options &options)
{
    if (options.renderScale > 0.0f) {
        return options.renderScale;
    }
    if (const char *text = std::getenv("MURASAKI_SCENE_VK_RENDER_SCALE"); text && text[0] != '\0') {
        const double value = std::strtod(text, nullptr);
        if (value >= 0.25 && value <= 2.0) {
            return value;
        }
    }
    return 0.0;
}

// MURASAKI_SCENE_VK_GPU_BUDGET: the share of each frame interval the GPU may take before the resolution is lowered (0
// turns the adaptive resolution off); the default is in ScaleController::Config.
ScaleController::Config scaleConfig(const Options &options);

// The frame rate steps down when the GPU cannot keep up (85% of the interval). With the automatic resolution, which already
// holds the GPU to a quarter of the interval where it can, a scene that still takes over a third of it at the lowest
// resolution loses frame rate too: 35% steps down, and the next rate up is tried when it would load under 25%.
FpsController::Config fpsConfig(const Options &options)
{
    FpsController::Config config;
    if (fixedScale(options) <= 0.0 && scaleConfig(options).budget > 0.0) {
        config.downLoad = 0.35;
        config.upLoad = 0.25;
    }
    return config;
}

ScaleController::Config scaleConfig(const Options &options)
{
    ScaleController::Config config;
    if (fixedScale(options) > 0.0) {
        config.budget = 0.0; // a resolution was chosen: it stays
        return config;
    }
    if (const char *text = std::getenv("MURASAKI_SCENE_VK_GPU_BUDGET"); text && text[0] != '\0') {
        char *end = nullptr;
        const double value = std::strtod(text, &end);
        if (end != text && value >= 0.0 && value <= 1.0) {
            config.budget = value;
        }
    }
    return config;
}

std::string joinReasons(const std::vector<std::string> &reasons)
{
    std::string out;
    for (const std::string &r : reasons) {
        out += (out.empty() ? "" : "; ") + r;
    }
    return out;
}

bool forcedByEnvironment()
{
    const char *forced = std::getenv("MURASAKI_SCENE_VK_FORCE");
    return forced && forced[0] != '\0' && std::strcmp(forced, "0") != 0;
}

std::unique_ptr<Renderer> createRenderer(VkContext &ctx, const Options &options, const PropertyStore &props, std::string &error)
{
    if (options.itemDir.empty()) {
        error = "--bg DIR is required";
        return nullptr;
    }
    const std::string assets = findAssetsDir(options.assetsDir, options.itemDir);
    if (assets.empty()) {
        logInfo("Wallpaper Engine assets directory not found: items that need it will fail");
    }
    std::string packageError;
    const Vfs vfs = Vfs::forItem(options.itemDir, assets, &packageError);
    if (!packageError.empty()) {
        logError("{}", packageError);
    }
    auto scene = loadScene(vfs, sceneFileName(vfs), &error);
    if (!scene) {
        return nullptr;
    }
    if (!scene->supported()) {
        if (!forcedByEnvironment()) {
            error = "unsupported scene: " + joinReasons(scene->unsupported);
            return nullptr;
        }
        logInfo("MURASAKI_SCENE_VK_FORCE is set, drawing only what is supported ({})", joinReasons(scene->unsupported));
    }
    if (!scene->degraded.empty()) {
        logInfo("drawing without: {}", joinReasons(scene->degraded));
    }
    scene->scriptStorageFile = scriptStorageFile(options.itemDir); // the scripts' localStorage outlives the run
    try {
        auto renderer = std::make_unique<SceneRenderer>(ctx, std::move(*scene), vfs, props);
        renderer->setPointerEnabled(!options.disableMouse);
        renderer->setParallaxEnabled(!options.disableParallax);
        if (fixedScale(options) > 0.0) {
            renderer->setRenderScale(static_cast<float>(fixedScale(options)));
        }
        return renderer;
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}

class Engine
{
public:
    explicit Engine(const Options &options) : m_options(options), m_fps(options.fps, fpsConfig(options)), m_scale(fixedScale(options) > 0.0 ? fixedScale(options) : 1.0, scaleConfig(options)) {}
    ~Engine();
    int run();

private:
    bool setup();
    void handleControl(const ControlMessage &message);
    bool renderFrame(uint32_t buffer);

    Options m_options;
    PropertyStore m_props;
    std::unique_ptr<VkContext> m_ctx;
    std::unique_ptr<BridgeSink> m_sink;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<ChunkedSubmit> m_submit; // the frame's command buffers, sent in pieces (core/chunkedsubmit.h)
    VkFence m_fence = VK_NULL_HANDLE;
    Clock::time_point m_start = Clock::now();
    FpsController m_fps;
    ScaleController m_scale;
    bool m_adaptiveFps = std::getenv("MURASAKI_SCENE_VK_ADAPTIVE_FPS") == nullptr || std::strcmp(std::getenv("MURASAKI_SCENE_VK_ADAPTIVE_FPS"), "0") != 0;
    bool m_realFrame = false; // the frame being drawn changes the picture (not a keep-alive of a still scene)
    bool m_paused = false;
    bool m_quit = false;
    std::optional<std::chrono::steady_clock::time_point> m_lastShortcutLaunch; // the last request a script made to the host
    bool m_forceFrame = true;          // draw the next frame whatever the renderer says (first frame, unpause, new mode)
    bool m_skipStill = std::getenv("MURASAKI_SCENE_VK_REDRAW_ALWAYS") == nullptr;
    uint32_t m_renderedGeneration = ~0u;
    Clock::time_point m_lastPresent = Clock::now();
    bool m_started = false; // setup() finished: errors from here on are runtime errors
    uint64_t m_frames = 0;
};

Engine::~Engine()
{
    if (m_ctx && m_ctx->device()) {
        vkDeviceWaitIdle(m_ctx->device());
        if (m_fence) {
            vkDestroyFence(m_ctx->device(), m_fence, nullptr);
        }
        m_submit.reset();
    }
    m_renderer.reset();
    m_sink.reset();
    m_ctx.reset();
}

bool Engine::setup()
{
    bridge::Channel channel{bridge::UniqueFd(m_options.bridgeFd)};
    if (!channel.valid()) {
        logError("--murasaki-bridge-fd is required in host mode");
        return false;
    }
    std::string error;
    m_ctx = VkContext::create(contextOptionsFromEnvironment(), error);
    if (!m_ctx) {
        // Without a device there is no sink, but the consumer can still be told why.
        logError("{}", error);
        msg::Error e{};
        e.code = kErrorInit;
        bridge::setString(e.text, error);
        channel.send(e);
        return false;
    }
    m_sink = std::make_unique<BridgeSink>(*m_ctx, std::move(channel));
    for (const auto &[key, value] : m_options.properties) {
        m_props.setFromText(key, value);
    }
    // what nobody has set reads as the project's own default, as in Wallpaper Engine
    applyProjectDefaults(Vfs::forItem(m_options.itemDir, {}), m_props);
    m_renderer = createRenderer(*m_ctx, m_options, m_props, error);
    if (!m_renderer) {
        logError("{}", error);
        m_sink->sendError(kErrorInit, error);
        return false;
    }
    trimHeap(); // the pictures are decoded and uploaded: what the decoders freed goes back to the system
    m_renderer->setVolume(static_cast<unsigned>(std::clamp(m_options.volume, 0, 128)));
    logInfo("fps: {} requested, adaptive {}", m_fps.requested(), m_adaptiveFps ? "on" : "off");
    m_sink->setHandler([this](const ControlMessage &m) { handleControl(m); });
    if (!m_sink->start(m_options.width, m_options.height, m_options.viewportName)) {
        logError("{}", m_sink->lastError());
        m_sink->sendError(kErrorInit, m_sink->lastError());
        return false;
    }

    m_submit = std::make_unique<ChunkedSubmit>(*m_ctx);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(m_ctx->device(), &fci, nullptr, &m_fence), "vkCreateFence");

    const std::string mode = m_options.scaling;
    m_renderer->setScaling(mode == "fit" ? msg::ScalingMode::Fit : mode == "stretch" ? msg::ScalingMode::Stretch : msg::ScalingMode::Fill);
    m_sink->sendMediaWants(m_renderer->wantsMedia());
    // engine.openUserShortcut(name) asks the host to start the application the user chose; the host judges the request. At most one a
    // 500 ms here, so a script cannot fill the socket (the host has its own limits).
    m_renderer->setShortcutLaunchHandler([this](const std::string &name) {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastShortcutLaunch && now - *m_lastShortcutLaunch < std::chrono::milliseconds(500)) {
            return false;
        }
        if (!m_sink->sendShortcutLaunch(name)) {
            return false;
        }
        m_lastShortcutLaunch = now;
        return true;
    });
    m_started = true;
    return true;
}

void Engine::handleControl(const ControlMessage &message)
{
    std::visit(
        [this](const auto &m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, msg::Pause>) {
                m_paused = m.paused != 0;
                m_forceFrame = true;
                m_renderer->setPaused(m_paused);
            } else if constexpr (std::is_same_v<T, msg::Pointer>) {
                m_renderer->setPointer(m.x, m.y, m.buttons, m_sink->buffers().extent());
            } else if constexpr (std::is_same_v<T, msg::Resize>) {
                if (!m_sink->resize(m.width, m.height)) {
                    logError("{}", m_sink->lastError());
                    m_sink->sendError(kErrorRuntime, m_sink->lastError());
                    m_quit = true;
                }
            } else if constexpr (std::is_same_v<T, msg::Volume>) {
                m_renderer->setVolume(m.volume);
            } else if constexpr (std::is_same_v<T, msg::Scaling>) {
                m_renderer->setScaling(static_cast<msg::ScalingMode>(m.mode));
                m_forceFrame = true;
            } else if constexpr (std::is_same_v<T, msg::SpanView>) {
                m_renderer->setSpanView(m);
                m_forceFrame = true;
            } else if constexpr (std::is_same_v<T, msg::Fps>) {
                m_fps.setRequested(static_cast<int>(std::clamp(m.fps, 1u, 1000u)));
                logInfo("fps: {} requested", m_fps.requested());
            } else if constexpr (std::is_same_v<T, msg::Quit>) {
                logInfo("quit requested by the bridge peer");
                m_quit = true;
            } else if constexpr (std::is_same_v<T, MediaJson>) {
                if (const auto state = parseMediaState(m.json)) {
                    m_renderer->setMedia(*state);
                } else {
                    logError("ignoring Media: the payload is not a JSON object");
                }
                m_forceFrame = true;
            } else if constexpr (std::is_same_v<T, MediaThumbnailData>) {
                m_renderer->setMediaThumbnail(m.width, m.height, m.rgba);
                m_forceFrame = true;
            } else if constexpr (std::is_same_v<T, ShortcutIconData>) {
                m_renderer->setShortcutIcon(m.name, m.width, m.height, m.rgba);
                m_forceFrame = true;
            } else if constexpr (std::is_same_v<T, PropertiesJson>) {
                if (!m_props.setFromJson(m.json)) {
                    logError("ignoring Properties: the payload is not a JSON object");
                }
            }
        },
        message);
}

bool Engine::renderFrame(uint32_t buffer)
{
    FrameBufferSet &set = m_sink->buffers();
    VkCommandBuffer cb = m_submit->begin();
    set.cmdAcquire(cb, buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_renderer->setFps(m_fps.current());
    FrameParams params;
    params.cb = cb;
    params.split = [&](VkCommandBuffer full) { return cb = m_submit->split(full); };
    params.image = set.image(buffer);
    params.view = set.view(buffer);
    params.extent = set.extent();
    params.timeSeconds = std::chrono::duration<double>(Clock::now() - m_start).count();
    m_renderer->render(params);
    set.cmdRelease(cb, buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_submit->finish(cb, m_fence);
    // The consumer samples the buffer as soon as it hears about it: the frame has to be complete first.
    vkCheck(vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");
    vkCheck(vkResetFences(m_ctx->device(), 1, &m_fence), "vkResetFences");
    set.finishFrame(buffer);
    m_renderer->frameCompleted();
    if (m_adaptiveFps && m_realFrame) {
        const double gpuMs = m_renderer->gpuFrameMs();
        if (gpuMs >= 0.0) {
            const double load = gpuMs * static_cast<double>(m_fps.current()) / 1000.0;
            if (const auto scale = m_scale.update(load, m_fps.current() == m_fps.requested())) {
                m_renderer->setRenderScale(static_cast<float>(*scale));
                m_scale.reset();
                logInfo("adaptive resolution: {:.2f} (GPU {:.1f} ms per frame, {:.0f}% of the frame interval)", *scale, gpuMs, load * 100.0);
            } else if (m_scale.atFloor() || !m_scale.enabled()) {
                // the resolution cannot give more: the frame rate does
                if (const auto stepped = m_fps.update(gpuMs)) {
                    m_scale.reset();
                    logInfo("adaptive fps: {} fps (requested {}, GPU {:.1f} ms per frame)", *stepped, m_fps.requested(), gpuMs);
                }
            }
        }
    }
    // Test hook (tests/tst_scenevk_engine.cpp): behave as if the GPU was lost after the first frames.
    if (m_frames == 0)
        trimHeap(); // the first frame builds the lazily made targets and pipelines
    if (++m_frames == 3) {
        if (const char *fault = std::getenv("MURASAKI_SCENE_VK_FAULT"); fault && std::strcmp(fault, "devicelost") == 0) {
            throw VkException("injected fault", VK_ERROR_DEVICE_LOST);
        }
    }

    switch (m_sink->present(buffer)) {
    case PresentResult::Sent:
    case PresentResult::Skipped:
        return true;
    case PresentResult::Closed:
        logInfo("bridge peer closed the socket");
        m_quit = true;
        return true;
    case PresentResult::Failed:
        break;
    }
    return false;
}

int Engine::run()
{
    int status = 0;
    auto nextFrame = Clock::now();
    try {
        if (!setup()) {
            return 1;
        }
        while (!m_quit) {
            const ServiceResult serviced = m_sink->service();
            if (serviced == ServiceResult::PeerClosed) {
                logInfo("bridge peer closed the socket, quitting");
                break;
            }
            if (serviced == ServiceResult::Failed) {
                status = 1;
                break;
            }
            if (m_quit) {
                break;
            }
            if (m_paused) {
                m_sink->waitForMessage(std::chrono::seconds(1));
                nextFrame = Clock::now();
                continue;
            }
            const auto now = Clock::now();
            if (now < nextFrame) {
                m_sink->waitForMessage(nextFrame - now);
                continue;
            }
            const auto interval = std::chrono::nanoseconds(1000000000LL / std::max(1, m_fps.current()));
            nextFrame += interval;
            if (nextFrame < now) {
                nextFrame = now + interval; // fell behind: do not try to catch up
            }
            // Nothing changed and nothing depends on time: the consumer keeps the last frame. One frame per kKeepAlive still
            // goes out, so that a consumer (or a test) waiting for a frame never waits long.
            const bool changedSet = m_sink->generation() != m_renderedGeneration;
            if (m_skipStill && !m_forceFrame && !changedSet && !m_renderer->needsFrame() && now - m_lastPresent < kKeepAlive) {
                m_sink->waitForMessage(std::min<Clock::duration>(kKeepAlive - (now - m_lastPresent), std::chrono::milliseconds(250)));
                nextFrame = Clock::now();
                continue;
            }
            // A buffer the consumer still holds is not drawn into: skip the frame.
            if (const auto buffer = m_sink->acquire()) {
                m_forceFrame = false;
                m_realFrame = m_renderer->needsFrame();
                m_renderedGeneration = m_sink->generation();
                m_lastPresent = Clock::now();
                if (!renderFrame(*buffer)) {
                    logError("{}", m_sink->lastError());
                    m_sink->sendError(kErrorRuntime, m_sink->lastError());
                    status = 1;
                    break;
                }
            }
        }
    } catch (const VkException &e) {
        // A lost device cannot be recovered in this process: a distinct status lets the bridge's restart policy
        // tell it from an ordinary failure.
        logError("{}", e.what());
        if (m_sink) {
            m_sink->sendError(m_started ? kErrorRuntime : kErrorInit, e.what());
        }
        status = e.result() == VK_ERROR_DEVICE_LOST ? kExitDeviceLost : 1;
    } catch (const std::exception &e) {
        logError("{}", e.what());
        if (m_sink) {
            m_sink->sendError(m_started ? kErrorRuntime : kErrorInit, e.what());
        }
        status = 1;
    }
    return status;
}

} // namespace

int runEngine(const Options &options)
{
    Engine engine(options);
    return engine.run();
}

} // namespace murasaki::scene
