#include "dmabufimport.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>

namespace murasaki {

namespace {

// Core GL types and enums are spelled out so this file needs no GL header (and no GL/GLES mix-up).
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
constexpr GLenum kGlTexture2D = 0x0DE1;
constexpr GLenum kGlTextureMinFilter = 0x2801;
constexpr GLenum kGlTextureMagFilter = 0x2800;
constexpr GLenum kGlTextureWrapS = 0x2802;
constexpr GLenum kGlTextureWrapT = 0x2803;
constexpr GLint kGlLinear = 0x2601;
constexpr GLint kGlClampToEdge = 0x812F;
constexpr GLenum kGlNoError = 0;

using GenTexturesFn = void (*)(GLsizei, GLuint *);
using DeleteTexturesFn = void (*)(GLsizei, const GLuint *);
using BindTextureFn = void (*)(GLenum, GLuint);
using TexParameteriFn = void (*)(GLenum, GLenum, GLint);
using GetErrorFn = GLenum (*)();
using TargetTextureFn = void (*)(GLenum, void *);

constexpr EGLint kPlaneFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                EGL_DMA_BUF_PLANE3_FD_EXT};
constexpr EGLint kPlaneOffset[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                    EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
constexpr EGLint kPlanePitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                   EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
constexpr EGLint kPlaneModLo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                   EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
constexpr EGLint kPlaneModHi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                   EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

bool hasExtension(const char *list, const char *name)
{
    if (!list)
        return false;
    const size_t len = std::strlen(name);
    for (const char *p = list; (p = std::strstr(p, name)) != nullptr; p += len) {
        const bool startOk = p == list || p[-1] == ' ';
        const bool endOk = p[len] == ' ' || p[len] == '\0';
        if (startOk && endOk)
            return true;
    }
    return false;
}

void setError(std::string *error, const char *text)
{
    if (error)
        *error = text;
}

} // namespace

void *DmabufImporter::procAddress(const char *name)
{
    return reinterpret_cast<void *>(eglGetProcAddress(name));
}

bool DmabufImporter::init(std::string *error)
{
    m_ready = false;
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        setError(error, "the current OpenGL context is not an EGL context");
        return false;
    }
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import") || !hasExtension(extensions, "EGL_KHR_image_base")) {
        setError(error, "EGL_EXT_image_dma_buf_import is not available");
        return false;
    }
    m_hasModifiers = hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    m_createImage = procAddress("eglCreateImageKHR");
    m_destroyImage = procAddress("eglDestroyImageKHR");
    m_targetTexture = procAddress("glEGLImageTargetTexture2DOES");
    m_genTextures = procAddress("glGenTextures");
    m_deleteTextures = procAddress("glDeleteTextures");
    m_bindTexture = procAddress("glBindTexture");
    m_texParameteri = procAddress("glTexParameteri");
    m_getError = procAddress("glGetError");
    if (!m_createImage || !m_destroyImage || !m_targetTexture || !m_genTextures || !m_deleteTextures || !m_bindTexture
        || !m_texParameteri || !m_getError) {
        setError(error, "the EGL image entry points are missing (GL_OES_EGL_image)");
        return false;
    }
    m_ready = true;
    return true;
}

std::optional<ImportedDmabuf> DmabufImporter::import(const DmabufDescription &d, std::string *error) const
{
    if (!m_ready) {
        setError(error, "dma-buf import is not initialised");
        return std::nullopt;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || d.planeCount < 1 || d.planeCount > 4 || d.width == 0 || d.height == 0) {
        setError(error, "no EGL display or bad dma-buf description");
        return std::nullopt;
    }
    const bool useModifier = d.modifier != kDrmModInvalid;
    if (useModifier && !m_hasModifiers && d.modifier != 0) {
        setError(error, "the driver cannot import a buffer with an explicit modifier");
        return std::nullopt;
    }

    EGLint attribs[8 + 4 * 10 + 1];
    size_t n = 0;
    const auto put = [&](EGLint key, EGLint value) {
        attribs[n++] = key;
        attribs[n++] = value;
    };
    put(EGL_WIDTH, static_cast<EGLint>(d.width));
    put(EGL_HEIGHT, static_cast<EGLint>(d.height));
    put(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(d.fourcc));
    for (uint32_t i = 0; i < d.planeCount; ++i) {
        put(kPlaneFd[i], d.planes[i].fd);
        put(kPlaneOffset[i], static_cast<EGLint>(d.planes[i].offset));
        put(kPlanePitch[i], static_cast<EGLint>(d.planes[i].stride));
        if (useModifier && m_hasModifiers) {
            put(kPlaneModLo[i], static_cast<EGLint>(d.modifier & 0xffffffffu));
            put(kPlaneModHi[i], static_cast<EGLint>(d.modifier >> 32));
        }
    }
    attribs[n] = EGL_NONE;

    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(m_createImage);
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        setError(error, "eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) failed");
        return std::nullopt;
    }

    const auto getError = reinterpret_cast<GetErrorFn>(m_getError);
    for (int i = 0; i < 16 && getError() != kGlNoError; ++i) { // drop errors that are not ours (bounded: a lost
                                                                // context returns an error forever)
    }
    GLuint texture = 0;
    reinterpret_cast<GenTexturesFn>(m_genTextures)(1, &texture);
    reinterpret_cast<BindTextureFn>(m_bindTexture)(kGlTexture2D, texture);
    reinterpret_cast<TargetTextureFn>(m_targetTexture)(kGlTexture2D, image);
    const GLenum glError = getError();
    // Wrap/filter are set here because Qt will not touch parameters of a texture it wraps from outside.
    auto texParameteri = reinterpret_cast<TexParameteriFn>(m_texParameteri);
    texParameteri(kGlTexture2D, kGlTextureMinFilter, kGlLinear);
    texParameteri(kGlTexture2D, kGlTextureMagFilter, kGlLinear);
    texParameteri(kGlTexture2D, kGlTextureWrapS, kGlClampToEdge);
    texParameteri(kGlTexture2D, kGlTextureWrapT, kGlClampToEdge);
    reinterpret_cast<BindTextureFn>(m_bindTexture)(kGlTexture2D, 0);
    if (glError != kGlNoError) {
        reinterpret_cast<DeleteTexturesFn>(m_deleteTextures)(1, &texture);
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(m_destroyImage)(display, image);
        setError(error, "glEGLImageTargetTexture2DOES rejected the image");
        return std::nullopt;
    }
    return ImportedDmabuf{texture, image, display};
}

void DmabufImporter::destroy(ImportedDmabuf &imported) const
{
    if (imported.texture != 0 && m_deleteTextures) {
        const GLuint texture = imported.texture;
        reinterpret_cast<DeleteTexturesFn>(m_deleteTextures)(1, &texture);
    }
    if (imported.eglImage && m_destroyImage)
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(m_destroyImage)(static_cast<EGLDisplay>(imported.eglDisplay),
                                                                    static_cast<EGLImageKHR>(imported.eglImage));
    imported = {};
}

} // namespace murasaki
