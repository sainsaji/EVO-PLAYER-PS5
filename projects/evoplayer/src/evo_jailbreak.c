/* evo_jailbreak.c - see evo_jailbreak.h. app-module only. */
#ifdef EVO_APP_MODULE

#include "evo_jailbreak.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "evo_boot_trace.h"   /* evo_bt() - notification + klog */

/*
 * PS5-Lapy-JB-Daemon (and etaHEN) jailbreak-on-demand, file-drop form:
 * write "{"PID":"<pid>"}" to /download0/etahen_jailbreak. A resident daemon
 * polls /mnt/sandbox/<TID>_<NNN>/download0/etahen_jailbreak every 250 ms,
 * reads the pid, applies caps + authid + uid + sceAttr@0x83 + fd_rdir/fd_jdir
 * = rootvnode on that process, then unlink()s the file as the "done" signal.
 * namei re-reads fd_rdir/fd_jdir per lookup, so one bump is enough and every
 * later "/mnt/usb0" or "/data" open resolves.
 */
#define JB_FILE   "/download0/etahen_jailbreak"

/* A jailed module's root is remapped, so /data (INTERNAL source) and
 * /mnt/usb0 (USB source) are ENOENT. /data is the reliable probe.
 * Public (evo_jailbreak.h) so evo_data_path() can pick its root at runtime. */
int evo_jailbreak_is_open(void)
{
    int fd = open("/data", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static int drop_request(void)
{
    int fd = open(JB_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        evo_bt("jailbreak: open(%s) failed errno=%d", JB_FILE, errno);
        return 0;
    }
    char body[48];
    int n = snprintf(body, sizeof body, "{\"PID\":\"%d\"}", (int)getpid());
    ssize_t w = write(fd, body, (size_t)n);
    close(fd);
    if (w != n) {
        evo_bt("jailbreak: write(%s) short w=%zd errno=%d", JB_FILE, w, errno);
        return 0;
    }
    return 1;
}

/* One promote attempt: drop the file, wait for the daemon to unlink it and
 * the sandbox to open. Returns 1 if opened. */
static int attempt(int tenths)
{
    if (!drop_request())
        return 0;

    int consumed = 0, opened = 0;
    for (int i = 0; i < tenths; i++) {
        usleep(100 * 1000);
        if (!consumed && access(JB_FILE, F_OK) != 0) consumed = 1;
        if (evo_jailbreak_is_open()) { opened = 1; break; }
    }
    evo_bt("jailbreak: pid=%d file=%s daemon_saw=%s sandbox=%s (/data errno=%d)",
           (int)getpid(), JB_FILE, consumed ? "yes" : "NO",
           opened ? "OPEN" : "closed", opened ? 0 : errno);
    return opened;
}

int evo_jailbreak_self(void)
{
    if (evo_jailbreak_is_open()) {
        evo_bt("jailbreak: sandbox already open");
        return 1;
    }
    /* At boot the daemon may not be polling yet; a shorter first try, the
     * real one happens on browser entry (evo_jailbreak_ensure). */
    return attempt(12);
}

int evo_jailbreak_ensure(void)
{
    if (evo_jailbreak_is_open())
        return 1;
    /* Two harder tries - Lapy's own note is that a first attempt can lose a
     * timing race. */
    if (attempt(25)) return 1;
    return attempt(25);
}

static uint64_t jb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Render-loop retry.
 *
 * evo_jailbreak_self() waits 1.2 s at boot, which is a coin flip on a console
 * that has just come up: after the 2026-09-11 panic reboot etaHEN was still
 * patching shellui when EVO launched 4 s later, so the daemon consumed the
 * request (daemon_saw=yes) but /data had not resolved before the wait expired.
 *
 * Losing that race used to be terminal, not cosmetic. The only other attempt
 * lives in load_usb_files() - i.e. behind the media browser - and a closed
 * sandbox is precisely what makes the browser unreachable: settings never load,
 * so the debug overlay's 2 Hz tick is off, so RmlUi is never dirty again, so
 * nothing ever swaps and the screen stays black. The app looks dead while the
 * daemon on the other side is alive and willing.
 *
 * So keep asking. Probing is one open("/data") every 250 ms and stops entirely
 * once promoted; a fresh request is dropped at most once every JB_RETRY_MS.
 * Returns 1 on the single call that observes the sandbox opening, so the caller
 * can rebind persistence (#46) and force the repaint the UI will not ask for.
 */
#define JB_PROBE_MS   250u
#define JB_RETRY_MS  3000u

int evo_jailbreak_poll(void)
{
    static int      settled = 0;
    static int      seen_closed = 0;
    static uint64_t next_probe_ms = 0;
    static uint64_t last_drop_ms = 0;

    if (settled)
        return 0;

    uint64_t now = jb_now_ms();
    if (now < next_probe_ms)
        return 0;
    next_probe_ms = now + JB_PROBE_MS;

    if (evo_jailbreak_is_open()) {
        settled = 1;
        /* Only a real closed->open TRANSITION is worth reporting. When the boot
         * attempt already succeeded, the first poll would otherwise announce a
         * "retry" that never happened and make the caller redo persistence and
         * pop a toast on every single launch. */
        if (!seen_closed)
            return 0;
        evo_bt("jailbreak: sandbox opened on a render-loop retry (pid=%d)",
               (int)getpid());
        return 1;
    }
    seen_closed = 1;
    if (!last_drop_ms || (now - last_drop_ms) >= JB_RETRY_MS) {
        last_drop_ms = now;
        drop_request();
    }
    return 0;
}

#endif /* EVO_APP_MODULE */
