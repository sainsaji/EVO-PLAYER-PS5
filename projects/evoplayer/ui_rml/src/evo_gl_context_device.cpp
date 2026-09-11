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

/* ===================================================================== *
 *  GL-4 (#80): YUV 4:2:0 video present — R8 (+ RG8) planes + YUV->RGB shader
 *
 *  glTexSubImage2D of a 1080p RGBA8 frame is ~65 ms on ps5-opengl G55 (a
 *  synchronous CPU staging copy); R8 / RG8 uploads take a fast path and are
 *  ~free (0.10 / 0.03 ms at 1080p). Zero-copy: the planes come straight from
 *  the decoder's frame pool (no CPU YUV->RGB, no intermediate buffer) and the
 *  shader does the conversion on the quad. Matrix matches the CPU converter
 *  (pp_converter.c: 298/409/516/-100/-208 >> 8, BT.601 limited) for #62 parity.
 * ===================================================================== */
namespace {

GLuint g_yuv_vao = 0;
GLuint g_yuv_nv_prog = 0, g_yuv_pl_prog = 0, g_yuv_pl10_prog = 0, g_yuv_pl10_hdr_prog = 0;
GLuint g_yuv_pl10_nv_hdr_prog = 0;
GLuint g_yuv_ytex = 0, g_yuv_uvtex = 0, g_yuv_utex = 0, g_yuv_vtex = 0;
int    g_yuv_tw = 0, g_yuv_th = 0, g_yuv_planar = -1, g_yuv_ten = -1;
GLint  g_yuv_nv_crop = -1, g_yuv_nv_scale = -1;
GLint  g_yuv_pl_crop = -1, g_yuv_pl_scale = -1;
GLint  g_yuv_pl10_crop = -1, g_yuv_pl10_scale = -1;
GLint  g_yuv_pl10_hdr_crop = -1, g_yuv_pl10_hdr_scale = -1;
GLint  g_yuv_pl10_nv_hdr_crop = -1, g_yuv_pl10_nv_hdr_scale = -1;

const char *k_yuv_vs =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "uniform vec2 uCrop;\n"    /* disp/coded — trims MB padding                 */
    "uniform vec2 uScale;\n"   /* aspect: letterbox (<1) / fill-overflow (>1)   */
    "void main(){\n"
    "  vec2 p = vec2(gl_VertexID & 1, (gl_VertexID >> 1) & 1);\n"  /* quad strip */
    /* flip V (EVO frames are top-down); scale into the valid disp region */
    "  vUV = vec2(p.x, 1.0 - p.y) * uCrop;\n"
    "  gl_Position = vec4((p*2.0-1.0) * uScale, 0.0, 1.0);\n"
    "}\n";
/*
 * BT.601 limited range. The coefficients are the CPU converter's fixed-point
 * matrix divided by 256 (298/409/516/-100/-208 -> exact binary fractions), and
 * the offsets are its `y - 16` / `uv - 128` expressed in the 0..1 the sampler
 * hands back: 16/255 and 128/255, NOT 16/256 and 0.5. Using the power-of-two
 * offsets instead costs up to 2/255 per channel against the reference - see
 * tools/gl_yuv_parity.py, which sweeps all 2^24 triples and is what #62's
 * parity claim rests on.
 *
 * Output is swizzled .bgr — the ps5-opengl default framebuffer scans out as
 * BGRA (same reason evo_gl_blit_bgra's sampler swizzles).
 */
#define YUV_MATRIX_GLSL \
    "  float Y = (y - 0.0627451) * 1.1640625;\n" \
    "  vec3 rgb = vec3(Y + 1.59765625*V,\n" \
    "                  Y - 0.390625*U - 0.8125*V,\n" \
    "                  Y + 2.015625*U);\n" \
    "  c = vec4(clamp(rgb, 0.0, 1.0).bgr, 1.0);\n"
const char *k_yuv_fs_nv =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uY; uniform sampler2D uUV;\n"
    "void main(){\n"
    "  float y = texture(uY, vUV).r;\n"
    "  vec2 uv = texture(uUV, vUV).rg;\n"
    "  float U = uv.x - 0.5; float V = uv.y - 0.5;\n"
    YUV_MATRIX_GLSL
    "}\n";
const char *k_yuv_fs_pl =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uY; uniform sampler2D uU; uniform sampler2D uV;\n"
    "void main(){\n"
    "  float y = texture(uY, vUV).r;\n"
    "  float U = texture(uU, vUV).r - 0.5;\n"
    "  float V = texture(uV, vUV).r - 0.5;\n"
    YUV_MATRIX_GLSL
    "}\n";
/*
 * GL-5 (#81): planar 10-bit (yuv420p10le). GL_R16 normalises the sample by
 * /65535, but the 10 significant bits sit in the low bits (max value 1023), so
 * scale back up by 65535/1023 to land 1023 -> 1.0. After that the same BT.601
 * limited matrix applies unchanged: the 8-bit black level 16/255 = 0.0627 and
 * the 10-bit 64/1023 = 0.0626 are the same number to within 1e-4, likewise
 * 512/1023 vs 0.5 for neutral chroma. SDR only - a PQ/HLG EOTF + tone-map would
 * go here (#41).
 */
const char *k_yuv_fs_pl10 =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uY; uniform sampler2D uU; uniform sampler2D uV;\n"
    "const float S = 65535.0 / 1023.0;\n"
    "void main(){\n"
    "  float y = texture(uY, vUV).r * S;\n"
    "  float U = texture(uU, vUV).r * S - 0.5;\n"
    "  float V = texture(uV, vUV).r * S - 0.5;\n"
    /* #41: PQ/HLG EOTF + SDR tone-map seam - would transform (y,U,V) here. */
    YUV_MATRIX_GLSL
    "}\n";

const char *k_yuv_fs_pl10_hdr =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uY; uniform sampler2D uU; uniform sampler2D uV;\n"
    "const float S = 65535.0 / 1023.0;\n"
    /* BT.2020 non-constant-luminance YCbCr -> R'G'B' (still PQ-encoded, not
     * linear yet) - Kr=0.2627 Kb=0.0593, same published matrix ffmpeg/zscale
     * use. Distinct from BT.601 (#62's matrix): using 601 on 2020 content is
     * a real color error, not just a missing tone-curve. */
    "vec3 yuv2020(float y, float U, float V) {\n"
    "  float Yp = (y - 0.0627451) * 1.1640625;\n"
    "  return vec3(Yp + 1.4746*V, Yp - 0.16455*U - 0.57135*V, Yp + 1.8814*U);\n"
    "}\n"
    /* ST.2084 (PQ) inverse EOTF: PQ code value (0..1) -> linear light,
     * normalized so 1.0 == 10000 nits. Constants are the published SMPTE
     * ST.2084 m1/m2/c1/c2/c3 - do not approximate these. */
    "float pq_eotf(float n) {\n"
    "  const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;\n"
    "  float np = pow(max(n,0.0), 1.0/m2);\n"
    "  return pow(max(np-c1,0.0) / (c2 - c3*np), 1.0/m1);\n"
    "}\n"
    "void main(){\n"
    "  float y = texture(uY, vUV).r * S;\n"
    "  float U = texture(uU, vUV).r * S - 0.5;\n"
    "  float V = texture(uV, vUV).r * S - 0.5;\n"
    "  vec3 pq_rgb = clamp(yuv2020(y, U, V), 0.0, 1.0);\n"
    /* Per-channel EOTF -> absolute nits, rescaled so 1.0 == 100-nit SDR white. */
    "  vec3 nits = vec3(pq_eotf(pq_rgb.r), pq_eotf(pq_rgb.g), pq_eotf(pq_rgb.b)) * 100.0;\n"
    /* Reinhard: simplest stable tone-map, folds unbounded highlights into
     * 0..1 without clipping. Swap for a filmic curve (Hable/ACES) later -
     * nothing else in this chain changes. */
    "  vec3 sdr_linear = nits / (1.0 + nits);\n"
    /* Re-encode for SDR scanout. The BT.601 shader never needed this step -
     * studio YCbCr there is already gamma-encoded; here we decoded all the
     * way to scene-linear to tone-map correctly, so it must go back. */
    "  vec3 rgb = pow(sdr_linear, vec3(1.0/2.2));\n"
    "  c = vec4(rgb.bgr, 1.0);\n"
    "}\n";

const char *k_yuv_fs_pl10_nv_hdr =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uY; uniform sampler2D uUV;\n"
    "const float S = 65535.0 / 1023.0;\n"
    "vec3 yuv2020(float y, float U, float V) {\n"
    "  float Yp = (y - 0.0627451) * 1.1640625;\n"
    "  return vec3(Yp + 1.4746*V, Yp - 0.16455*U - 0.57135*V, Yp + 1.8814*U);\n"
    "}\n"
    "float pq_eotf(float n) {\n"
    "  const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;\n"
    "  float np = pow(max(n,0.0), 1.0/m2);\n"
    "  return pow(max(np-c1,0.0) / (c2 - c3*np), 1.0/m1);\n"
    "}\n"
    "void main(){\n"
    "  float y = texture(uY, vUV).r * S;\n"
    "  vec2 uv = texture(uUV, vUV).rg * S - 0.5;\n"
    "  float U = uv.x;\n"
    "  float V = uv.y;\n"
    "  vec3 pq_rgb = clamp(yuv2020(y, U, V), 0.0, 1.0);\n"
    "  vec3 nits = vec3(pq_eotf(pq_rgb.r), pq_eotf(pq_rgb.g), pq_eotf(pq_rgb.b)) * 100.0;\n"
    "  vec3 sdr_linear = nits / (1.0 + nits);\n"
    "  vec3 rgb = pow(sdr_linear, vec3(1.0/2.2));\n"
    "  c = vec4(rgb.bgr, 1.0);\n"
    "}\n";

GLuint yuv_link(const char *fs_src)
{
    GLuint vs = blit_compile(GL_VERTEX_SHADER, k_yuv_vs);
    GLuint fs = blit_compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0; glGetProgramInfoLog(p, sizeof log, &n, log);
        evo_bt_("GL yuv: link failed: %.*s", (int)n, log);
        glDeleteProgram(p); p = 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

bool yuv_init(void)
{
    if (g_yuv_nv_prog && g_yuv_pl_prog && g_yuv_pl10_prog && g_yuv_pl10_hdr_prog && g_yuv_pl10_nv_hdr_prog) return true;
    if (!g_yuv_nv_prog)          g_yuv_nv_prog          = yuv_link(k_yuv_fs_nv);
    if (!g_yuv_pl_prog)          g_yuv_pl_prog          = yuv_link(k_yuv_fs_pl);
    if (!g_yuv_pl10_prog)        g_yuv_pl10_prog        = yuv_link(k_yuv_fs_pl10);
    if (!g_yuv_pl10_hdr_prog)    g_yuv_pl10_hdr_prog    = yuv_link(k_yuv_fs_pl10_hdr);
    if (!g_yuv_pl10_nv_hdr_prog) g_yuv_pl10_nv_hdr_prog = yuv_link(k_yuv_fs_pl10_nv_hdr);
    if (!g_yuv_nv_prog || !g_yuv_pl_prog || !g_yuv_pl10_prog || !g_yuv_pl10_hdr_prog || !g_yuv_pl10_nv_hdr_prog) return false;
    if (!g_yuv_vao) glGenVertexArrays(1, &g_yuv_vao);
    if (!g_yuv_ytex) {
        glGenTextures(1, &g_yuv_ytex);  glGenTextures(1, &g_yuv_uvtex);
        glGenTextures(1, &g_yuv_utex);  glGenTextures(1, &g_yuv_vtex);
    }
    glUseProgram(g_yuv_nv_prog);
    glUniform1i(glGetUniformLocation(g_yuv_nv_prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(g_yuv_nv_prog, "uUV"), 1);
    g_yuv_nv_crop  = glGetUniformLocation(g_yuv_nv_prog, "uCrop");
    g_yuv_nv_scale = glGetUniformLocation(g_yuv_nv_prog, "uScale");
    glUseProgram(g_yuv_pl_prog);
    glUniform1i(glGetUniformLocation(g_yuv_pl_prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(g_yuv_pl_prog, "uU"), 1);
    glUniform1i(glGetUniformLocation(g_yuv_pl_prog, "uV"), 2);
    g_yuv_pl_crop  = glGetUniformLocation(g_yuv_pl_prog, "uCrop");
    g_yuv_pl_scale = glGetUniformLocation(g_yuv_pl_prog, "uScale");
    glUseProgram(g_yuv_pl10_prog);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_prog, "uU"), 1);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_prog, "uV"), 2);
    g_yuv_pl10_crop  = glGetUniformLocation(g_yuv_pl10_prog, "uCrop");
    g_yuv_pl10_scale = glGetUniformLocation(g_yuv_pl10_prog, "uScale");
    glUseProgram(g_yuv_pl10_hdr_prog);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_hdr_prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_hdr_prog, "uU"), 1);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_hdr_prog, "uV"), 2);
    g_yuv_pl10_hdr_crop  = glGetUniformLocation(g_yuv_pl10_hdr_prog, "uCrop");
    g_yuv_pl10_hdr_scale = glGetUniformLocation(g_yuv_pl10_hdr_prog, "uScale");
    glUseProgram(g_yuv_pl10_nv_hdr_prog);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_nv_hdr_prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(g_yuv_pl10_nv_hdr_prog, "uUV"), 1);
    g_yuv_pl10_nv_hdr_crop  = glGetUniformLocation(g_yuv_pl10_nv_hdr_prog, "uCrop");
    g_yuv_pl10_nv_hdr_scale = glGetUniformLocation(g_yuv_pl10_nv_hdr_prog, "uScale");
    evo_bt_("GL yuv: initialised");
    return true;
}

void yuv_tex_setup(GLuint tex, GLenum ifmt, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, w, h);
}

} // namespace

extern "C" void evo_gl_blit_yuv(const uint8_t *y,  int y_pitch,
                                const uint8_t *uv, int uv_pitch,
                                const uint8_t *u,  int u_pitch,
                                const uint8_t *v,  int v_pitch,
                                int coded_w, int coded_h, int disp_w, int disp_h,
                                int view_mode, int ten_bit, int color_trc)
{
    if (!g_ready || !y || y_pitch <= 0 || coded_w <= 0 || coded_h <= 0)
        return;
    const int planar = (uv == nullptr);
    if (planar ? (!u || !v) : (uv == nullptr))
        return;
    if (!yuv_init())
        return;

    const int tw = coded_w, th = coded_h;   /* luma texture = coded image (stride via ROW_LENGTH) */
    const int cw2 = tw / 2, ch2 = th / 2;
    /* 10-bit planes are 16-bit; pitches arrive in bytes, ROW_LENGTH wants
     * samples. GL_UNPACK_ALIGNMENT 2 for GL_UNSIGNED_SHORT rows. */
    const int   sdiv = ten_bit ? 2 : 1;
    const GLenum luma_ifmt = ten_bit ? GL_R16 : GL_R8;
    const GLenum utype     = ten_bit ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    glPixelStorei(GL_UNPACK_ALIGNMENT, ten_bit ? 2 : 1);

    if (tw != g_yuv_tw || th != g_yuv_th || planar != g_yuv_planar ||
        ten_bit != g_yuv_ten) {
        glDeleteTextures(1, &g_yuv_ytex);  glGenTextures(1, &g_yuv_ytex);
        glDeleteTextures(1, &g_yuv_uvtex); glGenTextures(1, &g_yuv_uvtex);
        glDeleteTextures(1, &g_yuv_utex);  glGenTextures(1, &g_yuv_utex);
        glDeleteTextures(1, &g_yuv_vtex);  glGenTextures(1, &g_yuv_vtex);
        yuv_tex_setup(g_yuv_ytex, luma_ifmt, tw, th);
        if (planar) {
            yuv_tex_setup(g_yuv_utex, luma_ifmt, cw2, ch2);
            yuv_tex_setup(g_yuv_vtex, luma_ifmt, cw2, ch2);
        } else {
            yuv_tex_setup(g_yuv_uvtex, ten_bit ? GL_RG16 : GL_RG8, cw2, ch2);
        }
        g_yuv_tw = tw; g_yuv_th = th; g_yuv_planar = planar; g_yuv_ten = ten_bit;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_yuv_ytex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_pitch / sdiv);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RED, utype, y);
    if (planar) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_yuv_utex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, u_pitch / sdiv);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw2, ch2, GL_RED, utype, u);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_yuv_vtex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, v_pitch / sdiv);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw2, ch2, GL_RED, utype, v);
    } else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_yuv_uvtex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, ten_bit ? (uv_pitch / 4) : (uv_pitch / 2));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw2, ch2, GL_RG, utype, uv);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    /* 16 is AVCOL_TRC_SMPTE2084 (PQ). HLG (18) left on k_yuv_fs_pl10 passthrough for now (known gap). */
    GLuint prog;
    GLint  crop, scale;
    if (ten_bit) {
        if (!planar) {
            /* #41 Phase D: two-plane 10-bit is native-decoder-only today
             * (evo_vdec_native_supports() gates it to HEVC Main10 / VP9
             * Profile 2), and there is no non-PQ two-plane 10-bit shader, so
             * this always tone-maps. Fine while the only source is that gate
             * - the research repo's own Main10/Profile2 test streams were
             * BT.2020/PQ - but the gate itself doesn't check color_trc, so a
             * Main10-profiled file that ISN'T actually PQ-tagged would still
             * wash out here. Add a plain two-plane BT.601 passthrough
             * (mirroring k_yuv_fs_pl) if that ever turns out to matter. */
            prog  = g_yuv_pl10_nv_hdr_prog;
            crop  = g_yuv_pl10_nv_hdr_crop;
            scale = g_yuv_pl10_nv_hdr_scale;
        } else {
            prog  = (color_trc == 16) ? g_yuv_pl10_hdr_prog : g_yuv_pl10_prog;
            crop  = (color_trc == 16) ? g_yuv_pl10_hdr_crop : g_yuv_pl10_crop;
            scale = (color_trc == 16) ? g_yuv_pl10_hdr_scale : g_yuv_pl10_scale;
        }
    } else {
        prog  = planar ? g_yuv_pl_prog : g_yuv_nv_prog;
        crop  = planar ? g_yuv_pl_crop : g_yuv_nv_crop;
        scale = planar ? g_yuv_pl_scale : g_yuv_nv_scale;
    }
    glUseProgram(prog);
    float cx = (disp_w > 0 && disp_w <= tw) ? (float)disp_w / (float)tw : 1.0f;
    float cy = (disp_h > 0 && disp_h <= th) ? (float)disp_h / (float)th : 1.0f;
    glUniform2f(crop, cx, cy);

    /* Aspect: 0=FIT (letterbox), 1=FILL (crop overflow), 2=STRETCH. */
    float sx = 1.0f, sy = 1.0f;
    if (view_mode != 2 && disp_w > 0 && disp_h > 0 && g_w > 0 && g_h > 0) {
        float va = (float)disp_w / (float)disp_h;   /* video aspect  */
        float sa = (float)g_w    / (float)g_h;      /* screen aspect */
        if (view_mode == 0) {                       /* FIT           */
            if (va > sa) sy = sa / va; else sx = va / sa;
        } else {                                     /* FILL          */
            if (va > sa) sx = va / sa; else sy = sa / va;
        }
    }
    glUniform2f(scale, sx, sy);

    glBindVertexArray(g_yuv_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glActiveTexture(GL_TEXTURE0);   /* leave unit 0 active for the blit path */
}

/* ---- OSD composite over the video quad (GL-4 Stage 2c) ----------------
 *
 * The scratch is a full-frame BGRA buffer, and a 4-channel RGBA8
 * glTexSubImage2D of it is ps5-opengl's ~65 ms synchronous staging copy — so a
 * subtitle cue change (or the progress bar ticking) was a ~65 ms hitch every
 * time. RG8 uploads dodge that wall (same as the video Y/UV planes). So the
 * BGRA buffer is uploaded as an RG8 texture 2× the width: source pixel x is
 * texels 2x (= R,G) and 2x+1 (= B,A) in memory byte order, and the shader
 * reassembles with texelFetch. ~0.2 ms instead of ~65 ms.
 */
namespace {
GLuint g_osd_prog = 0, g_osd_vao = 0, g_osd_tex = 0;
int    g_osd_tw = 0, g_osd_th = 0;
GLint  g_osd_size = -1;
const char *k_osd_fs =
    "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D uTex;\n"   /* RG8, 2*W wide */
    "uniform ivec2 uSize;\n"      /* W, H of the source scratch */
    "void main(){\n"
    "  ivec2 p = clamp(ivec2(vUV * vec2(uSize)), ivec2(0), uSize - 1);\n"
    "  vec2 rg = texelFetch(uTex, ivec2(p.x*2,   p.y), 0).rg;\n"  /* R,G */
    "  vec2 ba = texelFetch(uTex, ivec2(p.x*2+1, p.y), 0).rg;\n"  /* B,A */
    /* memory order is R,G,B,A; the default framebuffer scans out BGRA */
    "  c = vec4(ba.x, rg.y, rg.x, ba.y);\n"
    "}\n";

bool osd_init(void)
{
    if (g_osd_prog) return true;
    GLuint vs = blit_compile(GL_VERTEX_SHADER, k_blit_vs);
    GLuint fs = blit_compile(GL_FRAGMENT_SHADER, k_osd_fs);
    if (!vs || !fs) return false;
    g_osd_prog = glCreateProgram();
    glAttachShader(g_osd_prog, vs); glAttachShader(g_osd_prog, fs);
    glLinkProgram(g_osd_prog);
    GLint ok = 0; glGetProgramiv(g_osd_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0; glGetProgramInfoLog(g_osd_prog, sizeof log, &n, log);
        evo_bt_("GL osd: link failed: %.*s", (int)n, log);
        glDeleteProgram(g_osd_prog); g_osd_prog = 0; return false;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    glGenVertexArrays(1, &g_osd_vao);
    glGenTextures(1, &g_osd_tex);
    glUseProgram(g_osd_prog);
    glUniform1i(glGetUniformLocation(g_osd_prog, "uTex"), 0);
    g_osd_size = glGetUniformLocation(g_osd_prog, "uSize");
    evo_bt_("GL osd: initialised");
    return true;
}
} // namespace

extern "C" void evo_gl_composite_bgra(const uint32_t *fb, int w, int h, int upload)
{
    if (!g_ready || !fb || w <= 0 || h <= 0)
        return;
    if (!osd_init())
        return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_osd_tex);
    if (w != g_osd_tw || h != g_osd_th) {
        glDeleteTextures(1, &g_osd_tex); glGenTextures(1, &g_osd_tex);
        glBindTexture(GL_TEXTURE_2D, g_osd_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, w * 2, h);
        g_osd_tw = w; g_osd_th = h;
        upload = 1;
    }
    if (upload) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w * 2, h, GL_RG, GL_UNSIGNED_BYTE, fb);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g_osd_prog);
    glUniform2i(g_osd_size, w, h);
    glBindVertexArray(g_osd_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
}

/* Compile every shader + allocate every VAO now, at boot, instead of lazily on
 * the first frame that needs each. A lazy GLSL compile through ps5-opengl's
 * PSBC is tens of ms; paying it inside the frame loop stalled the render thread
 * for ~2 s at the first video / first OSD and the presentation clock late-drop-
 * dropped everything that piled up behind it. Boot has a teal frame + font
 * build going anyway. */
extern "C" void evo_gl_warm(void)
{
    if (!g_ready)
        return;
    blit_init();
    yuv_init();
    osd_init();
    glFinish();
    evo_bt_("GL warm: shaders + VAOs ready");
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
