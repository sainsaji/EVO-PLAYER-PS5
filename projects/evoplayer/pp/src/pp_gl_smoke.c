/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pp_gl_smoke — #77 / render-overhaul GL-1 go/no-go probe. See pp_gl_smoke.h.
 *
 * Only compiled with -DEVO_GL_SMOKE (scripts/package-app.sh --gl-smoke).
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

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
