/* EVO Player - computequeue_test
 *
 * PHASE 4: memory and the compute queue.
 *
 * Phase 3 proved the decoder is reachable from a payload: a pure query
 * returned 0 and real, resolution-scaled memory requirements. This is the
 * first program that ALLOCATES rather than queries, and the first that can
 * fail for a reason specific to running as a payload rather than a title.
 *
 * WHAT IT ANSWERS, IN ONE DEPLOY
 *   1. How much memory does the GPU compute resource need?
 *      (sceVideodec2QueryComputeMemoryInfo - a query, as safe as Phase 3)
 *   2. Can a payload get direct memory in the sizes the decoder wants?
 *      Measured as a ladder rather than a single ask, because the ceiling is
 *      the answer, not the pass/fail.  videoout_test already established that
 *      64 MiB fails EAGAIN under elfldr while 32 MiB succeeds - and 1080p
 *      decode needs 89 MiB, so this is a live question, not a formality.
 *   3. Does sceVideodec2AllocateComputeQueue succeed in a payload?
 *      The single most valuable bit in the phase, and the likeliest hard
 *      blocker in the whole effort.
 *
 * ANSWERED 2026-08-10:
 *   1. 4.58 MiB, fixed - it does not scale with resolution.
 *   2. Depends entirely on the LAUNCH SLOT. Under ./scripts/deploy.sh the
 *      ceiling is between 41 and 64 MiB; under ./scripts/install-homebrew.sh
 *      --run, every size up to 322 MiB allocates. Same ELF, same console.
 *      RUN THIS WITH --run. deploy.sh lands in SceSpZeroConf (dmem#0), which
 *      is not where the player lives.
 *   3. YES - rc=0, handle 0x2002b0500, released cleanly, in both slots. See
 *      the module list below for the one thing that had to change.
 *
 * ---------------------------------------------------------------------------
 * THE ABI, READ OFF THE MODULE BEFORE ANY OF IT WAS CALLED
 *
 * Standing rule 1: read the prologue, then write the call. Every structure
 * below comes from disassembling proprietary/dump/evo_dump_libSceVideodec2_s0
 * offline - zero deploys spent guessing.
 *
 * sceVideodec2QueryComputeMemoryInfo (+0x550) takes ONE argument, not two:
 *
 *     cmp QWORD PTR [rbx], 0x18     ; size-prefixed, and 0x18 exactly
 *     jne -> 0x811D0101
 *     call <sceVdecCoreQueryComputeResourceInfo>
 *     mov QWORD PTR [rbx+0x10], 0   ; cpuGpuMemory  <- NULL
 *     mov QWORD PTR [rbx+0x8],  rax ; cpuGpuMemorySize <- computed
 *
 * sceVideodec2AllocateComputeQueue (+0x660) takes THREE:
 *
 *     cmp QWORD PTR [r15], 0x10     ; queueInfo   size 0x10
 *     cmp QWORD PTR [r14], 0x18     ; memoryInfo  size 0x18  (same struct)
 *     cmp BYTE  PTR [r15+0xd], 0    ; \ reserved bytes 0xd..0xf
 *     cmp WORD  PTR [r15+0xe], 0    ; / must be zero        -> 0x811D0200
 *     movzx r8d, WORD PTR [r15+0x8] ; computePipeId  < 5    -> 0x811D0201
 *     movzx r8d, WORD PTR [r15+0xa] ; computeQueueId < 8    -> 0x811D0202
 *     call <sceVdecCoreQueryComputeResourceInfo>   ; recompute the requirement
 *     cmp  QWORD PTR [r14+0x8], required           ; caller's size must be >=
 *     jb   -> 0x811D0104
 *     ...
 *     call <sceVdecCoreInitializeComputeResource>(&{size, mem, pipe, queue})
 *     mov  QWORD PTR [rbx], handle                 ; *out = queue handle
 *
 * sceVideodec2ReleaseComputeQueue (+0x970) takes ONE - the handle - and
 * forwards it to sceVdecCoreFinalizeComputeResource. NULL -> 0x811D0110.
 *
 * The three sceVdecCore* names are not guesses either: libSceVideodec2's PLT
 * entries were followed into its s2 GOT, and the resolved addresses land
 * exactly on libSceVdecCore's exported +0x2b0, +0x300 and +0x3a0.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE MEMORY-TYPE EVIDENCE SAYS
 *
 * AllocateComputeQueue hands the caller's buffer to an internal checker
 * (+0x2a0) with a mode selected by queueInfo+0xc:
 *
 *     mode 3 (byte == 0)  no memory check at all
 *     mode 0 (byte != 0)  SceKernelVirtualQueryInfo.memoryType must be 0
 *
 * memoryType 0 is SCE_KERNEL_WB_ONION. So the module's own validator expects
 * the compute-queue memory to be ONION (cached, CPU-coherent), NOT the
 * WC_GARLIC the phase plan assumed - garlic is for frame buffers, which is a
 * different allocation entirely. This probe tries onion first and garlic as
 * the fallback, and records which the hardware actually accepts.
 *
 * That checker is reached only when a global flag getter returns non-zero.
 * The checker itself calls a 4-argument function with a 0x48-byte output
 * struct whose +0x1c is read as a memory type and +0x20 as protection bits -
 * that is sceKernelVirtualQuery's exact signature and PS4 struct layout.
 * WHICH MATTERS: sceKernelVirtualQuery is one of the APIs measured BROKEN in
 * a payload (findings section 2). If that flag is ever set here, every
 * pointer argument in this module fails validation and everything returns
 * 0x811D0102 no matter how correct it is. So 0x811D0102 on an obviously
 * non-NULL pointer is a diagnosis, not a mystery - see the summary at the end.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 *   - Every allocating call is paired with its release in the same run, on
 *     every path. A leaked compute queue across deploys is a plausible way to
 *     make later runs fail for reasons unrelated to the change.
 *   - The pipe/queue sweep runs only after (0,0) has already failed, and a
 *     failed AllocateComputeQueue allocates nothing.
 *   - No watchdog thread: measured on 12.70, it does not fire. The guard is
 *     `timeout` around the deploy plus a log flushed after every line.
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

#define USB_DIR   "/mnt/usb0"
#define LOG_PATH  USB_DIR "/evo_computequeue_log.txt"

#define MiB(x) ((size_t)(x) * 1024u * 1024u)

/* Direct-memory allocations are physical; use the same 128 KiB alignment the
 * working VideoOut path uses rather than the decoder's 256-byte figure, which
 * describes the buffer it hands back, not the allocation granularity. */
#define DMEM_ALIGN 0x20000u

/* Struct sizes the module itself checks for. */
#define COMPUTE_MEMINFO_SIZE 0x18
#define COMPUTE_QUEUEINFO_SIZE 0x10
#define DECODER_MEMINFO_SIZE 0x48
#define DECODER_CFG_SIZE 0x48

/* The 1080p AVC figure Phase 3 measured. Re-measured here as the control: if
 * this number moved, the module or the ABI reading changed and nothing else in
 * this log can be trusted. */
#define PHASE3_1080P_FRAMEPOOL 86507776ull

/* Modules to bring up. libSceAudiodec is deliberately absent: loading it here
 * hangs the payload (reproduced twice on 2026-08-10).
 *
 * libSceGnmDriver IS REQUIRED, and that is the whole story of this phase. The
 * first run omitted it and AllocateComputeQueue hung - silently, forever, with
 * no fault and no error code. Disassembling the chain afterwards showed the
 * call bottoming out in sceGnmMapComputeQueue, whose GOT slot is unresolved in
 * the dumped image, so the call had to resolve a symbol in a module that was
 * not mapped. Adding this one line made the same call return 0.
 *
 * An unresolved lazy import blocks rather than faulting. It looks exactly like
 * a GPU permissions problem and is not one.
 *
 * libSceAjm is here because the libSceAudiodec precedent pairs it with
 * GnmDriver; it is not known to be required.
 *
 * Both runs are archived:
 *   research-logs/console/evo_computequeue_log-run1-hang.txt     (no GnmDriver)
 *   research-logs/console/evo_computequeue_log-run2-success.txt  (with it) */
static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",   /* sceGnmMapComputeQueue lives here */
    "libSceAjm.sprx",         /* pairs with GnmDriver, per the Audiodec fix */
};

/* 0x18 bytes. Query fills 0x08 and zeroes 0x10; the caller fills 0x10 in. */
typedef struct {
    uint64_t thisSize;          /* 0x00  must be 0x18                        */
    uint64_t cpuGpuMemorySize;  /* 0x08  out from Query, in to Allocate      */
    void    *cpuGpuMemory;      /* 0x10  NULL from Query, caller's on the way in */
} SceVideodec2ComputeMemoryInfo;

/* 0x10 bytes. */
typedef struct {
    uint64_t thisSize;          /* 0x00  must be 0x10                        */
    uint16_t computePipeId;     /* 0x08  < 5                                 */
    uint16_t computeQueueId;    /* 0x0a  < 8                                 */
    uint8_t  memoryCheckMode;   /* 0x0c  0 = unchecked, else require ONION   */
    uint8_t  reserved[3];       /* 0x0d  must be zero                        */
} SceVideodec2ComputeQueueInfo;

typedef int (*query_decoder_meminfo_fn)(void *cfg, void *mem);
typedef int (*query_compute_meminfo_fn)(SceVideodec2ComputeMemoryInfo *info);
typedef int (*alloc_compute_queue_fn)(const SceVideodec2ComputeQueueInfo *qi,
                                      SceVideodec2ComputeMemoryInfo *mi,
                                      void **queueOut);
typedef int (*release_compute_queue_fn)(void *queue);

static FILE *g_log;

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

/* Resolve one export by NID and report its offset, so the log can be checked
 * against the recorded export map without trusting the base address. */
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
/* Direct memory                                                             */
/* ------------------------------------------------------------------------- */

/* Allocate, optionally map, then hand back both. Returns 0 on success.
 * Every caller releases; nothing here leaks on any path. */
static int
dmem_get(size_t len, int memtype, int do_map,
         intptr_t *paddr_out, void **vaddr_out)
{
    intptr_t paddr = 0;
    void    *vaddr = NULL;
    int      rc;

    rc = sceKernelAllocateMainDirectMemory(len, DMEM_ALIGN, memtype, &paddr);
    if (rc)
        return rc;

    if (do_map) {
        rc = sceKernelMapDirectMemory(&vaddr, len,
                                      SCE_KERNEL_PROT_CPU_RW |
                                      SCE_KERNEL_PROT_GPU_ALL,
                                      0, paddr, DMEM_ALIGN);
        if (rc) {
            sceKernelReleaseDirectMemory(paddr, len);
            return rc;
        }
    }

    *paddr_out = paddr;
    *vaddr_out = vaddr;
    return 0;
}

static void
dmem_put(intptr_t paddr, void *vaddr, size_t len)
{
    if (vaddr)
        munmap(vaddr, len);
    if (paddr)
        sceKernelReleaseDirectMemory(paddr, len);
}

static size_t
round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* A ladder rather than a single ask. The interesting result is the ceiling. */
static void
dmem_ladder(size_t compute_size)
{
    static const struct { const char *label; size_t len; } kSizes[] = {
        { "16 MiB",                  MiB(16)   },
        { "32 MiB  (videoout ok)",   MiB(32)   },
        { "41 MiB  (720p decode)",   MiB(41)   },
        { "64 MiB  (videoout EAGAIN)", MiB(64) },
        { "90 MiB  (1080p decode)",  MiB(90)   },
        { "109 MiB (4K dpb4)",       MiB(109)  },
        { "160 MiB",                 MiB(160)  },
        { "322 MiB (4K dpb16)",      MiB(322)  },
    };
    static const struct { const char *label; int type; } kTypes[] = {
        { "WB_ONION ", SCE_KERNEL_WB_ONION  },
        { "WC_GARLIC", SCE_KERNEL_WC_GARLIC },
    };

    LOG("\n--- direct-memory ladder (allocate, then release immediately) ---\n");
    LOG("    THE LAUNCH SLOT IS THE VARIABLE, not the console. Measured\n"
        "    2026-08-10 with this same ELF:\n"
        "      deploy.sh  -> SceSpZeroConf, dmem#0: caps between 41 and 64 MiB\n"
        "      --run      -> PS Now app slot:       322 MiB and beyond, all OK\n"
        "    A ceiling reported here under deploy.sh says nothing about what\n"
        "    the player, which runs in the app slot, can allocate.\n");

    for (size_t t = 0; t < sizeof kTypes / sizeof *kTypes; t++) {
        /* The compute requirement first: it is the one size that must work. */
        if (compute_size) {
            size_t   len = round_up(compute_size, DMEM_ALIGN);
            intptr_t p = 0;
            void    *v = NULL;
            int      rc = dmem_get(len, kTypes[t].type, 0, &p, &v);

            LOG("  %s  %-24s (%7zu KiB) -> 0x%08x%s\n",
                kTypes[t].label, "compute requirement", len / 1024, rc,
                rc ? "" : "  OK");
            if (!rc)
                dmem_put(p, v, len);
        }

        for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
            intptr_t p = 0;
            void    *v = NULL;
            int      rc = dmem_get(kSizes[i].len, kTypes[t].type, 0, &p, &v);

            LOG("  %s  %-24s (%7zu KiB) -> 0x%08x%s\n",
                kTypes[t].label, kSizes[i].label, kSizes[i].len / 1024, rc,
                rc ? "" : "  OK");
            if (!rc)
                dmem_put(p, v, kSizes[i].len);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The compute queue                                                         */
/* ------------------------------------------------------------------------- */

/* One attempt. Logs, and on success releases the queue before returning 1. */
static int
try_queue(alloc_compute_queue_fn alloc, release_compute_queue_fn release,
          const char *label, uint16_t pipe, uint16_t queue, uint8_t mode,
          size_t size, void *mem)
{
    SceVideodec2ComputeQueueInfo  qi;
    SceVideodec2ComputeMemoryInfo mi;
    void *handle = NULL;
    int   rc;

    memset(&qi, 0, sizeof qi);
    qi.thisSize        = COMPUTE_QUEUEINFO_SIZE;
    qi.computePipeId   = pipe;
    qi.computeQueueId  = queue;
    qi.memoryCheckMode = mode;

    memset(&mi, 0, sizeof mi);
    mi.thisSize         = COMPUTE_MEMINFO_SIZE;
    mi.cpuGpuMemorySize = size;
    mi.cpuGpuMemory     = mem;

    LOG("  %-34s pipe=%u queue=%u mode=%u ... ", label, pipe, queue, mode);
    rc = alloc(&qi, &mi, &handle);
    LOG("rc=0x%08x handle=%p\n", rc, handle);

    if (rc || !handle)
        return 0;

    LOG("    *** COMPUTE QUEUE ALLOCATED *** releasing it now\n");
    LOG("    sceVideodec2ReleaseComputeQueue -> 0x%08x\n", release(handle));
    return 1;
}

int
main(void)
{
    int      have_query_control = 0;
    int      have_compute_size = 0;
    int      queue_ok = 0;
    size_t   compute_size = 0;
    intptr_t cq_paddr = 0;
    void    *cq_vaddr = NULL;
    size_t   cq_len = 0;
    int      cq_memtype = -1;

    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player computequeue_test - Phase 4 ===\n");
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

    /* -- resolve the entry points ----------------------------------------- */
    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), "libSceVideodec2.sprx", &dynh) != 0) {
        LOG("\nFATAL: no dynlib handle for libSceVideodec2.sprx\n");
        return EXIT_FAILURE;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    LOG("\nlibSceVideodec2 base 0x%lx\n", (unsigned long)base);

    intptr_t a_query_dec = resolve(dynh, base,
                                   "sceVideodec2QueryDecoderMemoryInfo", 0xa10);
    intptr_t a_query_cm  = resolve(dynh, base,
                                   "sceVideodec2QueryComputeMemoryInfo", 0x550);
    intptr_t a_alloc_cq  = resolve(dynh, base,
                                   "sceVideodec2AllocateComputeQueue", 0x660);
    intptr_t a_rel_cq    = resolve(dynh, base,
                                   "sceVideodec2ReleaseComputeQueue", 0x970);

    if (!a_query_dec || !a_query_cm || !a_alloc_cq || !a_rel_cq) {
        LOG("\nFATAL: an entry point did not resolve\n");
        return EXIT_FAILURE;
    }

    query_decoder_meminfo_fn query_dec = (query_decoder_meminfo_fn)a_query_dec;
    query_compute_meminfo_fn query_cm  = (query_compute_meminfo_fn)a_query_cm;
    alloc_compute_queue_fn   alloc_cq  = (alloc_compute_queue_fn)a_alloc_cq;
    release_compute_queue_fn rel_cq    = (release_compute_queue_fn)a_rel_cq;

    /* -- CONTROL: the Phase 3 call, re-measured ---------------------------
     * Standing rule 3: every probe needs a control. If this stops returning
     * the number Phase 3 recorded, nothing below this line means anything. */
    LOG("\n--- control: the Phase 3 query, 0xb6c8 1080p dpb16 ---\n");
    {
        static uint8_t cfg[0x80];
        static uint8_t mem[0x80];
        uint64_t pool = 0;
        int      rc;

        memset(cfg, 0, sizeof cfg);
        memset(mem, 0, sizeof mem);
        *(uint64_t *)cfg          = DECODER_CFG_SIZE;
        *(uint64_t *)mem          = DECODER_MEMINFO_SIZE;
        *(uint32_t *)(cfg + 0x08) = 0xb6c8;   /* codec type   */
        *(uint32_t *)(cfg + 0x0c) = 1;        /* variant      */
        *(uint32_t *)(cfg + 0x10) = 100;      /* profile_idc  */
        *(uint32_t *)(cfg + 0x14) = 51;       /* level_idc    */
        *(uint32_t *)(cfg + 0x18) = 1920;
        *(uint32_t *)(cfg + 0x1c) = 1080;
        *(uint32_t *)(cfg + 0x20) = 16;       /* DPB          */
        *(uint32_t *)(cfg + 0x24) = 4;        /* pipeline     */
        *(uint32_t *)(cfg + 0x38) = 700;      /* priority     */

        rc = query_dec(cfg, mem);
        memcpy(&pool, mem + 0x18, sizeof pool);
        LOG("  rc=0x%08x  frame pool +0x18 = %llu  (Phase 3: %llu)  %s\n",
            rc, (unsigned long long)pool,
            (unsigned long long)PHASE3_1080P_FRAMEPOOL,
            (rc == 0 && pool == PHASE3_1080P_FRAMEPOOL) ? "MATCH"
                                                        : "*** DIVERGED ***");
        have_query_control = (rc == 0 && pool == PHASE3_1080P_FRAMEPOOL);
    }

    /* -- 1. QueryComputeMemoryInfo ----------------------------------------
     * A query, so as safe as Phase 3, with its own controls first. */
    LOG("\n--- sceVideodec2QueryComputeMemoryInfo ---\n");
    {
        SceVideodec2ComputeMemoryInfo info;
        int rc;

        rc = query_cm(NULL);
        LOG("  control: NULL              -> 0x%08x %s\n", rc,
            (uint32_t)rc == 0x811d0102u ? "(0x811D0102 bad pointer, as read)"
                                        : "***");

        memset(&info, 0, sizeof info);
        info.thisSize = 0x20;
        rc = query_cm(&info);
        LOG("  control: thisSize=0x20     -> 0x%08x %s\n", rc,
            (uint32_t)rc == 0x811d0101u ? "(0x811D0101 wrong size, as read)"
                                        : "***");

        memset(&info, 0, sizeof info);
        info.thisSize = COMPUTE_MEMINFO_SIZE;
        rc = query_cm(&info);
        LOG("  real:    thisSize=0x18     -> 0x%08x\n", rc);
        LOG("    raw:\n");
        hexdump(&info, sizeof info, "      ");

        if (rc == 0) {
            compute_size = (size_t)info.cpuGpuMemorySize;
            have_compute_size = 1;
            LOG("    cpuGpuMemorySize = %llu (%.2f MiB)\n",
                (unsigned long long)info.cpuGpuMemorySize,
                (double)info.cpuGpuMemorySize / (1024.0 * 1024.0));
            LOG("    cpuGpuMemory     = %p %s\n", info.cpuGpuMemory,
                info.cpuGpuMemory == NULL ? "(zeroed, as read)" : "***");
        }
    }

    /* -- 2. how much direct memory can this payload actually get? --------- */
    dmem_ladder(compute_size);

    if (!have_compute_size) {
        LOG("\nno compute memory requirement - cannot attempt the queue.\n");
        goto done;
    }

    /* -- 3. the compute-queue memory -------------------------------------
     * ONION first: the module's own validator wants memoryType 0. */
    LOG("\n--- compute-queue memory ---\n");
    cq_len = round_up(compute_size, DMEM_ALIGN);
    {
        static const struct { const char *label; int type; } kTry[] = {
            { "WB_ONION",  SCE_KERNEL_WB_ONION  },
            { "WC_GARLIC", SCE_KERNEL_WC_GARLIC },
        };

        for (size_t i = 0; i < sizeof kTry / sizeof *kTry; i++) {
            int rc = dmem_get(cq_len, kTry[i].type, 1, &cq_paddr, &cq_vaddr);

            LOG("  %-10s %zu KiB alloc+map -> 0x%08x", kTry[i].label,
                cq_len / 1024, rc);
            if (rc) {
                LOG("\n");
                continue;
            }
            cq_memtype = kTry[i].type;
            LOG("  phys 0x%lx  virt %p\n",
                (unsigned long)cq_paddr, cq_vaddr);
            break;
        }
    }

    if (!cq_vaddr) {
        LOG("  no usable memory for the compute queue - stopping here.\n");
        goto done;
    }

    /* Prove the CPU can touch it before handing it to the GPU: a fault here
     * is a mapping problem, and it is much better to learn that now than to
     * blame the decoder for it later. */
    {
        volatile uint8_t *p = cq_vaddr;

        p[0] = 0xa5;
        p[cq_len - 1] = 0x5a;
        LOG("  cpu write/read back: 0x%02x 0x%02x\n",
            p[0], p[cq_len - 1]);
    }
    memset(cq_vaddr, 0, cq_len);

    /* -- 4. AllocateComputeQueue: controls first -------------------------
     * Each of these returns before the module touches any hardware, so they
     * cost nothing and they prove the structure reading is right rather than
     * a coincidence - the same discipline that validated the config struct in
     * Phase 3. */
    LOG("\n--- AllocateComputeQueue: validation controls ---\n");
    {
        SceVideodec2ComputeQueueInfo  qi;
        SceVideodec2ComputeMemoryInfo mi;
        void *h = NULL;
        int   rc;

        static const struct {
            const char *label;
            uint64_t qi_size, mi_size;
            uint16_t pipe, queue;
            uint8_t  resv;
            int64_t  size_delta;
            int      null_args;
            uint32_t expect;
        } kControls[] = {
            { "all-NULL",          0x10, 0x18, 0, 0, 0,  0, 1, 0x811d0102 },
            { "queueInfo size 0x18", 0x18, 0x18, 0, 0, 0,  0, 0, 0x811d0101 },
            { "memInfo size 0x10", 0x10, 0x10, 0, 0, 0,  0, 0, 0x811d0101 },
            { "reserved byte set", 0x10, 0x18, 0, 0, 1,  0, 0, 0x811d0200 },
            { "pipeId 5",          0x10, 0x18, 5, 0, 0,  0, 0, 0x811d0201 },
            { "queueId 8",         0x10, 0x18, 0, 8, 0,  0, 0, 0x811d0202 },
            { "size one byte short", 0x10, 0x18, 0, 0, 0, -1, 0, 0x811d0104 },
        };

        for (size_t i = 0; i < sizeof kControls / sizeof *kControls; i++) {
            if (kControls[i].null_args) {
                rc = alloc_cq(NULL, NULL, NULL);
            } else {
                memset(&qi, 0, sizeof qi);
                qi.thisSize       = kControls[i].qi_size;
                qi.computePipeId  = kControls[i].pipe;
                qi.computeQueueId = kControls[i].queue;
                qi.reserved[0]    = kControls[i].resv;

                memset(&mi, 0, sizeof mi);
                mi.thisSize = kControls[i].mi_size;
                mi.cpuGpuMemorySize =
                    (uint64_t)((int64_t)compute_size + kControls[i].size_delta);
                mi.cpuGpuMemory = cq_vaddr;

                h  = NULL;
                rc = alloc_cq(&qi, &mi, &h);
            }
            LOG("  %-22s -> 0x%08x  expect 0x%08x  %s\n",
                kControls[i].label, rc, kControls[i].expect,
                (uint32_t)rc == kControls[i].expect ? "ok" : "*** DIFFERS ***");
            if (rc == 0 && h) {
                LOG("    control unexpectedly SUCCEEDED - releasing\n");
                rel_cq(h);
            }
        }
    }

    /* -- 5. the real call ------------------------------------------------- */
    LOG("\n--- AllocateComputeQueue: the real call ---\n");
    queue_ok = try_queue(alloc_cq, rel_cq, "pipe 0 queue 0, unchecked",
                         0, 0, 0, compute_size, cq_vaddr);

    if (!queue_ok) {
        /* The checked mode, in case the flag means something to the layer
         * below rather than only to the validator. */
        queue_ok = try_queue(alloc_cq, rel_cq, "pipe 0 queue 0, checked",
                             0, 0, 1, compute_size, cq_vaddr);
    }

    if (!queue_ok) {
        LOG("\n  (0,0) refused - sweeping the pipes, then the queues.\n"
            "  A failed allocation allocates nothing, so this is cheap.\n");
        for (uint16_t pipe = 1; pipe < 5 && !queue_ok; pipe++)
            queue_ok = try_queue(alloc_cq, rel_cq, "pipe sweep",
                                 pipe, 0, 0, compute_size, cq_vaddr);
        for (uint16_t q = 1; q < 8 && !queue_ok; q++)
            queue_ok = try_queue(alloc_cq, rel_cq, "queue sweep",
                                 0, q, 0, compute_size, cq_vaddr);
    }

    /* If ONION was what we got and it failed, garlic is the other half of the
     * memory-type question the phase plan asked. */
    if (!queue_ok && cq_memtype == SCE_KERNEL_WB_ONION) {
        intptr_t p2 = 0;
        void    *v2 = NULL;
        int      rc = dmem_get(cq_len, SCE_KERNEL_WC_GARLIC, 1, &p2, &v2);

        LOG("\n  retry with WC_GARLIC memory: alloc+map -> 0x%08x\n", rc);
        if (rc == 0) {
            memset(v2, 0, cq_len);
            queue_ok = try_queue(alloc_cq, rel_cq, "garlic, pipe 0 queue 0",
                                 0, 0, 0, compute_size, v2);
            dmem_put(p2, v2, cq_len);
        }
    }

done:
    dmem_put(cq_paddr, cq_vaddr, cq_len);

    /* -- what this run established ---------------------------------------- */
    LOG("\n=== summary ===\n");
    LOG("  Phase 3 control re-measured   : %s\n",
        have_query_control ? "MATCH" : "DIVERGED - distrust everything above");
    LOG("  compute memory requirement    : %s",
        have_compute_size ? "" : "NOT OBTAINED\n");
    if (have_compute_size)
        LOG("%zu bytes (%.2f MiB)\n", compute_size,
            (double)compute_size / (1024.0 * 1024.0));
    LOG("  compute-queue memory type     : %s\n",
        cq_memtype == SCE_KERNEL_WB_ONION  ? "WB_ONION" :
        cq_memtype == SCE_KERNEL_WC_GARLIC ? "WC_GARLIC" : "none obtained");
    LOG("  compute queue                 : %s\n",
        queue_ok ? "ALLOCATED - the GPU path is open to a payload"
                 : "REFUSED");
    if (!queue_ok)
        LOG("\n  Reading the refusal:\n"
            "    0x811D0102 on non-NULL pointers -> the module's argument\n"
            "               validation is enabled and it runs through\n"
            "               sceKernelVirtualQuery, which is broken in a\n"
            "               payload. Not a permissions problem.\n"
            "    0x811D0111 -> sceVdecCoreQueryComputeResourceInfo failed\n"
            "               below us; the compute resource is unavailable.\n"
            "    0x811D0200 from the real call -> \n"
            "               sceVdecCoreInitializeComputeResource refused.\n"
            "               This is the permissions-shaped outcome: stop and\n"
            "               reassess, and consider Route A, which reaches the\n"
            "               decoder without the caller owning a queue.\n");

    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);

    evo_notify("EVO computequeue_test: queue %s",
               queue_ok ? "ALLOCATED" : "refused");
    return EXIT_SUCCESS;
}
