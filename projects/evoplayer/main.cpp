#include "evo/Application.hpp"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_crash_note.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#endif

#ifdef EVO_APP_MODULE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#include <unistd.h>

/* Dev-only crash catcher: the sandbox deletes its coredump and the popup
 * reports nothing, so record the fault address to /mnt/usb0/evo.log with
 * raw I/O (async-signal-safe - stdio could deadlock in the handler). */
static void evo_crash_handler(int sig, siginfo_t *si, void *ctx)
{
    /* The SDK's mcontext field offsets do not match this kernel's - mc_rsp read
     * back as 0, impossible for a running handler, so mc_rip was junk too. Dump
     * the raw ucontext words instead and identify RIP by inspection: `anchor` is
     * a known text address, so (word - anchor) symbolizes any code pointer
     * against output/app/.build/eboot.elf. */
    const unsigned long *raw = (const unsigned long *)ctx;
    char buf[2048];
    int len = snprintf(buf, sizeof buf,
                       "CRASH signal=%d addr=%p anchor=%p",
                       sig, si ? si->si_addr : (void *)0,
                       (void *)(uintptr_t)&evo_crash_handler);
    for (int i = 0; i < 72 && len > 0 && len < (int)sizeof buf - 32; ++i) {
        int n = snprintf(buf + len, sizeof buf - (size_t)len, "%s%02d:%lx",
                         (i % 6) ? " " : "\n", i, raw[i]);
        if (n < 0) break;
        len += n;
    }
    if (len > 0 && len < (int)sizeof buf - 2)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "\n");
    int fd = open("/mnt/usb0/evo.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, buf, (size_t)len);
        (void)close(fd);
    }
    /* If we died inside work that is known to be able to kill us - today only
     * software thumbnail decoding - quarantine the file so the next launch
     * does not walk straight back into it. See evo_crash_note.h. */
    evo_crash_note_commit();
    _exit(128 + sig);
}

/* Async-signal-safe by construction: one store to a sig_atomic_t and nothing
 * else. The frame loop polls it and exits through Application::shutdown().
 * See the note on g_evo_term_requested in Application.hpp. */
static void evo_term_handler(int sig)
{
    (void)sig;
    g_evo_term_requested = 1;
}

static void evo_install_crash_catcher(void)
{
    struct sigaction sa;
    sa.sa_sigaction = evo_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGABRT, &sa, 0);

    struct sigaction term;
    term.sa_handler = evo_term_handler;
    sigemptyset(&term.sa_mask);
    term.sa_flags = 0;
    sigaction(SIGTERM, &term, 0);
    sigaction(SIGINT, &term, 0);
    sigaction(SIGHUP, &term, 0);
}
#endif

int main(int argc, char* argv[]) {
#ifdef EVO_HAVE_BUILD_ID
    evo_bt("BUILD " EVO_BUILD_ID);   /* first thing on screen ? - catches a stale mount */
#endif
    evo_bt("main() entry");
#ifdef EVO_APP_MODULE
    evo_install_crash_catcher();
#endif

    auto& app = evo::Application::getInstance();
    if (!app.initialize(argc, argv)) {
        return 1;
    }
    const int rc = app.run();

    /* Last breadcrumb before the C runtime takes over. Anything that faults
     * after this line is in exit handling - static destructors, atexit, libc
     * teardown - not in EVO's own shutdown, which logs "shutdown: complete"
     * of its own accord. Worth keeping: distinguishing those two was the whole
     * difficulty in tracking down the QUIT EVO crash. */
    evo_bt("main: run() returned rc=%d - entering exit", rc);
    evo_boot_log_flush();
    return rc;
}
