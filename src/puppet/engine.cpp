#include "puppet/engine.h"

#include "core/bridgesink.h"
#include "core/context.h"
#include "core/framebuffers.h"
#include "puppet/manifest.h"
#include "puppet/openformat.h"
#include "puppet/session.h"
#include "puppet/view.h"
#include "puppetlimits.h"
#include "util/log.h"

#include "bridge/puppetview.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace murasaki::puppet {

using namespace murasaki::scene;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kKeepAlive = std::chrono::seconds(1);


class Engine
{
public:
    Engine(const EngineOptions &options, std::unique_ptr<ModelBackend> model, std::vector<TextureImage> textures, bool pointSampling, SessionAssets assets)
        : m_options(options), m_session(std::make_unique<PuppetSession>(std::move(model), options.seed, std::move(assets))), m_textures(std::move(textures)), m_point(pointSampling),
          m_fps(std::clamp(options.fps, 1, 240))
    {
    }

    int run();

private:
    bool setup(int &status);
    void onMessage(const ControlMessage &message);
    void clampSize(uint32_t &w, uint32_t &h) const;
    int renderFrame(uint32_t buffer); // kExitOk, or the status the engine ends with

    EngineOptions m_options;
    std::unique_ptr<PuppetSession> m_session;
    std::vector<TextureImage> m_textures;
    bool m_point;
    std::unique_ptr<VkContext> m_ctx;
    std::unique_ptr<BridgeSink> m_sink;
    std::unique_ptr<PuppetRenderer> m_renderer;
    HitMaskTracker m_hitTracker;
    Frame m_frame;
    ViewTransform m_view;
    int m_fps;
    bool m_paused = false;
    bool m_quit = false;
    bool m_forceFrame = true;
    uint32_t m_renderedGeneration = ~0u;
    unsigned m_heavyFrames = 0; // frames in a row over the overdraw budget
    unsigned m_slowFrames = 0;  // frames in a row whose GPU work was slow
    unsigned m_fastFrames = 0;  // frames in a row that were not
    int m_slowFps = 0;          // the engine's own cap while its GPU work is slow (0: none)
    bool m_watchTime = true;    // off on a software device: its time is the CPU's, not the model's
    FrameCost m_cost;           // of the last frame that changed
    uint32_t m_costWidth = 0, m_costHeight = 0;
    unsigned m_presented = 0;   // frames drawn so far (a model that fails before the first is the model's fault)
    HitMask m_mask;             // the footprint of the last frame that changed
    uint32_t m_maskWidth = 0, m_maskHeight = 0;
    Clock::time_point m_lastPresent{};
    Clock::time_point m_lastStep{};
};

void Engine::clampSize(uint32_t &w, uint32_t &h) const
{
    const uint32_t longSide = std::max(w, h);
    if (longSide > m_options.maxLongSide && longSide > 0) {
        const double s = static_cast<double>(m_options.maxLongSide) / longSide;
        w = std::max<uint32_t>(1, static_cast<uint32_t>(std::floor(w * s)));
        h = std::max<uint32_t>(1, static_cast<uint32_t>(std::floor(h * s)));
    }
}

void Engine::onMessage(const ControlMessage &message)
{
    if (const auto *pause = std::get_if<bridge::msg::Pause>(&message)) {
        m_paused = pause->paused != 0;
        m_forceFrame = true;
    } else if (const auto *fps = std::get_if<bridge::msg::Fps>(&message)) {
        m_fps = static_cast<int>(std::clamp(fps->fps, 1u, 240u));
    } else if (const auto *pointer = std::get_if<bridge::msg::Pointer>(&message)) {
        m_session->pointer(pointer->x, pointer->y, m_sink->width(), m_sink->height());
    } else if (const auto *resize = std::get_if<bridge::msg::Resize>(&message)) {
        uint32_t w = resize->width, h = resize->height;
        clampSize(w, h);
        if (w != m_sink->width() || h != m_sink->height()) {
            if (!m_sink->resize(w, h))
                m_quit = true;
            m_forceFrame = true;
        }
    } else if (const auto *properties = std::get_if<PropertiesJson>(&message)) {
        std::string error;
        if (m_session->command(properties->json, error)) {
            if (const auto click = m_session->takeClick()) { // (the picture is the engine's: where the click fell is judged on the frame shown)
                const std::string what = m_session->applyClick(*click, m_frame, m_view, m_sink->width(), m_sink->height());
                if (!what.empty())
                    logInfo("puppet click: {}", what);
            }
            m_forceFrame = true;
        }
        else
            logInfo("puppet command refused: {}", error);
    } else if (std::holds_alternative<bridge::msg::Quit>(message)) {
        m_quit = true;
    }
}

bool Engine::setup(int &status)
{
    std::string error;
    ContextOptions copts = contextOptionsFromEnvironment();
    if (!m_options.device.empty())
        copts.device = m_options.device;
    m_ctx = VkContext::create(copts, error);
    if (!m_ctx) {
        logError("no Vulkan device: {}", error);
        status = kExitNoGpu;
        return false;
    }
    logInfo("puppet engine on {}", m_ctx->deviceName());
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_ctx->physicalDevice(), &props);
    m_watchTime = props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU; // (llvmpipe and the like: slow is the machine's, whatever the model)
    CreateFailure failure = CreateFailure::None;
    m_renderer = PuppetRenderer::create(*m_ctx, m_session->model(), m_textures, m_point, error, &failure);
    if (!m_renderer) {
        logError("renderer: {}", error);
        status = failure == CreateFailure::Model ? kExitModelRefused : kExitNoGpu; // (a texture the device has no memory for is this model's, not "no GPU")
        return false;
    }
    m_textures.clear();
    m_textures.shrink_to_fit();
    uint32_t w = m_options.width, h = m_options.height;
    clampSize(w, h);
    m_sink = std::make_unique<BridgeSink>(*m_ctx, bridge::Channel(bridge::UniqueFd(m_options.bridgeFd)));
    m_sink->setAnnouncedVersion(bridge::kPuppetVersion);
    m_sink->setHandler([this](const ControlMessage &m) { onMessage(m); });
    if (!m_sink->start(w, h, "puppet")) {
        logError("bridge: {}", m_sink->lastError());
        status = kExitFailure;
        return false;
    }
    m_lastStep = Clock::now();
    return true;
}

int Engine::renderFrame(uint32_t buffer)
{
    const auto now = Clock::now();
    m_session->step(std::chrono::duration<double>(now - m_lastStep).count());
    m_lastStep = now;
    bool changed = false;
    if (!m_session->evaluate(m_frame, changed)) {
        m_sink->sendError(kErrorRuntime, "the model refused the values");
        return m_presented == 0 ? kExitModelRefused : kExitFailure;
    }
    const uint32_t w = m_sink->width(), h = m_sink->height();
    m_view = fitBounds(m_session->model().bounds(), w, h);
    // The GPU's work is bounded before it is recorded: the triangles of the frame, masks and groups included, may cover only so many pictures. A frame over it is not drawn (the consumer keeps the last
    // one); a model that keeps doing it is given up on, the way a compositor sharing this GPU would want.
    if (changed || m_costWidth != w || m_costHeight != h) {
        m_cost = frameCost(m_frame, m_session->model(), m_view, w, h);
        m_costWidth = w;
        m_costHeight = h;
    }
    const FrameCost &cost = m_cost;
    if (cost.pixels > m_options.maxOverdraw * static_cast<double>(w) * h || cost.vertices > static_cast<std::uint64_t>(kMaxPositionVertices)) {
        if (++m_heavyFrames >= m_options.maxHeavyFrames) {
            logError("puppet: too heavy ({:.0f} pictures of overdraw)", cost.pixels / (static_cast<double>(w) * h));
            m_sink->sendError(kErrorRuntime, "puppet: too heavy");
            return kExitModelRefused;
        }
        return kExitOk;
    }
    m_heavyFrames = 0;
    FrameBufferSet &set = m_sink->buffers();
    const auto gpuStart = Clock::now();
    m_ctx->submitAndWait([&](VkCommandBuffer cb) {
        set.cmdAcquire(cb, buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_renderer->cmdDraw(cb, set.view(buffer), set.extent(), m_frame, m_view);
        set.cmdRelease(cb, buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
    // Slow GPU work is the device's as often as the model's (a game or the compositor using the GPU, a weak iGPU): the engine lowers its own rate and says so, it does not give the model up. Only the estimate of
    // the frame's area can do that. On a software device there is no watch.
    if (m_watchTime) {
        if (Clock::now() - gpuStart > std::chrono::milliseconds(m_options.slowFrameMs)) {
            m_fastFrames = 0;
            if (++m_slowFrames >= m_options.maxSlowFrames) {
                m_slowFrames = 0;
                const int current = m_slowFps > 0 ? m_slowFps : m_fps;
                const int lowered = std::max(static_cast<int>(kMinSlowFps), current / 2);
                if (lowered < current) {
                    m_slowFps = lowered;
                    logWarn("puppet: GPU frames take over {} ms; drawing at {} fps instead of {}", m_options.slowFrameMs, lowered, current);
                }
            }
        } else {
            m_slowFrames = 0;
            if (m_slowFps > 0 && ++m_fastFrames >= static_cast<unsigned>(kSlowRecoveryFrames)) {
                m_fastFrames = 0;
                m_slowFps = m_slowFps * 2 >= m_fps ? 0 : m_slowFps * 2;
                logInfo("puppet: GPU frames are fast again; drawing at {} fps", m_slowFps > 0 ? m_slowFps : m_fps);
            }
        }
    }
    set.finishFrame(buffer);
    ++m_presented;
    // The footprint is worked out again only when something that shows has changed, or the picture's size has.
    if (changed || m_maskWidth != w || m_maskHeight != h || m_mask.bits.empty()) {
        m_mask = hitMaskOf(m_frame, m_session->model(), m_view, w, h);
        m_maskWidth = w;
        m_maskHeight = h;
    }
    if (m_hitTracker.update(m_mask)) {
        const auto payload = bridge::encodeHitMask({m_mask.cols, m_mask.rows, m_mask.cell, m_sink->generation()}, m_mask.bits);
        // not sent (the socket was full): the tracker offers the same mask again at the next frame
        if (payload && !m_sink->sendRaw(bridge::kHitMaskType, *payload))
            m_hitTracker.reset();
    }
    switch (m_sink->present(buffer)) {
    case PresentResult::Sent:
    case PresentResult::Skipped:
        return kExitOk;
    case PresentResult::Closed:
        m_quit = true;
        return kExitOk;
    case PresentResult::Failed:
        break;
    }
    return kExitFailure;
}

int Engine::run()
{
    int status = kExitOk;
    auto nextFrame = Clock::now();
    try {
        if (!setup(status))
            return status;
        while (!m_quit) {
            const ServiceResult serviced = m_sink->service();
            if (serviced == ServiceResult::PeerClosed)
                break;
            if (serviced == ServiceResult::Failed) {
                status = kExitFailure;
                break;
            }
            if (m_quit)
                break;
            if (m_paused) { // hidden or paused: no drawing, no wake-ups but the socket and a slow check
                m_sink->waitForMessage(std::chrono::seconds(1));
                nextFrame = Clock::now();
                m_lastStep = nextFrame;
                continue;
            }
            const auto now = Clock::now();
            if (now < nextFrame) {
                m_sink->waitForMessage(nextFrame - now);
                continue;
            }
            const int fps = m_slowFps > 0 ? std::min(m_fps, m_slowFps) : m_fps;
            const auto interval = std::chrono::nanoseconds(1000000000LL / std::max(1, fps));
            nextFrame += interval;
            if (nextFrame < now)
                nextFrame = now + interval;
            // nothing that shows can change: the consumer keeps its picture; one frame a second still goes out
            const bool changedSet = m_sink->generation() != m_renderedGeneration;
            if (!m_forceFrame && !changedSet && !m_session->animated() && now - m_lastPresent < kKeepAlive) {
                m_sink->waitForMessage(std::min<Clock::duration>(kKeepAlive - (now - m_lastPresent), std::chrono::milliseconds(250)));
                nextFrame = Clock::now();
                continue;
            }
            if (const auto buffer = m_sink->acquire()) {
                m_forceFrame = false;
                m_renderedGeneration = m_sink->generation();
                m_lastPresent = Clock::now();
                const int result = renderFrame(*buffer);
                if (result == kExitModelRefused) {
                    status = result;
                    break;
                }
                if (result != kExitOk) {
                    logError("{}", m_sink->lastError());
                    m_sink->sendError(kErrorRuntime, m_sink->lastError().empty() ? "the frame failed" : m_sink->lastError());
                    status = result;
                    break;
                }
            }
        }
    } catch (const VkException &e) {
        logError("{}", e.what());
        if (m_sink)
            m_sink->sendError(kErrorRuntime, e.what());
        status = e.result() == VK_ERROR_DEVICE_LOST ? kExitDeviceLost : kExitFailure;
    } catch (const std::exception &e) {
        logError("{}", e.what());
        if (m_sink)
            m_sink->sendError(kErrorRuntime, e.what());
        status = kExitFailure;
    }
    return status;
}

} // namespace

bool hasInpExtension(const std::string &name)
{
    if (name.size() < 4)
        return false;
    std::string tail = name.substr(name.size() - 4);
    for (char &c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".inp";
}

int runEngineWithModel(const EngineOptions &options, std::unique_ptr<ModelBackend> model, std::vector<TextureImage> textures, bool pointSampling, SessionAssets assets)
{
    if (options.bridgeFd < 0 || options.width == 0 || options.height == 0 || !model)
        return kExitUsage;
    Engine engine(options, std::move(model), std::move(textures), pointSampling, std::move(assets));
    return engine.run();
}

int runEngine(const EngineOptions &options)
{
    if (options.bridgeFd < 0 || options.width == 0 || options.height == 0 || options.itemDir.empty() || options.file.empty()) {
        logError("the engine needs --murasaki-bridge-fd, --murasaki-size, --item and --file");
        return kExitUsage;
    }
    if (!hasInpExtension(options.file)) {
        logError("{}: only Inochi2D .inp puppets are shown by this build (a Cubism model needs the Cubism runtime)", options.file);
        return kExitModelRefused;
    }
    OpenLoadResult loaded = OpenModel::loadFile(options.itemDir, options.file);
    if (!loaded.model) {
        logError("the puppet cannot be shown: {}", loaded.error);
        return kExitModelRefused;
    }
    std::vector<TextureImage> textures;
    const OpenModelInfo &info = loaded.model->info();
    std::string budgetError;
    std::int64_t decodedPixels = 0;
    if (!textureBudgetOk(*loaded.model, budgetError)) {
        logError("{}", budgetError);
        return kExitModelRefused;
    }
    for (size_t i = 0; i < info.textures.size(); ++i) {
        const std::string_view bytes = loaded.model->textureBytes(static_cast<int>(i));
        TextureImage image;
        std::string error;
        if (!decodeTexture(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), image, error)) {
            logError("texture {}: {}", i, error);
            return kExitModelRefused;
        }
        // (the header can lie: the total is kept with what was really decoded as well)
        decodedPixels += static_cast<std::int64_t>(image.width) * image.height;
        if (decodedPixels > kMaxTotalTexturePixels) {
            logError("the textures are over kMaxTotalTexturePixels");
            return kExitModelRefused;
        }
        textures.push_back(std::move(image));
    }
    loaded.model->releaseFile(); // the encoded bytes (up to 256 MiB) are not needed again: the decoded pictures are what the renderer uploads
    const bool point = info.preservePixels;
    return runEngineWithModel(options, std::move(loaded.model), std::move(textures), point);
}

} // namespace murasaki::puppet
