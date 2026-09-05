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

#define OUTDIR "/mnt/usb0/evo_shaders"

/* The scan body below KP'd the console (2026-09-04) and is kept only as a
 * record of the approach - #if 0'd out. The working textured pixel shader
 * (pp/blobs/blit_ps.*) was ripped OFFLINE from a decrypted game dump instead. */
#if 0
#define PAGE   0x4000u

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

/* scan one [base,base+len) region for shader blobs */
static void scan_region(const char *tag, const uint8_t *base, size_t len,
                        int *hdrs, int *feet)
{
    static uint8_t pg[PAGE + 16];
    for (size_t off = 0; off < len; off += PAGE) {
        const uint8_t *pp = base + off;
        if (!page_readable(pp)) continue;
        size_t want = PAGE + 16;
        if (off + want > len) want = len - off;
        if (guarded_copy(pg, pp, want) < 16) continue;
        for (size_t j = 0; j + 16 <= want; j++) {
            const uint8_t *b = pg + j;
            if (b[0] == '1' && b[1] == '2' && b[2] == '3' && b[3] == '4') {
                uint32_t csz = b[0x44] | (b[0x45] << 8) | (b[0x46] << 16) | ((uint32_t)b[0x47] << 24);
                if (csz >= 0x100 && csz <= 0x20000 && !(csz & 3)) {
                    char t[48]; snprintf(t, sizeof t, "%s_hdr_c0x%x", tag, csz);
                    dump(t, pp + j, 0x200);
                    evo_boot_log("shader_scan:   %s hdr @%p code=0x%x link=%02x%02x%02x%02x",
                                 tag, (void *)(pp + j), csz, b[0xb3], b[0xb2], b[0xb1], b[0xb0]);
                    (*hdrs)++;
                }
            }
            if (!memcmp(b, "barefoot", 8)) {
                const uint8_t *abs = pp + j;
                size_t back = (abs - base) > 0x3000 ? 0x3000 : (size_t)(abs - base);
                char t[32]; snprintf(t, sizeof t, "%s_code", tag);
                dump(t, abs - back, back + 0x40);
                evo_boot_log("shader_scan:   %s barefoot @%p link=%02x%02x%02x%02x",
                             tag, (void *)abs, b[11], b[10], b[9], b[8]);
                (*feet)++;
            }
        }
    }
}

static void scan_module(const char *tag, const void *anchor)
{
    static uint64_t raw[64];   /* 512 B - enough for the whole ModuleInfoEx */
    memset(raw, 0, sizeof raw);
    raw[0] = sizeof raw;       /* st_size */
    int rc = sceKernelGetModuleInfoFromAddr(anchor, 1, raw);

    /* dump the struct so we can pin the real layout (name @ 0x08 is ASCII;
     * segment addrs are 0x8000xxxxx; count is a small int nearby). */
    const uint8_t *r = (const uint8_t *)raw;
    evo_boot_log("shader_scan: %s rc=0x%x name='%.40s'", tag, (unsigned)rc, r + 8);
    for (int q = 0x100; q < 0x1c0; q += 0x20)
        evo_boot_log("  +%03x: %016llx %016llx %016llx %016llx", q,
                     (unsigned long long)*(uint64_t *)(r + q),
                     (unsigned long long)*(uint64_t *)(r + q + 8),
                     (unsigned long long)*(uint64_t *)(r + q + 16),
                     (unsigned long long)*(uint64_t *)(r + q + 24));
    evo_boot_log_flush();

    /* heuristic scan: walk 0x108..0x1a0 for (addr in 0x800000000..0x900000000,
     * size 0x1000..0x4000000) triples and scan each. */
    int hdrs = 0, feet = 0;
    for (int q = 0x100; q < 0x1a0; q += 8) {
        uint64_t a = *(uint64_t *)(r + q);
        uint32_t sz = *(uint32_t *)(r + q + 8);
        if (a >= 0x800000000ull && a < 0x900000000ull &&
            sz >= 0x1000 && sz <= 0x4000000u) {
            evo_boot_log("shader_scan:   %s cand seg @%llx +%x", tag,
                         (unsigned long long)a, sz);
            scan_region(tag, (const uint8_t *)(uintptr_t)a, sz, &hdrs, &feet);
        }
    }
    evo_boot_log("shader_scan: %s done - %d hdr, %d code", tag, hdrs, feet);
    evo_boot_log_flush();
}

#endif /* 0 - disabled scan body */

void evo_shader_scan(void)
{
    /* DISABLED - the in-process module scan KP'd the console (2026-09-04).
     * The working textured pixel shader (pp/blobs/blit_ps.*) was ripped OFFLINE
     * from a decrypted game dump instead (commit be90091). Kept flag-gated so a
     * stray --shader-scan build can't brick a console. */
    evo_boot_log("shader_scan: DISABLED (KP risk) - see pp/blobs/blit_ps");
    evo_boot_log_flush();
    (void)sceAgcCreateShader; (void)sceAgcDriverSubmitDcb;
    (void)sceVideoOutSubmitFlip; (void)sceKernelGetModuleInfoFromAddr;
}

#endif /* EVO_SHADER_SCAN */
