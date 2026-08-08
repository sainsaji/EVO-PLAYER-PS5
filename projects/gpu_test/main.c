/* EVO Player - gpu_test
 *
 * Milestone 5, and deliberately a *probe* rather than a renderer.
 *
 * The brief says: do not assume GPU APIs are available - inspect the SDK
 * first. Here is what the audit found (see docs/sdk-audit.md):
 *
 *   PRESENT   sce_stubs/libSceGnmDriver.c and libSceGnmDriverForNeoMode.c
 *             give link-time symbols for the GNM driver surface
 *             (sceGnmSubmitCommandBuffers, sceGnmSubmitAndFlipCommandBuffers,
 *             sceGnmAreSubmitsAllowed, ...).
 *
 *   ABSENT    Any GNM *headers*, any command-buffer construction helpers, any
 *             shader compiler. The PS4/PS5 SDK's libgnm C++ helper library
 *             (Gnmx) is proprietary and is not reproduced here. So while you
 *             CAN call sceGnmSubmitCommandBuffers, you must hand-assemble
 *             PM4 command packets to have anything worth submitting.
 *
 * THE PRACTICAL CONSEQUENCE
 *   Raw GNM is not the sensible first route to GPU YUV conversion. The
 *   pacbrew sysroot already ships mesa plus SDL2, so the supported path is
 *   SDL2/OpenGL on top of mesa's radeonsi, which drives the same hardware
 *   through an open stack. yuv_gpu_test is built on that instead.
 *
 * So this program does the honest thing: it reports what is actually
 * reachable, verifies the GNM stubs resolve at run time, and stops short of
 * pretending to render. Extend it once you have a PM4 stream worth sending.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      /* getpid() - the dynlib probes are per-process */

#include <ps5/kernel.h>

#include "evo_ps5.h"

/* Declared here rather than in evo_ps5.h because these are the only two GNM
 * entry points this probe touches, and their exact signatures are not part of
 * any public header. Both are exported by sce_stubs/libSceGnmDriver.c. */
int sceGnmAreSubmitsAllowed(void);
int sceGnmSubmitCommandBuffers(uint32_t count, void *dcbGpuAddrs[],
                               uint32_t *dcbSizesInBytes,
                               void *ccbGpuAddrs[],
                               uint32_t *ccbSizesInBytes);

/* Resolve a symbol in a module that is already loaded into this process.
 * kernel_dynlib_handle + kernel_dynlib_dlsym come from the SDK's own
 * <ps5/kernel.h> and work without a stub library, which makes them the right
 * tool for probing modules the SDK does not wrap. */
static void
probe_module(const char *basename, const char *const *symbols, size_t nsym)
{
    uint32_t handle = 0;
    pid_t pid = getpid();

    if (kernel_dynlib_handle(pid, basename, &handle) != 0) {
        printf("  %-28s NOT LOADED\n", basename);
        return;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    printf("  %-28s handle=0x%x base=0x%lx\n",
           basename, handle, (unsigned long)base);

    for (size_t i = 0; i < nsym; i++) {
        intptr_t addr = kernel_dynlib_dlsym(pid, handle, symbols[i]);
        printf("      %-40s %s0x%lx\n", symbols[i],
               addr ? "" : "unresolved ", (unsigned long)addr);
    }
}

int
main(void)
{
    printf("=== EVO Player gpu_test (capability probe) ===\n\n");

    printf("firmware: 0x%08x\n\n", kernel_get_fw_version());

    /* -- what the SDK gives us at link time -------------------------------- */
    printf("GNM stubs linked into this payload:\n");
    printf("  sceGnmAreSubmitsAllowed    @ %p\n",
           (void *)(uintptr_t)sceGnmAreSubmitsAllowed);
    printf("  sceGnmSubmitCommandBuffers @ %p\n",
           (void *)(uintptr_t)sceGnmSubmitCommandBuffers);

    /* Calling this is safe and tells us whether the process is even allowed
     * to talk to the GPU - a payload injected into a non-graphics process
     * generally is not. */
    int allowed = sceGnmAreSubmitsAllowed();
    printf("\nsceGnmAreSubmitsAllowed() -> %d  (%s)\n", allowed,
           allowed ? "this process may submit GPU work"
                   : "submits NOT allowed from this process");

    /* -- what is actually mapped in this process --------------------------- */
    static const char *gnm_syms[] = {
        "sceGnmSubmitCommandBuffers",
        "sceGnmSubmitAndFlipCommandBuffers",
        "sceGnmAreSubmitsAllowed",
    };
    static const char *videoout_syms[] = {
        "sceVideoOutOpen",
        "sceVideoOutSubmitFlip",
    };

    printf("\nLoaded module probe:\n");
    probe_module("libSceGnmDriver.sprx", gnm_syms,
                 sizeof gnm_syms / sizeof *gnm_syms);
    probe_module("libSceVideoOut.sprx", videoout_syms,
                 sizeof videoout_syms / sizeof *videoout_syms);

    printf("\n"
           "NEXT STEPS\n"
           "  Raw GNM needs hand-built PM4 packets; the SDK ships no Gnmx\n"
           "  helpers or shader compiler. For GPU YUV->RGB conversion use the\n"
           "  mesa + SDL2/OpenGL stack in the pacbrew sysroot instead.\n"
           "  See projects/yuv_gpu_test and docs/gpu-notes.md.\n");

    evo_notify("EVO gpu_test: submits_allowed=%d (see klog for detail)",
               allowed);
    return EXIT_SUCCESS;
}
