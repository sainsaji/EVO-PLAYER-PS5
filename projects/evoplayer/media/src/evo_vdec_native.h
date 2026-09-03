/*
 * evo_vdec_native.h — private seam between the evo_vdec.h dispatcher
 * (evo_vdec_ffmpeg.c owns the public struct + entry points) and the
 * sceVideodec2 hardware-decode backend (evo_vdec_native.c).
 *
 * Native-decode plan Phase 4 (#31). The backend is a near-verbatim port of the
 * hardware-verified bring-up in projects/evoplayer/src/evo_videodec2_probe.c —
 * see docs/evo-pro/videodec2-abi.md.
 *
 * Everything here is a hard stub on host + payload builds: evo_vdec_native.c
 * compiles its real body only under EVO_APP_MODULE (sceVideodec2 + a real
 * user session + the GPU driver stack exist only in the PPSA99039 app module).
 * Off that path evo_vdec_native_probe() returns 0 and evo_vdec_native_open()
 * returns NULL, so the dispatcher always has FFmpeg to fall back to.
 */
#ifndef EVO_VDEC_NATIVE_H
#define EVO_VDEC_NATIVE_H

#include <stdint.h>

#include "evo_vdec.h"
#include "pp_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_vdec_native evo_vdec_native;

/*
 * Preload libSceVideodec2 (sysmodule 207). MUST be called from main() BEFORE
 * the first evo_jailbreak_self() — the self-unjail's mid-run credential swap
 * makes sceSysmoduleLoadModule(207) return ESDKVERSION and the module never
 * finishes loading (docs/evo-pro/status.md, 2026-09-03). Idempotent.
 *   1 => native decode may be available this session
 *   0 => host / payload build, or the module could not be prepared
 */
int evo_vdec_native_probe(void);

/*
 * Bring up a decoder. Returns NULL unless: this is the app module, the probe
 * passed, p->backend == EVO_VDEC_BACKEND_NATIVE, the codec is supported
 * (H.264 8-bit and HEVC Main today — HEVC Main10/P010 is deferred to the
 * converter change in the plan §3 and falls back), and every sceVideodec2
 * bring-up call returned 0. On any failure the dispatcher opens FFmpeg.
 */
evo_vdec_native *evo_vdec_native_open(const evo_vdec_open_params *p);

/*
 * Feed one compressed access unit (data==NULL/size==0 => end-of-stream drain).
 *   0  = consumed
 *  >0  = reorder buffer full, drain evo_vdec_native_receive() then re-send
 *  <0  = fatal (dispatcher cannot fall back mid-stream — playback aborts)
 */
int evo_vdec_native_send(evo_vdec_native *v, const uint8_t *data, int size,
                         int64_t pts_us);

/*
 * Pull one decoded frame in display (PTS) order.
 *   1  = frame written to *out (planes point into a backend-owned copy, valid
 *        until the next send/receive/flush — present synchronously)
 *   0  = need more input
 *  <0  = fatal
 * Never returns 2: the native path only ever yields pp_frame-mappable NV12.
 */
int evo_vdec_native_receive(evo_vdec_native *v, pp_frame *out);

void evo_vdec_native_flush(evo_vdec_native *v);   /* seek: drop buffered state */
void evo_vdec_native_close(evo_vdec_native *v);

#ifdef __cplusplus
}
#endif

#endif /* EVO_VDEC_NATIVE_H */
