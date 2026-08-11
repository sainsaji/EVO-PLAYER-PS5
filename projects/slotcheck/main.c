/* EVO Player - slotcheck
 *
 * "Is the app slot free?" - answered without launching anything into it.
 *
 * DEPLOY THIS ONE WITH ./scripts/deploy.sh, NOT install-homebrew.sh --run.
 * That is the whole point: deploy.sh injects into SceSpZeroConf, a background
 * service that is NOT the PS Now app slot, so running this cannot stack on top
 * of whatever is in that slot. Every other probe in this repo wants --run;
 * this one specifically must not have it.
 *
 * ---------------------------------------------------------------------------
 * WHY IT EXISTS
 *
 * The stacking rule has been the most expensive rule in this project - ten
 * launches without exiting the previous instance kernel-panicked the console
 * and cost about fifty minutes. tools/launch.sh guards against firing launches
 * too close together, but a cooldown timer is a proxy: it knows when we last
 * launched, not whether anything is still running. After a payload HANGS -
 * which sceVideoDecoderArbitrationInitialize did on 2026-08-11 - the timer says
 * nothing useful, and "I closed the homebrew launcher" is ambiguous, because
 * closing the launcher UI is not the same as closing the application it
 * started.
 *
 * websrv exposes no process listing, so this asks the kernel.
 *
 * ---------------------------------------------------------------------------
 * HOW IT DECIDES, AND WHY THE FIRST VERSION WAS NOT ENOUGH
 *
 * The first version flagged any process with the decoder modules mapped. That
 * found five, which was misleading: system services legitimately map these too,
 * and the signatures differed (one had only libSceVideodec2, another had
 * VdecCore and Arbitration but no Videodec2 - neither matches what our probes
 * load). Module presence alone cannot tell "our hung probe" from "a Sony media
 * service doing its job".
 *
 * So this version reads the process NAME out of the kernel's proc structure,
 * and reports the module set alongside it rather than deciding from it.
 *
 * FreeBSD's struct proc carries p_comm, a short NUL-terminated name. Its offset
 * is not exported by the SDK, so rather than hardcode a firmware-specific
 * guess, the probe scans the first part of each proc structure for printable
 * strings - and validates the technique on ITSELF first. This payload knows its
 * own pid, so whatever offset yields a sensible name for us is the offset that
 * means the same thing for everyone else. A control that costs nothing.
 *
 * Everything here is read-only: kernel_get_proc, kernel_copyout and
 * kernel_dynlib_handle only read. Nothing writes to another process, signals
 * one, or tries to terminate one.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "evo_ps5.h"

/* PS5 pids stay small; 93 live processes were seen on 12.70. The sweep is one
 * kernel read per pid, so overshooting costs nothing. */
#define MAX_PID     0x4000
#define PROC_SCAN   0x600   /* bytes of struct proc to scan for p_comm */
#define MAX_SUSPECT 32

/* The eight modules our decoder probes load, in load order. A process holding
 * most of this set is one of ours; a process holding one or two of them is
 * almost certainly a system service. */
static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",
    "libSceSysmodule.sprx",
    "libSceAjm.sprx",
};
#define NMODULES ((int)(sizeof kModules / sizeof *kModules))

/* Find NUL-terminated printable strings in a kernel structure. p_comm is one
 * of very few such fields in struct proc, so the candidates are short. */
static int
scan_names(const uint8_t *buf, size_t len, char out[4][24], size_t off[4])
{
    int found = 0;

    for (size_t i = 0; i < len && found < 4; i++) {
        size_t j = 0;

        while (i + j < len && buf[i + j] >= 0x20 && buf[i + j] < 0x7f && j < 20)
            j++;

        if (j >= 3 && i + j < len && buf[i + j] == '\0') {
            memcpy(out[found], buf + i, j);
            out[found][j] = '\0';
            off[found] = i;
            found++;
            i += j;
        }
    }
    return found;
}

int
main(void)
{
    pid_t    self = getpid();
    int      live = 0, nsusp = 0;
    size_t   comm_off = 0;
    int      have_comm_off = 0;

    struct {
        pid_t    pid;
        intptr_t proc;
        int      hits;
        char     mods[320];
    } susp[MAX_SUSPECT];

    printf("=== EVO Player slotcheck ===\n");
    printf("firmware : 0x%08x\n", kernel_get_fw_version());
    printf("this pid : %d  (SceSpZeroConf - NOT the app slot)\n\n", self);
    fflush(stdout);

    /* -- CONTROL: find p_comm using ourselves ----------------------------- *
     * We know what this process is, so whichever offset yields a sensible
     * short name here is the offset to trust for every other pid. */
    {
        intptr_t proc = kernel_get_proc(self);
        uint8_t  buf[PROC_SCAN];
        char     names[4][24];
        size_t   offs[4];

        printf("control: locating p_comm in our own proc structure\n");
        if (!proc) {
            printf("  kernel_get_proc(self) returned 0 - the sweep below can\n"
                   "  still list pids, but names will be unavailable\n\n");
        } else if (kernel_copyout(proc, buf, sizeof buf) != 0) {
            printf("  kernel_copyout failed - names unavailable\n\n");
        } else {
            int n = scan_names(buf, sizeof buf, names, offs);

            for (int i = 0; i < n; i++)
                printf("  proc+0x%-4zx = \"%s\"\n", offs[i], names[i]);
            if (n) {
                comm_off = offs[0];
                have_comm_off = 1;
                printf("  using proc+0x%zx as p_comm\n\n", comm_off);
            } else {
                printf("  no printable name found - names unavailable\n\n");
            }
        }
        fflush(stdout);
    }

    printf("sweeping pids 1..%d ...\n\n", MAX_PID);
    fflush(stdout);

    for (pid_t pid = 1; pid < MAX_PID && nsusp < MAX_SUSPECT; pid++) {
        intptr_t proc = kernel_get_proc(pid);
        int      hits = 0;
        char     mods[320] = {0};

        if (!proc)
            continue;
        live++;

        for (int i = 0; i < NMODULES; i++) {
            uint32_t h = 0;

            if (kernel_dynlib_handle(pid, kModules[i], &h) == 0 && h) {
                hits++;
                if (mods[0])
                    strncat(mods, " ", sizeof mods - strlen(mods) - 1);
                /* Trim the "libSce" prefix and ".sprx" suffix for width. */
                strncat(mods, kModules[i] + 6,
                        strlen(kModules[i]) - 6 - 5 <
                            sizeof mods - strlen(mods) - 1
                            ? strlen(kModules[i]) - 6 - 5
                            : sizeof mods - strlen(mods) - 1);
            }
        }

        if (!hits)
            continue;

        susp[nsusp].pid  = pid;
        susp[nsusp].proc = proc;
        susp[nsusp].hits = hits;
        snprintf(susp[nsusp].mods, sizeof susp[nsusp].mods, "%s", mods);
        nsusp++;
    }

    printf("processes holding any decoder module (%d of %d live):\n\n",
           nsusp, live);
    printf("  %-6s %-20s %-5s %s\n", "pid", "name", "mods", "which");

    int ours = 0;
    for (int i = 0; i < nsusp; i++) {
        char name[24] = "?";

        if (have_comm_off) {
            uint8_t b[24] = {0};

            if (kernel_copyout(susp[i].proc + (intptr_t)comm_off, b,
                               sizeof b - 1) == 0) {
                for (size_t k = 0; k < sizeof b - 1; k++) {
                    if (b[k] == '\0')
                        break;
                    if (b[k] < 0x20 || b[k] >= 0x7f) {
                        b[k] = '\0';
                        break;
                    }
                }
                if (b[0])
                    snprintf(name, sizeof name, "%s", (char *)b);
            }
        }

        /* Ours load all eight. A system service holding one or two of them is
         * doing its own job and is not a leftover launch. */
        int mine = (susp[i].hits >= NMODULES - 1) && susp[i].pid != self;
        if (mine)
            ours++;

        printf("  %-6d %-20s %d/%-3d %s%s\n", susp[i].pid, name,
               susp[i].hits, NMODULES, susp[i].mods,
               susp[i].pid == self ? "   <- this payload"
                                   : mine ? "   *** LEFTOVER PROBE ***" : "");
    }

    printf("\n=== verdict ===\n");
    printf("  live processes      : %d\n", live);
    printf("  holding any module  : %d\n", nsusp);
    printf("  leftover probes     : %d\n", ours);

    if (ours == 0) {
        printf("\n  THE APP SLOT IS CLEAR.\n"
               "  Nothing is holding the full decoder module set, so no probe\n"
               "  is still resident. Safe to launch with\n"
               "  install-homebrew.sh --run.\n"
               "\n  (Processes listed above with only one or two modules are\n"
               "  system services, not ours - they are always there.)\n");
    } else {
        printf("\n  DO NOT LAUNCH. %d leftover probe(s) still resident.\n"
               "  Closing the homebrew LAUNCHER does not close the application\n"
               "  it started. On the console: PS button -> highlight the running\n"
               "  app -> Options -> Close Game. If that does not clear it,\n"
               "  reboot and re-run the jailbreak.\n", ours);
    }

    printf("\ndone\n");
    fflush(stdout);

    evo_notify("EVO slotcheck: %s (%d leftover)",
               ours ? "APP SLOT BUSY" : "app slot clear", ours);
    return EXIT_SUCCESS;
}
