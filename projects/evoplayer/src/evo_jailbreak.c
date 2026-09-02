/* evo_jailbreak.c - see evo_jailbreak.h. app-module only. */
#ifdef EVO_APP_MODULE

#include "evo_jailbreak.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "evo_boot_trace.h"   /* evo_bt() - notification + klog */

/*
 * PS5-Lapy-JB-Daemon (and etaHEN) jailbreak-on-demand, file-drop form:
 * write "{"PID":"<pid>"}" to /download0/etahen_jailbreak. A resident daemon
 * polls /mnt/sandbox/<TID>_<NNN>/download0/etahen_jailbreak every 250 ms,
 * reads the pid, applies caps + authid + uid + sceAttr@0x83 + fd_rdir/fd_jdir
 * = rootvnode on that process, then unlink()s the file as the "done" signal.
 * namei re-reads fd_rdir/fd_jdir per lookup, so one bump at boot is enough
 * and every later "/mnt/usb0" open resolves.
 *
 * (Lapy's README: a first attempt can lose a timing race - relaunching the
 * app succeeds. We poll ~3 s for the unlink and re-probe the sandbox.)
 */
#define JB_FILE   "/download0/etahen_jailbreak"

/*
 * A jailed module has its root remapped to /mnt/sandbox/<TID>_NNN, so real
 * paths outside it (/data = EVO's INTERNAL STORAGE source, /mnt/usb0 = USB,
 * /mnt/sandbox itself) are ENOENT. After the daemon points fd_rdir/fd_jdir
 * at the real rootvnode they all resolve. /data is the reliable probe: it
 * always exists on the real root and is never in the sandbox view.
 */
static int sandbox_is_open(void)
{
    int fd = open("/data", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static int drop_request(void)
{
    int fd = open(JB_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    char body[48];
    int n = snprintf(body, sizeof body, "{\"PID\":\"%d\"}", (int)getpid());
    ssize_t w = write(fd, body, (size_t)n);
    close(fd);
    return w == n;
}

int evo_jailbreak_self(void)
{
    if (sandbox_is_open()) {
        evo_bt("jailbreak: sandbox already open");
        return 1;
    }

    if (!drop_request()) {
        evo_bt("jailbreak: cannot write %s", JB_FILE);
        return 0;
    }

    /* Wait for the daemon to consume the file (unlink) + the sandbox to open.
     * The daemon polls at 250 ms; ~1.2 s here is enough without a visible boot
     * stall, and the loop exits early on the common path. */
    for (int i = 0; i < 12; i++) {
        usleep(100 * 1000);
        int consumed = (access(JB_FILE, F_OK) != 0);
        if (consumed && sandbox_is_open()) {
            evo_bt("jailbreak: promoted (Lapy/etaHEN daemon)");
            return 1;
        }
    }

    /* Leave the request in place - the daemon may still pick it up before the
     * user reaches the browser. Otherwise USB browse falls back to
     * tools/sandbox-unjail.sh. */
    evo_bt("jailbreak: daemon slow/absent - USB may need sandbox-unjail.sh");
    return 0;
}

#endif /* EVO_APP_MODULE */
