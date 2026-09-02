/* EVO Player - agc_probe
 *
 * Gate question for docs/evo-pro/gpu-rendering-plan.md Step 2:
 *   Can a plain elfldr/hbldr payload reach sceAgc at all?
 *
 * ProsperoLight runs the whole sceAgc shader pipeline, but it runs as a
 * REGISTERED game app module - libSceAgc.sprx / libSceAgcDriver.sprx are
 * mapped into that process by the loader. The payload SDK ships no Agc stub,
 * so this probe resolves everything at run time the same way decoder_test
 * does: sceKernelLoadStartModule + kernel_dynlib_resolve over Sony NIDs.
 *
 * v2: does NOT link or touch libSceGnmDriver - calling sceGnmAreSubmitsAllowed
 * from an elfldr payload crashed the process ("sce::Gnm::Initialize Error: Get
 * CU Mask Fails"). This build stays away from GNM entirely.
 *
 * PHASE A (always): pure reconnaissance, zero GPU calls, zero panic risk.
 *   - are the Agc modules already mapped? can we load them?
 *   - do the sceAgc* / sceAgcDriver* NIDs resolve?
 *
 * PHASE B (-DAGC_PROBE_INIT=1): the single most informative risky call -
 *   sceAgcInit(). Notifies before and after so a crash is attributable.
 *
 * Panic-vector note (docs/hardware-decode.md): this probe never opens VideoOut
 * and never allocates a queue. Phase A touches no GPU state; Phase B calls
 * only sceAgcInit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

/* Flush after every report line: if a later call crashes, the transcript
 * still shows exactly how far we got. */
#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

/* Watchdog: sceKernelLoadStartModule("libSceAgc.sprx") HUNG from an elfldr
 * payload (run 2). If it hangs from hbldr too, a wedged app slot is much
 * worse than a wedged elfldr host - so a timer thread force-exits the process
 * unless the main thread clears g_wd_stage in time. Recovery is then just the
 * normal PS-button close. */
static volatile int g_wd_stage = 0;   /* bumped past each risky call */
static volatile int g_wd_last = -1;

static void *
watchdog(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20; i++) {
        sceKernelUsleep(1000 * 1000);
        if (g_wd_stage != g_wd_last) { g_wd_last = g_wd_stage; i = 0; }
    }
    evo_notify("agc_probe: WATCHDOG fired at stage %d - force exit", g_wd_stage);
    fflush(stdout);
    _exit(42 + g_wd_stage);
    return NULL;
}

static const char *const kAgcSyms[] = {
    "sceAgcInit",
    "sceAgcGetRegisterDefaults",
    "sceAgcCreateShader",
    "sceAgcLinkShaders",
    "sceAgcDcbSetCxRegistersIndirect",
    "sceAgcDcbSetShRegistersIndirect",
    "sceAgcDcbSetUcRegistersIndirect",
    "sceAgcCbSetShRegisterRangeDirect",
    "sceAgcDcbDrawIndexAuto",
    "sceAgcDcbSetFlip",
    "sceAgcSuspendPoint",
};

static const char *const kAgcDriverSyms[] = {
    "sceAgcDriverSubmitDcb",
    "sceAgcDriverGetWaitRenderingPacketSizeInDwords",
    "sceAgcDriverWaitUntilSafeForRendering",
};

static int
resolve_syms(uint32_t dynh, const char *const *syms, size_t nsym)
{
    int found = 0;
    for (size_t i = 0; i < nsym; i++) {
        char nid[12] = {0};
        nid_encode(syms[i], nid);
        intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
        P("      %-46s %-12s %s0x%lx\n", syms[i], nid,
          addr ? "" : "unresolved ", (unsigned long)addr);
        if (addr)
            found++;
    }
    return found;
}

/* Report whether a module is mapped; if not, try the usual system paths. */
static int
ensure_module(const char *basename, uint32_t *dynh)
{
    if (kernel_dynlib_handle(getpid(), basename, dynh) == 0) {
        P("  [x] %-34s already mapped  dynlib=0x%x base=0x%lx\n",
          basename, *dynh,
          (unsigned long)kernel_dynlib_mapbase_addr(getpid(), *dynh));
        return 1;
    }

    static const char *const dirs[] = {
        "/system/common/lib/",
        "/system/priv/lib/",
        "/system_ex/common_ex/lib/",
    };
    char path[256];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        int res = 0;
        snprintf(path, sizeof path, "%s%s", dirs[i], basename);
        P("      trying %s ...\n", path);
        g_wd_stage++;   /* arm watchdog around the call that hung on elfldr */
        int modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        g_wd_stage++;
        if (modid >= 0) {
            int have = (kernel_dynlib_handle(getpid(), basename, dynh) == 0);
            P("  [x] %-34s loaded  modid=0x%x dynlib=0x%x res=0x%x\n",
              basename, modid, have ? *dynh : 0, res);
            return have;
        }
        P("  [ ] %-34s load -> 0x%08x\n", basename, modid);
    }
    return 0;
}

int
main(void)
{
    P("=== EVO Player agc_probe v2 ===\n");
    P("firmware: 0x%08x  pid: %d\n\n", kernel_get_fw_version(), getpid());

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_detach(wd);

    /* -- Phase A: modules + symbol resolution --------------------------- */
    P("Agc modules:\n");
    uint32_t agc_h = 0, agcd_h = 0;
    int have_agc  = ensure_module("libSceAgc.sprx", &agc_h);
    int have_agcd = ensure_module("libSceAgcDriver.sprx", &agcd_h);

    int agc_found = 0, agcd_found = 0;
    if (have_agc) {
        P("\n  libSceAgc.sprx symbols:\n");
        agc_found = resolve_syms(agc_h, kAgcSyms,
                                 sizeof kAgcSyms / sizeof *kAgcSyms);
    }
    if (have_agcd) {
        P("\n  libSceAgcDriver.sprx symbols:\n");
        agcd_found = resolve_syms(agcd_h, kAgcDriverSyms,
                                  sizeof kAgcDriverSyms / sizeof *kAgcDriverSyms);
    }
    if (have_agc && !agcd_found) {
        P("\n  libSceAgc.sprx (driver syms):\n");
        agcd_found = resolve_syms(agc_h, kAgcDriverSyms,
                                  sizeof kAgcDriverSyms / sizeof *kAgcDriverSyms);
    }

    int total_syms = (int)(sizeof kAgcSyms / sizeof *kAgcSyms +
                           sizeof kAgcDriverSyms / sizeof *kAgcDriverSyms);
    int total_found = agc_found + agcd_found;

    P("\n--- Phase A result ---\n");
    P("  libSceAgc       = %s\n", have_agc  ? "mapped" : "NOT AVAILABLE");
    P("  libSceAgcDriver = %s\n", have_agcd ? "mapped" : "not separately mapped");
    P("  sceAgc* NIDs    = %d / %d resolved\n", total_found, total_syms);

    int phase_a_pass = have_agc && total_found >= 8;

#if defined(AGC_PROBE_INIT) && AGC_PROBE_INIT
    if (!phase_a_pass) {
        P("\nPhase A failed - skipping sceAgcInit.\n");
        evo_notify("agc_probe: A FAIL agc=%d nids=%d/%d",
                   have_agc, total_found, total_syms);
        return 0;
    }

    typedef int32_t (*agc_init_fn)(void *state, uint32_t revision);
    char nid[12] = {0};
    nid_encode("sceAgcInit", nid);
    agc_init_fn fn = (agc_init_fn)kernel_dynlib_resolve(getpid(), agc_h, nid);

    P("\nPhase B: calling sceAgcInit(state, 8) ...\n");
    evo_notify("agc_probe: about to call sceAgcInit");

    static uint64_t agc_state;
    g_wd_stage = 100;
    int32_t rc = fn(&agc_state, 8);
    g_wd_stage = 101;

    P("sceAgcInit -> 0x%08x  (state word 0x%llx)\n",
      rc, (unsigned long long)agc_state);
    evo_notify("agc_probe: sceAgcInit -> 0x%08x %s",
               rc, rc == 0 ? "OK" : "FAIL");
#else
    P("\nPhase B (sceAgcInit) not built - rebuild with AGC_INIT=1\n");
    evo_notify("agc_probe: A %s  agc=%s nids=%d/%d",
               phase_a_pass ? "PASS" : "FAIL",
               have_agc ? "yes" : "no", total_found, total_syms);
#endif

    return 0;
}
