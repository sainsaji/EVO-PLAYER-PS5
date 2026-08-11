/* EVO Player - codecdump_test
 *
 * PHASE 6, the impasse-breaker: dump the codec module that CreateDecoder loads.
 *
 * RUN IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
 * The 1080p working set is 90 MiB and the elfldr slot caps out below 64.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 *
 * decodeframe_test walked sceVideodec2Decode through four successive gates on
 * 2026-08-11, clearing each one by reading the module that refused it:
 *
 *     GpDec state 0            -> call MapDirectMemory
 *     pin ioctl errno 5031     -> round the size up to 16 KiB
 *     overlap, status 0x33     -> give each block its real physical offset
 *     containment, line 0xDA6  -> register the AU, the work memory and the pool
 *
 * The fifth refusal is the hardware submit itself, driver errno 5200, and the
 * same technique stalled on it. Three facts say why:
 *
 *   1. `mov ecx,0x254` - the line number in [VDECCORE@D0A10254:00001450] -
 *      occurs NOWHERE in libSceVdecCore's text. That is a raw byte-pattern
 *      search over the segment, not a grep of a disassembly listing, so it is
 *      immune to the desync a linear sweep of 512 KB suffers. All 44 other
 *      0xD0A1 diagnostic sites have their line numbers present.
 *
 *   2. The driver-interface vtables are not in the dump either. The submit is
 *      a virtual call [[gpdec+0x38]] + 0x18 and the map call goes through
 *      [[obj+0xb0]] + 0x00; none of those implementation addresses appears as
 *      a pointer in any dumped segment of any module.
 *
 *   3. sceVdecCoreCreateDecoder loads a codec module ON DEMAND through
 *      sceSysmoduleLoadModuleInternal, dispatching on the codec index:
 *
 *          codec index 0 (H.264)  ->  internal id 0x80000036
 *          codec index 4 (HEVC)   ->  internal id 0x8000003C
 *          a resource-class path  ->  internal id 0x80000035
 *
 *      with 0x805A1001 treated as "already loaded" at each site.
 *
 * So an H.264 CreateDecoder maps a module we have never enumerated, and THAT
 * module owns the submit, the vtables and the failing ioctl. findings.md's
 * "loading the media modules pulls in no new modules at all" was measured
 * before any decoder existed and does not describe the process afterwards.
 *
 * This probe therefore does the thing that has broken every previous impasse
 * in this effort: ONE DEPLOY THAT DUMPS, rather than N that guess.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT DOES, AND WHAT IT DELIBERATELY DOES NOT
 *
 *   1. Sweep modids 0..0xFF and record the module table. This is the baseline
 *      and it is also the control: sceKernelGetModuleList is useless here and
 *      the sweep through sceKernelGetModuleInfo is the only thing that
 *      enumerates modules at all.
 *   2. Create an H.264 decoder, exactly as Phase 5 proved and decodeframe_test
 *      repeated. Nothing here is new or risky.
 *   3. Sweep again and diff. Anything that appeared is what CreateDecoder
 *      loaded.
 *   4. Dump every new module's segments to /mnt/usb0, with a manifest, so the
 *      submit path becomes an offline objdump question like every gate before
 *      it.
 *   5. Delete the decoder and release everything.
 *
 * IT NEVER CALLS Decode. That is the whole point: the decode is the only call
 * in this chain that reaches the hardware and can block, and none of the
 * information this probe wants requires it. A create-and-enumerate run carries
 * the risk profile of Phase 5, which is to say almost none.
 *
 * It also dumps the HEVC codec module when asked (`--args "eboot.elf hevc"`),
 * because that is one more create on an already-open decoder path and it costs
 * nothing extra - but it is opt-in, since the HEVC create is the one that
 * reaches a module load on a path Phase 5 flagged as the hang candidate.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 *   - Never speculatively dereference module memory. Every segment read is
 *     probed with kernel_proc_copyout first, which goes through the kernel and
 *     cannot fault the caller. Only when the segment's own declared prot says
 *     the CPU may read it, AND a kernel-side read of the same bytes has already
 *     succeeded, is a direct memcpy used.
 *   - Execute-only text gets PROT_READ added with kernel_mprotect, then the
 *     same two-step proof, exactly as the Phase 0 dump did.
 *   - A page that cannot be read becomes a zero-filled hole rather than a lost
 *     segment, so every file offset still maps to the right virtual address.
 *     An offline disassembler depends on that.
 *   - Log flushed after every line and written to /mnt/usb0. No watchdog
 *     thread: measured on 12.70, it does not fire.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

#define USB_DIR       "/mnt/usb0"
#define LOG_PATH      USB_DIR "/evo_codecdump_log.txt"
#define MANIFEST_PATH USB_DIR "/evo_codecdump_manifest.txt"

#define MAX_MODID  0x100
#define MAX_MODS   64
#define CHUNK      (64u * 1024u)
#define DMEM_ALIGN 0x20000u

#define DECODER_CFG_SIZE       0x48
#define DECODER_MEMINFO_SIZE   0x48
#define COMPUTE_MEMINFO_SIZE   0x18
#define COMPUTE_QUEUEINFO_SIZE 0x10
#define DECODER_OBJECT_SIZE    0x40000

#define RES_STD 0xb6c8u
#define RES_BIG 0x12384u

#define CODEC_AVC  0x1u
#define CODEC_HEVC 0xee049u

/* The internal sysmodule ids sceVdecCoreCreateDecoder loads, read off its
 * codec dispatch. Logged rather than used - the probe never calls sysmodule
 * itself, it just wants to see what the create pulled in. */
#define SYSMODULE_CODEC_AVC   0x80000036u
#define SYSMODULE_CODEC_HEVC  0x8000003Cu
#define SYSMODULE_CODEC_OTHER 0x80000035u

static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",
    "libSceSysmodule.sprx",
    "libSceAjm.sprx",
};

/* ------------------------------------------------------------------------- */
/* Structures (identical to createdecoder_test / decodeframe_test)           */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    uint32_t maxFrameWidth;
    uint32_t maxFrameHeight;
    uint32_t maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void    *computeQueue;
    uint64_t cpuAffinityMask;
    int32_t  cpuThreadPriority;
    uint8_t  optimizeProgressiveVideo;
    uint8_t  checkMemoryType;
    uint8_t  extraDecoderLatency;
    uint8_t  enableStorageIntegrity;
    void    *extraConfigInfo;
} SceVideodec2DecoderConfigInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t workMemorySize;
    void    *pWorkMemory;
    uint64_t frameMemorySize;
    void    *pFrameMemory;
    uint64_t extraMemorySize;
    void    *pExtraMemory;
    uint64_t mapMemorySize;
    uint32_t alignment;
    uint32_t reserved;
} SceVideodec2DecoderMemoryInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void    *cpuGpuMemory;
} SceVideodec2ComputeMemoryInfo;

typedef struct {
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    uint8_t  memoryCheckMode;
    uint8_t  reserved[3];
} SceVideodec2ComputeQueueInfo;

_Static_assert(sizeof(SceVideodec2DecoderConfigInfo) == DECODER_CFG_SIZE, "cfg 0x48");
_Static_assert(sizeof(SceVideodec2DecoderMemoryInfo) == DECODER_MEMINFO_SIZE, "mem 0x48");

typedef int (*query_decoder_meminfo_fn)(const SceVideodec2DecoderConfigInfo *,
                                        SceVideodec2DecoderMemoryInfo *);
typedef int (*create_decoder_fn)(const SceVideodec2DecoderConfigInfo *,
                                 SceVideodec2DecoderMemoryInfo *, void **);
typedef int (*delete_decoder_fn)(void *);
typedef int (*query_compute_meminfo_fn)(SceVideodec2ComputeMemoryInfo *);
typedef int (*alloc_compute_queue_fn)(const SceVideodec2ComputeQueueInfo *,
                                      SceVideodec2ComputeMemoryInfo *, void **);
typedef int (*release_compute_queue_fn)(void *);

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static FILE *g_log;
static FILE *g_manifest;
static size_t g_written;

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
        fflush(g_log);
    }
}

static const char *
prot_str(int prot, char *buf)
{
    buf[0] = (prot & PROT_READ)  ? 'r' : '-';
    buf[1] = (prot & PROT_WRITE) ? 'w' : '-';
    buf[2] = (prot & PROT_EXEC)  ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

static void
sanitise(const char *in, char *out, size_t outlen)
{
    size_t j = 0;

    for (size_t i = 0; in[i] && j + 1 < outlen; i++) {
        char c = in[i];

        if (c == '.' && strcmp(in + i, ".sprx") == 0)
            break;
        out[j++] = (c == '/' || c == ' ') ? '_' : c;
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Reading module memory without ever risking a fault                        */
/* ------------------------------------------------------------------------- */

enum read_method { RM_NONE = 0, RM_MEMCPY, RM_MPROTECT, RM_PROC_COPYOUT };

static const char *
read_method_name(enum read_method m)
{
    switch (m) {
    case RM_MEMCPY:       return "memcpy";
    case RM_MPROTECT:     return "kernel_mprotect+memcpy";
    case RM_PROC_COPYOUT: return "kernel_proc_copyout";
    default:              return "none";
    }
}

/* kernel_proc_copyout goes through the kernel: it can read a page this process
 * may not, and it cannot kill us if the address is wrong. It is both the probe
 * and the fallback. A direct memcpy is used only where the segment's declared
 * prot says the CPU may read AND a kernel-side read of the same bytes has
 * already succeeded. */
static enum read_method
pick_read_method(intptr_t addr, size_t len, int prot)
{
    uint8_t probe[16];
    size_t  n = len < sizeof probe ? len : sizeof probe;
    int     kernel_ok = (kernel_proc_copyout(getpid(), addr, probe, n) == 0);

    if ((prot & PROT_READ) && kernel_ok)
        return RM_MEMCPY;

    if (!(prot & PROT_READ)) {
        if (kernel_mprotect(getpid(), addr, len, prot | PROT_READ) == 0 &&
            kernel_proc_copyout(getpid(), addr, probe, n) == 0)
            return RM_MPROTECT;
    }

    return kernel_ok ? RM_PROC_COPYOUT : RM_NONE;
}

static int
read_chunk(enum read_method m, void *dst, intptr_t addr, size_t len)
{
    switch (m) {
    case RM_MEMCPY:
    case RM_MPROTECT:
        memcpy(dst, (const void *)addr, len);
        return 1;
    case RM_PROC_COPYOUT:
        return kernel_proc_copyout(getpid(), addr, dst, len) == 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Module enumeration                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    int      modid;
    char     name[256];
    unsigned nseg;
    struct { intptr_t addr; size_t size; int prot; } seg[4];
} modinfo_t;

static int
get_module_info(int modid, modinfo_t *out)
{
    /* Oversized and zeroed: if the PS5 struct is larger than the PS4 one, a
     * callee writing past 0x160 lands in slack rather than on the stack. */
    static uint8_t big[0x800];
    evo_module_info_t *info = (evo_module_info_t *)big;

    memset(big, 0, sizeof big);
    info->st_size = sizeof(evo_module_info_t);

    if (sceKernelGetModuleInfo(modid, info) != 0)
        return 0;
    if (info->name[0] == '\0')
        return 0;

    out->modid = modid;
    snprintf(out->name, sizeof out->name, "%s", info->name);
    out->nseg = info->segment_count > 4 ? 4 : info->segment_count;
    for (unsigned i = 0; i < out->nseg; i++) {
        out->seg[i].addr = (intptr_t)info->segments[i].address;
        out->seg[i].size = info->segments[i].size;
        out->seg[i].prot = info->segments[i].prot;
    }
    return 1;
}

static size_t
sweep_modules(modinfo_t *out, size_t max)
{
    size_t n = 0;

    for (int id = 0; id < MAX_MODID && n < max; id++)
        if (get_module_info(id, &out[n]))
            n++;
    return n;
}

static void
print_module_table(const char *title, const modinfo_t *m, size_t n)
{
    char pbuf[8];

    LOG("\n%s (%zu modules):\n", title, n);
    for (size_t i = 0; i < n; i++) {
        LOG("  modid=0x%-3x %-44s segs=%u\n", m[i].modid, m[i].name, m[i].nseg);
        for (unsigned s = 0; s < m[i].nseg; s++)
            LOG("        s%u 0x%012lx size=0x%-8zx prot=%s(%d)\n", s,
                (unsigned long)m[i].seg[s].addr, m[i].seg[s].size,
                prot_str(m[i].seg[s].prot, pbuf), m[i].seg[s].prot);
    }
}

static int
seen_before(const modinfo_t *base, size_t nbase, const modinfo_t *m)
{
    for (size_t i = 0; i < nbase; i++)
        if (base[i].modid == m->modid && strcmp(base[i].name, m->name) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* The dump                                                                  */
/* ------------------------------------------------------------------------- */

static size_t
dump_segment(const char *modname, int idx, intptr_t addr, size_t len, int prot,
             void *buf)
{
    char shortname[128];
    char path[256];
    char pbuf[8];

    sanitise(modname, shortname, sizeof shortname);
    snprintf(path, sizeof path, USB_DIR "/evo_codec_%s_s%d.bin", shortname, idx);

    enum read_method m = pick_read_method(addr, len, prot);
    LOG("    s%d 0x%lx size=0x%zx prot=%s(%d) read=%s\n", idx,
        (unsigned long)addr, len, prot_str(prot, pbuf), prot,
        read_method_name(m));

    if (m == RM_NONE) {
        LOG("    s%d UNREADABLE by any method - skipped\n", idx);
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("    s%d cannot open %s for writing\n", idx, path);
        return 0;
    }

    size_t done = 0, holes = 0;
    while (done < len) {
        size_t n = len - done;

        if (n > CHUNK)
            n = CHUNK;

        if (!read_chunk(m, buf, addr + (intptr_t)done, n)) {
            /* One bad page must not cost the rest of the segment. Zero-fill so
             * every file offset still maps to the right virtual address - an
             * offline disassembler depends on that. */
            memset(buf, 0, n);
            holes++;
        }
        if (fwrite(buf, 1, n, f) != n) {
            LOG("    s%d short write at 0x%zx\n", idx, done);
            break;
        }
        done += n;
    }
    fclose(f);
    g_written += done;

    LOG("    s%d wrote %zu bytes to %s%s\n", idx, done, path,
        holes ? " (WITH HOLES)" : "");

    if (g_manifest) {
        fprintf(g_manifest,
                "%s\tseg%d\tvaddr=0x%lx\tsize=0x%zx\tprot=%d\tmethod=%s\t"
                "holes=%zu\tfile=evo_codec_%s_s%d.bin\n",
                modname, idx, (unsigned long)addr, done, prot,
                read_method_name(m), holes, shortname, idx);
        fflush(g_manifest);
    }
    return done;
}

static size_t
dump_module(const modinfo_t *m, void *buf)
{
    size_t total = 0;

    LOG("\n  MODULE %s  (modid=0x%x, %u segments)\n", m->name, m->modid,
        m->nseg);

    for (unsigned i = 0; i < m->nseg; i++)
        total += dump_segment(m->name, (int)i, m->seg[i].addr, m->seg[i].size,
                              m->seg[i].prot, buf);
    return total;
}

/* ------------------------------------------------------------------------- */
/* Direct memory                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    intptr_t paddr;
    void    *vaddr;
    size_t   len;
} dmem_block;

static size_t
round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static int
dmem_get(dmem_block *b, size_t len, int memtype)
{
    intptr_t paddr = 0;
    void    *vaddr = NULL;
    int      rc;

    memset(b, 0, sizeof *b);
    len = round_up(len, DMEM_ALIGN);

    rc = sceKernelAllocateMainDirectMemory(len, DMEM_ALIGN, memtype, &paddr);
    if (rc)
        return rc;

    rc = sceKernelMapDirectMemory(&vaddr, len,
                                  SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_ALL,
                                  0, paddr, DMEM_ALIGN);
    if (rc) {
        sceKernelReleaseDirectMemory(paddr, len);
        return rc;
    }

    b->paddr = paddr;
    b->vaddr = vaddr;
    b->len   = len;
    return 0;
}

static void
dmem_put(dmem_block *b)
{
    if (b->vaddr)
        munmap(b->vaddr, b->len);
    if (b->paddr)
        sceKernelReleaseDirectMemory(b->paddr, b->len);
    memset(b, 0, sizeof *b);
}

static void
cfg_init(SceVideodec2DecoderConfigInfo *cfg, uint32_t resourceType,
         uint32_t codecType, uint32_t profile, uint32_t level,
         uint32_t w, uint32_t h, uint32_t dpb, void *queue)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->thisSize            = DECODER_CFG_SIZE;
    cfg->resourceType        = resourceType;
    cfg->codecType           = codecType;
    cfg->profile             = profile;
    cfg->maxLevel            = level;
    cfg->maxFrameWidth       = w;
    cfg->maxFrameHeight      = h;
    cfg->maxDpbFrameCount    = dpb;
    cfg->decodePipelineDepth = 4;
    cfg->computeQueue        = queue;
    cfg->cpuThreadPriority   = 700;
}

static intptr_t
resolve(uint32_t dynh, intptr_t base, const char *name, unsigned expect_off)
{
    char     nid[12] = {0};
    intptr_t addr;

    nid_encode(name, nid);
    addr = kernel_dynlib_resolve(getpid(), dynh, nid);

    if (!addr) {
        LOG("  %-38s %s  DID NOT RESOLVE\n", name, nid);
        return 0;
    }
    LOG("  %-38s %s  +0x%-6lx %s\n", name, nid,
        (unsigned long)(addr - base),
        (unsigned long)(addr - base) == expect_off ? "ok" : "*** MOVED ***");
    return addr;
}

/* ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    static modinfo_t before[MAX_MODS], after[MAX_MODS];
    size_t nbefore = 0, nafter = 0;
    int    want_hevc = 0;
    int    created = 0, newmods = 0;

    dmem_block cq_mem = {0}, work = {0}, frame = {0};
    void      *queue = NULL, *dec = NULL;
    void      *buf = NULL;

    /* Declared up here, not beside the other entry points: the cleanup at
     * `done:` needs it and several error paths jump straight there. */
    release_compute_queue_fn rel_cq = NULL;

    for (int i = 1; i < argc; i++)
        if (argv[i] && strcmp(argv[i], "hevc") == 0)
            want_hevc = 1;

    g_log      = fopen(LOG_PATH, "w");
    g_manifest = fopen(MANIFEST_PATH, "w");

    LOG("=== EVO Player codecdump_test - Phase 6 impasse-breaker ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", getpid());
    LOG("hevc     : %s\n\n",
        want_hevc ? "ENABLED (argv)" : "off - pass \"hevc\" to also dump it");

    LOG("Looking for the module sceVdecCoreCreateDecoder loads on demand.\n"
        "Read off its codec dispatch, the internal sysmodule ids are:\n"
        "  H.264 (codec index 0) -> 0x%08x\n"
        "  HEVC  (codec index 4) -> 0x%08x\n"
        "  a resource-class path -> 0x%08x\n"
        "This probe never calls sysmodule itself; it creates a decoder and\n"
        "looks at what appeared.\n",
        SYSMODULE_CODEC_AVC, SYSMODULE_CODEC_HEVC, SYSMODULE_CODEC_OTHER);

    buf = malloc(CHUNK);
    if (!buf) {
        LOG("\nFATAL: no dump buffer\n");
        return EXIT_FAILURE;
    }

    /* -- modules ---------------------------------------------------------- */
    for (size_t i = 0; i < sizeof kModules / sizeof *kModules; i++) {
        char path[256];
        int  res = 0;
        int  modid;

        snprintf(path, sizeof path, "/system/common/lib/%s", kModules[i]);
        modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        LOG("load %-38s modid=0x%x res=0x%x\n", kModules[i], modid, res);
    }

    /* -- CONTROL: the sweep works at all ---------------------------------- *
     * sceKernelGetModuleList reports one module however many are loaded, so
     * the sweep is the only enumeration that works here. If it cannot even
     * see the modules just loaded, the diff below means nothing. */
    nbefore = sweep_modules(before, MAX_MODS);
    print_module_table("BASELINE - before any decoder exists", before, nbefore);
    LOG("\n  control: %zu modules found by the modid sweep. The eight loaded\n"
        "  above must be among them, or the diff below is meaningless.\n",
        nbefore);

    /* -- entry points ----------------------------------------------------- */
    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), "libSceVideodec2.sprx", &dynh) != 0) {
        LOG("\nFATAL: no dynlib handle for libSceVideodec2.sprx\n");
        goto done;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    LOG("\nlibSceVideodec2 base 0x%lx\n", (unsigned long)base);

    intptr_t a_query    = resolve(dynh, base, "sceVideodec2QueryDecoderMemoryInfo", 0xa10);
    intptr_t a_create   = resolve(dynh, base, "sceVideodec2CreateDecoder", 0xba0);
    intptr_t a_delete   = resolve(dynh, base, "sceVideodec2DeleteDecoder", 0x3220);
    intptr_t a_query_cm = resolve(dynh, base, "sceVideodec2QueryComputeMemoryInfo", 0x550);
    intptr_t a_alloc_cq = resolve(dynh, base, "sceVideodec2AllocateComputeQueue", 0x660);
    intptr_t a_rel_cq   = resolve(dynh, base, "sceVideodec2ReleaseComputeQueue", 0x970);

    if (!a_query || !a_create || !a_delete || !a_query_cm || !a_alloc_cq ||
        !a_rel_cq) {
        LOG("\nFATAL: an entry point did not resolve\n");
        goto done;
    }

    query_decoder_meminfo_fn query    = (query_decoder_meminfo_fn)a_query;
    create_decoder_fn        create   = (create_decoder_fn)a_create;
    delete_decoder_fn        del      = (delete_decoder_fn)a_delete;
    query_compute_meminfo_fn query_cm = (query_compute_meminfo_fn)a_query_cm;
    alloc_compute_queue_fn   alloc_cq = (alloc_compute_queue_fn)a_alloc_cq;
    rel_cq = (release_compute_queue_fn)a_rel_cq;

    /* -- compute queue, exactly as Phase 4 proved it ---------------------- */
    {
        SceVideodec2ComputeMemoryInfo mi;
        SceVideodec2ComputeQueueInfo  qi;
        int rc;

        memset(&mi, 0, sizeof mi);
        mi.thisSize = COMPUTE_MEMINFO_SIZE;
        rc = query_cm(&mi);
        LOG("\nQueryComputeMemoryInfo -> 0x%08x size %llu\n", rc,
            (unsigned long long)mi.cpuGpuMemorySize);
        if (rc)
            goto done;

        rc = dmem_get(&cq_mem, (size_t)mi.cpuGpuMemorySize, SCE_KERNEL_WB_ONION);
        if (rc) {
            LOG("queue memory -> 0x%08x\n", rc);
            goto done;
        }
        memset(cq_mem.vaddr, 0, cq_mem.len);

        memset(&qi, 0, sizeof qi);
        qi.thisSize         = COMPUTE_QUEUEINFO_SIZE;
        mi.cpuGpuMemory     = cq_mem.vaddr;

        rc = alloc_cq(&qi, &mi, &queue);
        LOG("AllocateComputeQueue -> 0x%08x handle %p\n", rc, queue);
        if (rc || !queue)
            goto done;
    }

    /* -- create the decoder ----------------------------------------------- *
     * Exactly the configuration Phase 5 proved and decodeframe_test repeated.
     * Nothing here is new; the create is only the lever that makes the codec
     * module load. */
    {
        SceVideodec2DecoderConfigInfo cfg;
        SceVideodec2DecoderMemoryInfo mem;
        int rc;

        cfg_init(&cfg, want_hevc ? RES_BIG : RES_STD,
                 want_hevc ? CODEC_HEVC : CODEC_AVC,
                 want_hevc ? 1 : 100, want_hevc ? 120 : 51,
                 1920, 1080, 16, queue);

        memset(&mem, 0, sizeof mem);
        mem.thisSize = DECODER_MEMINFO_SIZE;

        rc = query(&cfg, &mem);
        LOG("\nQueryDecoderMemoryInfo (%s) -> 0x%08x work %llu frame %llu\n",
            want_hevc ? "HEVC" : "H.264", rc,
            (unsigned long long)mem.workMemorySize,
            (unsigned long long)mem.frameMemorySize);
        if (rc)
            goto done;

        rc = dmem_get(&work, mem.workMemorySize, SCE_KERNEL_WB_ONION);
        if (rc) {
            LOG("work memory -> 0x%08x\n", rc);
            goto done;
        }
        rc = dmem_get(&frame, mem.frameMemorySize, SCE_KERNEL_WC_GARLIC);
        if (rc) {
            LOG("frame memory -> 0x%08x\n", rc);
            goto done;
        }
        memset(work.vaddr, 0, DECODER_OBJECT_SIZE);

        mem.pWorkMemory  = work.vaddr;
        mem.pFrameMemory = frame.vaddr;
        mem.pExtraMemory = NULL;

        LOG("\n*** sceVideodec2CreateDecoder - the call that loads the codec "
            "module ***\n");
        rc = create(&cfg, &mem, &dec);
        LOG("  -> 0x%08x decoder=%p\n", rc, dec);
        created = (rc == 0 && dec != NULL);
        if (!created) {
            LOG("  create failed; the codec module will not have loaded and\n"
                "  the diff below will be empty. That is itself a result.\n");
        }
    }

    /* -- what appeared? --------------------------------------------------- */
    nafter = sweep_modules(after, MAX_MODS);
    print_module_table("AFTER CreateDecoder", after, nafter);

    LOG("\n=== the diff: what CreateDecoder loaded ===\n");
    for (size_t i = 0; i < nafter; i++) {
        if (seen_before(before, nbefore, &after[i]))
            continue;
        newmods++;
        LOG("  NEW: modid=0x%-3x %s\n", after[i].modid, after[i].name);
    }
    if (!newmods)
        LOG("  nothing new.\n"
            "  Readings, in order of likelihood:\n"
            "    - the codec module was already mapped before the create (the\n"
            "      0x805A1001 \"already loaded\" path), in which case it is in\n"
            "      the BASELINE table above and the submit code is in one of\n"
            "      those modules after all - compare that table against the\n"
            "      fourteen findings.md section 3 records;\n"
            "    - or the codec runs somewhere the modid sweep cannot see, and\n"
            "      the next question is what sceKernelGetModuleInfo is missing.\n");

    /* -- dump ------------------------------------------------------------- */
    if (newmods) {
        LOG("\n=== dumping %d new module(s) ===\n", newmods);
        for (size_t i = 0; i < nafter; i++) {
            if (seen_before(before, nbefore, &after[i]))
                continue;
            dump_module(&after[i], buf);
        }
    }

    /* Whatever happened, dump the modules whose text we know the submit chain
     * runs through but which are NOT in proprietary/dump/ yet. Cheap, and it
     * removes a second trip if the diff was empty for the boring reason. */
    {
        static const char *const kWanted[] = {
            "libSceVdecwrap.sprx", "libSceVdecShevc.sprx", "libSceAjm.sprx",
            "libSceGnmDriver.sprx", "libSceSysmodule.sprx",
        };

        LOG("\n=== also dumping known-but-undumped modules ===\n");
        for (size_t i = 0; i < nafter; i++) {
            for (size_t k = 0; k < sizeof kWanted / sizeof *kWanted; k++) {
                if (strcmp(after[i].name, kWanted[k]) != 0)
                    continue;
                if (newmods && !seen_before(before, nbefore, &after[i]))
                    break;   /* already dumped above */
                dump_module(&after[i], buf);
                break;
            }
        }
    }

done:
    if (dec) {
        int rc = del(dec);
        LOG("\nsceVideodec2DeleteDecoder -> 0x%08x\n", rc);
    }
    /* Both pointers are guarded rather than reasoned about: several error
     * paths goto here from before these were assigned, and a local guard is
     * cheaper to trust than tracing the control flow that makes it safe. */
    if (queue && rel_cq) {
        int rc = rel_cq(queue);
        LOG("ReleaseComputeQueue -> 0x%08x\n", rc);
    }
    dmem_put(&frame);
    dmem_put(&work);
    dmem_put(&cq_mem);
    free(buf);

    LOG("\n=== summary ===\n");
    LOG("  modules before create : %zu\n", nbefore);
    LOG("  decoder created       : %s\n", created ? "yes" : "NO");
    LOG("  modules after create  : %zu\n", nafter);
    LOG("  new modules           : %d\n", newmods);
    LOG("  bytes dumped          : %zu\n", g_written);

    if (newmods)
        LOG("\n  Pull the dumps off the stick and disassemble them:\n"
            "    curl -sfO http://$PS5_HOST:8080/fs/mnt/usb0/evo_codec_*.bin\n"
            "  Then look for the line number 0x254 next to a 0xD0A1 tag - that\n"
            "  is the ioctl reporting errno 5200, and it is the last unknown\n"
            "  between here and a decoded frame.\n");

    LOG("\ndone\n");
    if (g_manifest) {
        fprintf(g_manifest, "#end\tbefore=%zu\tafter=%zu\tnew=%d\tbytes=%zu\n",
                nbefore, nafter, newmods, g_written);
        fclose(g_manifest);
    }
    if (g_log)
        fclose(g_log);

    evo_notify("EVO codecdump: %d new module(s), %zu KB dumped",
               newmods, g_written / 1024);
    return EXIT_SUCCESS;
}
