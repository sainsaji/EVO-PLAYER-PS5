/* sandbox_unjail - elfldr payload that lifts the app sandbox on the running
 * EVO Player (PPSA99039) process.
 *
 * Phase 1b milestone-1 task 7 - docs/evo-pro/phase-1b-app-module.md section 5,
 * Step B.2.
 *
 * A registered title launched from the home screen (ShadowMountPlus) runs
 * under the normal sceSblACMgr sandbox: from inside, /data and the /mnt
 * mounts are ENOENT (Step A, 2026-09-02). opendir/getdents on /download0
 * work, but the
 * media browser needs the real /mnt/usb0.
 *
 * This payload runs in the elfldr context, which has kernel R/W through
 * <ps5/kernel.h>. It applies the exact privilege lift that ps5-payload-elfldr
 * gives its own payloads (elfldr_raise_privileges):
 *
 *     fd_rdir  -> real root vnode     kernel_set_proc_rootdir()
 *     fd_jdir  -> 0                   kernel_set_proc_jaildir()
 *     cr_uid   -> 0                   kernel_set_ucred_uid()
 *     cr_sceCaps -> all 0xff          kernel_set_ucred_caps()
 *
 * Path resolution (namei) reads fd_rdir / fd_jdir at lookup time, so the
 * running EVO process sees the real filesystem on its next open() - no
 * remount, no path changes, the literal "/mnt/usb0" strings just resolve.
 * The same primitive is what HEN applies to the hbldr PS-Now slot.
 *
 *   sandbox_unjail                 unjail the first jailed "eboot.bin"
 *   sandbox_unjail <pid>           unjail that pid specifically
 *
 * Launch-safety (docs/tooling.md): only one app instance may be resident.
 * Close any running game with the PS button before using this - it matches by
 * process name and a game also runs as "eboot.bin".
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "evo_ps5.h"

/* {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0} - namelen 4, exactly what the
 * repo's own hbldr.c find_pid() uses on this console. KERN_PROC_ALL (0) with
 * namelen 3 returns EINVAL here. */
#define KI_MIB_0  1     /* CTL_KERN       */
#define KI_MIB_1  14    /* KERN_PROC      */
#define KI_MIB_2  8     /* KERN_PROC_PROC */

/* kinfo_proc layout on the 12.xx FreeBSD-derived kernel, same offsets
 * hbldr.c walks: ki_structsize (int) at 0, ki_pid at 72, ki_tdname at 447.
 * Iterate by ki_structsize, never hardcode the whole struct. */
#define KI_OFF_STRUCTSIZE  0
#define KI_OFF_PID         72
#define KI_OFF_COMM        447

int sysctl(const int *name, unsigned namelen, void *old, size_t *oldlen,
           const void *newp, size_t newlen);

/* Snapshot of the process table. */
static char  *g_snap;
static size_t g_snap_len;

static int
proc_snapshot(void)
{
    int mib[4] = { KI_MIB_0, KI_MIB_1, KI_MIB_2, 0 };
    size_t len = 0;

    errno = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) {
        evo_notify("sandbox_unjail: sysctl sizing failed errno=%d len=%zu",
                   errno, len);
        return -1;
    }

    /* over-allocate: the table can grow between the two calls */
    len += len / 4 + 0x4000;
    g_snap = malloc(len);
    if (!g_snap)
        return -1;

    errno = 0;
    if (sysctl(mib, 4, g_snap, &len, NULL, 0) != 0) {
        evo_notify("sandbox_unjail: sysctl fetch failed errno=%d", errno);
        free(g_snap);
        g_snap = NULL;
        return -1;
    }
    g_snap_len = len;
    return 0;
}

/* Collect every pid whose ki_comm matches `want`. Returns the count, writes up
 * to `max` pids into `out`. */
static int
find_pids_by_name(const char *want, pid_t *out, int max)
{
    int n = 0;
    pid_t mypid = getpid();

    for (size_t off = 0; off + 4 <= g_snap_len && n < max; ) {
        int ssize = *(int *)(g_snap + off + KI_OFF_STRUCTSIZE);
        if (ssize <= 0 || off + (size_t)ssize > g_snap_len)
            break;

        int pid = *(int *)(g_snap + off + KI_OFF_PID);
        const char *comm = g_snap + off + KI_OFF_COMM;

        if (pid > 0 && pid != mypid && strncmp(comm, want, 19) == 0)
            out[n++] = pid;

        off += (size_t)ssize;
    }

    return n;
}

/* Emit the whole process table (pid:comm) in ~10-per-notification batches, so
 * we can see what EVO's app-module process is actually called. */
static void
dump_proc_table(void)
{
    char line[1024];
    int col = 0;
    line[0] = 0;

    for (size_t off = 0; off + 4 <= g_snap_len; ) {
        int ssize = *(int *)(g_snap + off + KI_OFF_STRUCTSIZE);
        if (ssize <= 0 || off + (size_t)ssize > g_snap_len)
            break;

        int pid = *(int *)(g_snap + off + KI_OFF_PID);
        const char *comm = g_snap + off + KI_OFF_COMM;
        char frag[80];
        snprintf(frag, sizeof frag, "%d:%.19s  ", pid, comm);
        strncat(line, frag, sizeof line - strlen(line) - 1);

        if (++col == 10) {
            evo_notify("procs: %s", line);
            line[0] = 0;
            col = 0;
        }
        off += (size_t)ssize;
    }
    if (col)
        evo_notify("procs: %s", line);
}

/* Returns 0 on success. Mirrors elfldr_raise_privileges(). */
static int
unjail_pid(pid_t pid)
{
    static const uint8_t caps_all[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    intptr_t root_vnode;

    if (!(root_vnode = kernel_get_root_vnode())) {
        evo_notify("sandbox_unjail: kernel_get_root_vnode failed");
        return -1;
    }
    if (kernel_set_proc_rootdir(pid, root_vnode)) {
        evo_notify("sandbox_unjail: set_proc_rootdir(pid=%d) failed", pid);
        return -1;
    }
    if (kernel_set_proc_jaildir(pid, 0)) {
        evo_notify("sandbox_unjail: set_proc_jaildir(pid=%d) failed", pid);
        return -1;
    }
    if (kernel_set_ucred_uid(pid, 0)) {
        evo_notify("sandbox_unjail: set_ucred_uid(pid=%d) failed", pid);
        return -1;
    }
    if (kernel_set_ucred_caps(pid, caps_all)) {
        evo_notify("sandbox_unjail: set_ucred_caps(pid=%d) failed", pid);
        return -1;
    }

    return 0;
}

/* Candidate ki_comm values for a fake-signed app module. Tried in order. */
static const char *const k_candidates[] = {
    "eboot.bin", "EVOPlayer", "PS5MediaPlayerPRO", "EVO Player", "SceApp",
};

int
main(int argc, char **argv)
{
    evo_notify("sandbox_unjail (PPSA99039) - fw=0x%08x",
               (unsigned)kernel_get_fw_version());

    if (proc_snapshot() != 0) {
        evo_notify("sandbox_unjail: sysctl(KERN_PROC_ALL) failed");
        return 0;
    }

    /* Explicit pid override. */
    if (argc > 1) {
        pid_t pid = (pid_t)atoi(argv[1]);
        intptr_t jdir = kernel_get_proc_jaildir(pid);
        if (unjail_pid(pid) == 0)
            evo_notify("sandbox_unjail: pid=%d unjailed (was jdir=0x%lx) - "
                       "reopen the media browser", pid, (long)jdir);
        return 0;
    }

    pid_t pids[8];
    int n = 0;
    const char *matched = NULL;
    for (size_t c = 0; c < sizeof k_candidates / sizeof k_candidates[0]; c++) {
        n = find_pids_by_name(k_candidates[c], pids, 8);
        if (n > 0) { matched = k_candidates[c]; break; }
    }

    if (n == 0) {
        evo_notify("sandbox_unjail: no app process matched - dumping proc "
                   "table so we can see EVO's real name (is it running?)");
        dump_proc_table();
        return 0;
    }

    int done = 0;
    for (int i = 0; i < n; i++) {
        intptr_t jdir = kernel_get_proc_jaildir(pids[i]);
        if (jdir == 0) {
            /* Already unsandboxed - a prior run, or not a jailed app. */
            continue;
        }
        if (unjail_pid(pids[i]) == 0) {
            evo_notify("sandbox_unjail: '%s' pid=%d unjailed (was jdir=0x%lx) "
                       "- reopen the media browser to see /mnt/usb0",
                       matched, pids[i], (long)jdir);
            done++;
        }
    }

    if (!done)
        evo_notify("sandbox_unjail: '%s' x%d, none jailed (jdir=0 already) - "
                   "dumping proc table", matched, n);
    if (!done)
        dump_proc_table();

    return 0;
}
