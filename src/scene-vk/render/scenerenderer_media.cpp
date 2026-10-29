#include "render/scenerenderer.h"
#include "render/scenerenderer_internal.h"

#include "assets/texfile.h"
#include "assets/imagedecode.h"
#include "util/parallel.h"
#include "render/puppetdraw.h"

#include "core/framebuffers.h"
#include "render/gpuprofiler.h"
#include "render/shake.h"
#include "render/sprite.h"
#include "render/viewmapping.h"
#include "util/log.h"


#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <set>
#include <stdexcept>

namespace murasaki::scene {

namespace {
SceneRenderer::AudioBackendFactory &audioFactory()
{
    static SceneRenderer::AudioBackendFactory factory = [] { return makePipeWireBackend(); };
    return factory;
}
} // namespace

void SceneRenderer::setAudioBackendFactory(AudioBackendFactory factory)
{
    audioFactory() = factory ? std::move(factory) : [] { return makePipeWireBackend(); };
}

void SceneRenderer::startAudio()
{
    std::vector<SoundTrack> tracks;
    for (const SoundObject &s : m_scene.sounds) {
        SoundTrack t;
        t.name = s.name;
        t.volume = resolve(s.volume, m_props);
        t.loop = s.loop;
        t.random = s.random;
        t.minDelay = s.minTime;
        t.maxDelay = s.maxTime;
        // each file is read when its turn comes and dropped when it is over; the loader holds its own copy of the file system
        // (the package is shared), since the player's thread asks for them
        const auto vfs = std::make_shared<const Vfs>(m_vfs);
        for (const std::string &file : s.files) {
            t.sources.push_back([vfs, file]() { return vfs->read(file); });
        }
        tracks.push_back(std::move(t));
    }
    m_usesAudio = m_usesAudio || (m_scripts && m_scripts->usesAudio());
    if (const char *off = std::getenv("MURASAKI_SCENE_VK_NO_AUDIO"); off && off[0] == '1') {
        return; // (measurements: the spectrum follows the wall clock, which makes two runs of a scene differ)
    }
    if (tracks.empty() && !m_usesAudio) {
        return;
    }
    m_audio = std::make_unique<SceneAudio>(std::move(tracks), m_usesAudio, audioFactory()());
    m_audio->setVolume(m_volume);
}

void SceneRenderer::setVolume(unsigned bridgeVolume)
{
    m_volume = bridgeVolume;
    if (m_audio) {
        m_audio->setVolume(bridgeVolume);
    }
}

void SceneRenderer::setFps(int fps)
{
    if (fps == m_fps) {
        return;
    }
    m_fps = fps;
    for (VideoTexture &v : m_videos) {
        v.decoder->setMaxFps(fps);
    }
}

void SceneRenderer::setPaused(bool paused)
{
    m_audioPaused = paused;
    if (m_audio) {
        m_audio->setPaused(paused);
    }
}

namespace {

constexpr const char *kMediaCurrent = "$mediaThumbnail";
constexpr const char *kMediaPrevious = "$mediaPreviousThumbnail";
constexpr const char *kShortcutPrefix = "$shortcut:"; // + the property key: the icon of the application the user chose
constexpr size_t kMaxShortcutIcons = 64;

} // namespace

void SceneRenderer::applyMediaSlots()
{
    const auto rewrite = [&](std::vector<std::string> &textures, std::vector<MediaBinding> &media) {
        for (MediaBinding &b : media) {
            if (textures.size() <= b.slot) {
                textures.resize(b.slot + 1);
            }
            if (!b.captured) {
                b.placeholder = textures[b.slot]; // what the file lists there, shown while there is no cover
                b.captured = true;
            }
            const std::string cover = !b.shortcut.empty() ? kShortcutPrefix + b.shortcut : b.previous ? kMediaPrevious : kMediaCurrent;
            textures[b.slot] = textureNamed(cover) ? cover : b.placeholder;
        }
    };
    for (ImageLayer &layer : m_scene.layers) {
        rewrite(layer.base.textures, layer.base.media);
        for (EffectInstance &fx : layer.effects) {
            for (EffectPass &pass : fx.def.passes) {
                rewrite(pass.material.textures, pass.material.media);
            }
            for (PassOverride &o : fx.overrides) {
                rewrite(o.textures, o.media);
            }
        }
    }
    for (MeshLayer &mesh : m_scene.meshes) {
        for (MaterialPass &pass : mesh.materials) {
            rewrite(pass.textures, pass.media);
        }
    }
    m_dirty = true; // the layers kept in their buffers draw again with the new picture
}

void SceneRenderer::setMediaThumbnail(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
{
    const auto retire = [&](const char *name) {
        if (auto it = m_textures.find(name); it != m_textures.end()) {
            m_retiredTextures.emplace_back(m_frameIndex, std::move(it->second));
            m_textures.erase(it);
        }
    };
    const bool have = width > 0 && height > 0 && rgba.size() == size_t{width} * height * 4;
    std::unique_ptr<GpuTexture> cover;
    if (have) {
        std::string error;
        cover = GpuTexture::createDynamic(m_ctx, width, height, false, true, error);
        if (!cover) {
            logError("media cover: {}", error);
        } else {
            cover->update(m_ctx, rgba.data());
        }
    }
    // the cover shown becomes the previous one; whatever was previous goes
    retire(kMediaPrevious);
    if (auto it = m_textures.find(kMediaCurrent); it != m_textures.end()) {
        m_textures[kMediaPrevious] = std::move(it->second);
        m_textures.erase(it);
    }
    if (cover) {
        m_textures[kMediaCurrent] = std::move(cover);
    }
    applyMediaSlots();
    if (m_scripts) {
        const bool has = m_textures.count(kMediaCurrent) != 0;
        m_scripts->setMediaThumbnail(has, has ? mediaColors(width, height, rgba.data()) : MediaColors{});
    }
}

// The icon of the application the user chose for the shortcut property `name` (bridge/shortcutview.h); none (0 x 0): the user cleared it and
// the slot shows what the file lists there again.
void SceneRenderer::setShortcutIcon(const std::string &name, uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
{
    const std::string key = kShortcutPrefix + name;
    const bool have = width > 0 && height > 0 && rgba.size() == size_t{width} * height * 4;
    std::unique_ptr<GpuTexture> icon;
    if (have) {
        const bool known = m_textures.count(key) != 0;
        size_t held = 0;
        for (const auto &[n, t] : m_textures) {
            held += n.rfind(kShortcutPrefix, 0) == 0 ? 1 : 0;
        }
        if (!known && held >= kMaxShortcutIcons) {
            logError("shortcut icon '{}': more than {} icons", name, kMaxShortcutIcons);
            return;
        }
        std::string error;
        icon = GpuTexture::createDynamic(m_ctx, width, height, false, true, error);
        if (!icon) {
            logError("shortcut icon '{}': {}", name, error);
            return;
        }
        icon->update(m_ctx, rgba.data());
    }
    if (auto it = m_textures.find(key); it != m_textures.end()) {
        m_retiredTextures.emplace_back(m_frameIndex, std::move(it->second)); // a recorded command may still name it
        m_textures.erase(it);
    }
    if (icon) {
        m_textures[key] = std::move(icon);
    }
    applyMediaSlots();
}

void SceneRenderer::setShortcutLaunchHandler(std::function<bool(const std::string &)> handler)
{
    if (m_scripts) {
        m_scripts->setShortcutLaunchHandler(handler);
    }
    m_shortcutLaunchHandler = std::move(handler);
}

bool SceneRenderer::wantsMedia() const
{
    const auto uses = [](const std::vector<MediaBinding> &media) {
        return std::any_of(media.begin(), media.end(), [](const MediaBinding &b) { return b.shortcut.empty(); }); // (a shortcut slot is no media)
    };
    for (const ImageLayer &layer : m_scene.layers) {
        if (uses(layer.base.media)) {
            return true;
        }
        for (const EffectInstance &fx : layer.effects) {
            if (std::any_of(fx.def.passes.begin(), fx.def.passes.end(), [&](const EffectPass &p) { return uses(p.material.media); })
                || std::any_of(fx.overrides.begin(), fx.overrides.end(), [&](const PassOverride &o) { return uses(o.media); })) {
                return true;
            }
        }
    }
    for (const MeshLayer &mesh : m_scene.meshes) {
        if (std::any_of(mesh.materials.begin(), mesh.materials.end(), [&](const MaterialPass &p) { return uses(p.media); })) {
            return true;
        }
    }
    return m_scripts && m_scripts->wantsMedia();
}

void SceneRenderer::setMedia(const MediaState &state)
{
    m_media = state;
    if (m_scripts) {
        m_scripts->setMedia(state, m_inputs.time);
    }
}

} // namespace murasaki::scene
