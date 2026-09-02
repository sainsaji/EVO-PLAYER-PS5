/* app_ctl - elfldr/hbldr payload that drives the PPSA99039 app module for the
 * unattended bring-up loop (tools/app-loop.sh). No TV, no ShadowMountPlus UI.
 *
 *   app_ctl list       dump every pid + thread name (crude task manager)
 *   app_ctl launch     sceSystemServiceLaunchApp("PPSA99039")
 *   app_ctl kill        find the eboot.bin process, SIGKILL it (kernel R/W)
 *   app_ctl relaunch    kill (if running) then launch   [default]
 *
 * The title must already be registered once via ShadowMountPlus; after that
 * this relaunches it in place after each folder redeploy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "evo_ps5.h"

#ifndef EVO_APP_TITLE_ID
#define EVO_APP_TITLE_ID "PPSA99039"
#endif

/* libSceSystemService - no SDK header, verified exports. */
int sceSystemServiceLaunchApp(const char *titleId, const char *const *argv, void *param);
int sceSystemServiceKillApp(int appId, int reason, int detail, void *param);
int sceUserServiceInitialize(const void *params);

/* libkernel - process enumeration for the kill path. */
int sysctl(const int *name, unsigned namelen, void *old, size_t *oldlen,
           const void *newp, size_t newlen);
int kill(int pid, int sig);

#define SIGKILL 9

/* KERN_PROC_PROC, namelen 4 - the form the repo's own hbldr.c uses. The
 * {CTL_KERN,KERN_PROC,KERN_PROC_ALL} namelen-3 form returns EINVAL on this
 * console (confirmed on hardware 2026-09-02 via projects/sandbox_unjail).
 * kinfo_proc: ki_structsize @0, ki_pid @72, ki_tdname @447. */
static int
find_pid_by_name(const char *want)
{
    int mib[4] = { 1, 14, 8, 0 };
    int mypid = getpid();
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0)
        return -1;

    char *buf = malloc(len);
    if (!buf)
        return -1;
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        free(buf);
        return -1;
    }

    int found = -1;
    for (size_t off = 0; off + 4 <= len; ) {
        int ssize = *(int *)(buf + off);
        if (ssize <= 0 || off + (size_t)ssize > len)
            break;
        int pid = *(int *)(buf + off + 72);
        const char *tdname = buf + off + 447;
        if (pid > 0 && pid != mypid && strcmp(tdname, want) == 0) {
            found = pid;
            break;
        }
        off += (size_t)ssize;
    }
    free(buf);
    return found;
}

/* Dump every process: pid + thread name. A crude "task manager" for the
 * console - notifications are size-capped so this also prints to stdout
 * (visible when launched via /hbldr with pipe=1). */
static void
do_list(void)
{
    int mib[4] = { 1, 14, 8, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) {
        evo_notify("app_ctl: sysctl failed");
        return;
    }
    char *buf = malloc(len);
    if (!buf || sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        free(buf);
        evo_notify("app_ctl: sysctl(read) failed");
        return;
    }
    int n = 0;
    for (size_t off = 0; off + 4 <= len; ) {
        int ssize = *(int *)(buf + off);
        if (ssize <= 0 || off + (size_t)ssize > len)
            break;
        int pid = *(int *)(buf + off + 72);
        const char *tdname = buf + off + 447;
        printf("  pid=%-6d %s\n", pid, tdname);
        n++;
        off += (size_t)ssize;
    }
    fflush(stdout);
    free(buf);
    evo_notify("app_ctl: %d processes (see stdout / pipe=1)", n);
}

static void
do_kill(void)
{
    int pid = find_pid_by_name("eboot.bin");
    if (pid > 0) {
        evo_notify("app_ctl: killing eboot.bin pid=%d", pid);
        kill(pid, SIGKILL);
        sleep(2);
    } else {
        evo_notify("app_ctl: no eboot.bin process running");
    }
}

static void
do_launch(void)
{
    sceUserServiceInitialize(NULL);
    int rc = sceSystemServiceLaunchApp(EVO_APP_TITLE_ID, NULL, NULL);
    evo_notify("app_ctl: LaunchApp(%s) = 0x%x", EVO_APP_TITLE_ID, (unsigned)rc);
}

int
main(int argc, char **argv)
{
    const char *action = (argc > 1) ? argv[1] : "relaunch";

    if (strcmp(action, "list") == 0) {
        do_list();
    } else if (strcmp(action, "kill") == 0) {
        do_kill();
    } else if (strcmp(action, "launch") == 0) {
        do_launch();
    } else { /* relaunch */
        do_kill();
        do_launch();
    }
    return 0;
}
