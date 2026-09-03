/*
 * evo_jailbreak.h - self-service sandbox promotion for the app module.
 *
 * A registered app module (PPSA99039) launches into a fresh per-title sandbox
 * every time - /mnt/usb0 (USB source) and /data (INTERNAL STORAGE source) are
 * both ENOENT from inside, and the module has no kernel access to lift that
 * itself (see third_party/SharpProspero/docs/app-promotion.md). One promotion
 * opens the real root, so it covers both media sources.
 *
 * The fix that needs no per-launch command: ask the persistent jailbreak
 * daemon (PS5-Lapy-JB-Daemon, or etaHEN) already running on the console.
 * File-drop protocol: write {"PID":"<pid>"} to /download0/etahen_jailbreak;
 * the daemon polls the sandbox dirs, reads the pid, escalates the caller's
 * creds + points fd_rdir/fd_jdir at the real root, and unlinks the file.
 * namei re-reads those per lookup, so one drop at boot is enough and every
 * later "/mnt/usb0" open just resolves.
 *
 * Fallback when no daemon is running: the manual elfldr payload,
 * tools/sandbox-unjail.sh (projects/sandbox_unjail/), run once per EVO launch.
 *
 * Compiled only under EVO_APP_MODULE - an elfldr/hbldr payload is already
 * unjailed.
 */
#ifndef EVO_JAILBREAK_H
#define EVO_JAILBREAK_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EVO_APP_MODULE
/* Called once at main() entry - a short attempt (the daemon may not be
 * polling yet). Returns 1 if the sandbox is open. */
int evo_jailbreak_self(void);
/* Called on media-browser entry - two harder attempts. Idempotent; returns
 * early if already promoted. */
int evo_jailbreak_ensure(void);
/* 1 once the per-title sandbox has been lifted (probes /data). evo_data_path()
 * uses this to pick the durable data root at runtime - see issue #46. */
int evo_jailbreak_is_open(void);
#else
#define evo_jailbreak_self()    (1)
#define evo_jailbreak_ensure()  (1)
#define evo_jailbreak_is_open() (1)
#endif

#ifdef __cplusplus
}
#endif

#endif
