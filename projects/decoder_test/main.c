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

int
main(void)
{
    printf("=== EVO Player decoder_test (native media reconnaissance) ===\n");
    printf("firmware: 0x%08x\n\n", kernel_get_fw_version());

    printf("Module availability in this process:\n");
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

    printf("\nSymbol probes:\n");
    int hits = 0;
    hits += probe("libSceAvPlayer.sprx", kAvPlayerSymbols,
                  sizeof kAvPlayerSymbols / sizeof *kAvPlayerSymbols);
    hits += probe("libSceVdecCore.sprx", kVdecSymbols,
                  sizeof kVdecSymbols / sizeof *kVdecSymbols);

    printf("\n"
           "RESULT: %d/%zu candidate modules mapped, %d symbols resolved.\n",
           mapped, sizeof kMediaModules / sizeof *kMediaModules, hits);
    printf(
        "\n"
        "If a module shows [ ] it is simply not loaded into THIS process; a\n"
        "payload injected into a lightweight host process will not have the\n"
        "media stack mapped. Options, in order of preference:\n"
        "  1. try sceSysmoduleLoadModuleInternal() for the module id\n"
        "  2. run the payload inside a process that already uses the decoder\n"
        "  3. generate stubs from a decrypted .sprx (docs/proprietary.md)\n"
        "Record whatever you find in docs/native-media-research.md.\n");

    evo_notify("EVO decoder_test: %d modules mapped, %d symbols", mapped, hits);
    return EXIT_SUCCESS;
}
