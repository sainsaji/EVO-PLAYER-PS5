/*
 * evo_agc_probe.c - boot-time sceAgc reachability recon (app module only).
 * See evo_agc_probe.h. Compiled only under -DEVO_AGC_PROBE.
 */
#ifdef EVO_AGC_PROBE

#include "evo_agc_probe.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

struct agc_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int sceKernelDebugOutText(int, const char *);
extern int sceKernelLoadStartModule(const char *name, unsigned long argc,
                                    const void *argv, unsigned int flags,
                                    void *opt, int *res);

static void note(const char *fmt, ...)
{
    struct agc_note n;
    char line[512];
    va_list ap;

    memset(&n, 0, sizeof n);
    va_start(ap, fmt);
    vsnprintf(n.msg, sizeof n.msg, fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
    snprintf(line, sizeof line, "%s\n", n.msg);
    sceKernelDebugOutText(0, line);
}

static const char *const kAgcSyms[] = {
    "sceAgcInit",
    "sceAgcGetRegisterDefaults",
    "sceAgcCreateShader",
    "sceAgcLinkShaders",
    "sceAgcDcbSetShRegistersIndirect",
    "sceAgcCbSetShRegisterRangeDirect",
    "sceAgcDcbDrawIndexAuto",
    "sceAgcDcbSetFlip",
    "sceAgcDriverSubmitDcb",
    "sceAgcDriverWaitUntilSafeForRendering",
};

static int try_module(const char *basename, uint32_t *dynh)
{
    if (kernel_dynlib_handle(getpid(), basename, dynh) == 0)
        return 1;

    static const char *const dirs[] = {
        "/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
    };
    char path[256];
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        int res = 0;
        snprintf(path, sizeof path, "%s%s", dirs[i], basename);
        int modid = sceKernelLoadStartModule(path, 0, 0, 0, 0, &res);
        if (modid >= 0 && kernel_dynlib_handle(getpid(), basename, dynh) == 0)
            return 1;
    }
    return 0;
}

void evo_agc_probe(void)
{
    note("EVO agc: probe start (pid %d)", getpid());

    uint32_t h = 0;
    if (!try_module("libSceAgc.sprx", &h)) {
        note("EVO agc: libSceAgc.sprx NOT AVAILABLE - Step 2 needs a different route");
        return;
    }

    int found = 0;
    const int total = (int)(sizeof kAgcSyms / sizeof *kAgcSyms);
    for (int i = 0; i < total; i++) {
        char nid[12] = {0};
        nid_encode(kAgcSyms[i], nid);
        if (kernel_dynlib_resolve(getpid(), h, nid))
            found++;
    }

    uint32_t dh = 0;
    int have_driver = try_module("libSceAgcDriver.sprx", &dh);

    note("EVO agc: libSceAgc mapped, %d/%d NIDs, driver=%s -> Step 2 %s",
         found, total, have_driver ? "sep" : "folded",
         (found >= total - 2) ? "VIABLE" : "partial");
}

#endif /* EVO_AGC_PROBE */
