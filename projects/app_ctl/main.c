/* app_ctl - elfldr/hbldr payload that drives the PPSA99039 app module for the
 * unattended bring-up loop (tools/app-loop.sh). No TV, no ShadowMountPlus UI.
 *
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

#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_ALL 0
#define SIGKILL 9

static int
find_pid_by_name(const char *want)
{
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0)
        return -1;

    char *buf = malloc(len);
    if (!buf)
        return -1;
    if (sysctl(mib, 3, buf, &len, NULL, 0) != 0) {
        free(buf);
        return -1;
    }

    /* struct kinfo_proc: ki_structsize (int) at 0, ki_pid at 0x48,
     * ki_comm (char[20]) at 0x1bf on 12.xx FreeBSD-derived. Walk by
     * ki_structsize so we don't hardcode the whole struct. */
    int found = -1;
    for (size_t off = 0; off + 4 <= len; ) {
        int ssize = *(int *)(buf + off);
        if (ssize <= 0 || off + (size_t)ssize > len)
            break;
        int pid = *(int *)(buf + off + 0x48);
        const char *comm = buf + off + 0x1bf;
        if (pid > 0 && strncmp(comm, want, 19) == 0) {
            found = pid;
            break;
        }
        off += (size_t)ssize;
    }
    free(buf);
    return found;
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

    if (strcmp(action, "kill") == 0) {
        do_kill();
    } else if (strcmp(action, "launch") == 0) {
        do_launch();
    } else { /* relaunch */
        do_kill();
        do_launch();
    }
    return 0;
}
