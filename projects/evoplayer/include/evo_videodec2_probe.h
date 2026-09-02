/*
 * evo_videodec2_probe.h - boot-time sceVideodec2 (Route B) hardware-decode gate
 * for the app module. Native-decode plan Phase 2, Route B.
 *
 * Built only when EVO_VIDEODEC2_PROBE is defined
 * (scripts/package-app.sh --videodec2-probe). Reproduces ProsperoLight's
 * hardware-verified VDEC self-test (2026-09-01: every call returned 0, a valid
 * 1920x1088 NV12 H.264 frame came back) from inside PPSA99039:
 *   sceSysmoduleLoadModule(207) -> compute queue -> decoder memory ->
 *   CreateDecoder -> Reset -> Decode(one bundled IDR AU) -> Flush.
 * Reports via the system-notification channel. Call once, early in main().
 *
 * Route A (sceAvPlayer) is dead from a fake-signed module -
 * sceSysmoduleLoadModule(0xA5) returns 0x80020063. Route B is the way, and it
 * is already proven - this probe just confirms it in EVO's own package.
 */
#ifndef EVO_VIDEODEC2_PROBE_H
#define EVO_VIDEODEC2_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EVO_VIDEODEC2_PROBE
void evo_videodec2_probe(void);
#else
#define evo_videodec2_probe() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
