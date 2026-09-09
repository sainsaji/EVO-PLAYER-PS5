#pragma once

/*
 * The OpenGL context seam for the render overhaul.
 *
 * GL-2 (#78): the host preview harness (tools/uiview_playback_rml) creates a
 * headless EGL surfaceless context backed by Mesa llvmpipe, so RmlUi's
 * RenderInterface_GL3 can be exercised with no console and no display.
 * Implementation: evo_gl_context_host.cpp (compiled only by the preview
 * harness, which defines EVO_RML_GL_HOST).
 *
 * GL-3 (#79): the device gets a second implementation
 * (evo_gl_context_device.cpp) that opens an EGL window surface through
 * ps5-opengl in the pre-unjail boot slot. Same three entry points.
 *
 * evo_gl_context_create() must run before any RmlUi rendering: it makes a GL
 * 3.3 core context current and loads the GL function pointers
 * (RmlGL3::Initialize). It is not lazily initialisable.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create + make current a GL 3.3 core context and load GL entry points.
 * width/height seed the default framebuffer / viewport where that applies
 * (surfaceless host: advisory only). Returns 1 on success, 0 on failure
 * (message logged to stderr). */
int  evo_gl_context_create(int width, int height);

/* Tear the context down. Safe to call when none was created. */
void evo_gl_context_destroy(void);

/* 1 once a context is current and GL is loaded. */
int  evo_gl_context_ok(void);

/* Present the default framebuffer to the display (device: eglSwapBuffers;
 * host surfaceless: no-op - the harness reads the pbuffer back instead). */
void evo_gl_context_present(void);

/* Advisory backbuffer size for the current context (0,0 if none). */
void evo_gl_context_size(int *w, int *h);

/* GL-3 (#79) B2, device only. Called once per frame BEFORE the screen dispatch:
 * bind fb 0, set the viewport, clear it to the menu backdrop. Each RmlUi screen
 * / overlay pass then composites over it; evo_gl_context_present() swaps.
 * No-op on the host. */
void evo_gl_frame_begin(void);

/* GL-3 (#79) B2, device only. glReadPixels the composited default framebuffer
 * into a caller BGRA buffer (0xAABBGGRR, y-flipped to top-down) - the menu
 * screenshot path. No-op on the host. */
void evo_gl_read_default_fb(uint32_t *bgra, int w, int h);

/* GL-3 (#79) B2, device only. Upload a top-down EVO framebuffer (0xAABBGGRR)
 * and draw it as one fullscreen quad into fb 0 - the CPU-rasterise + GL-blit
 * present path (the fast path on ps5-opengl: one textured quad, no
 * render-to-texture). Follow with evo_gl_context_present(). No-op on host. */
void evo_gl_blit_bgra(const uint32_t *fb, int w, int h);

/* GL-4 (#80), device only. Draw a decoded YUV 4:2:0 frame as one fullscreen
 * quad into fb 0, YUV->RGB (BT.601 limited) in the fragment shader. Zero-copy:
 * the planes point straight into decoder / AVFrame memory, uploaded as R8 (+
 * RG8) textures that dodge the RGBA8 staging-copy wall. NV12 is
 * (y,uv,NULL,NULL); planar I420 is (y,NULL,u,v). coded_w/h is the padded luma
 * (texture) size, disp_w/h the region shown. Follow with
 * evo_gl_context_present(). No-op on host. */
void evo_gl_blit_yuv(const uint8_t *y,  int y_pitch,
                     const uint8_t *uv, int uv_pitch,
                     const uint8_t *u,  int u_pitch,
                     const uint8_t *v,  int v_pitch,
                     int coded_w, int coded_h, int disp_w, int disp_h,
                     int view_mode);   /* 0=FIT 1=FILL 2=STRETCH */

/* GL-4 (#80), device only. Composite an EVO BGRA scratch (0xAABBGGRR, alpha in
 * the top byte) over whatever is already in fb 0 — the player OSD on top of the
 * video quad. `upload` re-sends the pixels (a slow RGBA8 staging copy — do it
 * only when the scratch changed); otherwise the last upload is reused. Alpha
 * blended. No-op on host. */
void evo_gl_composite_bgra(const uint32_t *fb, int w, int h, int upload);

#ifdef __cplusplus
}
#endif
