/*
 * evo_gl_context_host.cpp - headless EGL/GL 3.3 context for the UI preview
 * harness (render-overhaul GL-2, #78). See evo_gl_context.h.
 *
 * Host only. The .ffpfsc app module never compiles this (EVO_APP_MODULE) - its
 * GL context comes from ps5-opengl (GL-3, evo_gl_context_device.cpp).
 *
 * Mesa's surfaceless platform + llvmpipe: an OpenGL context current against no
 * surface at all (EGL_KHR_surfaceless_context). All rendering then targets an
 * FBO the render interface owns; nothing is ever presented. The container has
 * no display and no /dev/dri, so LIBGL_ALWAYS_SOFTWARE / GALLIUM_DRIVER=llvmpipe
 * are exported by tools/uiview_playback_rml.sh.
 */
#ifdef EVO_RML_GL_HOST

#include "evo_gl_context.h"
#include "rmlui_gl3/RmlUi_Renderer_GL3.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <string>

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLSurface g_srf = EGL_NO_SURFACE;
bool       g_ready = false;

const char* egl_err_str(EGLint e)
{
    switch (e) {
    case EGL_SUCCESS:             return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
    default:                      return "EGL_<other>";
    }
}

} // namespace

extern "C" int evo_gl_context_create(int width, int height)
{
    if (width  < 1) width  = 1920;
    if (height < 1) height = 1080;

    if (g_ready)
        return 1;

    /* Prefer the explicit surfaceless platform; fall back to the default
     * display (which Mesa also routes to surfaceless when EGL_PLATFORM is
     * set). */
    auto get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        g_dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (g_dpy == EGL_NO_DISPLAY)
        g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "[EVO GL] eglGetDisplay failed\n");
        return 0;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(g_dpy, &egl_major, &egl_minor)) {
        std::fprintf(stderr, "[EVO GL] eglInitialize failed (%s)\n", egl_err_str(eglGetError()));
        return 0;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        std::fprintf(stderr, "[EVO GL] eglBindAPI(EGL_OPENGL_API) failed (%s)\n", egl_err_str(eglGetError()));
        return 0;
    }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg = nullptr;
    EGLint    cfg_n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &cfg_n) || cfg_n < 1) {
        std::fprintf(stderr, "[EVO GL] eglChooseConfig failed (%s, n=%d)\n",
                     egl_err_str(eglGetError()), cfg_n);
        return 0;
    }

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[EVO GL] eglCreateContext failed (%s)\n", egl_err_str(eglGetError()));
        return 0;
    }

    /* A pbuffer, not a truly surfaceless context: RmlUi's RenderInterface_GL3
     * EndFrame() blits its final composite to the default framebuffer (0), so
     * framebuffer 0 has to exist and be readable. The pbuffer is that
     * backbuffer; EvoRenderInterfaceGL glReadPixels() it. Sized to the UI
     * (1920x1080) - EVO's host UI never changes resolution. */
    const EGLint pb_attr[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    g_srf = eglCreatePbufferSurface(g_dpy, cfg, pb_attr);
    if (g_srf == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[EVO GL] eglCreatePbufferSurface failed (%s)\n", egl_err_str(eglGetError()));
        return 0;
    }

    if (!eglMakeCurrent(g_dpy, g_srf, g_srf, g_ctx)) {
        std::fprintf(stderr, "[EVO GL] eglMakeCurrent failed (%s)\n", egl_err_str(eglGetError()));
        return 0;
    }

    std::string msg;
    if (!RmlGL3::Initialize(&msg)) {
        std::fprintf(stderr, "[EVO GL] RmlGL3::Initialize failed: %s\n", msg.c_str());
        return 0;
    }

    std::fprintf(stderr, "[EVO GL] EGL %d.%d, %s\n", egl_major, egl_minor,
                 msg.empty() ? "GL loaded" : msg.c_str());
    g_ready = true;
    return 1;
}

extern "C" void evo_gl_context_destroy(void)
{
    if (!g_ready && g_ctx == EGL_NO_CONTEXT)
        return;
    RmlGL3::Shutdown();
    if (g_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_srf != EGL_NO_SURFACE)
            eglDestroySurface(g_dpy, g_srf);
        if (g_ctx != EGL_NO_CONTEXT)
            eglDestroyContext(g_dpy, g_ctx);
        eglTerminate(g_dpy);
    }
    g_srf = EGL_NO_SURFACE;
    g_ctx = EGL_NO_CONTEXT;
    g_dpy = EGL_NO_DISPLAY;
    g_ready = false;
}

extern "C" int evo_gl_context_ok(void)
{
    return g_ready ? 1 : 0;
}

extern "C" void evo_gl_context_present(void)
{
    /* Surfaceless pbuffer: nothing to present. EvoRenderInterfaceGL reads the
     * backbuffer back into the caller's framebuffer in FrameEnd(). */
}

extern "C" void evo_gl_context_size(int *w, int *h)
{
    if (w) *w = g_ready ? 1920 : 0;
    if (h) *h = g_ready ? 1080 : 0;
}

#endif /* EVO_RML_GL_HOST */
