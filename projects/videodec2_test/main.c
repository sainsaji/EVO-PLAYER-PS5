/* EVO Player - videodec2_test
 *
 * ROUTE B, first call. This is the first program in this project to CALL a
 * Sony media function rather than merely resolve one.
 *
 * WHY THIS FUNCTION, AND WHY IT IS THE SAFE ONE
 *   sceVideodec2QueryDecoderMemoryInfo is a pure query: it validates two
 *   structures and computes how much memory a decoder with that configuration
 *   would need. It creates nothing, allocates nothing and touches no hardware,
 *   which makes it the cheapest possible test of whether the decode stack is
 *   reachable from a payload at all.
 *
 * WHY THE FIRST CALL CAN BE CORRECT RATHER THAN GUESSED
 *   Its prologue was read offline first, from the dumped module image
 *   (docs/native-media-research.md, 2026-08-10):
 *
 *       cmp QWORD PTR [rsi], 0x48     ; memory-info struct: size must BE 0x48
 *       jne -> return 0x811D0101
 *       mov rax, QWORD PTR [rdi]      ; config struct
 *       cmp rax, 0x48
 *       je  ok
 *       cmp rax, 0x50                 ; ...0x48 or 0x50, two ABI revisions
 *       jne -> return 0x811D0101
 *
 *   So both arguments are pointers to SIZE-PREFIXED structures, and the sizes
 *   are known exactly. That is the version check the review predicted would be
 *   in the first few instructions, and it removes the guesswork the plan
 *   budgeted several deploys for.
 *
 *   Known error codes, also read off the code:
 *       0x811D0101   wrong structure size
 *       0x811D0102   bad pointer
 *
 * WHAT IT DOES
 *   1. A zeroed, correctly-sized call, for both config sizes (0x48 and 0x50).
 *   2. A field sweep: set exactly one 32-bit field to 1, leaving the rest
 *      zero, and record the return. Offsets where the code CHANGES are fields
 *      the module validates - which maps the structure without a single extra
 *      deploy. This is the review's "vary one field at a time" discriminator,
 *      done exhaustively rather than by guess.
 *
 * SAFETY
 *   No watchdog thread: measured on 12.70, a watchdog thread inside a payload
 *   does not fire (twice). The guard is `timeout` around the deploy, and a log
 *   flushed after every line so a hang is diagnosable from where it stopped.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

#define USB_DIR   "/mnt/usb0"
#define LOG_PATH  USB_DIR "/evo_vdec2_log.txt"

/* Sizes the module itself checks for. */
#define MEMINFO_SIZE 0x48
#define CFG_SIZE_A   0x48
#define CFG_SIZE_B   0x50

/* Modules to bring up. libSceAudiodec is deliberately absent: loading it here
 * hangs the payload (reproduced twice on 2026-08-10). */
static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
};

typedef int (*query_meminfo_fn)(void *cfg, void *mem);

static FILE *g_log;

static void
LOG(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);   /* the only diagnostic that has survived a hang */
    }
}

static void
hexdump(const void *p, size_t len, const char *indent)
{
    const uint8_t *b = p;

    for (size_t i = 0; i < len; i += 16) {
        char line[160];
        int  n = snprintf(line, sizeof line, "%s%04zx  ", indent, i);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, "%02x ", b[i + j]);
        LOG("%s\n", line);
    }
}

/* Every non-zero 8-byte word in the output, which is where the answer is. */
static void
report_meminfo(const uint8_t *mem)
{
    int any = 0;

    for (size_t off = 8; off + 8 <= MEMINFO_SIZE; off += 8) {
        uint64_t v;
        memcpy(&v, mem + off, sizeof v);
        if (v) {
            LOG("      +0x%02zx = 0x%llx (%llu)\n", off,
                (unsigned long long)v, (unsigned long long)v);
            any = 1;
        }
    }
    if (!any)
        LOG("      (all zero)\n");
}

int
main(void)
{
    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player videodec2_test - Route B first call ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n\n", getpid());

    /* -- bring the modules up --------------------------------------------- */
    for (size_t i = 0; i < sizeof kModules / sizeof *kModules; i++) {
        char path[256];
        int  res = 0;

        snprintf(path, sizeof path, "/system/common/lib/%s", kModules[i]);
        int modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        LOG("load %-38s modid=0x%x res=0x%x\n", kModules[i], modid, res);
    }

    /* -- resolve the entry point ------------------------------------------ */
    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), "libSceVideodec2.sprx", &dynh) != 0) {
        LOG("\nFATAL: no dynlib handle for libSceVideodec2.sprx\n");
        return EXIT_FAILURE;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    char nid[12] = {0};
    nid_encode("sceVideodec2QueryDecoderMemoryInfo", nid);
    intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);

    LOG("\nsceVideodec2QueryDecoderMemoryInfo nid=%s addr=0x%lx (+0x%lx)\n",
        nid, (unsigned long)addr, (unsigned long)(addr - base));
    if (!addr) {
        LOG("FATAL: did not resolve\n");
        return EXIT_FAILURE;
    }

    query_meminfo_fn query = (query_meminfo_fn)addr;

    /* -- 1. the correctly-sized, zeroed call ------------------------------ */
    static uint8_t cfg[0x80];
    static uint8_t mem[0x80];
    static uint8_t best[0x80];
    const uint32_t cfg_sizes[] = { CFG_SIZE_A, CFG_SIZE_B };

    LOG("\n--- zeroed call, both accepted config sizes ---\n");
    for (size_t i = 0; i < sizeof cfg_sizes / sizeof *cfg_sizes; i++) {
        memset(cfg, 0, sizeof cfg);
        memset(mem, 0, sizeof mem);
        *(uint64_t *)cfg = cfg_sizes[i];
        *(uint64_t *)mem = MEMINFO_SIZE;

        LOG("  cfg.size=0x%x mem.size=0x%x ... ", cfg_sizes[i], MEMINFO_SIZE);
        int rc = query(cfg, mem);
        LOG("rc=0x%08x\n", rc);

        LOG("    config after:\n");
        hexdump(cfg, cfg_sizes[i], "      ");
        LOG("    memory info out:\n");
        report_meminfo(mem);
    }

    /* -- 2. deliberate wrong sizes, to confirm the reading is right -------
     * A control. If a bad size does NOT produce 0x811D0101, the prologue was
     * misread and everything above is suspect. */
    LOG("\n--- control: deliberately wrong sizes (expect 0x811d0101) ---\n");
    memset(cfg, 0, sizeof cfg);
    memset(mem, 0, sizeof mem);
    *(uint64_t *)cfg = 0x48;
    *(uint64_t *)mem = 0x40;
    LOG("  mem.size=0x40 -> rc=0x%08x\n", query(cfg, mem));

    memset(cfg, 0, sizeof cfg);
    memset(mem, 0, sizeof mem);
    *(uint64_t *)cfg = 0x44;
    *(uint64_t *)mem = MEMINFO_SIZE;
    LOG("  cfg.size=0x44 -> rc=0x%08x\n", query(cfg, mem));

    /* -- 3. the full configuration, every field read from the validator ----
     *
     * The blind hill-climb this program started with found two fields and then
     * went wrong, because it assumed a larger error code meant more progress.
     * It does not: 0x811D020B is a REJECTION of a non-NULL pointer at +0x40,
     * while the path that gets furthest returns the numerically SMALLER
     * 0x811D0205. Reading the validator settled in minutes what the search
     * could not settle at all.
     *
     * SceVideodec2DecoderConfigInfo, as the module itself checks it
     * (libSceVideodec2 +0x3eb0 and +0x44f7 onwards):
     *
     *   +0x00  u64  structure size: 0x48 or 0x50, else 0x811D0101
     *   +0x08  u32  codec type: 1, 0xb6c8, 0x12384, 0x24708, 0x24709
     *                                             else 0x811D0203
     *   +0x0c  u32  1, 0xee049 or 0x245bfd        else 0x811D0205
     *   +0x10  u32  H.264 profile_idc: 66, 77 or 100 (Baseline/Main/High)
     *   +0x14  u32  H.264 level_idc: 10..111, used as a jump-table index
     *   +0x18  u64  copied out wholesale - frame dimensions
     *   +0x20  u32  must be <= 16              else 0x811D0209  (DPB count)
     *   +0x24  u32  must be in 1..8            else 0x811D0206
     *   +0x38  u32  256..767, or 0xffffffff    else 0x811D0208
     *                 - the SCE thread-priority range, exactly as in
     *                   sceAvPlayerInit's own priority handling
     *   +0x3c  u8   flag, inverted into the output
     *   +0x3e  u8   must be 0                  else 0x811D0200
     *   +0x3f  u8   must be 0                  else 0x811D0200
     *   +0x40  u64  optional extra-config pointer; must be NULL on this path,
     *                                          else 0x811D020B
     */
    int accepted_n = 0;

    LOG("\n--- full configuration ---\n");
    {
        static const struct {
            const char *label;
            uint32_t codec, f0c, profile, level, width, height, dpb, f24, prio;
        } kConfigs[] = {
            /* The two codec types the query accepts, across resolutions.
             * Whether the numbers scale with resolution is what says they are
             * really computed rather than read from a fixed table. */
            { "0xb6c8  720p",      0xb6c8,  1, 100, 51, 1280,  720, 16, 4, 700 },
            { "0xb6c8  1080p",     0xb6c8,  1, 100, 51, 1920, 1080, 16, 4, 700 },
            { "0xb6c8  4K",        0xb6c8,  1, 100, 51, 3840, 2160, 16, 4, 700 },
            { "0xb6c8  4K dpb4",   0xb6c8,  1, 100, 51, 3840, 2160,  4, 4, 700 },
            { "0x12384 720p",      0x12384, 1, 100, 51, 1280,  720, 16, 4, 700 },
            { "0x12384 1080p",     0x12384, 1, 100, 51, 1920, 1080, 16, 4, 700 },
            { "0x12384 4K",        0x12384, 1, 100, 51, 3840, 2160, 16, 4, 700 },
            { "0x12384 4K dpb4",   0x12384, 1, 100, 51, 3840, 2160,  4, 4, 700 },
            /* +0x0c selects a variant. 0xee049 takes the branch that inspects
             * +0x1c with an alignment test, which is HEVC-shaped. If any of
             * these fails with something other than a validation error, that
             * is the entitlement gate the AvPlayer strings describe. */
            { "0xb6c8 v=ee049",    0xb6c8,  0xee049,  100, 51, 1920, 1080, 16, 4, 700 },
            { "0xb6c8 v=245bfd",   0xb6c8,  0x245bfd, 100, 51, 1920, 1080, 16, 4, 700 },
            { "0x12384 v=ee049",   0x12384, 0xee049,  100, 51, 1920, 1080, 16, 4, 700 },
            { "0x24708 v=ee049",   0x24708, 0xee049,  100, 51, 1920, 1080, 16, 4, 700 },
            { "0x24709 v=ee049",   0x24709, 0xee049,  100, 51, 1920, 1080, 16, 4, 700 },
            { "0x24708 v=245bfd",  0x24708, 0x245bfd, 100, 51, 1920, 1080, 16, 4, 700 },
            { "0x24708 v=ee049 4K",0x24708, 0xee049,  100, 51, 3840, 2160, 16, 4, 700 },
            /* Kept as the control: codec type 1 validated profile and level
             * but always ends at 0x811D0200, so it is not a usable type. */
            { "codec 1 (control)", 1, 1, 100, 51, 1920, 1080, 16, 4, 700 },
        };

        for (size_t i = 0; i < sizeof kConfigs / sizeof *kConfigs; i++) {
            memset(cfg, 0, sizeof cfg);
            memset(mem, 0, sizeof mem);
            *(uint64_t *)cfg = CFG_SIZE_A;
            *(uint64_t *)mem = MEMINFO_SIZE;
            *(uint32_t *)(cfg + 0x08) = kConfigs[i].codec;
            *(uint32_t *)(cfg + 0x0c) = kConfigs[i].f0c;
            *(uint32_t *)(cfg + 0x10) = kConfigs[i].profile;
            *(uint32_t *)(cfg + 0x14) = kConfigs[i].level;
            *(uint32_t *)(cfg + 0x18) = kConfigs[i].width;
            *(uint32_t *)(cfg + 0x1c) = kConfigs[i].height;
            *(uint32_t *)(cfg + 0x20) = kConfigs[i].dpb;
            *(uint32_t *)(cfg + 0x24) = kConfigs[i].f24;
            *(uint32_t *)(cfg + 0x38) = kConfigs[i].prio;

            int rc = query(cfg, mem);
            if (rc == 0) {
                uint64_t a, b, c;

                accepted_n++;
                memcpy(&a, mem + 0x08, sizeof a);
                memcpy(&b, mem + 0x18, sizeof b);
                memcpy(&c, mem + 0x38, sizeof c);
                LOG("  %-20s OK  a=%-9llu b=%-10llu c=%-9llu  total %.1f MiB\n",
                    kConfigs[i].label,
                    (unsigned long long)a, (unsigned long long)b,
                    (unsigned long long)c,
                    (double)(a + b + c) / (1024.0 * 1024.0));
            } else {
                LOG("  %-20s rc=0x%08x\n", kConfigs[i].label, rc);
            }
        }
        LOG("  %d configuration(s) accepted\n", accepted_n);
    }


    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);

    evo_notify("EVO videodec2_test: %d config(s) accepted", accepted_n);
    return EXIT_SUCCESS;
}
