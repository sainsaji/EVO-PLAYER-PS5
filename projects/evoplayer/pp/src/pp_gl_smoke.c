/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pp_gl_smoke — #77 / render-overhaul GL-1 go/no-go probe. See pp_gl_smoke.h.
 *
 * Only compiled with -DEVO_GL_SMOKE (scripts/package-app.sh --gl-smoke).
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "evo_boot_log.h"
#include "evo_boot_trace.h"   /* evo_bt_ -> klog (live, pre-unjail) + evo.log */
#include "pp_gl_smoke.h"

/* fence shared with pp_gl_fatal.c */
extern jmp_buf      g_pp_gl_fence;
extern volatile int g_pp_gl_fence_armed;

#define SMOKE_W 1920
#define SMOKE_H 1080

/* Clear colour, 8-bit: a distinctive teal so a photo of the TV is unambiguous
 * (not black, not a primary). Framebuffer is BGRA in memory (0xAABBGGRR) but
 * glReadPixels(GL_RGBA) hands us R,G,B,A bytes regardless. */
#define CLR_R 0x18
#define CLR_G 0x9E
#define CLR_B 0x8C

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "void main(){ gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *k_fs =
    "#version 330 core\n"
    "layout(location=0) out vec4 c;\n"
    "void main(){ c = vec4(1.0, 0.35, 0.10, 1.0); }\n";   /* orange triangle */

static GLuint compile_one(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof log, &n, log);
        evo_boot_log("GL-1 SMOKE: shader compile failed: %.*s", (int)n, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* ===================================================================== *
 *  GL-4 (#80) texture-upload micro-benchmark
 *
 *  GL-3 B3 measured glTexSubImage2D of a 1080p RGBA8 frame at ~68 ms on
 *  ps5-opengl G47 (synchronous CPU staging copy per upload) — the ~14 fps
 *  video-stutter wall. This runs after the smoke's render/readback and times
 *  the client-upload path for the formats a GL video path would use, so GL-4
 *  has a hard baseline to measure a zero-copy import against and a G55
 *  regression datapoint. Bench only — the smoke result stays the triangle
 *  readback. All GL calls stay inside the smoke's g_pp_gl_fence guard.
 * ===================================================================== */
static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double d = *(const double *)a - *(const double *)b;
    return (d > 0) - (d < 0);
}

#define BENCH_ITERS 32

/* One case: glTexStorage2D once, then time BENCH_ITERS glTexSubImage2D+glFinish.
 * pbo != 0 routes the upload through a mapped GL_PIXEL_UNPACK_BUFFER
 * (GL_MAP_UNSYNCHRONIZED_BIT) — the variant GL-3 tried with no effect on G47. */
static void bench_upload_case(const char *name, GLenum internalfmt, GLenum fmt,
                              GLenum type, int w, int h, int bpp, int pbo)
{
    const size_t bytes = (size_t)w * (size_t)h * (size_t)bpp;
    uint8_t *src = (uint8_t *)malloc(bytes);
    if (!src) {
        evo_bt_("GL bench: %-16s alloc %zuKiB FAILED", name, bytes / 1024);
        return;
    }
    memset(src, 0x7F, bytes);

    GLuint tex = 0, buf = 0;
    double s[BENCH_ITERS];
    double sum = 0.0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalfmt, w, h);

    if (pbo) {
        glGenBuffers(1, &buf);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
    }

    if (glGetError() != GL_NO_ERROR) {
        evo_bt_("GL bench: %-16s setup GL error — skipped", name);
        goto out;
    }

    for (int i = -3; i < BENCH_ITERS; ++i) {         /* -3..-1 = warm-up */
        src[(size_t)(i & 0x3FF)] ^= 0xFFu;           /* defeat any content cache */
        double t0 = mono_ms();
        if (pbo) {
            void *p = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)bytes,
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                                      GL_MAP_UNSYNCHRONIZED_BIT);
            if (p) { memcpy(p, src, bytes); glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER); }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, (const void *)0);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, src);
        }
        glFinish();
        double dt = mono_ms() - t0;
        if (i >= 0)
            s[i] = dt;
    }

    qsort(s, BENCH_ITERS, sizeof s[0], cmp_double);
    for (int i = 0; i < BENCH_ITERS; ++i)
        sum += s[i];
    evo_bt_("GL bench: %-16s %4dx%-4d %-5s%s mean=%.2fms p50=%.2fms p95=%.2fms "
            "min=%.2fms (%zuKiB, n=%d)",
            name, w, h,
            internalfmt == GL_RGBA8 ? "RGBA8" :
            internalfmt == GL_R8    ? "R8"    :
            internalfmt == GL_RG8   ? "RG8"   : "?",
            pbo ? " PBO" : "    ",
            sum / BENCH_ITERS, s[BENCH_ITERS / 2], s[(BENCH_ITERS * 95) / 100],
            s[0], bytes / 1024, BENCH_ITERS);

out:
    if (pbo) { glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); glDeleteBuffers(1, &buf); }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
    free(src);
}

static void smoke_bench_uploads(void)
{
    evo_boot_log("GL-4 bench: glTexSubImage2D client-upload cost (GL-3 B3 wall = ~68ms/1080p-RGBA8 on G47)");
    evo_boot_log_flush();
    /*                     name              internalfmt  fmt      type               w     h     bpp pbo */
    bench_upload_case("1080p RGBA8",     GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1920, 1080, 4, 0);
    bench_upload_case("1080p RGBA8 PBO", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1920, 1080, 4, 1);
    bench_upload_case("1080p luma R8",   GL_R8,    GL_RED,  GL_UNSIGNED_BYTE, 1920, 1080, 1, 0);
    bench_upload_case("1080p chroma RG8",GL_RG8,   GL_RG,   GL_UNSIGNED_BYTE,  960,  540, 2, 0);
    bench_upload_case("4K RGBA8",        GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 3840, 2160, 4, 0);
    bench_upload_case("4K luma R8",      GL_R8,    GL_RED,  GL_UNSIGNED_BYTE, 3840, 2160, 1, 0);
    bench_upload_case("4K chroma RG8",   GL_RG8,   GL_RG,   GL_UNSIGNED_BYTE, 1920, 1080, 2, 0);
    evo_boot_log_flush();
}

int pp_gl_smoke_run(void)
{
    volatile int stage = 0;        /* last stage attempted; survives the longjmp */
    int result = 1;
    const char *v_vendor = "?", *v_render = "?", *v_ver = "?", *v_glsl = "?";
    unsigned px_r = 0, px_g = 0, px_b = 0;

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface srf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig  cfg = NULL;

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE,
    };

    evo_boot_log("GL-1 SMOKE: begin (ps5-opengl, %dx%d, GL 3.3 core)", SMOKE_W, SMOKE_H);
    evo_boot_log_flush();

    /* Catch a ps5-opengl fail-stop (patched _Exit -> ps5gl_fatal -> longjmp). */
    if (setjmp(g_pp_gl_fence)) {
        evo_boot_log("GL-1 SMOKE: result=FAIL stage=%d reason=driver-fail-stop", stage);
        evo_boot_log_flush();
        return stage ? stage : 99;
    }
    g_pp_gl_fence_armed = 1;

    /* 1: EGL display + init */
    stage = 1;
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint egl_major = 0, egl_minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &egl_major, &egl_minor)) {
        evo_boot_log("GL-1 SMOKE: eglInitialize failed (0x%x)", eglGetError());
        goto done;
    }
    evo_boot_log("GL-1 SMOKE: EGL %d.%d up", egl_major, egl_minor);

    /* 2: bind API + choose config */
    stage = 2;
    EGLint cfg_count = 0;
    if (!eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &cfg_count) || cfg_count != 1) {
        evo_boot_log("GL-1 SMOKE: eglChooseConfig failed (0x%x, n=%d)", eglGetError(), cfg_count);
        goto done;
    }

    /* 3: window surface + context + make current */
    stage = 3;
    srf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (srf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, srf, srf, ctx)) {
        evo_boot_log("GL-1 SMOKE: surface/context/makeCurrent failed (0x%x)", eglGetError());
        goto done;
    }
    v_vendor = (const char *)glGetString(GL_VENDOR);
    v_render = (const char *)glGetString(GL_RENDERER);
    v_ver    = (const char *)glGetString(GL_VERSION);
    v_glsl   = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (!v_vendor) v_vendor = "?";
    if (!v_render) v_render = "?";
    if (!v_ver)    v_ver    = "?";
    if (!v_glsl)   v_glsl   = "?";
    evo_boot_log("GL-1 SMOKE: context current — %s / %s / GL %s / GLSL %s",
                 v_vendor, v_render, v_ver, v_glsl);
    evo_boot_log_flush();

    /* 4: trivial GLSL 330 program (exercises the runtime PSBC compile) */
    stage = 4;
    GLuint vs = compile_one(GL_VERTEX_SHADER, k_vs);
    GLuint fs = compile_one(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) goto done;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512]; GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof log, &n, log);
        evo_boot_log("GL-1 SMOKE: program link failed: %.*s", (int)n, log);
        goto done;
    }

    /* 5: clear to the known colour + draw one fullscreen-ish triangle */
    stage = 5;
    static const GLfloat tri[] = { -0.6f, -0.6f,  0.6f, -0.6f,  0.0f, 0.7f };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glUseProgram(prog);
    glViewport(0, 0, SMOKE_W, SMOKE_H);
    glClearColor(CLR_R / 255.0f, CLR_G / 255.0f, CLR_B / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        evo_boot_log("GL-1 SMOKE: GL error after draw");
        goto done;
    }

    /* 6: read back a clear-colour pixel (top-left corner, away from the tri) */
    stage = 6;
    uint8_t rgba[4] = { 0, 0, 0, 0 };
    glReadPixels(4, SMOKE_H - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    px_r = rgba[0]; px_g = rgba[1]; px_b = rgba[2];
    int near = (abs((int)px_r - CLR_R) <= 8 &&
                abs((int)px_g - CLR_G) <= 8 &&
                abs((int)px_b - CLR_B) <= 8);

    /* 7: present a few frames so the TV shows it for the photo */
    stage = 7;
    for (int i = 0; i < 3; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        eglSwapBuffers(dpy, srf);
    }

    /* 7b: GL-4 (#80) — texture-upload cost baseline. Bench only; does not
     * affect `result`. Still under the fence (armed until `done:`). */
    stage = 71;
    smoke_bench_uploads();
    stage = 7;

    result = near ? 0 : 8;   /* rendered but wrong pixel => stage 8 */

done:
    g_pp_gl_fence_armed = 0;

    /* evo_bt_ -> klog immediately (tools/klog.sh, no unjail / USB needed) AND
     * into the evo.log buffer for the post-unjail flush in main(). */
    evo_bt_("GL-1 SMOKE: result=%s stage=%d dead=%d "
            "px=%02X%02X%02X want=%02X%02X%02X "
            "vendor=\"%s\" renderer=\"%s\" gl=\"%s\" glsl=\"%s\"",
            result == 0 ? "PASS" : "FAIL", stage, g_pp_gl_dead,
            px_r, px_g, px_b, CLR_R, CLR_G, CLR_B,
            v_vendor, v_render, v_ver, v_glsl);
    evo_boot_log_flush();

    if (ctx != EGL_NO_CONTEXT || srf != EGL_NO_SURFACE) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        if (srf != EGL_NO_SURFACE) eglDestroySurface(dpy, srf);
    }
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);

    return result;
}
