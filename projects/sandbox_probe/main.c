/* sandbox_probe - Phase 1b milestone-1, step A (docs/evo-pro/phase-1b-app-module.md sec 5).
 *
 * A registered game-category title launched from the home screen goes through
 * normal sceSblACMgr sandbox setup - NOT hbldr's already-unsandboxed borrowed
 * slot. This probe reports, over on-screen notifications, what the app-module
 * process can actually see and write.
 *
 * Step A (2026-09-02) established: real user session, sandbox active
 * (/download0 writable), but opendir() returns NULL on EVERY path including "/".
 * That is not the sandbox - a walled process lists its own root - so it points
 * at the clean-room libc.prx shim's dirent path.
 *
 * This revision isolates the layer. Per path it now runs four probes, from
 * thickest libc wrapper to thinnest syscall:
 *
 *   od  opendir()                         full wrapper: open+fstat+fdopendir+malloc
 *   st  stat()                            the stat wrapper alone
 *   o   open(O_RDONLY|O_DIRECTORY)         POSIX open syscall wrapper
 *   gd  getdents(fd, buf, n)              thin getdents syscall wrapper
 *
 * Reading the result:
 *   o + gd OK but od fails   -> opendir's fstat/fdopendir/malloc step is the
 *                               shim gap; EVO can route its browser through a
 *                               getdents-based readdir, and task-4's
 *                               api-surface re-harvest should fix opendir too.
 *   o fails / gd fails       -> the sandbox ACL blocks directory reads; needs a
 *                               structural fix (nullfs bind or sandbox-unjail),
 *                               not just a symbol re-harvest.
 *
 * Built as the PPSA99039 app module by scripts/package-app.sh --probe.
 * Also buildable as a plain elfldr payload (make -C projects/sandbox_probe)
 * for an A/B against the unsandboxed context.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "evo_ps5.h"

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x00020000
#endif

/* FreeBSD libc, declared in <dirent.h>: wraps the getdents(2) syscall directly
 * with no malloc / fstat / fdopendir in the way. */
int getdents(int fd, char *buf, int nbytes);

static void
probe_path(const char *path)
{
    int e_od = 0, e_st = 0, e_o = 0;
    int od = 0, st_ok = 0;

    errno = 0;
    DIR *d = opendir(path);
    if (d != NULL) {
        od = 1;
        closedir(d);
    } else {
        e_od = errno;
    }

    struct stat sb;
    errno = 0;
    if (stat(path, &sb) == 0) {
        st_ok = 1;
    } else {
        e_st = errno;
    }

    errno = 0;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        e_o = errno;

    int gd = -1;
    char first[64] = "";
    if (fd >= 0) {
        char buf[4096];
        errno = 0;
        gd = getdents(fd, buf, (int)sizeof buf);
        if (gd > 0) {
            const struct dirent *de = (const struct dirent *)buf;
            snprintf(first, sizeof first, " '%.*s'", (int)de->d_namlen, de->d_name);
        }
        close(fd);
    }

    /* write test kept from the first revision */
    int wr = 0;
    char probe[512];
    snprintf(probe, sizeof probe, "%s/.evo_sandbox_probe", path);
    int wfd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd >= 0) {
        wr = (write(wfd, "evo", 3) == 3) ? 1 : 0;
        close(wfd);
        unlink(probe);
    }

    evo_notify("%-15s od:%s%s%d st:%s%s%d o:%s%s%d gd:%d%s w:%s",
               path,
               od ? "OK" : "--", od ? " e" : "e", e_od,
               st_ok ? "OK" : "--", st_ok ? " e" : "e", e_st,
               (e_o == 0) ? "OK" : "--", (e_o == 0) ? " e" : "e", e_o,
               gd, first,
               wr ? "OK" : "--");
}

int
main(void)
{
    static const char *const paths[] = {
        "/",
        "/app0",
        "/download0",
        "/data",
        "/data/evoplayer",
        "/mnt/usb0",
        "/mnt/ext0",
    };

    evo_notify("EVO sandbox probe (PPSA99039) - start");
    evo_notify("errno: 2=ENOENT 13=EACCES 20=ENOTDIR 1=EPERM 93/94=NOTCAPABLE");

    int32_t uid = -1;
    int init_rc = sceUserServiceInitialize(NULL);
    int user_rc = sceUserServiceGetInitialUser(&uid);
    evo_notify("userService: init=0x%x  getInitialUser=0x%x  uid=0x%x",
               (unsigned)init_rc, (unsigned)user_rc, (unsigned)uid);

    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        probe_path(paths[i]);

    evo_notify("EVO sandbox probe - done (PS button to close)");

    /* Stay resident so the notifications persist and the title does not
     * exit immediately (which reads as a crash on the home screen). */
    for (;;)
        sceKernelUsleep(2 * 1000 * 1000);

    return 0;
}
