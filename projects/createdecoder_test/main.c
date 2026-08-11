/* EVO Player - createdecoder_test
 *
 * PHASE 5: create a decoder.
 *
 * Phase 3 proved the decoder answers a query. Phase 4 proved a payload can get
 * a GPU compute queue and, in the app slot, the memory the query asks for.
 * This is the first program that asks the module to BUILD something: it hands
 * back the buffers the query demanded and calls sceVideodec2CreateDecoder.
 *
 * RUN IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
 * deploy.sh injects into SceSpZeroConf, which is spawned with dmem#0 and caps
 * out between 41 and 64 MiB. The 1080p working set is 90 MiB. Under deploy.sh
 * this probe fails at the allocator and never reaches the decoder at all.
 *
 * WHAT IT ANSWERS, IN ONE DEPLOY, CHEAPEST QUESTION FIRST (standing rule 9)
 *   1. Does the Phase 3 query still return the number Phase 3 recorded?
 *      (the control - if this moved, nothing below it means anything)
 *   2. What is in the WHOLE 0x48-byte memory-info struct? Phase 3 logged three
 *      fields and never dumped the rest, so the size at +0x28 - which
 *      CreateDecoder checks and which needs its own buffer - is unmeasured.
 *   3. Which codecs does the module accept? Phase 3 believed +0x08 was the
 *      codec and got 0x811D0205 for everything except one value. It is not.
 *      See "THE CORRECTION" below. This asks the question properly for the
 *      first time, as a pure query, at zero risk.
 *   4. Does sceVideodec2CreateDecoder succeed? The phase's actual goal.
 *
 * ---------------------------------------------------------------------------
 * THE CORRECTION - +0x08 IS NOT THE CODEC TYPE
 *
 * docs/hardware-decode-findings.md 7 records the config struct as
 * "+0x08 codec type (1, 0xb6c8, 0x12384, 0x24708, 0x24709)" and "+0x0c variant
 * (1, 0xee049, 0x245bfd), must be 1 for the working codec types". Reading the
 * validator at +0x3eb0 all the way through says the two fields are the other
 * way round, and the evidence is not subtle - it is the profile and level
 * checks that follow, which are codec-specific:
 *
 *   [cfg+0x0c] == 1        profile in {66, 77, 100}      level 10..111
 *                          ^ H.264 Baseline/Main/High    ^ level_idc
 *   [cfg+0x0c] == 0xee049  profile in {1, 2}             level {30,63,90,93}
 *                          ^ HEVC Main / Main10          ^ or 120..186
 *                                                          = HEVC level x 30
 *   [cfg+0x0c] == 0x245bfd profile in {0, 2}             level in {10,11,20,
 *                          ^ VP9 profile 0 / 2              21,30,31,40,41,
 *                                                           50,51,52,60,61,62}
 *                                                        ^ VP9 level x 10
 *   anything else                                        -> 0x811D0204
 *
 * So [cfg+0x0c] is the CODEC and [cfg+0x08] is a resource class, which the
 * validator maps to a small integer that libSceVdecCore uses to decide which
 * buffers it needs:  1 -> 2 or 9,  0xb6c8 -> 4,  0x12384 -> 8,  0x24708 -> 0xa,
 * 0x24709 -> 0xb.  Only 4 and 8 pass sceVdecCoreCreateDecoder's own class
 * check (`cmp r8,8; ja error`), which is why 0xb6c8 and 0x12384 are the only
 * two values that have ever worked.
 *
 * WHY THIS MATTERS: every one of Phase 3's six accepted configurations used
 * codec type 1 - H.264. Its "0xb6c8 v=ee049 -> 0x811D0205" lines were not the
 * module refusing HEVC; they were the probe offering HEVC a profile of 100 and
 * a level of 51, which are H.264 numbers, and 0x811D0205 is "unsupported
 * profile or level". HEVC HAS NEVER BEEN VALIDLY QUERIED. The phase plan's
 * discriminator "succeeds for 0xb6c8 and fails for 0x12384 -> the HEVC
 * entitlement gate is real" cannot work, because neither of those is HEVC.
 *
 * The codec matrix below asks each codec with its own profile and level.
 *
 * ---------------------------------------------------------------------------
 * THE ABI, READ OFF THE MODULE BEFORE ANY OF IT WAS CALLED
 *
 * sceVideodec2CreateDecoder (+0xba0) is a two-instruction thunk:
 *
 *     mov ecx,0x1 ; jmp <body at +0xbb0>
 *
 * sceVideodec2CreateHevcDecoder (+0x1230) is byte-for-byte the same thunk. It
 * is an ALIAS, not a different entry point - there is nothing HEVC-specific
 * about it, and calling it changes nothing. sceVideodec2CreateDecoderBid
 * (+0x1240) is the same body with a caller-supplied value in ecx instead of 1,
 * and it rejects resource class 1 outright.
 *
 * The body takes THREE arguments plus that hidden fourth:
 *
 *     int body(const ConfigInfo *cfg, MemoryInfo *mem, void **decoderOut,
 *              int bid);          // bid is always 1 from CreateDecoder
 *
 * so the "third argument beyond config and memory-info" the phase plan
 * expected is the output handle. What it does, in order:
 *
 *     cmp QWORD PTR [rsi], 0x48         ; memInfo size, exactly    -> 0x811D0101
 *     cmp QWORD PTR [rdi], 0x48 / 0x50  ; cfg size, either         -> 0x811D0101
 *     call <validator +0x3eb0>          ; the SAME validator the query uses,
 *                                       ; writing a memory-info-shaped struct
 *                                       ; to the stack           -> 0x811D02xx
 *     <check mem+0x10, mem+0x20, and mem+0x30 if mem+0x28 != 0>  -> 0x811D0105
 *     cmp DWORD PTR [rsi+0x44], 0                                -> 0x811D010C
 *     cmp QWORD PTR [rsi+0x08], <computed +0x08>   ; caller's >=  -> 0x811D0104
 *     cmp QWORD PTR [rsi+0x28], <computed +0x28>   ;              -> 0x811D0104
 *     cmp QWORD PTR [rsi+0x18], <computed +0x18>   ;              -> 0x811D0104
 *     cmp QWORD PTR [rsi+0x38], <computed +0x38>   ;              -> 0x811D0106
 *     cmp <computed +0x40>, 0x100       ; internal sanity check   -> 0x811D0111
 *     test BYTE PTR [rsi+0x40], 0xff    ; caller's alignment      -> 0x811D0108
 *     ...
 *     [decoder] = "U5JD7RL"             ; decoder object IS mem+0x10
 *     call <+0x820>                     ; 12 ScePthread mutexes at decoder+0xc8
 *                                       ; and 9 cond vars at decoder+0x80
 *     call <sceVdecCoreCreateDecoder>(buf3, {sizes,pointers}, cfg->computeQueue,
 *                                     &decoder[0x78])
 *     [decoder+0x68] = 0xa824d9799010a455
 *     *decoderOut = decoder
 *
 * THE DECODER HANDLE IS THE CALLER'S OWN BUFFER. mem+0x10 is not just working
 * memory the module borrows - the module places its object at the front of it
 * and hands the same pointer back. The first 0x40000 bytes are the object;
 * everything past that is passed down to VdecCore. That is why the reported
 * +0x08 size is ~3.4 MiB and barely moves with resolution.
 *
 * WHICH MEMORY TYPE EACH BUFFER WANTS. The module's own checker (+0x2a0) is a
 * four-way jump table selected by a mode argument:
 *
 *     mode 0   SceKernelVirtualQueryInfo.memoryType must be 0 (WB_ONION)
 *     mode 1   ... must be 3 (WC_GARLIC)
 *     mode 2   protection must include write
 *     mode 3   no check at all, NULL is still rejected with 0x811D0105
 *
 * CreateDecoder computes the mode from cfg->checkMemoryType:
 *
 *     mem+0x10 -> 3*(flag^1)   = 0 (ONION)  when the flag is set
 *     mem+0x20 -> 2*(flag^1)+1 = 1 (GARLIC) when the flag is set
 *     mem+0x30 -> 3*(flag^1)   = 0 (ONION)  when the flag is set
 *
 * The check only runs when a module-global flag is set, and that flag is clear
 * here (it gates sceKernelVirtualQuery, which is broken in a payload), so
 * checkMemoryType is inert - but the mapping still states the INTENT. This
 * probe allocates onion, garlic, onion accordingly, because that is what the
 * hardware is being told to expect even when nobody is checking.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE LAYER BELOW NEEDS (standing rule 10)
 *
 * sceVdecCoreCreateDecoder (VdecCore +0x1410) receives the buffers repacked as
 * {sizeA-0x40000, ptrA+0x40000, sizeC, ptrC, sizeB, ptrB} and requires:
 *
 *     class 4 (0xb6c8) and class 8 (0x12384)   ptrA and ptrB non-NULL
 *     class 2, 3                               ptrA, ptrB and ptrC non-NULL
 *     class > 8                                rejected outright, 0x80C00002
 *
 * and for the HEVC codec index it calls sceSysmoduleLoadModuleInternal to pull
 * in its codec module at create time. THAT IS A LAZY IMPORT INTO
 * libSceSysmodule, and an unresolved lazy import here does not fault and does
 * not return - it blocks forever. That was the entire Phase 4 hang. So
 * libSceSysmodule.sprx is in the module list below even though nothing in this
 * file calls it directly, and the HEVC create is opt-in for the same reason.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 *   - Every CreateDecoder is paired with DeleteDecoder on every path, and each
 *     attempt allocates and releases its own buffers, so a failure leaks
 *     nothing into the next attempt or the next deploy.
 *   - The compute queue is released at the end on every path.
 *   - The HEVC create is behind an argv flag (`--args "eboot.elf hevc"`),
 *     because it is the one call here that can reach a module load, and a hang
 *     in the app slot holds the slot. Run it as a second deploy once the AVC
 *     path has been recorded.
 *   - NOT TESTED ON PURPOSE: CreateDecoder with a NULL computeQueue. It would
 *     settle whether the queue is mandatory, but VdecCore does not NULL-check
 *     it, so the failure mode is a fault rather than an error code - and a
 *     faulted payload in the app slot costs far more than the answer is worth.
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
#define LOG_PATH  USB_DIR "/evo_createdecoder_log.txt"

#define MiB(x) ((size_t)(x) * 1024u * 1024u)

/* Direct-memory allocations are physical; 128 KiB alignment, as the working
 * VideoOut path uses. The decoder's own 256-byte figure describes the buffer
 * it hands back, not the allocation granularity. */
#define DMEM_ALIGN 0x20000u

/* Struct sizes the module itself checks for. */
#define DECODER_CFG_SIZE     0x48
#define DECODER_MEMINFO_SIZE 0x48
#define COMPUTE_MEMINFO_SIZE 0x18
#define COMPUTE_QUEUEINFO_SIZE 0x10

/* The decoder object lives in the first 0x40000 of the +0x10 buffer. */
#define DECODER_OBJECT_SIZE 0x40000

/* Magic cookies CreateDecoder writes, so the log can prove the object is real
 * rather than a plausible-looking pointer. */
#define DECODER_MAGIC0 0x004c5237444a3555ull   /* "U5JD7RL" at +0x00 */
#define DECODER_MAGIC1 0xa824d9799010a455ull   /* at +0x68; MapMemory checks it */

/* Resource classes. Only these two survive sceVdecCoreCreateDecoder. */
#define RES_STD 0xb6c8u    /* capped below 4K */
#define RES_BIG 0x12384u   /* 4K capable      */

/* Codec types, read off the validator's profile/level dispatch. */
#define CODEC_AVC  0x1u
#define CODEC_HEVC 0xee049u
#define CODEC_VP9  0x245bfdu

/* The 1080p AVC figure Phase 3 measured. Re-measured here as the control: if
 * this number moved, the module or the ABI reading changed and nothing else in
 * this log can be trusted. */
#define PHASE3_1080P_FRAMEPOOL 86507776ull

/* Modules to bring up.
 *
 * libSceGnmDriver is required and non-obvious: the compute queue bottoms out
 * in sceGnmMapComputeQueue, and without the module that call hangs silently
 * and forever. That cost Phase 4 a deploy.
 *
 * libSceSysmodule is required for the same class of reason, one layer further
 * out: sceVdecCoreCreateDecoder calls sceSysmoduleLoadModuleInternal on the
 * HEVC path. Phase 4's probe did not load it and did not need to, because it
 * never created a decoder.
 *
 * libSceAudiodec is deliberately absent: loading it hangs the payload when it
 * follows the video modules (reproduced twice on 2026-08-10). */
static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",   /* sceGnmMapComputeQueue lives here          */
    "libSceSysmodule.sprx",   /* VdecCore loads its codec module through it */
    "libSceAjm.sprx",         /* pairs with GnmDriver, per the Audiodec fix */
};

/* 0x48 bytes. Field names past thisSize are [I]: the offsets and the accepted
 * value ranges are read off the validator, the names are the reading that fits
 * them. */
typedef struct {
    uint64_t thisSize;                 /* +0x00  0x48 or 0x50                */
    uint32_t resourceType;             /* +0x08  RES_STD | RES_BIG | 1 | ... */
    uint32_t codecType;                /* +0x0c  CODEC_AVC | HEVC | VP9      */
    uint32_t profile;                  /* +0x10  codec-specific              */
    uint32_t maxLevel;                 /* +0x14  codec-specific              */
    uint32_t maxFrameWidth;            /* +0x18                              */
    uint32_t maxFrameHeight;           /* +0x1c                              */
    uint32_t maxDpbFrameCount;         /* +0x20  1..16    else 0x811D0209    */
    uint32_t decodePipelineDepth;      /* +0x24  1..8     else 0x811D0206    */
    void    *computeQueue;             /* +0x28  from AllocateComputeQueue   */
    uint64_t cpuAffinityMask;          /* +0x30  unread unless resourceType 1*/
    int32_t  cpuThreadPriority;        /* +0x38  256..767 or -1              */
    uint8_t  optimizeProgressiveVideo; /* +0x3c  inverted into the output    */
    uint8_t  checkMemoryType;          /* +0x3d  selects the checker's mode  */
    uint8_t  extraDecoderLatency;      /* +0x3e  must be 0 else 0x811D0200   */
    uint8_t  enableStorageIntegrity;   /* +0x3f  must be 0 else 0x811D0200   */
    void    *extraConfigInfo;          /* +0x40  NULL on the AVC path        */
} SceVideodec2DecoderConfigInfo;

/* 0x48 bytes. The query fills every size and then explicitly ZEROES +0x10,
 * +0x20, +0x30 and +0x44 - which is how we know those three are the caller's
 * to fill in and +0x44 must stay clear. */
typedef struct {
    uint64_t thisSize;         /* +0x00  0x48 exactly                        */
    uint64_t workMemorySize;   /* +0x08  out; includes the 0x40000 object    */
    void    *pWorkMemory;      /* +0x10  in;  intended WB_ONION              */
    uint64_t frameMemorySize;  /* +0x18  out; scales with resolution x DPB   */
    void    *pFrameMemory;     /* +0x20  in;  intended WC_GARLIC             */
    uint64_t extraMemorySize;  /* +0x28  out; never yet measured             */
    void    *pExtraMemory;     /* +0x30  in;  needed only if the size is > 0 */
    uint64_t mapMemorySize;    /* +0x38  out; has no pointer of its own      */
    uint32_t alignment;        /* +0x40  out = 256; low byte must be 0 in    */
    uint32_t reserved;         /* +0x44  must be 0 else 0x811D010C           */
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

/* The module checks these sizes itself and refuses with 0x811D0101 otherwise,
 * so a padding surprise would cost a deploy to discover. Prove it at compile
 * time instead, along with the two offsets the correction above turns on. */
_Static_assert(sizeof(SceVideodec2DecoderConfigInfo) == DECODER_CFG_SIZE,
               "config struct must be exactly 0x48 bytes");
_Static_assert(sizeof(SceVideodec2DecoderMemoryInfo) == DECODER_MEMINFO_SIZE,
               "memory-info struct must be exactly 0x48 bytes");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderConfigInfo, resourceType)
               == 0x08, "resource class is at cfg+0x08");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderConfigInfo, codecType)
               == 0x0c, "codec is at cfg+0x0c");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderConfigInfo, computeQueue)
               == 0x28, "compute queue is at cfg+0x28");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderConfigInfo, checkMemoryType)
               == 0x3d, "checkMemoryType is at cfg+0x3d");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderMemoryInfo, pFrameMemory)
               == 0x20, "frame buffer pointer is at mem+0x20");
_Static_assert(__builtin_offsetof(SceVideodec2DecoderMemoryInfo, reserved)
               == 0x44, "the must-be-zero word is at mem+0x44");

typedef int (*query_decoder_meminfo_fn)(const SceVideodec2DecoderConfigInfo *,
                                        SceVideodec2DecoderMemoryInfo *);
typedef int (*create_decoder_fn)(const SceVideodec2DecoderConfigInfo *,
                                 SceVideodec2DecoderMemoryInfo *,
                                 void **decoderOut);
typedef int (*delete_decoder_fn)(void *decoder);
typedef int (*query_compute_meminfo_fn)(SceVideodec2ComputeMemoryInfo *);
typedef int (*alloc_compute_queue_fn)(const SceVideodec2ComputeQueueInfo *,
                                      SceVideodec2ComputeMemoryInfo *,
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

/* Every 0x811Dxxxx code this probe can provoke, named from the disassembly.
 * A named refusal is a diagnosis; a bare hex code is a second deploy. */
static const char *
vdec2_err(uint32_t rc)
{
    switch (rc) {
    case 0x00000000: return "OK";
    case 0x811d0100: return "internal failure below the API";
    case 0x811d0101: return "wrong struct size";
    case 0x811d0102: return "bad pointer (module argument validation)";
    case 0x811d0103: return "not a decoder handle";
    case 0x811d0104: return "a caller size is below the computed requirement";
    case 0x811d0105: return "memory check failed / NULL buffer";
    case 0x811d0106: return "mapMemorySize below the computed requirement";
    case 0x811d0108: return "memInfo alignment field not clear";
    case 0x811d0109: return "wrong memory type - wanted WB_ONION";
    case 0x811d010a: return "wrong memory type - wanted WC_GARLIC";
    case 0x811d010b: return "memory not writable";
    case 0x811d010c: return "memInfo reserved field (+0x44) not zero";
    case 0x811d0110: return "release failed / codec-1 buffer check";
    case 0x811d0111: return "computed alignment was not 256";
    case 0x811d01ff: return "GnmDriver returned 0x80C00015";
    case 0x811d0200: return "invalid configuration (size computation refused)";
    case 0x811d0203: return "unsupported resource type (cfg+0x08)";
    case 0x811d0204: return "unsupported codec type (cfg+0x0c)";
    case 0x811d0205: return "unsupported profile or level for this codec";
    case 0x811d0206: return "decodePipelineDepth outside 1..8";
    case 0x811d0207: return "cpuAffinityMask out of range";
    case 0x811d0208: return "cpuThreadPriority outside 256..767";
    case 0x811d0209: return "maxDpbFrameCount outside 1..16";
    case 0x811d020b: return "extraConfigInfo must be NULL on this path";
    case 0x811d020c: return "VdecCore returned 0x80C00004";
    case 0x80020023: return "EAGAIN - out of direct memory budget";
    default:         return "";
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

/* Allocate and map. Returns 0 on success; on any failure the block is left
 * zeroed and nothing is held. */
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
                                  SCE_KERNEL_PROT_CPU_RW |
                                  SCE_KERNEL_PROT_GPU_ALL,
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

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

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
    /* checkMemoryType stays 0: the checker's gate is clear in a payload, so
     * setting it changes nothing except which error a broken mapping would
     * produce - and the mode it selects routes through sceKernelVirtualQuery,
     * which does not work here. */
}

/* ------------------------------------------------------------------------- */
/* The codec matrix - queries only, so this is as safe as Phase 3             */
/* ------------------------------------------------------------------------- */

static void
codec_matrix(query_decoder_meminfo_fn query)
{
    static const struct {
        const char *label;
        uint32_t    res, codec, profile, level, w, h, dpb;
    } kCases[] = {
        /* H.264. The six configurations Phase 3 accepted, as the control. */
        { "AVC  High  L5.1  1080p  RES_STD", RES_STD, CODEC_AVC,  100,  51, 1920, 1080, 16 },
        { "AVC  High  L5.1  1080p  RES_BIG", RES_BIG, CODEC_AVC,  100,  51, 1920, 1080, 16 },
        { "AVC  High  L5.1  4K     RES_BIG", RES_BIG, CODEC_AVC,  100,  51, 3840, 2160, 16 },
        { "AVC  Main  L4.0  1080p  RES_STD", RES_STD, CODEC_AVC,   77,  40, 1920, 1080, 16 },

        /* HEVC. profile 1 = Main, 2 = Main10; level is general_level_idc,
         * i.e. the HEVC level x 30. Phase 3 offered this codec a profile of
         * 100 and a level of 51 and read the resulting 0x811D0205 as a
         * refusal of the codec. These are the numbers it actually wants. */
        { "HEVC Main   L4.0 1080p  RES_STD", RES_STD, CODEC_HEVC,   1, 120, 1920, 1080, 16 },
        { "HEVC Main   L4.0 1080p  RES_BIG", RES_BIG, CODEC_HEVC,   1, 120, 1920, 1080, 16 },
        { "HEVC Main10 L4.0 1080p  RES_BIG", RES_BIG, CODEC_HEVC,   2, 120, 1920, 1080, 16 },
        { "HEVC Main   L5.1 4K     RES_BIG", RES_BIG, CODEC_HEVC,   1, 153, 3840, 2160, 16 },
        { "HEVC Main10 L5.1 4K     RES_BIG", RES_BIG, CODEC_HEVC,   2, 153, 3840, 2160, 16 },
        { "HEVC Main   L4.0 1088p  RES_BIG", RES_BIG, CODEC_HEVC,   1, 120, 1920, 1088, 16 },

        /* VP9. profile 0 = 8-bit 4:2:0, 2 = 10/12-bit; level x 10. */
        { "VP9  p0     L4.0 1080p  RES_BIG", RES_BIG, CODEC_VP9,    0,  40, 1920, 1080, 16 },
        { "VP9  p2     L5.1 4K     RES_BIG", RES_BIG, CODEC_VP9,    2,  51, 3840, 2160, 16 },

        /* Resource classes VdecCore rejects, asked at query level so the log
         * records where each one stops. */
        { "AVC  High  L5.1  1080p  res 1  ", 1,       CODEC_AVC,  100,  51, 1920, 1080, 16 },
        { "AVC  High  L5.1  1080p  0x24708", 0x24708, CODEC_AVC,  100,  51, 1920, 1080, 16 },
        { "AVC  High  L5.1  1080p  0x24709", 0x24709, CODEC_AVC,  100,  51, 1920, 1080, 16 },

        /* Deliberate errors, to prove the corrected field reading rather than
         * assume it. A bogus CODEC must give 0x811D0204 and a bogus RESOURCE
         * class must give 0x811D0203 - if those two came back swapped, the
         * correction above is wrong. */
        { "control: bogus codec  0xdead   ", RES_STD, 0xdeadu,    100,  51, 1920, 1080, 16 },
        { "control: bogus resource 0xdead ", 0xdeadu, CODEC_AVC,  100,  51, 1920, 1080, 16 },
        { "control: AVC profile 99        ", RES_STD, CODEC_AVC,   99,  51, 1920, 1080, 16 },
        { "control: HEVC given AVC prof/lv", RES_STD, CODEC_HEVC, 100,  51, 1920, 1080, 16 },
        { "control: DPB 17                ", RES_STD, CODEC_AVC,  100,  51, 1920, 1080, 17 },
    };

    LOG("\n--- codec matrix (queries only - nothing is allocated) ---\n");
    LOG("    cfg+0x08 is the RESOURCE class, cfg+0x0c is the CODEC.\n"
        "    Phase 3 had these two the other way round and therefore asked\n"
        "    HEVC and VP9 for H.264 profiles. This asks each codec properly.\n\n");
    LOG("  %-33s %10s %10s %10s %10s  %s\n",
        "configuration", "work+0x08", "frame+0x18", "extra+0x28", "map+0x38",
        "result");

    for (size_t i = 0; i < sizeof kCases / sizeof *kCases; i++) {
        SceVideodec2DecoderConfigInfo cfg;
        SceVideodec2DecoderMemoryInfo mem;
        int rc;

        cfg_init(&cfg, kCases[i].res, kCases[i].codec, kCases[i].profile,
                 kCases[i].level, kCases[i].w, kCases[i].h, kCases[i].dpb,
                 NULL);   /* the query never reads computeQueue */

        memset(&mem, 0, sizeof mem);
        mem.thisSize = DECODER_MEMINFO_SIZE;

        rc = query(&cfg, &mem);

        if (rc == 0)
            LOG("  %-33s %10llu %10llu %10llu %10llu  OK  (%.1f MiB, align %u)\n",
                kCases[i].label,
                (unsigned long long)mem.workMemorySize,
                (unsigned long long)mem.frameMemorySize,
                (unsigned long long)mem.extraMemorySize,
                (unsigned long long)mem.mapMemorySize,
                (double)(mem.workMemorySize + mem.frameMemorySize +
                         mem.extraMemorySize) / (1024.0 * 1024.0),
                mem.alignment);
        else
            LOG("  %-33s %10s %10s %10s %10s  0x%08x %s\n",
                kCases[i].label, "-", "-", "-", "-", rc, vdec2_err((uint32_t)rc));
    }
}

/* ------------------------------------------------------------------------- */
/* CreateDecoder validation controls                                         */
/* ------------------------------------------------------------------------- */

/* Each of these returns before the module touches hardware or allocates
 * anything, so they are free - and they are what makes a later refusal
 * readable rather than mysterious. Standing rule 3.
 *
 * `good` must be a configuration and memory-info pair that is otherwise ready
 * to succeed; each control perturbs exactly one field of a copy. */
static void
create_controls(create_decoder_fn create, delete_decoder_fn del,
                const SceVideodec2DecoderConfigInfo *good_cfg,
                const SceVideodec2DecoderMemoryInfo *good_mem)
{
    static const struct {
        const char *label;
        uint64_t    cfg_size, mem_size;
        uint32_t    reserved44;
        uint32_t    alignment;
        int64_t     work_delta, map_delta;
        int         null_work, null_frame, null_args;
        uint32_t    expect;
    } kControls[] = {
        { "all-NULL",              0x48, 0x48, 0, 256,  0,  0, 0, 0, 1, 0x811d0102 },
        { "memInfo size 0x40",     0x48, 0x40, 0, 256,  0,  0, 0, 0, 0, 0x811d0101 },
        { "cfg size 0x44",         0x44, 0x48, 0, 256,  0,  0, 0, 0, 0, 0x811d0101 },
        { "pWorkMemory NULL",      0x48, 0x48, 0, 256,  0,  0, 1, 0, 0, 0x811d0105 },
        { "pFrameMemory NULL",     0x48, 0x48, 0, 256,  0,  0, 0, 1, 0, 0x811d0105 },
        { "reserved +0x44 set",    0x48, 0x48, 1, 256,  0,  0, 0, 0, 0, 0x811d010c },
        { "workSize one byte low", 0x48, 0x48, 0, 256, -1,  0, 0, 0, 0, 0x811d0104 },
        { "mapSize one byte low",  0x48, 0x48, 0, 256,  0, -1, 0, 0, 0, 0x811d0106 },
        { "alignment 0x101",       0x48, 0x48, 0, 0x101, 0,  0, 0, 0, 0, 0x811d0108 },
    };

    LOG("\n--- CreateDecoder: validation controls ---\n");
    LOG("    Every one of these returns before the module allocates anything.\n"
        "    They cost nothing and they are what turns a later refusal into a\n"
        "    diagnosis instead of a second deploy.\n");

    for (size_t i = 0; i < sizeof kControls / sizeof *kControls; i++) {
        SceVideodec2DecoderConfigInfo cfg = *good_cfg;
        SceVideodec2DecoderMemoryInfo mem = *good_mem;
        void *dec = NULL;
        int   rc;

        if (kControls[i].null_args) {
            rc = create(NULL, NULL, NULL);
        } else {
            cfg.thisSize = kControls[i].cfg_size;
            mem.thisSize = kControls[i].mem_size;
            mem.reserved = kControls[i].reserved44;
            mem.alignment = kControls[i].alignment;
            mem.workMemorySize =
                (uint64_t)((int64_t)mem.workMemorySize + kControls[i].work_delta);
            mem.mapMemorySize =
                (uint64_t)((int64_t)mem.mapMemorySize + kControls[i].map_delta);
            if (kControls[i].null_work)
                mem.pWorkMemory = NULL;
            if (kControls[i].null_frame)
                mem.pFrameMemory = NULL;

            rc = create(&cfg, &mem, &dec);
        }

        LOG("  %-22s -> 0x%08x  expect 0x%08x  %s\n",
            kControls[i].label, rc, kControls[i].expect,
            (uint32_t)rc == kControls[i].expect ? "ok" : "*** DIFFERS ***");

        if (rc == 0 && dec) {
            LOG("    control unexpectedly SUCCEEDED - deleting\n");
            LOG("    sceVideodec2DeleteDecoder -> 0x%08x\n", del(dec));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* One create attempt, self-contained                                        */
/* ------------------------------------------------------------------------- */

/* Queries, allocates, creates, dumps, deletes, releases. Allocates and frees
 * its own buffers on every path, so an attempt that fails leaves nothing
 * behind for the next one. Returns 1 if a decoder was created.
 *
 * `controls` runs the validation battery once, using this attempt's own
 * ready-to-succeed argument pair. */
static int
attempt_create(query_decoder_meminfo_fn query, create_decoder_fn create,
               delete_decoder_fn del, const char *label,
               uint32_t resourceType, uint32_t codecType, uint32_t profile,
               uint32_t level, uint32_t w, uint32_t h, uint32_t dpb,
               void *queue, int controls)
{
    SceVideodec2DecoderConfigInfo cfg;
    SceVideodec2DecoderMemoryInfo mem;
    dmem_block work = {0}, frame = {0}, extra = {0};
    void      *dec = NULL;
    int        rc, ok = 0;

    LOG("\n=== create: %s ===\n", label);
    LOG("  resourceType 0x%x  codecType 0x%x  profile %u  level %u  %ux%u dpb %u\n",
        resourceType, codecType, profile, level, w, h, dpb);

    cfg_init(&cfg, resourceType, codecType, profile, level, w, h, dpb, queue);

    /* -- 1. what does this configuration need? ---------------------------- */
    memset(&mem, 0, sizeof mem);
    mem.thisSize = DECODER_MEMINFO_SIZE;

    rc = query(&cfg, &mem);
    LOG("  QueryDecoderMemoryInfo -> 0x%08x %s\n", rc, vdec2_err((uint32_t)rc));
    if (rc)
        return 0;

    LOG("    memory info, all 0x48 bytes:\n");
    hexdump(&mem, sizeof mem, "      ");
    LOG("    work  +0x08 %12llu   ptr +0x10 %p\n",
        (unsigned long long)mem.workMemorySize, mem.pWorkMemory);
    LOG("    frame +0x18 %12llu   ptr +0x20 %p\n",
        (unsigned long long)mem.frameMemorySize, mem.pFrameMemory);
    LOG("    extra +0x28 %12llu   ptr +0x30 %p\n",
        (unsigned long long)mem.extraMemorySize, mem.pExtraMemory);
    LOG("    map   +0x38 %12llu   align +0x40 %u   reserved +0x44 %u\n",
        (unsigned long long)mem.mapMemorySize, mem.alignment, mem.reserved);

    if (mem.alignment != 256)
        LOG("    *** alignment is not 256 - CreateDecoder will return "
            "0x811D0111 ***\n");

    /* -- 2. the buffers --------------------------------------------------- */
    LOG("  allocating %.1f MiB\n",
        (double)(mem.workMemorySize + mem.frameMemorySize +
                 mem.extraMemorySize) / (1024.0 * 1024.0));

    rc = dmem_get(&work, mem.workMemorySize, SCE_KERNEL_WB_ONION);
    LOG("    work  %8llu B  WB_ONION  -> 0x%08x  %s virt %p\n",
        (unsigned long long)mem.workMemorySize, rc, vdec2_err((uint32_t)rc),
        work.vaddr);
    if (rc)
        goto out;

    rc = dmem_get(&frame, mem.frameMemorySize, SCE_KERNEL_WC_GARLIC);
    LOG("    frame %8llu B  WC_GARLIC -> 0x%08x  %s virt %p\n",
        (unsigned long long)mem.frameMemorySize, rc, vdec2_err((uint32_t)rc),
        frame.vaddr);
    if (rc)
        goto out;

    if (mem.extraMemorySize) {
        rc = dmem_get(&extra, mem.extraMemorySize, SCE_KERNEL_WB_ONION);
        LOG("    extra %8llu B  WB_ONION  -> 0x%08x  %s virt %p\n",
            (unsigned long long)mem.extraMemorySize, rc,
            vdec2_err((uint32_t)rc), extra.vaddr);
        if (rc)
            goto out;
    } else {
        LOG("    extra          0 B            -> not needed, pointer stays NULL\n");
    }

    mem.pWorkMemory  = work.vaddr;
    mem.pFrameMemory = frame.vaddr;
    mem.pExtraMemory = extra.vaddr;

    /* The module writes its object into the front of the work buffer. Prove
     * the CPU can reach it first: a fault here is a mapping problem, and it is
     * much better to learn that now than to blame the decoder for it. */
    {
        volatile uint8_t *p = work.vaddr;

        p[0] = 0xa5;
        p[DECODER_OBJECT_SIZE - 1] = 0x5a;
        LOG("    cpu write/read back at work[0] and work[0x3ffff]: 0x%02x 0x%02x\n",
            p[0], p[DECODER_OBJECT_SIZE - 1]);
    }
    memset(work.vaddr, 0, DECODER_OBJECT_SIZE);

    if (controls)
        create_controls(create, del, &cfg, &mem);

    /* -- 3. the real call ------------------------------------------------- */
    LOG("\n  sceVideodec2CreateDecoder ... ");
    rc = create(&cfg, &mem, &dec);
    LOG("rc=0x%08x %s decoder=%p\n", rc, vdec2_err((uint32_t)rc), dec);

    if (rc || !dec)
        goto out;

    ok = 1;
    LOG("\n  *** DECODER CREATED ***\n");

    /* The object is the front of our own work buffer, so this is a read of
     * memory we allocated - not a speculative dereference of module memory. */
    if (dec != work.vaddr)
        LOG("    NOTE: handle %p is not pWorkMemory %p - the object moved\n",
            dec, work.vaddr);

    {
        const uint8_t  *d  = dec;
        uint64_t        m0, m1, cq;
        uint32_t        res, chk, codec, depth;
        void           *core;

        memcpy(&m0,    d + 0x00,  sizeof m0);
        memcpy(&depth, d + 0x58,  sizeof depth);
        memcpy(&res,   d + 0x5c,  sizeof res);
        memcpy(&chk,   d + 0x60,  sizeof chk);
        memcpy(&codec, d + 0x64,  sizeof codec);
        memcpy(&m1,    d + 0x68,  sizeof m1);
        memcpy(&core,  d + 0x78,  sizeof core);
        memcpy(&cq,    d + 0x128, sizeof cq);

        LOG("    +0x00 magic        0x%016llx  %s\n", (unsigned long long)m0,
            m0 == DECODER_MAGIC0 ? "\"U5JD7RL\", as read" : "*** UNEXPECTED ***");
        LOG("    +0x58 pipelineDepth %u  (4 requested; the validator clamps >=6 to 5)\n",
            depth);
        LOG("    +0x5c resourceType  0x%x  %s\n", res,
            res == resourceType ? "matches the request" : "*** DIFFERS ***");
        LOG("    +0x60 checkMemType  %u\n", chk);
        LOG("    +0x64 codec index   %u  (0 = H.264, 4 = HEVC, 6 = VP9)\n", codec);
        LOG("    +0x68 magic        0x%016llx  %s\n", (unsigned long long)m1,
            m1 == DECODER_MAGIC1 ? "as read; MapMemory checks this one"
                                 : "*** UNEXPECTED ***");
        LOG("    +0x78 VdecCore obj  %p  %s\n", core,
            core ? "the layer below built something" : "*** NULL ***");
        LOG("    +0x128 computeQueue 0x%llx  %s\n", (unsigned long long)cq,
            (void *)(uintptr_t)cq == queue ? "the queue we passed in"
                                           : "*** DIFFERS ***");
        LOG("    first 0x140 bytes of the decoder object:\n");
        hexdump(dec, 0x140, "      ");
    }

out:
    /* Standing rule: never leave a decoder alive across a deploy. */
    if (dec) {
        rc = del(dec);
        LOG("\n  sceVideodec2DeleteDecoder -> 0x%08x %s\n", rc,
            vdec2_err((uint32_t)rc));
        if (rc)
            LOG("    *** the decoder did NOT delete cleanly ***\n");
    }

    dmem_put(&extra);
    dmem_put(&frame);
    dmem_put(&work);
    LOG("  buffers released\n");

    return ok;
}

/* ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    int    want_hevc = 0;
    int    have_control = 0;
    int    queue_ok = 0;
    int    created_std = 0, created_big = 0, created_hevc = 0;
    size_t compute_size = 0;

    dmem_block cq_mem = {0};
    void      *queue  = NULL;

    for (int i = 1; i < argc; i++)
        if (argv[i] && strcmp(argv[i], "hevc") == 0)
            want_hevc = 1;

    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player createdecoder_test - Phase 5 ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", getpid());
    LOG("hevc create: %s\n\n", want_hevc ? "ENABLED (argv)" : "off - pass \"hevc\" to enable");

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

    intptr_t a_query   = resolve(dynh, base,
                                 "sceVideodec2QueryDecoderMemoryInfo", 0xa10);
    intptr_t a_create  = resolve(dynh, base,
                                 "sceVideodec2CreateDecoder", 0xba0);
    intptr_t a_delete  = resolve(dynh, base,
                                 "sceVideodec2DeleteDecoder", 0x3220);
    intptr_t a_query_cm = resolve(dynh, base,
                                  "sceVideodec2QueryComputeMemoryInfo", 0x550);
    intptr_t a_alloc_cq = resolve(dynh, base,
                                  "sceVideodec2AllocateComputeQueue", 0x660);
    intptr_t a_rel_cq   = resolve(dynh, base,
                                  "sceVideodec2ReleaseComputeQueue", 0x970);

    /* Resolved but deliberately not called: CreateHevcDecoder is the same two
     * instructions as CreateDecoder, jumping to the same body. Logging its
     * offset records that alias rather than asserting it. */
    (void)resolve(dynh, base, "sceVideodec2CreateHevcDecoder", 0x1230);

    if (!a_query || !a_create || !a_delete || !a_query_cm || !a_alloc_cq ||
        !a_rel_cq) {
        LOG("\nFATAL: an entry point did not resolve\n");
        return EXIT_FAILURE;
    }

    query_decoder_meminfo_fn query    = (query_decoder_meminfo_fn)a_query;
    create_decoder_fn        create   = (create_decoder_fn)a_create;
    delete_decoder_fn        del      = (delete_decoder_fn)a_delete;
    query_compute_meminfo_fn query_cm = (query_compute_meminfo_fn)a_query_cm;
    alloc_compute_queue_fn   alloc_cq = (alloc_compute_queue_fn)a_alloc_cq;
    release_compute_queue_fn rel_cq   = (release_compute_queue_fn)a_rel_cq;

    /* -- CONTROL: the Phase 3 call, re-measured ---------------------------
     * Standing rule 3. If this stops returning the number Phase 3 recorded,
     * nothing below this line means anything. */
    LOG("\n--- control: the Phase 3 query, AVC 1080p dpb16 ---\n");
    {
        SceVideodec2DecoderConfigInfo cfg;
        SceVideodec2DecoderMemoryInfo mem;
        int rc;

        cfg_init(&cfg, RES_STD, CODEC_AVC, 100, 51, 1920, 1080, 16, NULL);
        memset(&mem, 0, sizeof mem);
        mem.thisSize = DECODER_MEMINFO_SIZE;

        rc = query(&cfg, &mem);
        LOG("  rc=0x%08x  frame pool +0x18 = %llu  (Phase 3: %llu)  %s\n",
            rc, (unsigned long long)mem.frameMemorySize,
            (unsigned long long)PHASE3_1080P_FRAMEPOOL,
            (rc == 0 && mem.frameMemorySize == PHASE3_1080P_FRAMEPOOL)
                ? "MATCH" : "*** DIVERGED ***");
        have_control = (rc == 0 &&
                        mem.frameMemorySize == PHASE3_1080P_FRAMEPOOL);
    }

    /* -- the codec matrix: free, and it settles the HEVC question ---------- */
    codec_matrix(query);

    /* -- the compute queue, exactly as Phase 4 proved it ------------------- */
    LOG("\n--- compute queue ---\n");
    {
        SceVideodec2ComputeMemoryInfo mi;
        SceVideodec2ComputeQueueInfo  qi;
        int rc;

        memset(&mi, 0, sizeof mi);
        mi.thisSize = COMPUTE_MEMINFO_SIZE;
        rc = query_cm(&mi);
        LOG("  QueryComputeMemoryInfo -> 0x%08x  size %llu (%.2f MiB)\n", rc,
            (unsigned long long)mi.cpuGpuMemorySize,
            (double)mi.cpuGpuMemorySize / (1024.0 * 1024.0));
        if (rc)
            goto done;

        compute_size = (size_t)mi.cpuGpuMemorySize;

        rc = dmem_get(&cq_mem, compute_size, SCE_KERNEL_WB_ONION);
        LOG("  queue memory %zu B WB_ONION -> 0x%08x  virt %p\n",
            compute_size, rc, cq_mem.vaddr);
        if (rc)
            goto done;
        memset(cq_mem.vaddr, 0, cq_mem.len);

        memset(&qi, 0, sizeof qi);
        qi.thisSize = COMPUTE_QUEUEINFO_SIZE;

        mi.cpuGpuMemorySize = compute_size;
        mi.cpuGpuMemory     = cq_mem.vaddr;

        rc = alloc_cq(&qi, &mi, &queue);
        LOG("  AllocateComputeQueue(pipe 0, queue 0) -> 0x%08x  handle %p\n",
            rc, queue);
        queue_ok = (rc == 0 && queue != NULL);
        if (!queue_ok) {
            LOG("  no compute queue - CreateDecoder would be passed a NULL one,\n"
                "  which VdecCore does not check. Stopping here rather than\n"
                "  faulting the app slot.\n");
            goto done;
        }
    }

    /* -- 1080p H.264, the proven configuration ---------------------------- */
    created_std = attempt_create(query, create, del,
                                 "AVC 1080p, resource class 0xb6c8",
                                 RES_STD, CODEC_AVC, 100, 51, 1920, 1080, 16,
                                 queue, /*controls=*/1);

    /* -- the same thing on the 4K-capable class --------------------------- */
    created_big = attempt_create(query, create, del,
                                 "AVC 1080p, resource class 0x12384",
                                 RES_BIG, CODEC_AVC, 100, 51, 1920, 1080, 16,
                                 queue, /*controls=*/0);

    /* -- HEVC, opt-in ------------------------------------------------------
     * Last, and only when asked for. sceVdecCoreCreateDecoder reaches
     * sceSysmoduleLoadModuleInternal on this path; a module load is the one
     * thing here that has previously hung rather than failed. Everything above
     * is already in the log by the time this runs. */
    if (want_hevc) {
        created_hevc = attempt_create(query, create, del,
                                      "HEVC Main 1080p, resource class 0x12384",
                                      RES_BIG, CODEC_HEVC, 1, 120, 1920, 1080,
                                      16, queue, /*controls=*/0);
    } else {
        LOG("\n=== HEVC create skipped ===\n"
            "  Re-run with --args \"eboot.elf hevc\" once the AVC result above\n"
            "  is recorded. It is separated because VdecCore loads its codec\n"
            "  module through sceSysmodule on that path, and an unresolved\n"
            "  lazy import blocks silently and indefinitely.\n");
    }

done:
    if (queue) {
        int rc = rel_cq(queue);
        LOG("\nReleaseComputeQueue -> 0x%08x\n", rc);
    }
    dmem_put(&cq_mem);

    /* -- what this run established ---------------------------------------- */
    LOG("\n=== summary ===\n");
    LOG("  Phase 3 control re-measured : %s\n",
        have_control ? "MATCH" : "DIVERGED - distrust everything above");
    LOG("  compute queue               : %s\n",
        queue_ok ? "allocated" : "NOT OBTAINED");
    LOG("  CreateDecoder AVC 0xb6c8    : %s\n",
        created_std ? "*** SUCCEEDED ***" : "refused");
    LOG("  CreateDecoder AVC 0x12384   : %s\n",
        created_big ? "*** SUCCEEDED ***" : "refused");
    LOG("  CreateDecoder HEVC 0x12384  : %s\n",
        want_hevc ? (created_hevc ? "*** SUCCEEDED ***" : "refused")
                  : "not attempted");

    if (!created_std && !created_big)
        LOG("\n  Reading the refusal:\n"
            "    identical code for a correct AND a deliberately wrong config\n"
            "               -> permission, not configuration.\n"
            "    0x811D0102 on non-NULL pointers -> the module's argument\n"
            "               validation is enabled and runs through\n"
            "               sceKernelVirtualQuery, which is broken in a\n"
            "               payload. Not a permissions problem.\n"
            "    0x811D0104/0106 -> a size was below the computed requirement;\n"
            "               re-read the memory-info dump above.\n"
            "    0x811D0109/010A -> the memory-type check IS enabled after all,\n"
            "               and wants WB_ONION for work / WC_GARLIC for frame.\n"
            "    0x811D0100 -> the layer below refused. Compare the AVC and\n"
            "               HEVC rows: if AVC works and HEVC does not, that is\n"
            "               the entitlement gate, and AVC-only hardware decode\n"
            "               is still a large win.\n");

    if (created_std || created_big)
        LOG("\n  Next: Phase 6 needs sceVideodec2MapMemory / MapDirectMemory,\n"
            "  which check the 0x%016llx cookie at decoder+0x68 that this run\n"
            "  has now seen written, plus an Annex-B access unit on the stick.\n",
            (unsigned long long)DECODER_MAGIC1);

    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);

    evo_notify("EVO createdecoder_test: AVC %s, HEVC %s",
               (created_std || created_big) ? "CREATED" : "refused",
               !want_hevc ? "skipped" : created_hevc ? "CREATED" : "refused");
    return EXIT_SUCCESS;
}
