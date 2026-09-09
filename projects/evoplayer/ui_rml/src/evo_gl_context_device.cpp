/*
 * evo_gl_context_device.cpp - persistent EGL/GL 3.3 core context on the PS5,
 * through ps5-opengl (render-overhaul GL-3, #79). See evo_gl_context.h.
 *
 * Device only. Compiled solely into a `--gl` .ffpfsc (EVO_GL_DEVICE, the
 * Makefile GL_DEVICE=1 branch). The host preview harness gets its context from
 * evo_gl_context_host.cpp (EVO_RML_GL_HOST) instead.
 *
 * Unlike pp_gl_smoke.c (the GL-1 go/no-go probe, which brings GL up, reads one
 * pixel back and tears it all down) this context is PROCESS-LIFETIME: created
 * once in main()'s pre-unjail slot, next to where pp_agc_init used to run, and
 * never destroyed. ps5-opengl calls sceAgcInit and owns the sceVideoOut flip
 * queue for the whole session - pp_videoout / pp_agc present are removed, not
 * run alongside (a second sceVideoOut open panics the console).
 *
 * B1 scope: EGL window surface + GL 3.3 core context + eglSwapBuffers. No RmlUi
 * wiring yet - main()'s B1 loop just glClear()s a solid colour and presents.
 * B2 grafts RmlGL3 / EvoRenderInterfaceGL onto this same context.
 */
#if defined(EVO_GL_DEVICE)

#include "evo_gl_context.h"

#include <cstdlib>
#include <cstring>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "evo_boot_log.h"
#include "evo_boot_trace.h"   /* evo_bt_ -> klog (live, pre-unjail) + evo.log */

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_srf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;
int        g_w = 0, g_h = 0;
bool       g_ready = false;

const EGLint k_cfg_attr[] = {
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE,
};
const EGLint k_ctx_attr[] = {
    EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
    EGL_CONTEXT_MINOR_VERSION_KHR, 3,
    EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
    EGL_NONE,
};

} // namespace

extern "C" int evo_gl_context_create(int width, int height)
{
    if (g_ready)
        return 1;
    if (width  < 1) width  = 1920;
    if (height < 1) height = 1080;
    g_w = width;
    g_h = height;

    /* 1: EGL display + init. ps5-opengl's platform layer routes
     * EGL_DEFAULT_DISPLAY to its own sceVideoOut-backed display. */
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint egl_major = 0, egl_minor = 0;
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &egl_major, &egl_minor)) {
        evo_bt_("GL ctx: eglInitialize failed (0x%x)", eglGetError());
        return 0;
    }
    evo_bt_("GL ctx: EGL %d.%d up", egl_major, egl_minor);

    /* 2: bind the desktop-GL API + choose a config */
    EGLConfig cfg = nullptr;
    EGLint    cfg_n = 0;
    if (!eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(g_dpy, k_cfg_attr, &cfg, 1, &cfg_n) || cfg_n != 1) {
        evo_bt_("GL ctx: eglChooseConfig failed (0x%x, n=%d)", eglGetError(), cfg_n);
        return 0;
    }

    /* 3: fullscreen window surface + context + make current. ps5-opengl takes a
     * null native window and drives the flip queue itself. */
    g_srf = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)0, nullptr);
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, k_ctx_attr);
    if (g_srf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(g_dpy, g_srf, g_srf, g_ctx)) {
        evo_bt_("GL ctx: surface/context/makeCurrent failed (0x%x)", eglGetError());
        return 0;
    }

    const char *v_vendor = (const char *)glGetString(GL_VENDOR);
    const char *v_render = (const char *)glGetString(GL_RENDERER);
    const char *v_ver    = (const char *)glGetString(GL_VERSION);
    const char *v_glsl   = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    evo_bt_("GL ctx: current - %s / %s / GL %s / GLSL %s",
            v_vendor ? v_vendor : "?", v_render ? v_render : "?",
            v_ver ? v_ver : "?", v_glsl ? v_glsl : "?");

    /* Don't block the frame loop on vblank inside eglSwapBuffers - the loop
     * needs to keep polling the pad. Menu redraws are change-only (GL-3 B2), so
     * tearing is a non-issue; a slow synchronous flip here was eating button
     * presses. */
    eglSwapInterval(g_dpy, 0);

    glViewport(0, 0, g_w, g_h);
    g_ready = true;
    return 1;
}

extern "C" void evo_gl_context_destroy(void)
{
    if (g_dpy == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_dpy, g_ctx);
    if (g_srf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, g_srf);
    eglTerminate(g_dpy);
    g_dpy = EGL_NO_DISPLAY;
    g_srf = EGL_NO_SURFACE;
    g_ctx = EGL_NO_CONTEXT;
    g_ready = false;
}

extern "C" int evo_gl_context_ok(void)
{
    return g_ready ? 1 : 0;
}

extern "C" void evo_gl_context_present(void)
{
    if (g_ready)
        eglSwapBuffers(g_dpy, g_srf);
}

extern "C" void evo_gl_frame_begin(void)
{
    if (!g_ready)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* Menu backdrop - dark neutral. Most RmlUi screens paint an opaque body
     * over this; it only shows through translucent overlays / letterboxing. */
    glClearColor(0x0d / 255.0f, 0x0d / 255.0f, 0x10 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* ===================================================================== *
 *  CPU-rasterise + GL-blit present (GL-3 B2 default path)
 * ===================================================================== */
namespace {

GLuint g_blit_prog = 0, g_blit_vao = 0, g_blit_vbo = 0, g_blit_tex = 0;
int    g_blit_tw = 0, g_blit_th = 0;

const char *k_blit_vs =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    /* fullscreen triangle; flip V so a top-down EVO buffer samples upright */
    "  vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  vUV = vec2(p.x, 1.0 - p.y);\n"
    "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
    "}\n";
const char *k_blit_fs =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c; uniform sampler2D uTex;\n"
    /* Immutable single-mip RGBA8 storage (glTexStorage2D) is ps5-opengl's fast
     * texture path (its G13 note). EVO's buffer is BGRA-in-memory, so upload it
     * as GL_RGBA and swizzle .bgr in the sampler. */
    "void main(){ c = vec4(texture(uTex, vUV).bgr, 1.0); }\n";

GLuint blit_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof log, &n, log);
        evo_bt_("GL blit: shader compile failed: %.*s", (int)n, log);
        glDeleteShader(s); return 0;
    }
    return s;
}

bool blit_init(void)
{
    if (g_blit_prog) return true;
    GLuint vs = blit_compile(GL_VERTEX_SHADER, k_blit_vs);
    GLuint fs = blit_compile(GL_FRAGMENT_SHADER, k_blit_fs);
    if (!vs || !fs) return false;
    g_blit_prog = glCreateProgram();
    glAttachShader(g_blit_prog, vs);
    glAttachShader(g_blit_prog, fs);
    glLinkProgram(g_blit_prog);
    GLint ok = 0; glGetProgramiv(g_blit_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0; glGetProgramInfoLog(g_blit_prog, sizeof log, &n, log);
        evo_bt_("GL blit: link failed: %.*s", (int)n, log);
        glDeleteProgram(g_blit_prog); g_blit_prog = 0; return false;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    glGenVertexArrays(1, &g_blit_vao);   /* attribute-less; VAO still required in core */
    glGenTextures(1, &g_blit_tex);       /* storage allocated lazily in the blit */
    evo_bt_("GL blit: initialised");
    return true;
}

} // namespace

extern "C" void evo_gl_blit_bgra(const uint32_t *fb, int w, int h)
{
    if (!g_ready || !fb || w <= 0 || h <= 0)
        return;
    if (!blit_init())
        return;

    glBindTexture(GL_TEXTURE_2D, g_blit_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (w != g_blit_tw || h != g_blit_th) {
        /* Immutable single-mip RGBA8 storage - ps5-opengl's least-slow texture
         * path (its "G13"). It still does a CPU staging copy on the upload
         * (~68 ms for 1080p on G47) - the video stutters until that's fixed
         * driver-side or the video path switches to a zero-copy / smaller
         * upload (GL-4). Re-create on a resolution change. */
        if (g_blit_tw) { glDeleteTextures(1, &g_blit_tex); glGenTextures(1, &g_blit_tex); }
        glBindTexture(GL_TEXTURE_2D, g_blit_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        g_blit_tw = w; g_blit_th = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, fb);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g_blit_prog);
    glBindVertexArray(g_blit_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_blit_tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

extern "C" void evo_gl_read_default_fb(uint32_t *bgra, int w, int h)
{
    if (!g_ready || !bgra || w <= 0 || h <= 0)
        return;
    /* fb 0 bytes are R,G,B,A == 0xAABBGGRR in memory, y-up. Read into a scratch
     * row-reversed into the caller's top-down buffer, alpha forced opaque. */
    static uint32_t *s_row = nullptr;
    static int s_row_w = 0;
    if (s_row_w < w) {
        free(s_row);
        s_row = (uint32_t *)malloc((size_t)w * 4);
        s_row_w = s_row ? w : 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int y = 0; y < h; y++) {
        uint32_t *dst = &bgra[(size_t)y * w];
        if (s_row) {
            glReadPixels(0, h - 1 - y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, s_row);
            for (int x = 0; x < w; x++)
                dst[x] = 0xFF000000u | (s_row[x] & 0x00FFFFFFu);
        }
    }
}

extern "C" void evo_gl_context_size(int *w, int *h)
{
    if (w) *w = g_ready ? g_w : 0;
    if (h) *h = g_ready ? g_h : 0;
}

#endif /* EVO_GL_DEVICE */
