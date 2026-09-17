#include "evo/Application.hpp"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"

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
    _exit(128 + sig);
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
    return app.run();
}
