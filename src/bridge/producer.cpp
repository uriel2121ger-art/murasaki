#include "producer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#ifdef MURASAKI_BRIDGE_HAVE_GBM
#include <gbm.h>
#endif

namespace murasaki::bridge {

namespace {

// OpenGL constants, spelled out so that no GL header is needed.
constexpr unsigned kGlTexture2D = 0x0DE1;
constexpr unsigned kGlTextureBinding2D = 0x8069;
constexpr unsigned kGlRgba = 0x1908;
constexpr unsigned kGlRgba8 = 0x8058;
constexpr unsigned kGlUnsignedByte = 0x1401;
constexpr unsigned kGlTextureMagFilter = 0x2800;
constexpr unsigned kGlTextureMinFilter = 0x2801;
constexpr unsigned kGlLinear = 0x2601;
constexpr unsigned kGlFramebuffer = 0x8D40;
constexpr unsigned kGlReadFramebuffer = 0x8CA8;
constexpr unsigned kGlDrawFramebuffer = 0x8CA9;
constexpr unsigned kGlDrawFramebufferBinding = 0x8CA6;
constexpr unsigned kGlPixelUnpackBuffer = 0x88EC;
constexpr unsigned kGlPixelUnpackBufferBinding = 0x88EF;
constexpr unsigned kGlReadFramebufferBinding = 0x8CAA;
constexpr unsigned kGlColorAttachment0 = 0x8CE0;
constexpr unsigned kGlFramebufferComplete = 0x8CD5;
constexpr unsigned kGlPixelPackBuffer = 0x88EB;
constexpr unsigned kGlPixelPackBufferBinding = 0x88ED;
constexpr unsigned kGlPackAlignment = 0x0D05;
constexpr unsigned kGlPackRowLength = 0x0D02;

constexpr uint32_t kBuffers = kMaxBuffers; // count is always 3
constexpr uint32_t kFourccAbgr8888 = 0x34324241; // 'AB24': R, G, B, A in memory order, what an RGBA8 texture is
constexpr uint64_t kModifierLinear = 0;
constexpr uint32_t kErrorProtocol = 1;
constexpr uint32_t kErrorRuntime = 3;
constexpr int kSendRetries = 200; // x 10 ms: buffer sets are not droppable, wait for room in the socket

bool hasExtension(const char *list, const char *name)
{
    if (!list)
        return false;
    const size_t length = std::strlen(name);
    for (const char *cur = list; (cur = std::strstr(cur, name)) != nullptr; cur += length) {
        const bool startOk = cur == list || cur[-1] == ' ';
        const bool endOk = cur[length] == ' ' || cur[length] == '\0';
        if (startOk && endOk)
            return true;
    }
    return false;
}

bool envFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void note(const std::string &text)
{
    std::fprintf(stderr, "murasaki bridge producer: %s\n", text.c_str());
}

template <class F>
F proc(const char *name)
{
    return reinterpret_cast<F>(eglGetProcAddress(name));
}

} // namespace

// ---- entry points --------------------------------------------------------------------------------------

struct Producer::Gl {
    void (*genTextures)(int, unsigned *) = nullptr;
    void (*deleteTextures)(int, const unsigned *) = nullptr;
    void (*bindTexture)(unsigned, unsigned) = nullptr;
    void (*texImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = nullptr;
    void (*texParameteri)(unsigned, unsigned, int) = nullptr;
    void (*genFramebuffers)(int, unsigned *) = nullptr;
    void (*deleteFramebuffers)(int, const unsigned *) = nullptr;
    void (*bindFramebuffer)(unsigned, unsigned) = nullptr;
    void (*framebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
    unsigned (*checkFramebufferStatus)(unsigned) = nullptr;
    void (*finish)() = nullptr;
    unsigned (*getError)() = nullptr;
    void (*getIntegerv)(unsigned, int *) = nullptr;
    void (*pixelStorei)(unsigned, int) = nullptr;
    void (*bindBuffer)(unsigned, unsigned) = nullptr;
    void (*readPixels)(int, int, int, int, unsigned, unsigned, void *) = nullptr;

    bool load()
    {
        genTextures = proc<decltype(genTextures)>("glGenTextures");
        deleteTextures = proc<decltype(deleteTextures)>("glDeleteTextures");
        bindTexture = proc<decltype(bindTexture)>("glBindTexture");
        texImage2D = proc<decltype(texImage2D)>("glTexImage2D");
        texParameteri = proc<decltype(texParameteri)>("glTexParameteri");
        genFramebuffers = proc<decltype(genFramebuffers)>("glGenFramebuffers");
        deleteFramebuffers = proc<decltype(deleteFramebuffers)>("glDeleteFramebuffers");
        bindFramebuffer = proc<decltype(bindFramebuffer)>("glBindFramebuffer");
        framebufferTexture2D = proc<decltype(framebufferTexture2D)>("glFramebufferTexture2D");
        checkFramebufferStatus = proc<decltype(checkFramebufferStatus)>("glCheckFramebufferStatus");
        finish = proc<decltype(finish)>("glFinish");
        getError = proc<decltype(getError)>("glGetError");
        getIntegerv = proc<decltype(getIntegerv)>("glGetIntegerv");
        pixelStorei = proc<decltype(pixelStorei)>("glPixelStorei");
        bindBuffer = proc<decltype(bindBuffer)>("glBindBuffer");
        readPixels = proc<decltype(readPixels)>("glReadPixels");
        return genTextures && deleteTextures && bindTexture && texImage2D && texParameteri && genFramebuffers
            && deleteFramebuffers && bindFramebuffer && framebufferTexture2D && checkFramebufferStatus && finish
            && getError && getIntegerv && pixelStorei && bindBuffer && readPixels;
    }

    void clearErrors() const
    {
        // Bounded: a lost context reports an error forever.
        for (int i = 0; i < 16 && getError() != 0; ++i) {
        }
    }
};

struct Producer::Egl {
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC exportQuery = nullptr;
    PFNEGLEXPORTDMABUFIMAGEMESAPROC exportImage = nullptr;
    void (*targetTexture)(unsigned, void *) = nullptr; // glEGLImageTargetTexture2DOES
    PFNEGLQUERYDISPLAYATTRIBEXTPROC queryDisplayAttrib = nullptr;
    PFNEGLQUERYDEVICESTRINGEXTPROC queryDeviceString = nullptr;
    bool importModifiers = false;
    bool dmabufImport = false;

    bool canExport() const { return createImage && destroyImage && exportQuery && exportImage; }
    bool canImport() const { return createImage && destroyImage && targetTexture && dmabufImport; }
};

struct Producer::GbmDevice {
#ifdef MURASAKI_BRIDGE_HAVE_GBM
    UniqueFd fd;
    gbm_device *device = nullptr;
    ~GbmDevice()
    {
        if (device)
            gbm_device_destroy(device);
    }
#endif
};

struct Producer::Viewport {
    struct Dma {
        std::array<UniqueFd, kMaxPlanes> fds;
        std::array<uint32_t, kMaxPlanes> offset{};
        std::array<uint32_t, kMaxPlanes> stride{};
    };

    uint32_t id = 0;
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t generation = 0;
    Transport transport = Transport::Dmabuf;
    std::array<unsigned, kBuffers> textures{};
    std::array<unsigned, kBuffers> framebuffers{};
    std::array<bool, kBuffers> held{};
    std::array<Dma, kBuffers> dma;
    uint32_t fourcc = 0;
    uint32_t planes = 0;
    uint64_t modifier = 0;
    std::optional<ShmRing> ring;
    std::optional<uint32_t> current; // the buffer beginFrame handed out

    bool allocated() const { return textures[0] != 0; }
};

namespace {
// Restores the bindings the producer disturbs while it allocates (the texture binding of the current unit, the
// draw and read framebuffers separately, the unpack buffer, which is unbound meanwhile so texImage2D(nullptr)
// cannot read from it). The active texture unit is never changed.
template <class G>
class BindingGuard
{
public:
    explicit BindingGuard(const G &gl)
        : m_gl(gl)
    {
        gl.getIntegerv(kGlTextureBinding2D, &m_texture);
        gl.getIntegerv(kGlDrawFramebufferBinding, &m_drawFramebuffer);
        gl.getIntegerv(kGlReadFramebufferBinding, &m_readFramebuffer);
        gl.getIntegerv(kGlPixelUnpackBufferBinding, &m_unpackBuffer);
        gl.bindBuffer(kGlPixelUnpackBuffer, 0);
    }
    ~BindingGuard()
    {
        m_gl.bindTexture(kGlTexture2D, static_cast<unsigned>(m_texture));
        m_gl.bindFramebuffer(kGlDrawFramebuffer, static_cast<unsigned>(m_drawFramebuffer));
        m_gl.bindFramebuffer(kGlReadFramebuffer, static_cast<unsigned>(m_readFramebuffer));
        m_gl.bindBuffer(kGlPixelUnpackBuffer, static_cast<unsigned>(m_unpackBuffer));
    }
    BindingGuard(const BindingGuard &) = delete;
    BindingGuard &operator=(const BindingGuard &) = delete;

private:
    const G &m_gl;
    int m_texture = 0;
    int m_drawFramebuffer = 0;
    int m_readFramebuffer = 0;
    int m_unpackBuffer = 0;
};
} // namespace

// ---- construction --------------------------------------------------------------------------------------

Producer::Producer(Channel &channel, void *display)
    : m_channel(channel)
    , m_display(display)
    , m_gl(std::make_unique<Gl>())
    , m_egl(std::make_unique<Egl>())
{
    if (!display || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        m_error = "no current EGL context";
        return;
    }
    if (!m_gl->load()) {
        m_error = "missing OpenGL entry points";
        return;
    }
    m_valid = true;

    const char *extensions = eglQueryString(static_cast<EGLDisplay>(display), EGL_EXTENSIONS);
    Egl &egl = *m_egl;
    if (hasExtension(extensions, "EGL_KHR_image_base") && hasExtension(extensions, "EGL_KHR_gl_texture_2D_image")) {
        egl.createImage = proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        egl.destroyImage = proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    }
    if (hasExtension(extensions, "EGL_MESA_image_dma_buf_export")) {
        egl.exportQuery = proc<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>("eglExportDMABUFImageQueryMESA");
        egl.exportImage = proc<PFNEGLEXPORTDMABUFIMAGEMESAPROC>("eglExportDMABUFImageMESA");
    }
    if (hasExtension(extensions, "EGL_EXT_image_dma_buf_import")) {
        egl.dmabufImport = true;
        egl.importModifiers = hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
        egl.targetTexture = proc<void (*)(unsigned, void *)>("glEGLImageTargetTexture2DOES");
    }
    egl.queryDisplayAttrib = proc<PFNEGLQUERYDISPLAYATTRIBEXTPROC>("eglQueryDisplayAttribEXT");
    egl.queryDeviceString = proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");

    m_noGbm = envFlag("MURASAKI_BRIDGE_NO_GBM");
    if (envFlag("MURASAKI_BRIDGE_FORCE_SHM")) {
        m_shmOnly = true;
    } else if (!egl.canExport()) {
        note("EGL cannot export dma-bufs (EGL_MESA_image_dma_buf_export), using shared memory");
        m_shmOnly = true;
    }
}

Producer::~Producer()
{
    if (!m_valid)
        return;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        note("destroyed without a current OpenGL context: the textures and framebuffers leak");
        return;
    }
    for (auto &vp : m_viewports)
        release(*vp);
}

Producer::Viewport *Producer::find(uint32_t id)
{
    return id < m_viewports.size() ? m_viewports[id].get() : nullptr;
}

const Producer::Viewport *Producer::find(uint32_t id) const
{
    return id < m_viewports.size() ? m_viewports[id].get() : nullptr;
}

uint32_t Producer::generation(uint32_t viewport) const
{
    const Viewport *vp = find(viewport);
    return vp ? vp->generation : 0;
}

Transport Producer::transport(uint32_t viewport) const
{
    const Viewport *vp = find(viewport);
    return vp ? vp->transport : Transport::Shm;
}

uint32_t Producer::heldCount(uint32_t viewport) const
{
    const Viewport *vp = find(viewport);
    return vp ? static_cast<uint32_t>(std::count(vp->held.begin(), vp->held.end(), true)) : 0;
}

void Producer::fail(std::string text)
{
    if (m_dead)
        return;
    m_error = std::move(text);
    m_dead = true;
    note(m_error);
    sendError(kErrorProtocol, m_error);
}

void Producer::peerGone()
{
    m_dead = true;
    m_peerClosed = true;
}

void Producer::sendError(uint32_t code, std::string_view text)
{
    if (!m_channel.valid())
        return;
    msg::Error error{};
    error.code = code;
    setString(error.text, text);
    m_channel.send(error); // best effort, the peer may be gone already
}

// ---- GBM -----------------------------------------------------------------------------------------------

bool Producer::canLinear()
{
    if (!m_valid || m_shmOnly || m_noGbm || !m_egl->canImport())
        return false;
#ifdef MURASAKI_BRIDGE_HAVE_GBM
    if (!m_gbmTried) {
        m_gbmTried = true;
        std::string path;
        EGLAttrib device = 0;
        // The render node of the device EGL renders on, so the linear buffers live on the same GPU.
        if (m_egl->queryDisplayAttrib && m_egl->queryDeviceString
            && m_egl->queryDisplayAttrib(static_cast<EGLDisplay>(m_display), EGL_DEVICE_EXT, &device) == EGL_TRUE
            && device != 0) {
            if (const char *node = m_egl->queryDeviceString(reinterpret_cast<EGLDeviceEXT>(device),
                                                            EGL_DRM_RENDER_NODE_FILE_EXT))
                path = node;
        }
        for (int index = 128; index < 192 && !m_gbm; ++index) {
            const std::string candidate = path.empty() ? "/dev/dri/renderD" + std::to_string(index) : path;
            UniqueFd fd(::open(candidate.c_str(), O_RDWR | O_CLOEXEC));
            if (fd) {
                if (gbm_device *device = gbm_create_device(fd.get())) {
                    m_gbm = std::make_unique<GbmDevice>();
                    m_gbm->fd = std::move(fd);
                    m_gbm->device = device;
                }
            }
            if (!path.empty())
                break;
        }
    }
    return m_gbm != nullptr;
#else
    return false;
#endif
}

// ---- buffer sets ---------------------------------------------------------------------------------------

void Producer::release(Viewport &vp)
{
    if (vp.framebuffers[0] != 0)
        m_gl->deleteFramebuffers(static_cast<int>(kBuffers), vp.framebuffers.data());
    if (vp.textures[0] != 0)
        m_gl->deleteTextures(static_cast<int>(kBuffers), vp.textures.data());
    vp.framebuffers = {};
    vp.textures = {};
    vp.held = {};
    vp.dma = {};
    vp.ring.reset();
    vp.fourcc = 0;
    vp.planes = 0;
    vp.modifier = 0;
    vp.current.reset();
}

bool Producer::allocateTextures(Viewport &vp)
{
    Gl &gl = *m_gl;
    gl.clearErrors();
    gl.genTextures(static_cast<int>(kBuffers), vp.textures.data());
    for (uint32_t i = 0; i < kBuffers; ++i) {
        gl.bindTexture(kGlTexture2D, vp.textures[i]);
        gl.texImage2D(kGlTexture2D, 0, static_cast<int>(kGlRgba8), static_cast<int>(vp.width),
                      static_cast<int>(vp.height), 0, kGlRgba, kGlUnsignedByte, nullptr);
        gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinear));
        gl.texParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<int>(kGlLinear));
    }
    return gl.getError() == 0;
}

bool Producer::attachFramebuffers(Viewport &vp)
{
    Gl &gl = *m_gl;
    gl.genFramebuffers(static_cast<int>(kBuffers), vp.framebuffers.data());
    for (uint32_t i = 0; i < kBuffers; ++i) {
        gl.bindFramebuffer(kGlFramebuffer, vp.framebuffers[i]);
        gl.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2D, vp.textures[i], 0);
        if (gl.checkFramebufferStatus(kGlFramebuffer) != kGlFramebufferComplete) {
            note("incomplete framebuffer for viewport " + vp.name);
            return false;
        }
    }
    return true;
}

bool Producer::exportTextures(Viewport &vp)
{
    const Egl &egl = *m_egl;
    if (!egl.canExport())
        return false;
    auto display = static_cast<EGLDisplay>(m_display);
    const EGLContext context = eglGetCurrentContext();
    const EGLint imageAttributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
    for (uint32_t i = 0; i < kBuffers; ++i) {
        EGLImageKHR image = egl.createImage(display, context, EGL_GL_TEXTURE_2D_KHR,
                                            reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(vp.textures[i])),
                                            imageAttributes);
        if (image == EGL_NO_IMAGE_KHR)
            return false;
        int fourcc = 0;
        int planes = 0;
        EGLuint64KHR modifier = 0;
        int fds[kMaxPlanes] = {-1, -1, -1, -1};
        EGLint strides[kMaxPlanes] = {};
        EGLint offsets[kMaxPlanes] = {};
        bool exported = egl.exportQuery(display, image, &fourcc, &planes, &modifier) == EGL_TRUE && planes >= 1
            && planes <= static_cast<int>(kMaxPlanes);
        exported = exported && egl.exportImage(display, image, fds, strides, offsets) == EGL_TRUE;
        egl.destroyImage(display, image); // the dma-bufs keep the memory alive
        if (!exported) {
            for (int fd : fds) {
                if (fd >= 0)
                    ::close(fd);
            }
            return false;
        }
        Viewport::Dma &dma = vp.dma[i];
        for (int p = 0; p < planes; ++p) {
            dma.fds[p].reset(fds[p]);
            dma.offset[p] = static_cast<uint32_t>(offsets[p]);
            dma.stride[p] = static_cast<uint32_t>(strides[p]);
        }
        if (i == 0) {
            vp.fourcc = static_cast<uint32_t>(fourcc);
            vp.modifier = modifier; // DRM_FORMAT_MOD_INVALID passes through as is
            vp.planes = static_cast<uint32_t>(planes);
        } else if (static_cast<uint32_t>(fourcc) != vp.fourcc || modifier != vp.modifier
                   || static_cast<uint32_t>(planes) != vp.planes) {
            note("the driver exported the textures of " + vp.name + " with inconsistent formats");
            return false;
        }
    }
    return true;
}

bool Producer::importLinearTextures(Viewport &vp)
{
#ifdef MURASAKI_BRIDGE_HAVE_GBM
    if (!canLinear())
        return false;
    const Egl &egl = *m_egl;
    Gl &gl = *m_gl;
    auto display = static_cast<EGLDisplay>(m_display);
    gl.clearErrors();
    gl.genTextures(static_cast<int>(kBuffers), vp.textures.data());
    vp.fourcc = kFourccAbgr8888;
    vp.modifier = kModifierLinear;
    vp.planes = 1;
    for (uint32_t i = 0; i < kBuffers; ++i) {
        gbm_bo *bo = gbm_bo_create(m_gbm->device, vp.width, vp.height, vp.fourcc,
                                   GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo)
            return false;
        Viewport::Dma &dma = vp.dma[i];
        bool ok = gbm_bo_get_plane_count(bo) == 1;
        if (ok) {
            dma.fds[0].reset(gbm_bo_get_fd_for_plane(bo, 0));
            dma.offset[0] = gbm_bo_get_offset(bo, 0);
            dma.stride[0] = gbm_bo_get_stride_for_plane(bo, 0);
            ok = dma.fds[0].valid();
        }
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        if (ok) {
            std::vector<EGLint> attributes = {
                EGL_WIDTH, static_cast<EGLint>(vp.width), EGL_HEIGHT, static_cast<EGLint>(vp.height),
                EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(vp.fourcc),
                EGL_DMA_BUF_PLANE0_FD_EXT, dma.fds[0].get(),
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(dma.offset[0]),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(dma.stride[0]),
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            };
            if (egl.importModifiers) {
                attributes.insert(attributes.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
                                                     EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0});
            }
            attributes.push_back(EGL_NONE);
            image = egl.createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
        }
        gbm_bo_destroy(bo); // the image (and the texture bound to it) holds the memory from now on
        if (image == EGL_NO_IMAGE_KHR)
            return false;
        gl.bindTexture(kGlTexture2D, vp.textures[i]);
        egl.targetTexture(kGlTexture2D, image);
        gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinear));
        gl.texParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<int>(kGlLinear));
        egl.destroyImage(display, image);
        if (gl.getError() != 0)
            return false;
    }
    return true;
#else
    (void)vp;
    return false;
#endif
}

bool Producer::allocate(Viewport &vp, uint32_t width, uint32_t height, Transport wanted)
{
    if (wanted == Transport::DmabufLinear && !canLinear())
        wanted = Transport::Shm;
    if (m_shmOnly)
        wanted = Transport::Shm;

    // Every attempt is a new buffer set, even one that fails.
    const auto build = [&](Transport transport) {
        release(vp);
        ++vp.generation;
        vp.transport = transport;
        vp.width = width;
        vp.height = height;
        BindingGuard guard(*m_gl);
        bool ok = false;
        switch (transport) {
        case Transport::Shm: {
            ShmError error = ShmError::Ok;
            vp.ring = ShmRing::create({kBuffers, width, height, width * 4}, &error);
            ok = vp.ring.has_value() && allocateTextures(vp);
            break;
        }
        case Transport::Dmabuf:
            ok = allocateTextures(vp) && exportTextures(vp);
            break;
        case Transport::DmabufLinear:
            ok = importLinearTextures(vp);
            break;
        }
        ok = ok && attachFramebuffers(vp);
        if (!ok)
            release(vp);
        m_targetsChanged = true;
        return ok;
    };

    if (build(wanted))
        return true;
    if (wanted == Transport::Shm)
        return false;
    note("cannot export the render targets of " + vp.name + " as dma-bufs, falling back to shared memory");
    if (wanted == Transport::Dmabuf)
        m_shmOnly = true;
    return build(Transport::Shm);
}

std::optional<uint32_t> Producer::addViewport(std::string_view name, uint32_t width, uint32_t height)
{
    if (!m_valid || m_dead || width < 1 || height < 1 || width > kMaxDimension || height > kMaxDimension)
        return std::nullopt;
    auto vp = std::make_unique<Viewport>();
    vp->id = static_cast<uint32_t>(m_viewports.size());
    vp->name = std::string(name);
    if (!allocate(*vp, width, height, m_shmOnly ? Transport::Shm : Transport::Dmabuf)) {
        m_error = "cannot allocate the render targets for viewport " + vp->name;
        return std::nullopt;
    }
    m_viewports.push_back(std::move(vp));
    Viewport &added = *m_viewports.back();
    if (m_started) {
        msg::Viewport info{};
        info.id = added.id;
        info.width = added.width;
        info.height = added.height;
        setString(info.name, added.name);
        if (!sendBlocking(msg::Viewport::kType, asBytes(info), {}) || !sendBuffers(added)) {
            fail("cannot announce the new viewport");
            return std::nullopt;
        }
    }
    return added.id;
}

// ---- sending -------------------------------------------------------------------------------------------

bool Producer::sendBlocking(MsgType type, std::span<const uint8_t> payload, std::span<const int> fds)
{
    if (m_dead)
        return false;
    ChannelStatus status = m_channel.send(type, payload, fds);
    for (int retry = 0; status == ChannelStatus::WouldBlock && retry < kSendRetries; ++retry) {
        pollfd writable{m_channel.fd(), POLLOUT, 0};
        ::poll(&writable, 1, 10);
        status = m_channel.send(type, payload, fds);
    }
    if (status == ChannelStatus::Closed)
        peerGone();
    return status == ChannelStatus::Ok;
}

bool Producer::sendBuffers(Viewport &vp)
{
    if (vp.transport != Transport::Shm) {
        msg::DmabufBuffers buffers{};
        std::vector<int> fds;
        buffers.viewport = vp.id;
        buffers.count = kBuffers;
        buffers.planes = vp.planes;
        buffers.fourcc = vp.fourcc;
        buffers.width = vp.width;
        buffers.height = vp.height;
        buffers.generation = vp.generation;
        buffers.modifier = vp.modifier;
        for (uint32_t b = 0; b < kBuffers; ++b) { // buffer-major, plane-minor
            for (uint32_t p = 0; p < vp.planes; ++p) {
                buffers.plane[b][p] = {vp.dma[b].offset[p], vp.dma[b].stride[p]};
                fds.push_back(vp.dma[b].fds[p].get());
            }
        }
        const bool ok = sendBlocking(msg::DmabufBuffers::kType, asBytes(buffers), fds);
        // The consumer owns its copies now and the textures keep the memory alive.
        for (auto &dma : vp.dma) {
            for (auto &fd : dma.fds)
                fd.reset();
        }
        return ok;
    }
    const ShmGeometry &geometry = vp.ring->geometry();
    msg::ShmBuffers buffers{};
    buffers.viewport = vp.id;
    buffers.count = geometry.count;
    buffers.width = geometry.width;
    buffers.height = geometry.height;
    buffers.stride = geometry.stride;
    buffers.format = kFormatRgba8;
    buffers.generation = vp.generation;
    const int fds[] = {vp.ring->fd()};
    return sendBlocking(msg::ShmBuffers::kType, asBytes(buffers), fds);
}

bool Producer::start()
{
    if (!m_valid || m_dead || m_started)
        return false;
    if (!m_channel.setNonblocking(true)) {
        m_error = "cannot make the bridge socket nonblocking";
        m_dead = true;
        return false;
    }
    msg::Hello hello{};
    hello.version = kBridgeVersion;
    hello.pid = static_cast<uint32_t>(::getpid());
    if (!sendBlocking(msg::Hello::kType, asBytes(hello), {})) {
        m_error = "cannot send Hello";
        m_dead = true;
        return false;
    }
    for (auto &vp : m_viewports) {
        msg::Viewport info{};
        info.id = vp->id;
        info.width = vp->width;
        info.height = vp->height;
        setString(info.name, vp->name);
        if (!sendBlocking(msg::Viewport::kType, asBytes(info), {}) || !sendBuffers(*vp)) {
            m_error = "cannot send the viewport " + vp->name;
            m_dead = true;
            return false;
        }
    }
    m_started = true;
    return true;
}

// ---- frames --------------------------------------------------------------------------------------------

std::optional<FrameTarget> Producer::beginFrame(uint32_t viewport)
{
    Viewport *vp = find(viewport);
    if (!vp || !m_started || m_dead || !vp->allocated())
        return std::nullopt;
    if (!vp->current) {
        for (uint32_t i = 0; i < kBuffers; ++i) {
            if (!vp->held[i]) {
                vp->current = i;
                break;
            }
        }
    }
    if (!vp->current) // the consumer holds every buffer: skip the frame instead of overwriting one it may be reading
        return std::nullopt;
    const uint32_t b = *vp->current;
    return FrameTarget{b, vp->textures[b], vp->framebuffers[b], vp->width, vp->height};
}

FrameResult Producer::endFrame(uint32_t viewport, bool flipY)
{
    Viewport *vp = find(viewport);
    if (!vp || !vp->current || m_dead)
        return m_peerClosed ? FrameResult::Closed : (m_dead ? FrameResult::Failed : FrameResult::Skipped);
    const uint32_t buffer = *vp->current;
    vp->current.reset();
    Gl &gl = *m_gl;

    if (vp->transport == Transport::Shm) {
        const std::span<uint8_t> slot = vp->ring->frame(buffer);
        if (slot.empty())
            return FrameResult::Skipped;
        int readBinding = 0, packBuffer = 0, alignment = 4, rowLength = 0;
        gl.getIntegerv(kGlReadFramebufferBinding, &readBinding);
        gl.getIntegerv(kGlPixelPackBufferBinding, &packBuffer);
        gl.getIntegerv(kGlPackAlignment, &alignment);
        gl.getIntegerv(kGlPackRowLength, &rowLength);
        gl.clearErrors();
        gl.bindBuffer(kGlPixelPackBuffer, 0);
        gl.bindFramebuffer(kGlReadFramebuffer, vp->framebuffers[buffer]);
        gl.pixelStorei(kGlPackAlignment, 1);
        gl.pixelStorei(kGlPackRowLength, 0);
        // The stride is width * 4, see allocate(). glReadPixels blocks until the rendering is done.
        gl.readPixels(0, 0, static_cast<int>(vp->width), static_cast<int>(vp->height), kGlRgba, kGlUnsignedByte,
                      slot.data());
        const unsigned error = gl.getError();
        gl.pixelStorei(kGlPackRowLength, rowLength);
        gl.pixelStorei(kGlPackAlignment, alignment);
        gl.bindFramebuffer(kGlReadFramebuffer, static_cast<unsigned>(readBinding));
        gl.bindBuffer(kGlPixelPackBuffer, static_cast<unsigned>(packBuffer));
        if (error != 0) {
            note("OpenGL error " + std::to_string(error) + " reading back a frame of " + vp->name);
            return FrameResult::Skipped;
        }
    } else {
        // The consumer samples the dma-buf right away: the frame must be complete before it hears about it.
        gl.finish();
    }

    msg::Frame frame{};
    frame.viewport = vp->id;
    frame.buffer = buffer;
    frame.seq = ++m_sequence;
    frame.flipY = flipY ? 1u : 0u;
    frame.generation = vp->generation;
    switch (m_channel.send(frame)) {
    case ChannelStatus::Ok:
        vp->held[buffer] = true;
        return FrameResult::Sent;
    case ChannelStatus::WouldBlock:
        return FrameResult::Skipped; // the consumer is not reading: drop the frame, the buffer stays free
    case ChannelStatus::Closed:
        peerGone();
        return FrameResult::Closed;
    default:
        m_error = "cannot send a frame over the bridge socket";
        m_dead = true;
        sendError(kErrorRuntime, m_error);
        return FrameResult::Failed;
    }
}

bool Producer::resize(uint32_t viewport, uint32_t width, uint32_t height)
{
    Viewport *vp = find(viewport);
    if (!vp || m_dead || width < 1 || height < 1 || width > kMaxDimension || height > kMaxDimension)
        return false;
    if (vp->allocated() && vp->width == width && vp->height == height)
        return true;
    if (!allocate(*vp, width, height, vp->transport)) {
        fail("cannot allocate the render targets for the new size");
        return false;
    }
    if (m_started && !sendBuffers(*vp)) {
        if (!m_dead)
            fail("cannot send the new buffers");
        return false;
    }
    return true;
}

void Producer::switchTransport(Viewport &vp, Transport wanted)
{
    if (wanted == Transport::DmabufLinear && !canLinear())
        wanted = Transport::Shm; // no GBM render node: shared memory is the fallback
    if (vp.transport == wanted)
        return;
    if (wanted != Transport::Shm && m_shmOnly)
        return; // the peer keeps what it has, shared memory frames are still coming
    if (!allocate(vp, vp.width, vp.height, wanted)) {
        fail("cannot allocate the render targets for the requested transport");
        return;
    }
    if (!sendBuffers(vp) && !m_dead)
        fail("cannot send the new buffers");
}

// ---- control messages ----------------------------------------------------------------------------------

ServiceResult Producer::service(size_t maxMessages)
{
    if (!m_valid || !m_channel.valid() || m_dead)
        return m_peerClosed ? ServiceResult::PeerClosed : (m_dead ? ServiceResult::Failed : ServiceResult::Ok);
    const DrainResult result = m_channel.drain(
        [this](Message &&message) {
            if (!m_dead)
                handle(std::move(message));
        },
        maxMessages);
    if (!m_dead) {
        switch (result.last) {
        case ChannelStatus::Ok:
        case ChannelStatus::WouldBlock:
            break;
        case ChannelStatus::Closed:
            peerGone();
            break;
        case ChannelStatus::TooLarge:
        case ChannelStatus::Malformed:
            fail("malformed message from the bridge consumer");
            break;
        default:
            m_error = "cannot read from the bridge socket";
            m_dead = true;
            sendError(kErrorRuntime, m_error);
            break;
        }
    }
    return m_peerClosed ? ServiceResult::PeerClosed : (m_dead ? ServiceResult::Failed : ServiceResult::Ok);
}

void Producer::handle(Message &&message)
{
    const std::span<const uint8_t> payload{message.payload};
    const size_t fdCount = message.fds.size();

    // Decodes and validates a fixed-size message; anything that does not pass is a protocol violation.
    const auto read = [&](auto &out) {
        if (!decode(payload, out) || !msg::validate(out, fdCount)) {
            fail("invalid message from the bridge consumer");
            return false;
        }
        return true;
    };
    const auto deliver = [&](auto &&event) {
        if (m_handler)
            m_handler(ControlMessage(std::forward<decltype(event)>(event)));
    };

    switch (static_cast<MsgType>(message.type)) {
    case MsgType::Release: {
        msg::Release release{};
        if (!read(release))
            return;
        Viewport *vp = find(release.viewport);
        if (!vp || release.buffer >= kBuffers) {
            fail("Release refers to an unknown viewport or buffer");
            return;
        }
        // A Release of an older buffer set must never free a buffer of the current one.
        if (release.generation == vp->generation)
            vp->held[release.buffer] = false;
        break;
    }
    case MsgType::RequestLinear:
    case MsgType::RequestShm: {
        msg::RequestShm request{}; // both only carry the viewport id
        if (!read(request))
            return;
        Viewport *vp = find(request.viewport);
        if (!vp) {
            fail("transport request refers to an unknown viewport");
            return;
        }
        switchTransport(*vp, static_cast<MsgType>(message.type) == MsgType::RequestShm ? Transport::Shm
                                                                                       : Transport::DmabufLinear);
        break;
    }
    case MsgType::Pause: {
        msg::Pause m{};
        if (read(m))
            deliver(m);
        break;
    }
    case MsgType::Properties: {
        const auto json = fdCount == 0 ? decodeProperties(payload) : std::nullopt;
        if (!json) {
            fail("invalid Properties message from the bridge consumer");
            return;
        }
        deliver(PropertiesJson{std::string(*json)});
        break;
    }
    case MsgType::Volume: {
        msg::Volume m{};
        if (read(m))
            deliver(m);
        break;
    }
    case MsgType::Pointer: {
        msg::Pointer m{};
        if (!read(m))
            return;
        if (!find(m.viewport)) {
            fail("Pointer refers to an unknown viewport");
            return;
        }
        deliver(m);
        break;
    }
    case MsgType::Resize: {
        msg::Resize m{};
        if (!read(m))
            return;
        if (!find(m.viewport)) {
            fail("Resize refers to an unknown viewport");
            return;
        }
        deliver(m);
        break;
    }
    case MsgType::Scaling: {
        msg::Scaling m{};
        if (read(m))
            deliver(m);
        break;
    }
    case MsgType::Fps: {
        msg::Fps m{};
        if (read(m))
            deliver(m);
        break;
    }
    case MsgType::Quit: {
        msg::Quit m{};
        if (read(m))
            deliver(m);
        break;
    }
    default:
        break; // unknown (or producer-direction) types are ignored; their fds close with the message
    }
}

} // namespace murasaki::bridge
