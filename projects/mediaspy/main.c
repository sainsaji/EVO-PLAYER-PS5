/* EVO Player - mediaspy
 *
 * Watch a process that is ACTUALLY DECODING, and read what makes it different.
 *
 * DEPLOY THIS ONE WITH ./scripts/deploy.sh, NOT install-homebrew.sh --run.
 * That is not a preference, it is the whole design. --run borrows the PS Now
 * application slot, and the thing we want to observe - the Media Gallery
 * playing a video - IS an application. Launching into that slot would evict or
 * collide with the very process being measured. deploy.sh lands in
 * SceSpZeroConf, a background service, so this can watch without touching.
 *
 * ---------------------------------------------------------------------------
 * WHY
 *
 * Every measurement in this effort so far has been taken from a decoder that
 * FAILS. sceVideodec2Decode reaches the hardware submit and the driver refuses
 * the job with ioctl errno 5200, and findings.md section 13 has spent two
 * sessions unable to say whether that refusal is about the JOB or about the
 * CALLER's identity.
 *
 * The console can answer that itself. When the Media Gallery plays a video it
 * drives these same modules successfully, on this firmware, on this hardware.
 * So there is a process on this box for which the driver says yes, and three
 * SDK calls read the difference:
 *
 *     kernel_dynlib_handle(pid, module, &h)   - which pid is decoding
 *     kernel_get_ucred_authid(pid)            - who it is
 *     kernel_get_ucred_caps(pid, caps)        - what it may do
 *
 * All three are documented in findings.md section 2 as working CROSS-PROCESS,
 * and slotcheck already uses the first two of them in anger.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DOES *NOT* DO, AND WHY
 *
 * It does not scan anybody's memory. Not one byte.
 *
 * The obvious next thought - find the working decoder's objects and diff them
 * against ours - needs a sweep of a foreign address space, and on 2026-08-14 a
 * sweep of kernel .text panicked this console and cost about fifty minutes.
 * That was a different primitive (kernel_copyout over kernel text, rather than
 * kernel_proc_copyout over user pages, which goes through the kernel's VM
 * lookup and returns an error for unmapped pages) - but "different primitive,
 * should be fine" is exactly the reasoning that produced the panic.
 *
 * So this is deliberately the half that carries no such risk. If the authid
 * comparison alone settles the identity question, no sweep is ever needed. If
 * it does not, the sweep gets proposed on its own merits, with its own
 * sign-off, as a separate change.
 *
 * ---------------------------------------------------------------------------
 * THE COMPARISON THAT MATTERS - AND THE TRAP IN IT
 *
 * Authority is a property of the HOST PROCESS (standing rule 12). This payload
 * runs in SceSpZeroConf, which is NOT where our decode runs - our decode runs
 * in the app slot, and that slot was measured on 2026-08-14 at
 *
 *     authid 0x4800000000000027
 *     caps   ffffffffff1cff40ffffffffffffffff
 *
 * So the meaningful comparison is  media process  vs  THAT constant, not vs
 * whatever this payload's own credentials turn out to be. This program prints
 * its own for completeness and labels them as the wrong baseline, because
 * getting this backwards would produce a confident and completely wrong
 * conclusion.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "evo_ps5.h"

/* -- write everything to the USB stick as well as stdout ---------------------
 *
 * The first two watch runs both ended in `timeout` exit 124 with their output
 * truncated mid-table, because a watching payload outlives the deploy socket
 * and only the tail that made it through survives. evo-player-hardware-loop
 * and findings.md section 2 both say this outright - "anything that matters
 * should also be written to a file on /mnt/usb0" - and this probe was written
 * ignoring it.
 *
 * The #define is deliberate: it retargets every printf in this file without
 * touching snprintf, which is a different identifier. */
#define LOG_PATH "/mnt/usb0/evo_mediaspy_log.txt"

static FILE *g_log;

static void evo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
evo_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);          /* every line - it is how hangs get diagnosed */
    }
}

#define printf(...) evo_log(__VA_ARGS__)

#define MAX_PID     0x4000
#define PROC_SCAN   0x600
#define MAX_HITS    48
#define MAX_VISIBLE 128

/* The app slot's credentials, measured 2026-08-14. This is the baseline the
 * media process must be compared against - see the header. */
#define APPSLOT_AUTHID  0x4800000000000027ull
static const char *const kAppSlotCaps = "ffffffffff1cff40ffffffffffffffff";

/* psdevwiki records every decoder MODULE under this auth id. A process holding
 * it would make the identity hypothesis concrete. */
#define DECODER_MODULE_AUTHID 0x4900000000000002ull

/* Wider than slotcheck's list: this is looking for anything that touches media,
 * not specifically for one of our own probes. Order is for reading. */
static const char *const kModules[] = {
    "libSceVideodec2.sprx",
    "libSceVdecCore.sprx",
    "libSceVideodec.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceVdecSavc.sprx",
    "libSceVdecSavc2.sprx",
    "libSceAvPlayer.sprx",
    "libSceGnmDriver.sprx",
    "libSceAudiodec.sprx",
    "libSceAjm.sprx",
};
#define NMODULES ((int)(sizeof kModules / sizeof *kModules))

/* -- THE CONTROL, and this probe is worthless without it ---------------------
 *
 * The first run reported media modules in 1 of 89 processes and libSceGnmDriver
 * in NONE - including SceShellUI, which renders the entire user interface and
 * therefore cannot possibly lack a graphics driver. So the negative result was
 * not evidence about the Media Gallery; it was evidence that the sweep does not
 * see what it claims to see.
 *
 * hardware-decode.md already records this trap in our OWN process - "passive
 * probing lies: kernel_dynlib_handle only finds modules the payload actually
 * depends on" - and it was not applied when pointing the same call at somebody
 * else's process.
 *
 * These three are mapped by essentially every process on the console. If a
 * process reports zero of them, this method cannot see that process's modules
 * and its media-module count means nothing. Reported per process, next to the
 * media count, so the two can never be read apart. */
static const char *const kControlModules[] = {
    "libkernel_sys.sprx",
    "libSceLibcInternal.sprx",
    "libSceSysmodule.sprx",
};
#define NCONTROL ((int)(sizeof kControlModules / sizeof *kControlModules))

/* The three that actually mean "this process can decode video". A process with
 * only AvPlayer or only Ajm is doing something else. */
static int
is_decoder_module(int i)
{
    return i <= 1;   /* Videodec2, VdecCore */
}

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

static void
print_caps(const uint8_t caps[16])
{
    for (int i = 0; i < 16; i++)
        printf("%02x", caps[i]);
}

static int
caps_match_appslot(const uint8_t caps[16])
{
    char s[33];

    for (int i = 0; i < 16; i++)
        snprintf(s + i * 2, 3, "%02x", caps[i]);

    return strcmp(s, kAppSlotCaps) == 0;
}

int
main(void)
{
    pid_t  self = getpid();
    int    live = 0, nhits = 0, visible = 0;
    pid_t  vis[MAX_VISIBLE];
    size_t comm_off = 0;
    int    have_comm_off = 0;
    int    decoders = 0;

    struct {
        pid_t    pid;
        intptr_t proc;
        char     name[24];
        int      hits;
        int      canDecode;
        char     mods[420];
        uint64_t authid;
        uint8_t  caps[16];
        int      haveCaps;
    } h[MAX_HITS];

    g_log = fopen(LOG_PATH, "w");

    printf("=== EVO Player mediaspy ===\n");
    printf("firmware : 0x%08x\n", kernel_get_fw_version());
    printf("this pid : %d  (SceSpZeroConf - NOT the app slot)\n\n", self);
    fflush(stdout);

    printf("Play a video in the Media Gallery BEFORE running this. If nothing\n"
           "is decoding, the table below will show only idle system services\n"
           "and the run tells you nothing - that is a null result, not a bug.\n\n");
    fflush(stdout);

    /* -- CONTROL: locate p_comm on ourselves, as slotcheck does ----------- */
    {
        intptr_t proc = kernel_get_proc(self);
        uint8_t  buf[PROC_SCAN];
        char     names[4][24];
        size_t   offs[4];

        printf("control: locating p_comm in our own proc structure\n");
        if (!proc) {
            printf("  kernel_get_proc(self) = 0 - names unavailable\n\n");
        } else if (kernel_copyout(proc, buf, sizeof buf) != 0) {
            printf("  kernel_copyout failed - names unavailable\n\n");
        } else if (scan_names(buf, sizeof buf, names, offs)) {
            comm_off      = offs[0];
            have_comm_off = 1;
            printf("  proc+0x%zx = \"%s\"  <- using this as p_comm\n\n",
                   offs[0], names[0]);
        } else {
            printf("  no printable name found - names unavailable\n\n");
        }
        fflush(stdout);
    }

    /* -- CONTROL: our own credentials, and why they are the WRONG baseline */
    {
        uint8_t caps[16] = {0};
        uint64_t a = kernel_get_ucred_authid(self);

        printf("control: this payload's own credentials\n");
        printf("  authid : 0x%016llx\n", (unsigned long long)a);
        if (kernel_get_ucred_caps(self, caps) == 0) {
            printf("  caps   : ");
            print_caps(caps);
            printf("\n");
        }
        printf("  NOTE: this is SceSpZeroConf, NOT the slot our decode runs in.\n"
               "  Compare the media process against the APP SLOT instead:\n"
               "    authid 0x%016llx\n    caps   %s\n\n",
               (unsigned long long)APPSLOT_AUTHID, kAppSlotCaps);
        fflush(stdout);
    }

    /* -- the sweep. One kernel read per pid, no memory scanning ----------- */
    printf("sweeping pids 1..%d - every live process, named ...\n\n", MAX_PID);
    fflush(stdout);

    for (pid_t pid = 1; pid < MAX_PID && nhits < MAX_HITS; pid++) {
        intptr_t proc = kernel_get_proc(pid);
        int      hits = 0, canDecode = 0;
        char     mods[420] = {0};

        if (!proc)
            continue;
        live++;

        for (int i = 0; i < NMODULES; i++) {
            uint32_t dh = 0;

            if (kernel_dynlib_handle(pid, kModules[i], &dh) != 0 || !dh)
                continue;

            hits++;
            if (is_decoder_module(i))
                canDecode++;

            if (mods[0])
                strncat(mods, " ", sizeof mods - strlen(mods) - 1);
            strncat(mods, kModules[i] + 6, sizeof mods - strlen(mods) - 1);
        }

        /* -- list EVERY live process, not only the ones that matched --------
         *
         * The first run of this probe found media modules in exactly ONE of 89
         * live processes, and none at all in libSceGnmDriver - which cannot be
         * true of a console that is rendering a user interface. So the sweep
         * itself is the thing under suspicion, and a table filtered by the
         * suspect predicate is the one table that cannot show that.
         *
         * Printing every process name costs 89 lines and settles it: if the
         * Media Gallery is not in this list, it is not a process we can see and
         * the module question is moot; if it IS in the list with zero modules,
         * then kernel_dynlib_handle is not enumerating other processes the way
         * findings section 2 claims and the negative above means nothing. */
        {
            char nm[24] = "?";

            if (have_comm_off) {
                uint8_t b[24] = {0};

                if (kernel_copyout(proc + (intptr_t)comm_off, b,
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
                        snprintf(nm, sizeof nm, "%s", (char *)b);
                }
            }
            int ctl = 0;

            for (int i = 0; i < NCONTROL; i++) {
                uint32_t dh = 0;

                if (kernel_dynlib_handle(pid, kControlModules[i], &dh) == 0 && dh)
                    ctl++;
            }
            if (ctl) {
                /* Remember it. The watch loop below polls THIS list and
                 * nothing else: re-sweeping all 16384 pids twice a second is
                 * ~17 million kernel calls and simply does not finish - the
                 * first attempt at watch mode timed out without ever reaching
                 * the loop body. */
                if (visible < MAX_VISIBLE)
                    vis[visible] = pid;
                visible++;
            }

            printf("  pid %-6d %-24s media %2d   control %d/%d%s%s\n",
                   pid, nm, hits, ctl, NCONTROL,
                   ctl ? "" : "   <-- INVISIBLE, media count means nothing",
                   hits ? "   <-- maps media" : "");
            fflush(stdout);
        }

        if (!hits)
            continue;

        h[nhits].pid       = pid;
        h[nhits].proc      = proc;
        h[nhits].hits      = hits;
        h[nhits].canDecode = (canDecode == 2);
        snprintf(h[nhits].mods, sizeof h[nhits].mods, "%s", mods);

        h[nhits].name[0] = '?';
        h[nhits].name[1] = '\0';
        if (have_comm_off) {
            uint8_t b[24] = {0};

            if (kernel_copyout(proc + (intptr_t)comm_off, b, sizeof b - 1) == 0) {
                for (size_t k = 0; k < sizeof b - 1; k++) {
                    if (b[k] == '\0')
                        break;
                    if (b[k] < 0x20 || b[k] >= 0x7f) {
                        b[k] = '\0';
                        break;
                    }
                }
                if (b[0])
                    snprintf(h[nhits].name, sizeof h[nhits].name, "%s",
                             (char *)b);
            }
        }

        h[nhits].authid   = kernel_get_ucred_authid(pid);
        h[nhits].haveCaps = (kernel_get_ucred_caps(pid, h[nhits].caps) == 0);

        if (h[nhits].canDecode && pid != self)
            decoders++;
        nhits++;
    }

    /* -- WATCH MODE: catch the decode in the act ---------------------------
     *
     * The snapshot above is taken once, and that turned out to be the wrong
     * shape of measurement. The modules are loaded on demand: a 14-second clip
     * that has already finished leaves no trace, and three separate snapshot
     * runs all landed after playback ended.
     *
     * So instead of asking the operator to hit a timing window, the probe
     * watches. It re-samples every visible process twice a second for
     * WATCH_SECONDS and reports the moment any of them GAINS a media module,
     * capturing that process's credentials at that instant. Start playback any
     * time while this is running.
     *
     * Only processes that passed the control are polled - the rest cannot be
     * seen and polling them would manufacture zeros. */
#define WATCH_SECONDS 40
#define WATCH_HZ      2

    printf("\n=== watching for %d seconds - START PLAYBACK NOW ===\n\n",
           WATCH_SECONDS);
    printf("  Polling %d visible processes %d times a second for a process\n"
           "  that gains libSceVideodec2 or libSceVdecCore.\n\n",
           visible, WATCH_HZ);
    fflush(stdout);

    {
        static uint8_t seen[MAX_PID];       /* media-module count, per pid   */
        int events = 0;

        /* Baseline, so only CHANGES are reported. Visible list only. */
        for (int v = 0; v < visible && v < MAX_VISIBLE; v++) {
            pid_t    pid = vis[v];
            uint32_t dh = 0;
            int      n = 0;

            for (int i = 0; i < NMODULES; i++)
                if (kernel_dynlib_handle(pid, kModules[i], &dh) == 0 && dh)
                    n++;
            seen[pid] = (uint8_t)n;
        }

        for (int tick = 0; tick < WATCH_SECONDS * WATCH_HZ; tick++) {
            usleep(1000000 / WATCH_HZ);

            for (int v = 0; v < visible && v < MAX_VISIBLE; v++) {
                pid_t    pid = vis[v];
                intptr_t proc = kernel_get_proc(pid);
                uint32_t dh = 0;
                int      n = 0, dec = 0;
                char     mods[420] = {0};

                if (!proc)
                    continue;

                for (int i = 0; i < NMODULES; i++) {
                    if (kernel_dynlib_handle(pid, kModules[i], &dh) != 0 || !dh)
                        continue;
                    n++;
                    if (is_decoder_module(i))
                        dec++;
                    if (mods[0])
                        strncat(mods, " ", sizeof mods - strlen(mods) - 1);
                    strncat(mods, kModules[i] + 6,
                            sizeof mods - strlen(mods) - 1);
                }

                if (n <= seen[pid]) {
                    seen[pid] = (uint8_t)n;
                    continue;
                }

                /* It gained something. Grab everything, now. */
                {
                    char     nm[24] = "?";
                    uint8_t  caps[16] = {0};
                    uint64_t a = kernel_get_ucred_authid(pid);
                    int      haveCaps;

                    if (have_comm_off) {
                        uint8_t b[24] = {0};

                        if (kernel_copyout(proc + (intptr_t)comm_off, b,
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
                                snprintf(nm, sizeof nm, "%s", (char *)b);
                        }
                    }
                    haveCaps = (kernel_get_ucred_caps(pid, caps) == 0);

                    printf("  [t+%5.1fs] pid %d %s: %d -> %d module(s)%s\n",
                           (double)tick / WATCH_HZ, pid, nm, seen[pid], n,
                           dec == 2 ? "   *** CAN DECODE ***" : "");
                    printf("             modules : %s\n", mods);
                    printf("             authid  : 0x%016llx%s\n",
                           (unsigned long long)a,
                           a == APPSLOT_AUTHID ? "   SAME AS OUR APP SLOT"
                           : a == DECODER_MODULE_AUTHID
                                 ? "   *** the decoder-module auth id ***" : "");
                    if (haveCaps) {
                        printf("             caps    : ");
                        print_caps(caps);
                        printf("%s\n", caps_match_appslot(caps)
                                           ? "   SAME AS OUR APP SLOT" : "");
                    }
                    printf("\n");
                    fflush(stdout);
                    events++;
                }
                seen[pid] = (uint8_t)n;
            }
        }

        printf("  watch finished: %d module-load event(s) seen\n\n", events);
        if (!events)
            printf("  Nothing loaded a media module in %d seconds. If playback\n"
                   "  really was running, the media path on this firmware does\n"
                   "  not go through these modules at all - which is itself the\n"
                   "  answer, and it points at SceVideoCore2K / SceMediaCoreServer\n"
                   "  talking to the driver by some other route.\n\n",
                   WATCH_SECONDS);
        fflush(stdout);
    }

    /* -- the table -------------------------------------------------------- */
    printf("processes mapping any media module (%d of %d live):\n\n",
           nhits, live);

    for (int i = 0; i < nhits; i++) {
        printf("  pid %-6d %-20s %2d/%d module(s)%s\n",
               h[i].pid, h[i].name, h[i].hits, NMODULES,
               h[i].pid == self ? "   <- this payload"
                                : h[i].canDecode ? "   *** CAN DECODE ***" : "");
        printf("      modules : %s\n", h[i].mods);
        printf("      authid  : 0x%016llx%s\n",
               (unsigned long long)h[i].authid,
               h[i].authid == APPSLOT_AUTHID       ? "   SAME AS OUR APP SLOT"
               : h[i].authid == DECODER_MODULE_AUTHID
                     ? "   *** the decoder-module auth id ***"
                     : "");
        if (h[i].haveCaps) {
            printf("      caps    : ");
            print_caps(h[i].caps);
            printf("%s\n", caps_match_appslot(h[i].caps)
                               ? "   SAME AS OUR APP SLOT" : "");
        }
        printf("\n");
    }

    /* -- the verdict, stated so a null result cannot be misread ----------- */
    printf("=== what this run establishes ===\n\n");

    printf("  CONTROL FIRST: %d of %d live processes were visible to\n"
           "  kernel_dynlib_handle at all (they reported at least one of the\n"
           "  three modules every process maps).\n\n", visible, live);

    if (visible <= 1) {
        printf("  *** THE CONTROL FAILED. Only this payload - or nothing - is\n"
               "  visible, so kernel_dynlib_handle does NOT enumerate other\n"
               "  processes' modules on this firmware, whatever findings.md\n"
               "  section 2 says. Every media-module count above is therefore\n"
               "  meaningless, INCLUDING the zeros. Do not conclude anything\n"
               "  about where decode happens from this run. ***\n\n"
               "  What is still real: the process list itself, which comes from\n"
               "  kernel_get_proc and p_comm, and those are independently\n"
               "  validated against this payload's own name.\n");
        goto tail;
    }

    if (!decoders) {
        printf("  NO other process has both libSceVideodec2 and libSceVdecCore\n"
               "  mapped, so nothing was decoding while this ran.\n\n"
               "  That is a NULL RESULT, not evidence about identity. Start\n"
               "  playback in the Media Gallery, leave it PLAYING, and run this\n"
               "  again. If a video really was playing, then the gallery decodes\n"
               "  through some path that does not map these modules - which is\n"
               "  itself worth knowing, and the module lists above say what it\n"
               "  does map.\n");
    } else {
        printf("  %d process(es) can decode. For each one above, compare its\n"
               "  authid and caps against the app slot's:\n\n"
               "    authid 0x%016llx\n    caps   %s\n\n",
               decoders, (unsigned long long)APPSLOT_AUTHID, kAppSlotCaps);
        printf("  IF THEY MATCH: identity is dead as an explanation for errno\n"
               "  5200. A process with our exact credentials is decoding fine,\n"
               "  so the driver is refusing our JOB. Findings section 13 step 2\n"
               "  (the authid elevation) should not be spent.\n\n"
               "  IF THEY DIFFER: that difference is the first concrete target\n"
               "  this effort has had. Section 13 step 2 becomes a targeted\n"
               "  experiment - elevate to exactly that value - instead of a\n"
               "  guess at 0x4900000000000002 taken from psdevwiki.\n");
    }

tail:
    printf("\n  Nothing here scanned anybody's memory. The follow-up that would\n"
           "  (diffing a working decoder's objects against ours) is deliberately\n"
           "  not in this build - it needs its own sign-off.\n");

    printf("\ndone\n");
    fflush(stdout);
    if (g_log)
        fclose(g_log);

    evo_notify("EVO mediaspy: %d decoding process(es)", decoders);
    return EXIT_SUCCESS;
}
