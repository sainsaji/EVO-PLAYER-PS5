/*
 * evo_agc_probe.h - boot-time sceAgc reachability recon for the app module.
 *
 * Built only when EVO_AGC_PROBE is defined (scripts/package-app.sh --agc-probe).
 * Answers docs/evo-pro/gpu-rendering-plan.md Step 2's gate from inside the
 * PPSA99039 process: can it load libSceAgc.sprx and resolve the sceAgc* NIDs?
 * elfldr / hbldr payloads could not (agc_probe project). Reports via the
 * system-notification channel; call once, early in main().
 */
#ifndef EVO_AGC_PROBE_H
#define EVO_AGC_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EVO_AGC_PROBE
void evo_agc_probe(void);
#else
#define evo_agc_probe() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
