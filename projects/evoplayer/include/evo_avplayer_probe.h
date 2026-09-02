/*
 * evo_avplayer_probe.h - boot-time libSceAvPlayer native-decode recon for the
 * app module (native-decode plan Phase 2, Route A).
 *
 * Built only when EVO_AVPLAYER_PROBE is defined
 * (scripts/package-app.sh --avplayer-probe). Answers the Route A gate from
 * inside the PPSA99039 process — the ONLY context with hardware decode; elfldr
 * and hbldr payloads both hit the errno-5200 wall (docs/evo-pro/avplayer-abi.md,
 * docs/hardware-decode.md). Loads libSceAvPlayer.sprx, resolves the sceAvPlayer*
 * NIDs, then runs Init -> AddSource -> EnableStream -> Start -> GetVideoDataEx
 * on a small test file and characterises the first frame.
 *
 * Reports via the system-notification channel (no stdout in the app sandbox).
 * Call once, early in main(), after evo_jailbreak_self() (needs /data or
 * /mnt/usb0 for the test file).
 */
#ifndef EVO_AVPLAYER_PROBE_H
#define EVO_AVPLAYER_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EVO_AVPLAYER_PROBE
void evo_avplayer_probe(void);
#else
#define evo_avplayer_probe() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
