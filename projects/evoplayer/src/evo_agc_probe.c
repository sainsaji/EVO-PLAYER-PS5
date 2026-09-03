/*
 * evo_agc_probe.c - boot-time sceAgc reachability check (app module only).
 * See evo_agc_probe.h. Compiled only under -DEVO_AGC_PROBE.
 *
 * As of #31 the loader auto-loads libSceAgc + libSceAgcDriver from POSITIONAL
 * PRX import stubs (tools/native-app/stubs/prx/libSceAgc*.syms, unconditional
 * for MODE == player in package-app.sh step 6b). So sceAgc* are ordinary
 * imports - no sceKernelLoadStartModule, no NID/dlsym dance. This probe just
 * confirms the stubs bind and sceAgcInit succeeds on hardware (the #27 GPU
 * Step 2 go/no-go), reported via a notification popup.
 *
 * The old "EVO agc: libSceAgc.sprx load FAILED" path is dead - a fake-signed
 * module cannot sceKernelLoadStartModule an undeclared system PRX; the PRX
 * import stub is the fix (same mechanism that unblocked Route B decode).
 */
#ifdef EVO_AGC_PROBE

#include "evo_agc_probe.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* --- libSceAgc: stub-linked (package-app.sh step 6b), called directly ---- */
extern int32_t  sceAgcInit(void *state, uint32_t defaults_revision);
extern void    *sceAgcGetRegisterDefaults(void);

extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int sceKernelDebugOutText(int, const char *);

struct agc_note { char pad[45]; char msg[3075]; };
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

/* Fault guard: a NULL-bound stub call SIGSEGVs; report which call instead of a
 * bare crash popup. Mirrors evo_videodec2_probe.c. */
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_armed;
static const char *volatile g_where = "?";
static void fault_h(int sig)
{
    if (g_armed) { g_armed = 0; siglongjmp(g_jmp, sig); }
    _exit(150 + sig);
}
#define AT(x) (g_where = (x))

void evo_agc_probe(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fault_h;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    if (sigsetjmp(g_jmp, 1) != 0) {
        note("EVO agc: FAULT at [%s] - sceAgc stub NEEDED-mapped but not "
             "callable. Step 2 blocked.", g_where);
        g_armed = 0;
        goto restore;
    }
    g_armed = 1;

    uint64_t state = 0;
    AT("sceAgcInit");
    int32_t rc = sceAgcInit(&state, 8);   /* 8 = defaults revision (ProsperoLight) */

    AT("sceAgcGetRegisterDefaults");
    void *defaults = sceAgcGetRegisterDefaults();

    g_armed = 0;
    note("EVO agc: init=0x%08x defaults=%p  -> Step 2 %s",
         (unsigned)rc, defaults,
         (rc == 0 && defaults) ? "VIABLE (stubs bind, sceAgc works)"
                               : "NOT AVAILABLE");

restore:
    {
        struct sigaction d;
        memset(&d, 0, sizeof d);
        d.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &d, NULL);
        sigaction(SIGBUS, &d, NULL);
        sigaction(SIGILL, &d, NULL);
    }
}

#endif /* EVO_AGC_PROBE */
