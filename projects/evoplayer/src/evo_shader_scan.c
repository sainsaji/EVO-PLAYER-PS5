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

/* PS5 SceKernelModuleInfoEx (libkernel). Fields past segmentInfo we don't use.
 * st_size must be set to sizeof(*info) before the call, like the PS4 API. */
typedef struct { uint64_t addr; uint32_t size; int32_t prot; } sc_seg_t;
typedef struct {
    uint64_t st_size;                    /* 0x000 */
    char     name[256];                  /* 0x008 */
    int32_t  id;                         /* 0x108 */
    uint32_t tls_index;                  /* 0x10c */
    uint64_t tls_init_addr;              /* 0x110 */
    uint32_t tls_init_size, tls_size, tls_offset, tls_align;  /* 0x118 */
    uint64_t init_proc, fini_proc, eh_hdr, eh_frame;          /* 0x128 */
    uint32_t eh_hdr_size, eh_frame_size;                      /* 0x148 */
    sc_seg_t seg[4];                     /* 0x150 */
    uint32_t seg_count;                  /* 0x190 */
    uint32_t ref_count;                  /* 0x194 */
} sc_modinfo_t;                          /* 0x198 */

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
    static sc_modinfo_t mi;
    memset(&mi, 0, sizeof mi);
    mi.st_size = sizeof mi;
    int rc = sceKernelGetModuleInfoFromAddr(anchor, 1, &mi);
    if (rc != 0 || mi.seg_count == 0 || mi.seg_count > 4) {
        evo_boot_log("shader_scan: %s GetModuleInfoFromAddr(%p) rc=0x%x segs=%u",
                     tag, anchor, (unsigned)rc, mi.seg_count);
        evo_boot_log_flush();
        return;
    }
    mi.name[sizeof mi.name - 1] = 0;
    evo_boot_log("shader_scan: %s = '%s' id=%d segs=%u", tag, mi.name, mi.id, mi.seg_count);
    evo_boot_log_flush();
    /* if the anchor was an unbound GOT trampoline, GetModuleInfoFromAddr
     * returns EVO's own module - skip (nothing new to find, our own blobs). */
    if (strncmp(mi.name, "libSce", 6) != 0 && strncmp(mi.name, "libkernel", 9) != 0) {
        evo_boot_log("shader_scan: %s not a system module, skipping", tag);
        evo_boot_log_flush();
        return;
    }

    int hdrs = 0, feet = 0;
    for (uint32_t s = 0; s < mi.seg_count; s++) {
        if (!mi.seg[s].addr || !mi.seg[s].size || mi.seg[s].size > 0x4000000u)
            continue;
        evo_boot_log("shader_scan:   %s seg[%u] %llx +%x prot=%x",
                     tag, s, (unsigned long long)mi.seg[s].addr,
                     mi.seg[s].size, mi.seg[s].prot);
        scan_region(tag, (const uint8_t *)(uintptr_t)mi.seg[s].addr,
                    mi.seg[s].size, &hdrs, &feet);
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

    /* force-bind the lazy PRX imports so &fn is the real module address */
    (void)sceVideoOutSubmitFlip(-1, 0, 0, 0);
    { sc_modinfo_t d; (void)sceKernelGetModuleInfoFromAddr((void *)&sceKernelGetModuleInfoFromAddr, 1, &d); }

    scan_module("videoout", (const void *)&sceVideoOutSubmitFlip);
    scan_module("agc",      (const void *)&sceAgcCreateShader);
    scan_module("agcdrv",   (const void *)&sceAgcDriverSubmitDcb);
    scan_module("libc",     (const void *)&sceKernelGetModuleInfoFromAddr);

    evo_boot_log("shader_scan: DONE - %d files", g_dump_n);
    evo_boot_log_flush();
}

#endif /* EVO_SHADER_SCAN */
