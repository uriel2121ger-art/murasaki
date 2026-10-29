#include "assets/videodecoder.h"

#include "assets/videoapi.h"

#ifdef MURASAKI_SCENE_VK_VIDEO
#include <dlfcn.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#endif

namespace murasaki::scene {

#ifdef MURASAKI_SCENE_VK_VIDEO

namespace {

// Where the plugin may be: named by the environment (the tests, a development tree), next to the engine, in the lib directory beside
// its bin directory, and last where the build left it.
std::vector<std::string> pluginCandidates()
{
    std::vector<std::string> out;
    if (const char *named = std::getenv("MURASAKI_SCENE_VK_VIDEO_PLUGIN"); named && named[0] != '\0') {
        out.emplace_back(named);
    }
    char exe[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
        out.push_back((dir / "libmurasaki-scene-vk-video.so").string());
        out.push_back((dir / ".." / "lib" / "murasaki" / "libmurasaki-scene-vk-video.so").lexically_normal().string());
        out.push_back((dir / ".." / MURASAKI_SCENE_VK_LIBDIR / "murasaki" / "libmurasaki-scene-vk-video.so").lexically_normal().string());
    }
#ifdef MURASAKI_SCENE_VK_VIDEO_BUILD_PLUGIN
    out.emplace_back(MURASAKI_SCENE_VK_VIDEO_BUILD_PLUGIN);
#endif
    return out;
}

// The plugin's interface, loaded on the first ask (a scene without video never pays for the libraries behind it); null when it cannot be.
const MurasakiVideoApi *api()
{
    static const MurasakiVideoApi *loaded = [] () -> const MurasakiVideoApi * {
        if (const char *named = std::getenv("MURASAKI_SCENE_VK_VIDEO_PLUGIN"); named && std::string(named) == "none") {
            return nullptr; // no video at all (a test of the engine without its plugin; a user who does not want libav loaded)
        }
        for (const std::string &path : pluginCandidates()) {
            if (!std::filesystem::exists(path)) {
                continue;
            }
            void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                continue;
            }
            using Entry = const MurasakiVideoApi *(*)();
            if (auto entry = reinterpret_cast<Entry>(::dlsym(handle, "murasaki_video_api"))) {
                if (const MurasakiVideoApi *found = entry(); found && found->version == kMurasakiVideoApiVersion) {
                    return found;
                }
            }
            ::dlclose(handle);
        }
        return nullptr;
    }();
    return loaded;
}

} // namespace

#else

namespace {
const MurasakiVideoApi *api()
{
    return nullptr;
}
} // namespace

#endif

bool videoSupported()
{
    return api() != nullptr;
}

struct VideoDecoder::Impl {
    std::vector<uint8_t> bytes; // the stream, held for the plugin's decoder
    void *handle = nullptr;
    ~Impl()
    {
        if (handle) {
            api()->close(handle);
        }
    }
};

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(std::vector<uint8_t> bytes, std::string &error, const Options &options)
{
    const MurasakiVideoApi *plugin = api();
    if (!plugin) {
        error = "this build has no video decoder (libav plugin not found)";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->bytes = std::move(bytes);
    char why[256] = {};
    impl->handle = plugin->open(impl->bytes.data(), impl->bytes.size(), options.renderNode.c_str(), (options.hardware ? kMurasakiVideoHardware : 0u) | (options.hardware && options.zeroCopy ? kMurasakiVideoZeroCopy : 0u), why, sizeof(why));
    if (!impl->handle) {
        error = why[0] ? why : "the video cannot be played";
        return nullptr;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

std::unique_ptr<VideoDecoder> VideoDecoder::openFile(const std::string &path, uint64_t offset, uint64_t size, std::string &error, const Options &options)
{
    const MurasakiVideoApi *plugin = api();
    if (!plugin) {
        error = "this build has no video decoder (libav plugin not found)";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    char why[256] = {};
    impl->handle = plugin->openFile(path.c_str(), offset, size, options.renderNode.c_str(),
                                    (options.hardware ? kMurasakiVideoHardware : 0u) | (options.hardware && options.zeroCopy ? kMurasakiVideoZeroCopy : 0u), why, sizeof(why));
    if (!impl->handle) {
        error = why[0] ? why : "the video cannot be played";
        return nullptr;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

std::unique_ptr<VideoDecoder> VideoDecoder::open(std::vector<uint8_t> bytes, std::string &error)
{
    return open(std::move(bytes), error, Options{});
}

uint32_t VideoDecoder::width() const
{
    return api()->width(m_impl->handle);
}

uint32_t VideoDecoder::height() const
{
    return api()->height(m_impl->handle);
}

double VideoDecoder::duration() const
{
    return api()->duration(m_impl->handle);
}

bool VideoDecoder::hardware() const
{
    return api()->hardware(m_impl->handle) != 0;
}

void VideoDecoder::setMaxFps(double fps)
{
    api()->setMaxFps(m_impl->handle, fps);
}

const uint8_t *VideoDecoder::frameAt(double seconds)
{
    return api()->frameAt(m_impl->handle, seconds);
}

MurasakiVideoExport VideoDecoder::exportFrame(double seconds)
{
    MurasakiVideoExport out{};
    out.fd = -1;
    out.status = -1;
    if (api()->exportFrame) {
        api()->exportFrame(m_impl->handle, seconds, &out);
    }
    return out;
}

void VideoDecoder::seek(double seconds)
{
    api()->seek(m_impl->handle, seconds);
}

void VideoDecoder::setLoop(bool loop)
{
    api()->setLoop(m_impl->handle, loop ? 1 : 0);
}

double VideoDecoder::position() const
{
    return api()->position(m_impl->handle);
}

bool VideoDecoder::ended() const
{
    return api()->ended(m_impl->handle) != 0;
}

} // namespace murasaki::scene
