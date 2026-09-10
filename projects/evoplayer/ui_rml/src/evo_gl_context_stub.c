/*
 * evo_gl_context_stub.c — no-op OpenGL seam for builds without a GL backend.
 *
 * Since GL-4 (#80) Stage 3 the GL funnel is main.c's only present path: the CPU
 * YUV->BGRA converters, the tiled VideoOut blit and the V8/V3/1080 backend
 * dispatch are gone, so the frame loop calls evo_gl_* unconditionally.
 *
 * The app module (`package-app.sh --ffpfsc`, GL_DEVICE=1) links the real
 * implementation in evo_gl_context_device.cpp. The ELF compile check
 * (scripts/build-evoplayer.sh) links this instead: that build exists only to
 * keep main.c / pp / media compiling for the non-app-module path, it has no
 * graphics of its own and never reaches a console, so every entry point here
 * does nothing and evo_gl_context_create() reports failure.
 */
#if !defined(EVO_GL_DEVICE) && !defined(EVO_RML_GL_HOST)

#include "evo_gl_context.h"

int evo_gl_context_create(int width, int height)
{
    (void)width; (void)height;
    return 0;
}

void evo_gl_warm(void) {}

void evo_gl_context_destroy(void) {}

int evo_gl_context_ok(void) { return 0; }

void evo_gl_context_present(void) {}

void evo_gl_frame_begin(void) {}

void evo_gl_context_size(int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
}

void evo_gl_read_default_fb(uint32_t *bgra, int w, int h)
{
    (void)bgra; (void)w; (void)h;
}

void evo_gl_blit_bgra(const uint32_t *fb, int w, int h)
{
    (void)fb; (void)w; (void)h;
}

void evo_gl_blit_yuv(const uint8_t *y,  int y_pitch,
                     const uint8_t *uv, int uv_pitch,
                     const uint8_t *u,  int u_pitch,
                     const uint8_t *v,  int v_pitch,
                     int coded_w, int coded_h, int disp_w, int disp_h,
                     int view_mode)
{
    (void)y; (void)y_pitch; (void)uv; (void)uv_pitch;
    (void)u; (void)u_pitch; (void)v; (void)v_pitch;
    (void)coded_w; (void)coded_h; (void)disp_w; (void)disp_h; (void)view_mode;
}

void evo_gl_composite_bgra(const uint32_t *fb, int w, int h, int upload)
{
    (void)fb; (void)w; (void)h; (void)upload;
}

#endif /* !EVO_GL_DEVICE && !EVO_RML_GL_HOST */
