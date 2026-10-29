#pragma once

// Imports Linux dma-bufs into OpenGL textures through EGL (EGL_EXT_image_dma_buf_import). Qt-free so that the
// scene-graph render thread and a test with its own surfaceless EGL context use the same code.
//
// Every call needs a current OpenGL context created on an EGL display (Qt's OpenGL backend on Wayland or EGL/xcb,
// or a context the caller made current itself); the display is taken from eglGetCurrentDisplay(). On GLX the
// import reports itself unsupported and the caller falls back to shared memory.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace murasaki {

// DRM_FORMAT_MOD_INVALID: "no modifier information, let the driver pick" (implicit modifier).
inline constexpr uint64_t kDrmModInvalid = 0x00ffffffffffffffULL;

struct DmabufPlane {
    int fd = -1; // borrowed: the import does not take ownership
    uint32_t offset = 0;
    uint32_t stride = 0;
};

struct DmabufDescription {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = kDrmModInvalid;
    uint32_t planeCount = 0; // 1..4
    std::array<DmabufPlane, 4> planes;
};

// An EGLImage bound to a GL_TEXTURE_2D. Destroy it with DmabufImporter::destroy() while the same context (or one
// sharing with it) is current.
struct ImportedDmabuf {
    uint32_t texture = 0;
    void *eglImage = nullptr;  // EGLImage
    void *eglDisplay = nullptr; // EGLDisplay the image belongs to
};

class DmabufImporter
{
public:
    // Looks up the entry points on the current context. False when the context is not EGL based or lacks
    // EGL_EXT_image_dma_buf_import / GL_OES_EGL_image; `error` then says what is missing.
    bool init(std::string *error = nullptr);
    bool ready() const { return m_ready; }

    // Creates the EGLImage and a texture bound to it. The fds only need to stay open during the call.
    std::optional<ImportedDmabuf> import(const DmabufDescription &description, std::string *error = nullptr) const;
    void destroy(ImportedDmabuf &imported) const;

    // Entry point lookup shared with the tests that read the texture back.
    static void *procAddress(const char *name);

private:
    bool m_ready = false;
    bool m_hasModifiers = false;
    void *m_createImage = nullptr;  // eglCreateImageKHR
    void *m_destroyImage = nullptr; // eglDestroyImageKHR
    void *m_targetTexture = nullptr; // glEGLImageTargetTexture2DOES
    void *m_genTextures = nullptr;
    void *m_deleteTextures = nullptr;
    void *m_bindTexture = nullptr;
    void *m_texParameteri = nullptr;
    void *m_getError = nullptr;
};

} // namespace murasaki
