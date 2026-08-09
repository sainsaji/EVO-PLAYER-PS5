/* EVO Player - decoder_test
 *
 * RESEARCH SCAFFOLD - intentionally does not decode anything yet.
 *
 * The brief is explicit: prepare the build structure so native decoder
 * experiments stay isolated from the player, but do not implement it during
 * environment setup. So this is a symbol reconnaissance tool.
 *
 * WHY RECONNAISSANCE IS THE FIRST STEP
 *   The SDK audit found NO stubs for any of the native video decode modules:
 *       libSceVdecCore      libSceVdecShevc     libSceVdecSvp9
 *       libSceVdecwrap      libSceVideoDecoderArbitration
 *       libSceAvPlayer      libSceAvPlayerStreaming
 *   sce_stubs/ contains 32 modules and none of them are these. That means
 *   you cannot simply -lSceVdecCore; there is nothing to link against.
 *
 * THE TWO ROUTES FORWARD
 *   1. RUNTIME RESOLUTION (what this program does)
 *      If the module is already mapped into the process, or can be loaded
 *      with sceSysmoduleLoadModuleInternal, then kernel_dynlib_handle() plus
 *      kernel_dynlib_dlsym() will hand back function addresses directly. No
 *      stub, no NID table, no proprietary file required. This is the
 *      legitimate, self-contained way to explore.
 *
 *   2. GENERATED STUBS from a decrypted .sprx
 *      The SDK supports this natively: drop a .sprx into sce_stubs/ and run
 *      `make -C sce_stubs stubs`, which runs genstub.py over it, resolving
 *      NIDs through aerolib.csv, and emits a linkable .c. See
 *      docs/proprietary.md. Those files are yours to supply locally and must
 *      never enter this repository.
 *
 * Run this on the console and read the klog to learn which modules are
 * reachable on 12.70 before writing a single decode call.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>   /* nid_encode() - Sony symbol-name hashing */

#include "evo_ps5.h"

/* Candidate native media modules. Names follow the on-disk .sprx naming under
 * /system/common/lib/. Presence here is a hypothesis to be tested, not a
 * claim that the module exists on 12.70. */
static const char *const kMediaModules[] = {
    "libSceAvPlayer.sprx",
    "libSceAvPlayer.native.sprx",
    "libSceAvPlayerStreaming.sprx",
    "libSceVdecCore.sprx",
    "libSceVdecShevc.sprx",
    "libSceVdecSvp9.sprx",
    "libSceVdecwrap.sprx",
    "libSceVideoDecoderArbitration.sprx",
    /* Reference points we already know are present, so a run with zero hits
     * is distinguishable from a broken probe. */
    "libSceVideoOut.sprx",
    "libSceAudioOut.sprx",
};

/* Symbols worth looking for if the corresponding module does resolve. These
 * names come from the public PS4 AvPlayer/Vdec ABI; on PS5 they may differ,
 * which is exactly what this probe is meant to establish. */
static const char *const kAvPlayerSymbols[] = {
    "sceAvPlayerInit",
    "sceAvPlayerAddSource",
    "sceAvPlayerGetVideoData",
    "sceAvPlayerGetAudioData",
    "sceAvPlayerIsActive",
    "sceAvPlayerClose",
};

static const char *const kVdecSymbols[] = {
    "sceVideoDecoderQueryResourceInfo",
    "sceVideoDecoderCreate",
    "sceVideoDecoderDecode",
    "sceVideoDecoderFlush",
    "sceVideoDecoderDelete",
};

static int
probe(const char *module, const char *const *syms, size_t nsym)
{
    uint32_t handle = 0;
    pid_t pid = getpid();

    if (kernel_dynlib_handle(pid, module, &handle) != 0) {
        printf("  [ ] %-38s not mapped in this process\n", module);
        return 0;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    printf("  [x] %-38s handle=0x%-6x base=0x%lx\n",
           module, handle, (unsigned long)base);

    int found = 0;
    for (size_t i = 0; i < nsym; i++) {
        intptr_t addr = kernel_dynlib_dlsym(pid, handle, syms[i]);
        if (addr) {
            printf("        + %-44s 0x%lx\n", syms[i], (unsigned long)addr);
            found++;
        } else {
            printf("        - %-44s (unresolved)\n", syms[i]);
        }
    }
    return found;
}

/* Try to LOAD a module by path, then resolve symbols in it.
 *
 * Passive probing is not enough. A module is only mapped into the payload's
 * address space if the payload links against its stub - measured on 12.70,
 * gpu_test sees libSceGnmDriver.sprx (it links -lSceGnmDriver) while this
 * program saw 0/10, including libSceVideoOut which demonstrably works. So to
 * learn anything about modules with no stub, we have to load them ourselves.
 *
 * sceKernelLoadStartModule takes a path and returns a module handle; the SDK's
 * CRT supports SPRX loading (crt/rtld_sprx.c). Symbols then come back through
 * sceKernelDlsym without needing any stub or NID table.
 */
static int
load_and_probe(const char *path, const char *basename,
               const char *const *syms, size_t nsym)
{
    int res = 0;
    int modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);

    if (modid < 0) {
        printf("  [ ] %-46s load failed 0x%08x\n", basename, modid);
        return 0;
    }

    /* Once loaded, the module is mapped into this process, so the dynlib
     * handle becomes available. That handle - not the module id - is what
     * kernel_dynlib_resolve wants. */
    uint32_t dynh = 0;
    int have_dynh = (kernel_dynlib_handle(getpid(), basename, &dynh) == 0);

    printf("  [x] %-46s modid=0x%x dynlib=0x%x base=0x%lx\n",
           basename, modid, dynh,
           have_dynh ? (unsigned long)kernel_dynlib_mapbase_addr(getpid(), dynh) : 0UL);

    int found = 0;
    for (size_t i = 0; i < nsym; i++) {
        char nid[12] = {0};
        intptr_t addr = 0;

        /* Sony modules export NIDs, not plain names - which is why plain
         * sceKernelDlsym returns 0x80020003 (ESRCH) for every symbol here.
         * nid_encode() applies Sony's symbol hash so we can look the symbol
         * up the way the module actually advertises it. */
        nid_encode(syms[i], nid);

        if (have_dynh)
            addr = kernel_dynlib_resolve(getpid(), dynh, nid);

        if (addr) {
            printf("        + %-38s %-12s 0x%lx\n",
                   syms[i], nid, (unsigned long)addr);
            found++;
        } else {
            /* Fall back to the plain-name lookup so the output shows both
             * results; if THIS one ever succeeds the module exports names. */
            void *a2 = NULL;
            int rc = sceKernelDlsym(modid, syms[i], &a2);
            if (rc == 0 && a2) {
                printf("        + %-38s (by name)   %p\n", syms[i], a2);
                found++;
            } else {
                printf("        - %-38s nid=%-11s unresolved\n", syms[i], nid);
            }
        }
    }
    return found;
}

int
main(void)
{
    printf("=== EVO Player decoder_test (native media reconnaissance) ===\n");
    printf("firmware: 0x%08x\n\n", kernel_get_fw_version());

    /* -- control ---------------------------------------------------------
     * This payload links -lSceVideoOut and -lSceAudioOut, so those two MUST
     * show as mapped. If they do not, the probe itself is broken and every
     * other result below is meaningless. */
    printf("Control (modules this payload links against):\n");
    int control = 0;
    static const char *const kControl[] = {
        "libSceVideoOut.sprx", "libSceAudioOut.sprx", "libSceSysmodule.sprx",
    };
    for (size_t i = 0; i < sizeof kControl / sizeof *kControl; i++) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(getpid(), kControl[i], &h) == 0) {
            printf("  [x] %-32s handle=0x%x base=0x%lx\n", kControl[i], h,
                   (unsigned long)kernel_dynlib_mapbase_addr(getpid(), h));
            control++;
        } else {
            printf("  [ ] %-32s NOT MAPPED - probe is unreliable!\n", kControl[i]);
        }
    }

    /* -- passive: is anything media-related already mapped? --------------- */
    printf("\nAlready mapped in this process:\n");
    int mapped = 0;
    for (size_t i = 0; i < sizeof kMediaModules / sizeof *kMediaModules; i++) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(getpid(), kMediaModules[i], &h) == 0) {
            printf("  [x] %s\n", kMediaModules[i]);
            mapped++;
        } else {
            printf("  [ ] %s\n", kMediaModules[i]);
        }
    }

    /* -- active: try to load them ourselves -------------------------------
     * System modules live under /system/common/lib/. */
    printf("\nAttempting to load from /system/common/lib/:\n");
    int hits = 0;
    hits += load_and_probe("/system/common/lib/libSceAvPlayer.sprx",
                           "libSceAvPlayer.sprx", kAvPlayerSymbols,
                           sizeof kAvPlayerSymbols / sizeof *kAvPlayerSymbols);
    hits += load_and_probe("/system/common/lib/libSceVdecCore.sprx",
                           "libSceVdecCore.sprx", kVdecSymbols,
                           sizeof kVdecSymbols / sizeof *kVdecSymbols);
    hits += load_and_probe("/system/common/lib/libSceVideoDecoderArbitration.sprx",
                           "libSceVideoDecoderArbitration.sprx", kVdecSymbols,
                           sizeof kVdecSymbols / sizeof *kVdecSymbols);

    printf("\nRESULT: control %d/3, already-mapped %d/%zu, symbols resolved %d\n",
           control, mapped, sizeof kMediaModules / sizeof *kMediaModules, hits);
    printf(
        "\n"
        "Reading this: a module only appears in the passive list if this\n"
        "payload links its stub, so [ ] there means 'not a dependency', NOT\n"
        "'absent from the system'. The load attempts above are the real test.\n"
        "If loading fails too, the remaining route is generating stubs from a\n"
        "decrypted .sprx - see docs/proprietary.md.\n"
        "Record findings in docs/native-media-research.md.\n");

    evo_notify("EVO decoder_test: control %d/3, loaded symbols %d",
               control, hits);
    return EXIT_SUCCESS;
}
