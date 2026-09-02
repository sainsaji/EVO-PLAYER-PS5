/*
 * evo_jailbreak.h - self-service sandbox promotion for the app module.
 *
 * A registered app module (PPSA99039) launches into a fresh per-title sandbox
 * every time - /mnt/usb0, /data and the real root are ENOENT from inside, and
 * the module has no kernel access to lift that itself (see
 * third_party/SharpProspero/docs/app-promotion.md).
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
/* Returns 1 if the process is now promoted (daemon said ok, or a probe shows
 * the sandbox is already lifted), 0 otherwise. Safe to call more than once. */
int evo_jailbreak_self(void);
#else
#define evo_jailbreak_self() (1)
#endif

#ifdef __cplusplus
}
#endif

#endif
