/* EVO Player - kdump
 *
 * Dump the running PS5 kernel to /mnt/usb0, once.
 *
 * ---------------------------------------------------------------------------
 * WHY
 *
 * Every layer of this project has fallen to the same method: dump the code,
 * disassemble it offline, read the answer instead of guessing it. That method
 * stopped at the kernel boundary only because nobody had dumped the kernel.
 *
 * There is no reason for that. The payload SDK exports KERNEL_ADDRESS_TEXT_BASE
 * already resolved for this firmware, and kernel_copyout is an arbitrary
 * kernel read that findings.md section 2 records as working and unable to
 * fault the caller. So the kernel can simply be read out.
 *
 * FIRMWARE DECRYPTION IS NOT NEEDED AND WOULD BE WORSE. This gets the running,
 * relocated image with the real addresses in it - a PUP dump would need
 * decrypting and would still have to be rebased.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT IS FOR
 *
 * Three questions, in the order they are likely to matter:
 *
 * 1. A CHEAPER cr_ref PRIMITIVE FOR p2jb. The jailbreak's 50-minute wait is
 *    2^32 kqueueex syscalls, one credential reference each. If any syscall
 *    takes MORE than one reference per call, the required count divides by
 *    that factor - a path that holds four would turn 50 minutes into 12. That
 *    is a search for crhold-like sequences in kernel text, and once the text
 *    is on the PC it costs nothing to look.
 *
 * 2. What kstuff patches, and what changes when it is not running.
 *
 * 3. Where driver errno 5200 (0x1450) comes from, if the hardware-decode work
 *    is ever picked up again. The decode path is otherwise fully understood;
 *    the refusal is kernel-side. Scanning text for the immediate is the first
 *    step, which is why this reports those hits.
 *
 * ---------------------------------------------------------------------------
 * SIZE, AND WHY IT PROBES FIRST
 *
 * The extent of kernel text is not published anywhere we trust, and dumping a
 * guessed 64 MiB would waste minutes of USB writes for mostly nothing. So this
 * walks the range in PROBE_STEP chunks first, reading one word from each, and
 * only dumps as far as the reads actually succeed. The manifest records the
 * base address so tools/re/disas.sh can be pointed at the result with the real
 * virtual addresses intact.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "evo_ps5.h"

#define USB_DIR       "/mnt/usb0"
#define LOG_PATH      USB_DIR "/evo_kdump_log.txt"
#define TEXT_PATH     USB_DIR "/evo_kdump_text.bin"
#define MANIFEST_PATH USB_DIR "/evo_kdump_manifest.txt"

/* One word per step while probing; a page is plenty of granularity. */
#define PROBE_STEP   0x100000ull      /* 1 MiB                              */
#define PROBE_MAX    0x08000000ull    /* stop looking after 128 MiB         */
#define CHUNK        0x10000ull       /* 64 KiB per copyout while dumping   */

/* The 32-bit immediate the decoder driver's refusal carries, little-endian.
 * Reported as a byte offset from the text base so it can be disassembled. */
#define ERRNO_5200   0x1450u

static FILE *g_log;

static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
LOG(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);          /* rule 4: every line, every time */
    }
}

int
main(void)
{
    static uint8_t buf[CHUNK];
    intptr_t  base;
    uint64_t  span = 0, written = 0;
    uint64_t  hits5200 = 0;
    pid_t     pid = getpid();
    FILE     *out, *man;
    uint64_t  authid;
    uint8_t   caps[16] = {0};

    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player kdump ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", pid);

    /* -- who are we ------------------------------------------------------- *
     * Free, and the one measurement that would matter if the hardware-decode
     * work is ever resumed: if the uvd_dec driver refuses on identity rather
     * than on the job, this is the identity it is refusing. Authority is a
     * property of the host process, so a deploy.sh run measures SceSpZeroConf,
     * NOT the app slot where the player runs (standing rule 12). */
    authid = kernel_get_ucred_authid(pid);
    LOG("\n--- this process ---\n");
    LOG("  authid : 0x%016llx\n", (unsigned long long)authid);
    if (kernel_get_ucred_caps(pid, caps) == 0) {
        LOG("  caps   : ");
        for (int i = 0; i < 16; i++)
            LOG("%02x", caps[i]);
        LOG("\n");
    } else {
        LOG("  caps   : kernel_get_ucred_caps failed\n");
    }
    LOG("  NOTE: deploy.sh lands in SceSpZeroConf. The app slot may differ -\n"
        "        rule 12. Run both ways before drawing conclusions.\n");

    base = KERNEL_ADDRESS_TEXT_BASE;
    LOG("\n--- kernel text ---\n");
    LOG("  KERNEL_ADDRESS_TEXT_BASE = 0x%lx\n", (unsigned long)base);
    LOG("  KERNEL_ADDRESS_DATA_BASE = 0x%lx\n",
        (unsigned long)KERNEL_ADDRESS_DATA_BASE);

    /* -- probe: how far does the mapping actually go? --------------------- */
    LOG("\n  probing in %llu MiB steps, up to %llu MiB ...\n",
        (unsigned long long)(PROBE_STEP >> 20),
        (unsigned long long)(PROBE_MAX >> 20));
    {
        uint64_t off;
        uint64_t probe;

        for (off = 0; off < PROBE_MAX; off += PROBE_STEP) {
            if (kernel_copyout(base + (intptr_t)off, &probe, sizeof probe) != 0)
                break;
            span = off + PROBE_STEP;
        }
    }
    if (!span) {
        LOG("  the very first read failed. Either kernel R/W is not available\n"
            "  in this process, or the text base is wrong for this firmware.\n"
            "  Nothing further to do.\n");
        goto done;
    }
    LOG("  reads succeed for at least %llu MiB\n",
        (unsigned long long)(span >> 20));

    /* -- dump ------------------------------------------------------------- */
    out = fopen(TEXT_PATH, "wb");
    if (!out) {
        LOG("  cannot open %s for writing\n", TEXT_PATH);
        goto done;
    }

    LOG("\n  dumping to %s ...\n", TEXT_PATH);
    for (uint64_t off = 0; off < span; off += CHUNK) {
        size_t n = (size_t)((span - off) < CHUNK ? (span - off) : CHUNK);

        if (kernel_copyout(base + (intptr_t)off, buf, n) != 0) {
            LOG("  read failed at +0x%llx - stopping here\n",
                (unsigned long long)off);
            break;
        }
        if (fwrite(buf, 1, n, out) != n) {
            LOG("  short write at +0x%llx - stopping here\n",
                (unsigned long long)off);
            break;
        }
        written += n;

        /* Count the 5200 immediate while the bytes are in hand - free, and it
         * is the first step of the errno question if that work resumes. */
        for (size_t i = 0; i + 4 <= n; i++) {
            uint32_t v;
            memcpy(&v, buf + i, sizeof v);
            if (v == ERRNO_5200) {
                if (hits5200 < 24)
                    LOG("    0x1450 immediate at text+0x%llx\n",
                        (unsigned long long)(off + i));
                hits5200++;
            }
        }

        if ((off & 0x3FFFFF) == 0 && off)
            LOG("    %llu MiB ...\n", (unsigned long long)(off >> 20));
    }
    fclose(out);

    LOG("\n  wrote %llu bytes (%.1f MiB)\n",
        (unsigned long long)written, (double)written / (1024.0 * 1024.0));
    LOG("  0x1450 immediates found: %llu%s\n",
        (unsigned long long)hits5200,
        hits5200 > 24 ? " (first 24 listed)" : "");

    /* -- manifest, so disas.sh can use the real addresses ------------------ */
    man = fopen(MANIFEST_PATH, "w");
    if (man) {
        fprintf(man, "kernel\ttext\tvaddr=0x%lx\tsize=0x%llx\tfile=%s\n",
                (unsigned long)base, (unsigned long long)written,
                "evo_kdump_text.bin");
        fprintf(man, "kernel\tdata\tvaddr=0x%lx\n",
                (unsigned long)KERNEL_ADDRESS_DATA_BASE);
        fprintf(man, "fw\t0x%08x\n", kernel_get_fw_version());
        fclose(man);
        LOG("  manifest -> %s\n", MANIFEST_PATH);
    }

    LOG("\n  Retrieve over FTP (2121) or websrv /fs, then disassemble offline:\n"
        "    objdump -D -b binary -m i386:x86-64 -M intel \\\n"
        "            --adjust-vma=0x%lx evo_kdump_text.bin\n",
        (unsigned long)base);

done:
    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);
    evo_notify("EVO kdump: %.1f MiB of kernel text",
               (double)written / (1024.0 * 1024.0));
    return 0;
}
