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

VkDeviceSize alignUp4(VkDeviceSize v)
{
    return (v + 3) / 4 * 4;
}

// Parts with more indices than kChunkedIndices are cut into runs of kChunkIndices for the shadow pictures.
constexpr size_t kChunkedIndices = 3 * 16384;
constexpr size_t kChunkIndices = 3 * 2048;

} // namespace

const GpuTexture *SceneRenderer::textureNamed(const std::string &name) const
{
    const auto it = m_textures.find(name);
    return it == m_textures.end() ? nullptr : it->second.get();
}

void SceneRenderer::loadTextures()
{
    std::set<std::string> names;
    const auto add = [&](const MaterialPass &pass) {
        for (const std::string &n : pass.textures) {
            if (!n.empty() && !isRenderTarget(n) && n.rfind("__text:", 0) != 0) {
                names.insert(n);
            }
        }
    };
    for (const ImageLayer &layer : m_scene.layers) {
        add(layer.base);
        for (const EffectInstance &fx : layer.effects) {
            for (size_t i = 0; i < fx.def.passes.size(); ++i) {
                add(effectivePass(fx.def.passes[i], i < fx.overrides.size() ? &fx.overrides[i] : nullptr));
            }
        }
    }
    for (const MeshLayer &mesh : m_scene.meshes) {
        for (const MaterialPass &pass : mesh.materials) {
            add(pass);
        }
    }
    const std::function<void(const ParticleDefinition &)> addParticles = [&](const ParticleDefinition &def) {
        add(def.materialPass);
        for (const ParticleChild &child : def.children) {
            if (child.definition) {
                addParticles(*child.definition);
            }
        }
    };
    for (const ParticleLayer &p : m_scene.particles) {
        if (p.definition) {
            addParticles(*p.definition);
        }
    }
    // The files are read first (the Vfs is not for several threads), then every texture is decoded on a bounded set of threads
    // (the big embedded pictures are most of a scene's load time), then the pictures go to the GPU one by one.
    struct Pending {
        std::string name;
        std::string path;
        std::optional<std::vector<uint8_t>> tex;   // materials/<name>.tex
        std::optional<std::vector<uint8_t>> image; // else a plain png or jpg beside it
        std::optional<Tex> decoded;
        std::string error;
    };
    std::vector<Pending> pending;
    for (const std::string &name : names) {
        Pending p;
        p.name = name;
        p.path = "materials/" + name + ".tex";
        // a big video texture is not read: its stream is played from where it lies in the package
        if (auto inPlace = peekVideoTexFile(m_vfs, p.path)) {
            p.decoded = std::move(inPlace);
            pending.push_back(std::move(p));
            continue;
        }
        p.tex = m_vfs.read(p.path);
        if (!p.tex) {
            // Some effects ship their textures as plain image files next to the (absent) .tex.
            for (const char *ext : {".png", ".jpg"}) {
                if ((p.image = m_vfs.read("materials/" + name + ext))) {
                    break;
                }
            }
        }
        pending.push_back(std::move(p));
    }
    parallelFor(pending.size(), [&](size_t i) {
        Pending &p = pending[i];
        if (p.tex) {
            p.decoded = parseTex(std::span<const uint8_t>(p.tex->data(), p.tex->size()), {}, &p.error);
        } else if (p.image) {
            DecodedImage decoded;
            std::string decodeError;
            if (decodeImage(*p.image, 1u << 15, 1ull << 28, decoded, decodeError) && static_cast<uint64_t>(decoded.width) * decoded.height <= (1ull << 26)) {
                Tex t;
                t.textureWidth = t.imageWidth = decoded.width;
                t.textureHeight = t.imageHeight = decoded.height;
                TexMip mip;
                mip.width = decoded.width;
                mip.height = decoded.height;
                mip.data = std::move(decoded.rgba);
                t.mips.push_back(std::move(mip));
                p.decoded = std::move(t);
            }
        }
        p.tex.reset();
        p.image.reset();
    });
    for (Pending &p : pending) {
        if (!p.decoded) {
            throw std::runtime_error("texture '" + p.path + "': " + (p.error.empty() ? std::string("not found") : p.error));
        }
        std::string error;
        if (p.decoded->hasVideo()) {
            // a video texture is made as big as the pictures of its stream and rewritten as the clock runs
            VideoDecoder::Options decoding;
            decoding.renderNode = m_ctx.renderNode();
            // MURASAKI_SCENE_VK_VIDEO: "cpu" decodes on the CPU whatever the machine can do, "copy" decodes on the GPU and brings the pictures
            // through the CPU; by default the decoder's surfaces are imported as dma-bufs and converted on the GPU
            const char *mode = std::getenv("MURASAKI_SCENE_VK_VIDEO");
            const bool cpuOnly = mode && std::strcmp(mode, "cpu") == 0;
            const bool copyOnly = mode && std::strcmp(mode, "copy") == 0;
            decoding.hardware = !cpuOnly;
            if (!cpuOnly && !copyOnly && !m_videoConverterTried) {
                m_videoConverterTried = true;
                std::string why;
                m_videoConverter = VideoConverter::create(m_ctx, why);
                if (!m_videoConverter) {
                    logInfo("video textures: no zero-copy ({}), the pictures are copied through the CPU", why);
                }
            }
            decoding.zeroCopy = m_videoConverter != nullptr;
            auto decoder = p.decoded->videoFile.empty() ? VideoDecoder::open(std::move(p.decoded->video), error, decoding)
                                                        : VideoDecoder::openFile(p.decoded->videoFile, p.decoded->videoFileOffset, p.decoded->videoSize, error, decoding);
            if (decoder) {
                decoder->setMaxFps(m_fps);
                // once per video: which way it is decoded (a sandbox that hid the VA driver would show here as the CPU)
                logInfo("video texture '{}': {}x{}, decoded on the {}", p.name, decoder->width(), decoder->height(), decoder->hardware() ? "GPU (VAAPI)" : "CPU");
            }
            std::unique_ptr<GpuTexture> dynamic = decoder ? GpuTexture::createDynamic(m_ctx, decoder->width(), decoder->height(), p.decoded->nearest(),
                                                                                      p.decoded->clampUVs(), error)
                                                          : nullptr;
            if (!dynamic) {
                throw std::runtime_error("video texture '" + p.path + "': " + error);
            }
            bool zeroCopy = decoding.zeroCopy && decoder->hardware();
            if (zeroCopy) {
                MurasakiVideoExport first = decoder->exportFrame(0.0);
                std::string why = "the decoder cannot export its surfaces";
                if (first.status == 1 && m_videoConverter->convert(first, *dynamic, why)) {
                    logInfo("video texture '{}': the pictures reach the scene as dma-bufs, converted on the GPU (no copy through the CPU)", p.name);
                } else {
                    zeroCopy = false;
                    logInfo("video texture '{}': zero-copy is not possible ({}), the pictures are copied through the CPU", p.name, why);
                    if (const uint8_t *picture = decoder->frameAt(0.0)) {
                        dynamic->update(m_ctx, picture);
                    }
                }
            } else if (const uint8_t *first = decoder->frameAt(0.0)) {
                dynamic->update(m_ctx, first);
            }
            p.decoded.reset();
            {
                VideoControl &control = m_scene.videos[p.name];
                control.duration = decoder->duration();
                decoder->setLoop(control.loop);
            }
            m_videos.push_back({p.name, std::move(decoder), zeroCopy});
            m_textures.emplace(p.name, std::move(dynamic));
            continue;
        }
        std::unique_ptr<GpuTexture> gpu = GpuTexture::create(m_ctx, *p.decoded, error);
        if (!gpu) {
            throw std::runtime_error("texture '" + p.path + "': " + error);
        }
        p.decoded.reset();
        m_textures.emplace(p.name, std::move(gpu));
    }
}

// The meshes of the scene's puppets, uploaded once (the model's vertices go to the GPU as they are in the file).
void SceneRenderer::preparePuppets()
{
    for (size_t i = 0; i < m_scene.layers.size(); ++i) {
        const ImageLayer &layer = m_scene.layers[i];
        if (!layer.puppet) {
            continue;
        }
        const Puppet &puppet = *layer.puppet;
        PuppetState state;
        state.vertices = m_ctx.createBuffer(puppet.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        std::memcpy(state.vertices.mapped(), puppet.vertices.data(), puppet.vertices.size());
        // The model's texture coordinates run over the picture, not over the texture file: where the file is padded (a 3053 x 2160
        // picture in a 4096 x 4096 texture) the mesh's 0..1 is the picture's 0..uMax. docs/scene-vk/puppets.md, "Texture coordinates".
        if (const GpuTexture *base = layer.base.textures.empty() ? nullptr : textureNamed(layer.base.textures[0]);
            base && base->frames().size() <= 1 && (base->uMax() < 1.0f || base->vMax() < 1.0f)) {
            auto *vertex = static_cast<uint8_t *>(state.vertices.mapped());
            for (size_t v = 0; v < puppet.vertexCount(); ++v) {
                float uv[2];
                std::memcpy(uv, vertex + v * kPuppetVertexStride + kPuppetTexCoordOffset, sizeof uv);
                uv[0] *= base->uMax();
                uv[1] *= base->vMax();
                std::memcpy(vertex + v * kPuppetVertexStride + kPuppetTexCoordOffset, uv, sizeof uv);
            }
        }
        state.vertices.flush(0, puppet.vertices.size());
        state.indices = m_ctx.createBuffer(puppet.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(state.indices.mapped(), puppet.indices.data(), puppet.indices.size() * sizeof(uint32_t));
        state.indices.flush(0, puppet.indices.size() * sizeof(uint32_t));
        state.indexCount = static_cast<uint32_t>(puppet.indices.size());
        state.pass = puppetPass(layer);
        state.skinned = puppetSkinned(layer);
        m_puppets.emplace(i, std::move(state));
    }
}

// The parts of the scene's 3D models, uploaded once per model (objects that use the same model share the buffers) into device-local
// buffers through a staging copy. The scene then keeps the models without their vertices and indices: the GPU has them.
void SceneRenderer::prepareMeshes()
{
    // a static model that casts shadows also gets its positions alone: the shadow casters read a quarter of the bytes (a big mesh drawn
    // into several shadow pictures is limited by the memory it reads)
    std::set<const MeshModel *> casting;
    if (sceneHasShadows(m_scene)) {
        for (const MeshLayer &mesh : m_scene.meshes) {
            if (mesh.castShadow && mesh.model && !mesh.model->skinned) {
                casting.insert(mesh.model.get());
            }
        }
    }
    std::map<const MeshModel *, std::shared_ptr<const MeshGpu>> uploaded;
    std::map<const MeshModel *, std::shared_ptr<const MeshModel>> stripped;
    m_meshes.assign(m_scene.meshes.size(), nullptr);
    m_meshBones.assign(m_scene.meshes.size(), {});
    m_meshBonesFrame.assign(m_scene.meshes.size(), ~0ull);
    for (size_t i = 0; i < m_scene.meshes.size(); ++i) {
        MeshLayer &mesh = m_scene.meshes[i];
        if (!mesh.model) {
            continue;
        }
        const MeshModel *key = mesh.model.get();
        if (const auto have = uploaded.find(key); have != uploaded.end()) {
            m_meshes[i] = have->second;
            mesh.model = stripped.at(key);
            continue;
        }
        auto gpu = std::make_shared<MeshGpu>();
        VkDeviceSize total = 0;
        const bool positions = casting.count(key) != 0;
        for (const MeshPart &part : mesh.model->parts) {
            total += alignUp4(part.vertices.size()) + alignUp4(part.indices.size()) + (positions ? part.vertexCount * 12 : 0);
        }
        GpuBuffer staging = m_ctx.createBuffer(std::max<VkDeviceSize>(total, 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto *bytes = static_cast<uint8_t *>(staging.mapped());
        struct Copy {
            VkBuffer dst;
            VkDeviceSize offset, size;
        };
        std::vector<Copy> copies;
        VkDeviceSize at = 0;
        for (const MeshPart &part : mesh.model->parts) {
            MeshGpu::Part p;
            p.vertices = m_ctx.createBuffer(part.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            p.indices = m_ctx.createBuffer(part.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            p.indexCount = static_cast<uint32_t>(part.indexCount);
            p.indexType = part.wideIndices ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            std::memcpy(bytes + at, part.vertices.data(), part.vertices.size());
            copies.push_back({p.vertices.handle(), at, part.vertices.size()});
            at += alignUp4(part.vertices.size());
            std::memcpy(bytes + at, part.indices.data(), part.indices.size());
            copies.push_back({p.indices.handle(), at, part.indices.size()});
            at += alignUp4(part.indices.size());
            if (positions && part.vertexCount > 0 && part.stride >= 12) {
                p.positions = m_ctx.createBuffer(part.vertexCount * 12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
                for (size_t v = 0; v < part.vertexCount; ++v) {
                    std::memcpy(bytes + at + v * 12, part.vertices.data() + v * part.stride, 12);
                }
                copies.push_back({p.positions.handle(), at, part.vertexCount * 12});
                at += part.vertexCount * 12;
                if (part.indexCount >= kChunkedIndices) {
                    // runs of kChunkIndices indices with their bounds: the shadow pictures of a small cascade draw only the runs in it
                    for (size_t first = 0; first < part.indexCount; first += kChunkIndices) {
                        const size_t count = std::min(kChunkIndices, part.indexCount - first);
                        MeshGpu::Part::Chunk chunk;
                        chunk.range = {static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
                        ShadowBounds box;
                        for (size_t k = first; k < first + count; ++k) {
                            const uint32_t v = part.index(k);
                            if (v < part.vertexCount) {
                                float xyz[3];
                                std::memcpy(xyz, part.vertices.data() + v * part.stride, sizeof xyz);
                                box.add({xyz[0], xyz[1], xyz[2]});
                            }
                        }
                        chunk.low = box.min;
                        chunk.high = box.max;
                        p.chunks.push_back(chunk);
                    }
                }
            }
            gpu->parts.push_back(std::move(p));
        }
        staging.flush();
        m_ctx.submitAndWait([&](VkCommandBuffer cb) {
            for (const Copy &c : copies) {
                const VkBufferCopy region{c.offset, 0, c.size};
                vkCmdCopyBuffer(cb, staging.handle(), c.dst, 1, &region);
            }
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cb, &dep);
        });
        // the same model without its vertex and index bytes (the counts, bounds and materials stay)
        auto light = std::make_shared<MeshModel>(*mesh.model);
        for (MeshPart &part : light->parts) {
            std::vector<uint8_t>().swap(part.vertices);
            std::vector<uint8_t>().swap(part.indices);
        }
        uploaded.emplace(key, gpu);
        stripped.emplace(key, light);
        m_meshes[i] = gpu;
        mesh.model = light;
    }
}

void SceneRenderer::loadDefaultTexture(const std::string &name)
{
    if (name.empty() || isRenderTarget(name) || textureNamed(name)) {
        return;
    }
    const auto bytes = m_vfs.read("materials/" + name + ".tex");
    if (!bytes) {
        return;
    }
    std::string error;
    const auto tex = parseTex(std::span<const uint8_t>(bytes->data(), bytes->size()), {}, &error);
    if (!tex) {
        logError("default texture '{}': {}", name, error);
        return;
    }
    if (auto gpu = GpuTexture::create(m_ctx, *tex, error)) {
        m_textures.emplace(name, std::move(gpu));
    } else {
        logError("default texture '{}': {}", name, error);
    }
}

} // namespace murasaki::scene
