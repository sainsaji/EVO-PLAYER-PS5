/*
 * evo_shader_scan.c - rip PSSL-compiled shader blobs out of EVO's own loaded
 * system modules (#67 / #28: we need a working RGBA-sampling pixel shader, and
 * the sl00 / "barefoot" reflection trailer that sceAgcCreateShader validates
 * only exists in real PSSL-compiler output).
 *
 * Built only under -DEVO_SHADER_SCAN (scripts/package-app.sh --shader-scan).
 * Runs after the self-unjail (needs /mnt/usb0 writable). NO kernel R/W - the
 * self-unjail is a filesystem-caps bump only, so this can only see modules
 * mapped into EVO's own address space: libSceVideoOut / libSceAgc /
 * libSceAgcDriver / libkernel. Those are decrypted in process memory.
 * Cross-process (AgcCompositor, SceShellUI) is out of reach without an ELF
 * payload.
 *
 * A PS5 shader blob is a "1234"-magic header (>=0x180 B, [0x44] = code size)
 * plus a separate [ GCN code | "sl00" reflection | "barefoot" footer ]. The
 * header [0xb0] and the barefoot footer share a 4-byte link id.
 *
 * Method: -fno-plt makes `&imported_fn` a GOT load = the real resolved address,
 * so from one anchor per module we walk back page-aligned to the module's ELF
 * header, read its PT_LOAD extents, and scan that span for "1234" and
 * "barefoot". Every hit is dumped verbatim to /mnt/usb0/evo_shaders/ for
 * offline disassembly + matching.
 */
#ifdef EVO_SHADER_SCAN

#include "evo_boot_log.h"
#include "evo_data_path.h"   /* evo_mkdir - libc mkdir() isn't in the shim surface */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>

/* anchors - one actually-called import per module we can reach. -fno-plt +
 * eager bind => &fn is the real module address. */
extern int32_t sceAgcCreateShader(void **out, const void *header, const void *code);
extern int32_t sceAgcDriverSubmitDcb(void *desc);
extern int     sceVideoOutSubmitFlip(int h, int idx, uint32_t mode, int64_t arg);
extern int     sceKernelGetModuleInfoFromAddr(const void *addr, int flags, void *info);

#define PAGE   0x4000u
#define OUTDIR "/mnt/usb0/evo_shaders"

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_armed;

static void seg_h(int sig)
{
    if (g_armed) { g_armed = 0; siglongjmp(g_jmp, 1); }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* copy [src,src+n) -> dst, page by page; returns bytes actually copied (stops
 * at the first unreadable page). One sigsetjmp per page, not per byte. */
static size_t guarded_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t page_off = ((uintptr_t)(src + done)) & (PAGE - 1);
        size_t chunk = PAGE - page_off;
        if (chunk > n - done) chunk = n - done;
        if (sigsetjmp(g_jmp, 1) != 0) break;      /* faulted -> stop here */
        g_armed = 1;
        memcpy(dst + done, src + done, chunk);
        g_armed = 0;
        done += chunk;
    }
    return done;
}

static int page_readable(const uint8_t *p)
{
    if (sigsetjmp(g_jmp, 1) != 0) return 0;
    g_armed = 1;
    volatile uint8_t v = *p; (void)v;
    g_armed = 0;
    return 1;
}

static const uint8_t *find_elf_base(const void *anchor)
{
    uintptr_t a = (uintptr_t)anchor & ~(uintptr_t)(PAGE - 1);
    for (int i = 0; i < 8192; i++) {              /* up to 128 MiB back */
        const uint8_t *p = (const uint8_t *)(a - (uintptr_t)i * PAGE);
        if ((uintptr_t)p < PAGE) break;
        if (!page_readable(p)) continue;
        uint8_t h[4];
        if (guarded_copy(h, p, 4) == 4 &&
            h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F')
            return p;
    }
    return NULL;
}

static size_t elf_span(const uint8_t *base)
{
    uint8_t eh[64];
    if (guarded_copy(eh, base, sizeof eh) != sizeof eh) return 0;
    uint64_t phoff = 0;
    for (int i = 0; i < 8; i++) phoff |= (uint64_t)eh[0x20 + i] << (8 * i);
    uint16_t phentsize = (uint16_t)(eh[0x36] | (eh[0x37] << 8));
    uint16_t phnum     = (uint16_t)(eh[0x38] | (eh[0x39] << 8));
    if (!phentsize || phentsize > 128 || phnum == 0 || phnum > 64) return 0;

    uint64_t hi = 0;
    uint8_t ph[128];
    for (int i = 0; i < phnum; i++) {
        if (guarded_copy(ph, base + phoff + (uint64_t)i * phentsize, phentsize) != phentsize)
            break;
        uint32_t type = ph[0] | (ph[1] << 8) | (ph[2] << 16) | ((uint32_t)ph[3] << 24);
        if (type != 1) continue;                 /* PT_LOAD */
        uint64_t vaddr = 0, memsz = 0;
        for (int b = 0; b < 8; b++) vaddr |= (uint64_t)ph[0x10 + b] << (8 * b);
        for (int b = 0; b < 8; b++) memsz |= (uint64_t)ph[0x28 + b] << (8 * b);
        if (vaddr + memsz > hi) hi = vaddr + memsz;
    }
    return (size_t)hi;
}

static int g_dump_n;

static void dump(const char *tag, const uint8_t *p, size_t n)
{
    static uint8_t buf[0x8000];
    if (n > sizeof buf) n = sizeof buf;
    size_t got = guarded_copy(buf, p, n);
    if (got < 64) return;
    char path[160];
    snprintf(path, sizeof path, OUTDIR "/%03d_%s.bin", g_dump_n++, tag);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { g_dump_n--; return; }
    (void)!write(fd, buf, got);
    close(fd);
}

static void scan_module(const char *tag, const void *anchor)
{
    const uint8_t *base = find_elf_base(anchor);
    if (!base) { evo_boot_log("shader_scan: %s - no ELF near %p", tag, anchor);
                 evo_boot_log_flush(); return; }
    size_t span = elf_span(base);
    if (!span || span > 0x4000000u) span = 0x1800000u;   /* cap 24 MiB */
    evo_boot_log("shader_scan: %s base=%p span=%zuKB", tag, base, span >> 10);
    evo_boot_log_flush();

    int hdrs = 0, feet = 0;
    static uint8_t pg[PAGE + 16];
    for (size_t off = 0; off < span; off += PAGE) {
        const uint8_t *pp = base + off;
        if (!page_readable(pp)) continue;
        size_t want = PAGE + 16;
        if (off + want > span) want = span - off;
        if (guarded_copy(pg, pp, want) < 16) continue;
        for (size_t j = 0; j + 16 <= want; j++) {
            const uint8_t *b = pg + j;
            if (b[0] == '1' && b[1] == '2' && b[2] == '3' && b[3] == '4') {
                uint32_t csz = b[0x44] | (b[0x45] << 8) | (b[0x46] << 16) | ((uint32_t)b[0x47] << 24);
                if (csz >= 0x100 && csz <= 0x20000 && !(csz & 3)) {
                    char t[48]; snprintf(t, sizeof t, "%s_hdr_c0x%x", tag, csz);
                    dump(t, pp + j, 0x200);
                    evo_boot_log("shader_scan:   %s hdr @+0x%zx code=0x%x link=%02x%02x%02x%02x",
                                 tag, off + j, csz, b[0xb3], b[0xb2], b[0xb1], b[0xb0]);
                    hdrs++;
                }
            }
            if (!memcmp(b, "barefoot", 8)) {
                size_t abs = off + j;
                size_t back = abs > 0x3000 ? 0x3000 : abs;
                char t[32]; snprintf(t, sizeof t, "%s_code", tag);
                dump(t, base + abs - back, back + 0x40);
                evo_boot_log("shader_scan:   %s barefoot @+0x%zx link=%02x%02x%02x%02x",
                             tag, abs, b[11], b[10], b[9], b[8]);
                feet++;
            }
        }
    }
    evo_boot_log("shader_scan: %s done - %d hdr, %d code", tag, hdrs, feet);
    evo_boot_log_flush();
}

void evo_shader_scan(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = seg_h;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    evo_mkdir(OUTDIR);
    evo_boot_log("shader_scan: START -> " OUTDIR);
    evo_boot_log_flush();

    scan_module("videoout", (const void *)&sceVideoOutSubmitFlip);
    scan_module("agc",      (const void *)&sceAgcCreateShader);
    scan_module("agcdrv",   (const void *)&sceAgcDriverSubmitDcb);
    scan_module("libc",     (const void *)&sceKernelGetModuleInfoFromAddr);

    evo_boot_log("shader_scan: DONE - %d files", g_dump_n);
    evo_boot_log_flush();
}

#endif /* EVO_SHADER_SCAN */
