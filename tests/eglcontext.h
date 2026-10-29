#pragma once

// Shared by the GPU tests: render node detection and a surfaceless desktop-GL EGL context made current on the
// calling thread.

#include <QDir>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>

namespace testegl {

inline bool haveRenderNode()
{
    return !QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Files).isEmpty();
}

inline bool hasExt(const char *list, const char *name)
{
    return list && std::strstr(list, name) != nullptr;
}

// A surfaceless desktop-GL context of our own, current on this thread.
struct EglContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;

    ~EglContext()
    {
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT)
                eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }

    bool create()
    {
        const char *clientExt = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (hasExt(clientExt, "EGL_MESA_platform_surfaceless")) {
            auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (getPlatformDisplay)
                display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        }
        if (display == EGL_NO_DISPLAY)
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0, minor = 0;
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
            return false;
        if (!hasExt(eglQueryString(display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context"))
            return false;
        if (!eglBindAPI(EGL_OPENGL_API))
            return false;
        const EGLint attribs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        // Surfaceless contexts need no surface type; fall back to any desktop-GL config.
        if (!eglChooseConfig(display, attribs, &config, 1, &count) || count == 0) {
            const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
            if (!eglChooseConfig(display, any, &config, 1, &count) || count == 0)
                return false;
        }
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    }
};

} // namespace testegl
