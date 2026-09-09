/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pp_gl_fatal — EVO's strong override of ps5-opengl's fail-stop hook, plus a
 * --wrap=_Exit backstop.
 *
 * ps5-opengl hard-exits (`_Exit(EXIT_FAILURE)`) on a handful of unrecoverable
 * GPU submit / unmap paths. patches/ps5-opengl/0001-recoverable-fail.patch
 * routes those four sites through a weak `ps5gl_fatal(tag)`; this file provides
 * the strong definition. The app module must never _exit() — it strands the
 * PS5 app slot, loses resume state, skips teardown (CLAUDE.md).
 *
 * Behaviour:
 *   - record the tag to /mnt/usb0/evo.log and set g_pp_gl_dead
 *   - if a caller armed the fence (pp_gl_smoke_run), longjmp back to it so the
 *     smoke reports FAIL at that stage instead of the process dying
 *   - otherwise park (log a heartbeat, sleep) — still no _exit()
 *
 * Only linked with -DEVO_GL_SMOKE (scripts/package-app.sh --gl-smoke).
 * GL-2+ inherits this hook; a production fault-recovery contract is a
 * follow-up (see docs/evo-pro/gl1-spike.md).
 */
#include <setjmp.h>
#include <stdlib.h>
#include <unistd.h>

#include "evo_boot_log.h"
#include "pp_gl_smoke.h"

/* The linker's --wrap=_Exit also exposes __real__Exit(); we deliberately never
 * call it (the whole point is to not _exit from the app module). */

volatile int g_pp_gl_dead = 0;

/* Armed by pp_gl_smoke_run() around the ps5-opengl calls. */
jmp_buf      g_pp_gl_fence;
volatile int g_pp_gl_fence_armed = 0;

static void gl_fatal_common(const char *what)
{
    g_pp_gl_dead = 1;
    evo_boot_log("GL-1 SMOKE: driver fail-stop caught (%s)", what ? what : "?");
    evo_boot_log_flush();

    if (g_pp_gl_fence_armed) {
        g_pp_gl_fence_armed = 0;
        longjmp(g_pp_gl_fence, 1);       /* -> pp_gl_smoke_run(), reports FAIL */
    }

    /* No fence: park. Never _exit() from the app module. */
    for (unsigned i = 0; ; ++i) {
        if ((i % 6) == 0) {
            evo_boot_log("GL-1 SMOKE: parked after fail-stop — read evo.log, reboot the console");
            evo_boot_log_flush();
        }
        sleep(10);
    }
}

/* Strong override of the weak hook in src/platform/ps5gl_fatal.h.
 * Declared here (not #include'd) so this TU doesn't also pull the weak body. */
void ps5gl_fatal(const char *tag) __attribute__((noreturn));

void ps5gl_fatal(const char *tag)
{
    gl_fatal_common(tag);
    __builtin_unreachable();
}

/* Backstop for any _Exit the patch didn't cover (e.g. deep in a vendored
 * dependency). Linked via -Wl,--wrap=_Exit in scripts/package-app.sh. */
void __wrap__Exit(int code)
{
    (void)code;
    gl_fatal_common("_Exit");
    __builtin_unreachable();
}

/* Mesa builds its glapi context/dispatch as emulated-TLS (-femulated-tls under
 * the SDK clang). The compiler still emits a *weak* reference to the C++ TLS
 * init helper _ZTH<len><name>; emulated TLS initialises lazily and never calls
 * it, so lld leaves it as a harmless weak-undef. EVO's app-module converter
 * (tools/native-app) requires every undefined dynamic symbol to be backed by an
 * SDK stub, so give the helper a no-op strong definition. Same idea as the
 * libc gap-fillers in tools/native-app/stubs/libc_ext.c. */
void _ZTH23_mesa_glapi_tls_Context(void) {}
void _ZTH24_mesa_glapi_tls_Dispatch(void) {}
