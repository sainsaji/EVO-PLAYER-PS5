/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pp_gl_smoke — the #77 (render-overhaul GL-1) go/no-go probe.
 *
 * Compiled into the .ffpfsc app module ONLY with -DEVO_GL_SMOKE
 * (scripts/package-app.sh --gl-smoke, which also links ps5-opengl-core33).
 * main() runs it, pre-unjail, when /mnt/usb0/evo_gl_smoke exists — INSTEAD of
 * pp_agc_init and the normal boot, because ps5-opengl owns sceAgc +
 * sceVideoOut and a second sceVideoOut open panics the console
 * (see the "PS5 kernel panic vectors" note / docs/hardware-decode.md).
 *
 * It brings up EGL + a GL 3.3 core context through ps5-opengl, clears to a
 * known colour, compiles a trivial GLSL 330 program (exercises the runtime
 * PSBC path), draws one triangle, reads back a pixel, and writes a single
 * receipt line to /mnt/usb0/evo.log:
 *
 *   GL-1 SMOKE: result=PASS|FAIL stage=<n> ... vendor="..." renderer="..."
 *
 * The full spike write-up and the hardware-receipt slot live in
 * docs/evo-pro/gl1-spike.md.
 */
#ifndef PP_GL_SMOKE_H
#define PP_GL_SMOKE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = PASS (rendered + read back the expected pixel), non-zero = failed at
 * stage N. Never calls _exit(); a driver fail-stop is caught by an internal
 * setjmp fence (see pp_gl_fatal.c) and reported as a FAIL. */
int pp_gl_smoke_run(void);

/* Set to 1 by ps5gl_fatal() (pp_gl_fatal.c) after a ps5-opengl fail-stop path
 * was hit and unwound. Readable after pp_gl_smoke_run() returns. */
extern volatile int g_pp_gl_dead;

#ifdef __cplusplus
}
#endif

#endif /* PP_GL_SMOKE_H */
