/*
 * EVO Player - avplayer_test
 *
 * Hardware Video Decoding Research Harness with Inline Hooking & Auto-Mapping.
 * Captures native hardware-decoded video frames from PS5 libSceAvPlayer / libSceVideodec2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

/* --------------------------------------------------------------------------
 * Direct Memory and Kernel Declarations
 * -------------------------------------------------------------------------- */
extern int sceKernelAllocateMainDirectMemory(size_t length, size_t alignment,
                                             int memoryType, off_t *physAddr);
extern int sceKernelMapDirectMemory(void **addr, size_t length, int protection,
                                    int flags, off_t physAddr, size_t alignment);
extern int sceKernelReleaseDirectMemory(off_t physAddr, size_t length);
extern int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                                    uint32_t flags, void *opt, int *res);

extern uint64_t kernel_get_ucred_authid(pid_t pid);
extern int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
extern int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
extern int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

/* --------------------------------------------------------------------------
 * Route A: libSceAvPlayer ABI Definitions
 * -------------------------------------------------------------------------- */

typedef struct {
    void *objectPointer;
    void* (*allocate)(void *obj, uint32_t align, uint32_t size);
    void  (*deallocate)(void *obj, void *ptr);
    void* (*allocateTexture)(void *obj, uint32_t align, uint32_t size);
    void  (*deallocateTexture)(void *obj, void *ptr);
} SceAvPlayerMemAllocator;

typedef struct {
    void *objectPointer;
    int  (*open)(void *obj, const char *path);
    int  (*close)(void *obj);
    int  (*readOffset)(void *obj, uint8_t *buf, uint64_t pos, uint32_t len);
    uint64_t (*size)(void *obj);
} SceAvPlayerFileReplacement;

typedef struct {
    void *objectPointer;
    void (*eventCallback)(void *obj, int eventId, int sourceId, void *eventData);
} SceAvPlayerEventReplacement;

typedef struct {
    SceAvPlayerMemAllocator     memoryReplacement;
    SceAvPlayerFileReplacement  fileReplacement;
    SceAvPlayerEventReplacement eventReplacement;
    int32_t                     debugLevel;
    uint32_t                    basePriority;
    int32_t                     numOutputVideoFrameBuffers;
    uint8_t                     autoStart;
    uint8_t                     reserved0;
    uint8_t                     reserved1;
    uint8_t                     reserved2;
    const char                 *defaultLanguage;
} SceAvPlayerInitData;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint8_t  details[32];
    uint64_t duration;
} SceAvPlayerStreamInfo;

typedef struct {
    uint8_t                 *data;
    uint32_t                 reserved0;
    uint32_t                 reserved1;
    uint64_t                 timeStamp;
    uint8_t                  details[32];
} SceAvPlayerFrameInfo;

typedef struct {
    void    *data;              /* Offset 0x00: NV12 plane pointer */
    uint64_t reserved0;         /* Offset 0x08 */
    uint64_t timeStamp;         /* Offset 0x10 */
    uint32_t videoWidth;        /* Offset 0x18 */
    uint32_t videoHeight;       /* Offset 0x1c */
    uint64_t reserved1;         /* Offset 0x20 */
    uint64_t reserved2;         /* Offset 0x28 */
    uint32_t cropLeft;          /* Offset 0x2c (44) */
    uint32_t cropRight;         /* Offset 0x30 (48) */
    uint32_t cropTop;           /* Offset 0x34 (52) */
    uint32_t cropBottom;        /* Offset 0x38 (56) */
    uint32_t videoPitch;        /* Offset 0x3c (60) */
    uint8_t  padding[40];       /* Offset 0x40..0x68 (total 104 bytes) */
} SceAvPlayerFrameInfoEx;

typedef void* (*fn_sceAvPlayerInit)(const SceAvPlayerInitData *data);
typedef int   (*fn_sceAvPlayerAddSource)(void *handle, const char *path);
typedef int   (*fn_sceAvPlayerStart)(void *handle);
typedef int   (*fn_sceAvPlayerStop)(void *handle);
typedef int   (*fn_sceAvPlayerPause)(void *handle);
typedef int   (*fn_sceAvPlayerResume)(void *handle);
typedef bool  (*fn_sceAvPlayerIsActive)(void *handle);
typedef int   (*fn_sceAvPlayerClose)(void *handle);
typedef int   (*fn_sceAvPlayerStreamCount)(void *handle);
typedef int   (*fn_sceAvPlayerGetStreamInfo)(void *handle, uint32_t streamIndex, SceAvPlayerStreamInfo *info);
typedef int   (*fn_sceAvPlayerEnableStream)(void *handle, uint32_t streamIndex);
typedef int   (*fn_sceAvPlayerDisableStream)(void *handle, uint32_t streamIndex);
typedef bool  (*fn_sceAvPlayerGetVideoData)(void *handle, SceAvPlayerFrameInfo *info);
typedef bool  (*fn_sceAvPlayerGetVideoDataEx)(void *handle, SceAvPlayerFrameInfoEx *info);
typedef bool  (*fn_sceAvPlayerGetAudioData)(void *handle, SceAvPlayerFrameInfo *info);
typedef uint64_t (*fn_sceAvPlayerCurrentTime)(void *handle);
typedef int   (*fn_sceAvPlayerSetLooping)(void *handle, bool loop);

static fn_sceAvPlayerInit            p_sceAvPlayerInit = NULL;
static fn_sceAvPlayerAddSource       p_sceAvPlayerAddSource = NULL;
static fn_sceAvPlayerStart           p_sceAvPlayerStart = NULL;
static fn_sceAvPlayerStop            p_sceAvPlayerStop = NULL;
static fn_sceAvPlayerPause           p_sceAvPlayerPause = NULL;
static fn_sceAvPlayerResume          p_sceAvPlayerResume = NULL;
static fn_sceAvPlayerIsActive        p_sceAvPlayerIsActive = NULL;
static fn_sceAvPlayerClose           p_sceAvPlayerClose = NULL;
static fn_sceAvPlayerStreamCount     p_sceAvPlayerStreamCount = NULL;
static fn_sceAvPlayerGetStreamInfo   p_sceAvPlayerGetStreamInfo = NULL;
static fn_sceAvPlayerEnableStream    p_sceAvPlayerEnableStream = NULL;
static fn_sceAvPlayerDisableStream   p_sceAvPlayerDisableStream = NULL;
static fn_sceAvPlayerGetVideoData    p_sceAvPlayerGetVideoData = NULL;
static fn_sceAvPlayerGetVideoDataEx  p_sceAvPlayerGetVideoDataEx = NULL;
static fn_sceAvPlayerGetAudioData    p_sceAvPlayerGetAudioData = NULL;
static fn_sceAvPlayerCurrentTime     p_sceAvPlayerCurrentTime = NULL;
static fn_sceAvPlayerSetLooping      p_sceAvPlayerSetLooping = NULL;

/* --------------------------------------------------------------------------
 * Videodec2 Struct Definitions & Inline Hook Trampolines
 * -------------------------------------------------------------------------- */

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
    uint64_t thisSize;   /* +0x00: exactly 0x20 */
    uint64_t size;       /* +0x08: size of buffer */
    void    *addr;       /* +0x10: virtual address */
    uint64_t physAddr;   /* +0x18: direct memory physical address */
} SceVideodec2MapDirectMemoryInfo;
_Static_assert(sizeof(SceVideodec2MapDirectMemoryInfo) == 0x20, "mapinfo 0x20");

typedef struct {
    uint64_t thisSize;       /* +0x00  must be 0x30 */
    const void *auData;      /* +0x08 */
    uint64_t auSize;         /* +0x10 */
    uint64_t ptsData;        /* +0x18 */
    uint64_t dtsData;        /* +0x20 */
    uint64_t attachedData;   /* +0x28 */
} SceVideodec2InputData;

typedef struct {
    uint64_t thisSize;       /* +0x00  must be 0x20 */
    void    *frameBuffer;    /* +0x08 */
    uint64_t frameBufferSize;/* +0x10 */
    uint8_t  isAccepted;     /* +0x18 */
    uint8_t  pad[7];
} SceVideodec2FrameBuffer;

typedef struct {
    uint64_t thisSize;        /* +0x00  0x38 */
    uint8_t  isValid;         /* +0x08 */
    uint8_t  isErrorFrame;    /* +0x09 */
    uint8_t  pictureCount;    /* +0x0a */
    uint8_t  streamState;     /* +0x0b */
    uint32_t codecType;       /* +0x0c */
    uint64_t ptsData;         /* +0x10 */
    uint32_t word18;          /* +0x18 */
    uint32_t pad1c;
    void    *frameBuffer;     /* +0x20 */
    uint64_t frameBufferSize; /* +0x28 */
    uint32_t word30;          /* +0x30 */
    uint32_t word34;          /* +0x34 */
} SceVideodec2OutputInfo;

typedef int (*fn_QueryDecoderMemoryInfo)(const SceVideodec2DecoderConfigInfo *cfg, SceVideodec2DecoderMemoryInfo *mem);
typedef int (*fn_CreateDecoder)(const SceVideodec2DecoderConfigInfo *cfg, const SceVideodec2DecoderMemoryInfo *mem, void **decoder);
typedef int (*fn_MapDirectMemory)(void *decoder, const SceVideodec2MapDirectMemoryInfo *info);
typedef int (*fn_Decode)(void *decoder, SceVideodec2InputData *in, SceVideodec2FrameBuffer *fb, SceVideodec2OutputInfo *out);

static uint8_t g_tramp_query[64] __attribute__((aligned(4096)));
static uint8_t g_tramp_create[64] __attribute__((aligned(4096)));
static uint8_t g_tramp_decode[64] __attribute__((aligned(4096)));

static fn_QueryDecoderMemoryInfo g_orig_query = NULL;
static fn_CreateDecoder          g_orig_create = NULL;
static fn_MapDirectMemory        p_sceVideodec2MapDirectMemory = NULL;
static fn_Decode                 g_orig_decode = NULL;

static void *g_mapped_au_buf = NULL;
static off_t g_mapped_au_phys = 0;

/* --------------------------------------------------------------------------
 * Direct Memory Allocators (sceKernelAllocateMainDirectMemory)
 * -------------------------------------------------------------------------- */

typedef struct {
    void   *address;
    off_t   phys;
    size_t  size;
    bool    is_direct;
    bool    is_texture;
} MemSlot;

#define MAX_MEM_SLOTS 256
static MemSlot         g_mem_slots[MAX_MEM_SLOTS];
static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;

#define SCE_KERNEL_WB_ONION  0
#define SCE_KERNEL_WC_GARLIC 3

static void* direct_alloc(uint32_t align, uint32_t size, const char *tag, bool is_tex)
{
    if (size == 0) return NULL;
    if (align < 0x4000) align = 0x4000;
    size_t bytes = (size + align - 1) & ~(align - 1);

    off_t phys = 0;
    int memType = is_tex ? SCE_KERNEL_WC_GARLIC : SCE_KERNEL_WB_ONION;
    int ret = sceKernelAllocateMainDirectMemory(bytes, align, memType, &phys);
    if (ret != 0) {
        printf("  [%s] sceKernelAllocateMainDirectMemory FAILED: bytes=%zu, align=%u, type=%d, ret=0x%x\n",
               tag, bytes, align, memType, ret);
        fflush(stdout);
        return NULL;
    }

    void *mapped = NULL;
    ret = sceKernelMapDirectMemory(&mapped, bytes, 0x33, 0, phys, align);
    if (ret != 0 || mapped == NULL) {
        printf("  [%s] sceKernelMapDirectMemory FAILED: ret=0x%x\n", tag, ret);
        fflush(stdout);
        sceKernelReleaseDirectMemory(phys, bytes);
        return NULL;
    }

    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (g_mem_slots[i].address == NULL) {
            g_mem_slots[i].address = mapped;
            g_mem_slots[i].phys = phys;
            g_mem_slots[i].size = bytes;
            g_mem_slots[i].is_direct = true;
            g_mem_slots[i].is_texture = is_tex;
            pthread_mutex_unlock(&g_mem_lock);
            printf("  [%s] DirectMem mapped: %p (bytes=%zu, phys=0x%lx, is_tex=%d)\n",
                   tag, mapped, bytes, (unsigned long)phys, is_tex);
            fflush(stdout);
            return mapped;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);

    munmap(mapped, bytes);
    sceKernelReleaseDirectMemory(phys, bytes);
    return NULL;
}

static void* cb_alloc_general(void *obj, uint32_t align, uint32_t size)
{
    (void)obj;
    void *ptr = direct_alloc(align, size, "AllocGeneral-Direct", false);
    if (ptr) return ptr;

    if (align < 32) align = 32;
    if (posix_memalign(&ptr, align, size) != 0) {
        ptr = malloc(size);
    }
    if (ptr) {
        pthread_mutex_lock(&g_mem_lock);
        for (int i = 0; i < MAX_MEM_SLOTS; i++) {
            if (g_mem_slots[i].address == NULL) {
                g_mem_slots[i].address = ptr;
                g_mem_slots[i].phys = 0;
                g_mem_slots[i].size = size;
                g_mem_slots[i].is_direct = false;
                g_mem_slots[i].is_texture = false;
                break;
            }
        }
        pthread_mutex_unlock(&g_mem_lock);
    }
    return ptr;
}

static void cb_deallocate_general(void *obj, void *ptr)
{
    (void)obj;
    if (!ptr) return;

    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (g_mem_slots[i].address == ptr) {
            bool is_direct = g_mem_slots[i].is_direct;
            void *addr = g_mem_slots[i].address;
            off_t phys = g_mem_slots[i].phys;
            size_t size = g_mem_slots[i].size;
            g_mem_slots[i].address = NULL;
            g_mem_slots[i].phys = 0;
            g_mem_slots[i].size = 0;
            g_mem_slots[i].is_direct = false;
            g_mem_slots[i].is_texture = false;
            pthread_mutex_unlock(&g_mem_lock);

            if (is_direct) {
                munmap(addr, size);
                sceKernelReleaseDirectMemory(phys, size);
            } else {
                free(addr);
            }
            return;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
    free(ptr);
}

static void* cb_alloc_texture(void *obj, uint32_t align, uint32_t size)
{
    (void)obj;
    return direct_alloc(align, size, "AllocTexture", true);
}

static void cb_deallocate_texture(void *obj, void *ptr)
{
    cb_deallocate_general(obj, ptr);
}

/* --------------------------------------------------------------------------
 * BMP Frame Saver for NV12 Decoded Pictures
 * -------------------------------------------------------------------------- */

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void save_nv12_to_bmp(const char *filepath, const uint8_t *nv12_data,
                             int width, int height, int pitch,
                             int cropLeft, int cropTop, int cropRight, int cropBottom)
{
    int visW = pitch - cropLeft - cropRight;
    int visH = height - cropTop - cropBottom;
    if (visW <= 0 || visH <= 0 || !nv12_data) return;

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        printf("  [BMP] Failed to open %s for writing\n", filepath);
        fflush(stdout);
        return;
    }

    int row_stride = (visW * 3 + 3) & ~3;
    uint32_t image_size = (uint32_t)row_stride * visH;
    uint32_t file_size = 54 + image_size;

    uint8_t header[54] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        (uint8_t)(visW), (uint8_t)(visW >> 8), (uint8_t)(visW >> 16), (uint8_t)(visW >> 24),
        (uint8_t)(visH), (uint8_t)(visH >> 8), (uint8_t)(visH >> 16), (uint8_t)(visH >> 24),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0x13, 0x0B, 0, 0,
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    fwrite(header, 1, 54, fp);

    uint8_t *row_buf = (uint8_t*)malloc(row_stride);
    if (!row_buf) {
        fclose(fp);
        return;
    }

    const uint8_t *y_plane = nv12_data;
    const uint8_t *uv_plane = nv12_data + (pitch * height);

    for (int y = visH - 1; y >= 0; y--) {
        int src_y = y + cropTop;
        const uint8_t *y_line = y_plane + src_y * pitch + cropLeft;
        const uint8_t *uv_line = uv_plane + (src_y / 2) * pitch + (cropLeft & ~1);

        for (int x = 0; x < visW; x++) {
            int Y = y_line[x];
            int U = uv_line[(x / 2) * 2] - 128;
            int V = uv_line[(x / 2) * 2 + 1] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            row_buf[x * 3 + 0] = clamp_u8(B);
            row_buf[x * 3 + 1] = clamp_u8(G);
            row_buf[x * 3 + 2] = clamp_u8(R);
        }
        for (int p = visW * 3; p < row_stride; p++) {
            row_buf[p] = 0;
        }
        fwrite(row_buf, 1, row_stride, fp);
    }

    free(row_buf);
    fclose(fp);
    printf("  [BMP] Saved hardware decoded frame: %s (%dx%d, %u bytes)\n",
           filepath, visW, visH, file_size);
    fflush(stdout);
}

/* --------------------------------------------------------------------------
 * Videodec2 Inline Hook Trampolines
 * -------------------------------------------------------------------------- */

static uint8_t g_tramp_query[64]  __attribute__((aligned(4096)));
static uint8_t g_tramp_create[64] __attribute__((aligned(4096)));
static uint8_t g_tramp_decode[64] __attribute__((aligned(4096)));

typedef int (*fn_QueryMem)(SceVideodec2DecoderConfigInfo *cfg, SceVideodec2DecoderMemoryInfo *mem);
typedef int (*fn_CreateDec)(SceVideodec2DecoderConfigInfo *cfg, const SceVideodec2DecoderMemoryInfo *mem, void **decoder);
typedef int (*fn_Decode)(void *decoder, SceVideodec2InputData *in, SceVideodec2FrameBuffer *fb, SceVideodec2OutputInfo *out);

static int hook_QueryDecoderMemoryInfo(SceVideodec2DecoderConfigInfo *cfg, SceVideodec2DecoderMemoryInfo *mem)
{
    uint32_t orig_res = cfg ? cfg->resourceType : 0;
    if (cfg && cfg->resourceType != 0xb6c8 && cfg->resourceType != 0x12384) {
        cfg->resourceType = 0xb6c8;
    }

    fn_QueryMem call_tramp = (fn_QueryMem)g_tramp_query;
    int rc = call_tramp(cfg, mem);
    printf("  >>> [INLINE HOOK Query] codec=%u orig_res=0x%x (W=%u H=%u) -> setting res=0x%x <<<\n",
           cfg ? cfg->codecType : 0, orig_res, cfg ? cfg->maxFrameWidth : 0, cfg ? cfg->maxFrameHeight : 0,
           cfg ? cfg->resourceType : 0);
    printf("  >>> [INLINE HOOK Query] rc=0x%08x (work=%llu pool=%llu map=%llu) <<<\n",
           rc, mem ? (unsigned long long)mem->workMemorySize : 0,
           mem ? (unsigned long long)mem->frameMemorySize : 0,
           mem ? (unsigned long long)mem->mapMemorySize : 0);
    fflush(stdout);
    return rc;
}

static int hook_CreateDecoder(SceVideodec2DecoderConfigInfo *cfg, const SceVideodec2DecoderMemoryInfo *mem, void **decoder)
{
    uint32_t orig_res = cfg ? cfg->resourceType : 0;
    if (cfg && cfg->resourceType != 0xb6c8 && cfg->resourceType != 0x12384) {
        cfg->resourceType = 0xb6c8;
    }

    fn_CreateDec call_tramp = (fn_CreateDec)g_tramp_create;
    int rc = call_tramp(cfg, mem, decoder);
    printf("  >>> [INLINE HOOK Create] codec=%u orig_res=0x%x (W=%u H=%u) -> setting res=0x%x <<<\n",
           cfg ? cfg->codecType : 0, orig_res, cfg ? cfg->maxFrameWidth : 0, cfg ? cfg->maxFrameHeight : 0,
           cfg ? cfg->resourceType : 0);
    printf("  >>> [INLINE HOOK Create] rc=0x%08x handle=%p <<<\n", rc, decoder ? *decoder : NULL);

    /* Automatically register all Direct Memory buffers with MapDirectMemory */
    if (rc == 0 && decoder && *decoder && p_sceVideodec2MapDirectMemory) {
        pthread_mutex_lock(&g_mem_lock);
        for (int i = 0; i < MAX_MEM_SLOTS; i++) {
            if (!g_mem_slots[i].address || !g_mem_slots[i].is_direct) continue;

            if (g_mem_slots[i].size >= 1048576) {
                SceVideodec2MapDirectMemoryInfo mapInfo;
                memset(&mapInfo, 0, sizeof(mapInfo));
                mapInfo.thisSize = 0x20;
                mapInfo.size = (g_mem_slots[i].size + 0x3fff) & ~0x3fff;
                mapInfo.addr = g_mem_slots[i].address;
                mapInfo.physAddr = (uint64_t)g_mem_slots[i].phys;

                int mrc = p_sceVideodec2MapDirectMemory(*decoder, &mapInfo);
                printf("  >>> [AUTO-MAP Buffer %d] addr=%p phys=0x%lx size=%llu -> 0x%08x <<<\n",
                       i, mapInfo.addr, (unsigned long)mapInfo.physAddr,
                       (unsigned long long)mapInfo.size, mrc);
            }
        }
        pthread_mutex_unlock(&g_mem_lock);
    }

    fflush(stdout);
    return rc;
}

static int g_decode_counter = 0;

static int hook_Decode(void *decoder, SceVideodec2InputData *in, SceVideodec2FrameBuffer *fb, SceVideodec2OutputInfo *out)
{
    fn_Decode call_tramp = (fn_Decode)g_tramp_decode;
    int rc = call_tramp(decoder, in, fb, out);
    if (rc != 0 || (out && out->isValid) || g_decode_counter < 10) {
        printf("  >>> [HOOK Decode #%d] rc=0x%08x (valid=%d isErr=%d fb=%p size=%llu pts=%llu) <<<\n",
               g_decode_counter++, rc, out ? out->isValid : -1, out ? out->isErrorFrame : -1,
               out ? out->frameBuffer : NULL, out ? (unsigned long long)out->frameBufferSize : 0,
               out ? (unsigned long long)out->ptsData : 0);
        fflush(stdout);
    }

    if (out && out->isValid && out->frameBuffer) {
        printf("\n**********************************************************\n");
        printf(">>> [HOOK Decode] HARDWARE DECODED FRAME PRODUCED! <<<\n");
        printf("**********************************************************\n");
        fflush(stdout);

        save_nv12_to_bmp("/data/hw_decoded_frame.bmp", (const uint8_t*)out->frameBuffer,
                         1280, 720, 1280, 0, 0, 0, 0);
        save_nv12_to_bmp("/data/evoplayer/hw_decoded_frame.bmp", (const uint8_t*)out->frameBuffer,
                         1280, 720, 1280, 0, 0, 0, 0);
        save_nv12_to_bmp("/mnt/usb0/hw_decoded_frame.bmp", (const uint8_t*)out->frameBuffer,
                         1280, 720, 1280, 0, 0, 0, 0);
        evo_notify("EVO HW Decode SUCCESS! Frame extracted!");
    }

    return rc;
}

static void build_inline_hook(void *target_func, void *hook_func, uint8_t *trampoline, size_t stolen_len)
{
    /* 1. Make target function writable */
    intptr_t page_addr = (intptr_t)((uintptr_t)target_func & ~0x3fffULL);
    kernel_mprotect(getpid(), page_addr, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC);

    /* 2. Make trampoline writable & executable */
    kernel_mprotect(getpid(), (intptr_t)trampoline, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);

    /* 3. Copy stolen bytes into trampoline */
    memcpy(trampoline, target_func, stolen_len);

    /* 4. Append jump back to target_func + stolen_len into trampoline */
    uint8_t *t_jmp = trampoline + stolen_len;
    t_jmp[0] = 0xFF; t_jmp[1] = 0x25; t_jmp[2] = 0x00; t_jmp[3] = 0x00; t_jmp[4] = 0x00; t_jmp[5] = 0x00;
    uintptr_t ret_target = (uintptr_t)target_func + stolen_len;
    memcpy(t_jmp + 6, &ret_target, sizeof(ret_target));

    /* 5. Overwrite target_func with jump to hook_func */
    uint8_t hook_jmp[16];
    hook_jmp[0] = 0xFF; hook_jmp[1] = 0x25; hook_jmp[2] = 0x00; hook_jmp[3] = 0x00; hook_jmp[4] = 0x00; hook_jmp[5] = 0x00;
    uintptr_t dest_hook = (uintptr_t)hook_func;
    memcpy(hook_jmp + 6, &dest_hook, sizeof(dest_hook));

    memcpy(target_func, hook_jmp, 14);
    printf("  [+] Inline hook installed at %p -> %p (trampoline at %p)\n",
           target_func, hook_func, trampoline);
    fflush(stdout);
}

static intptr_t resolve_symbol(const char *module_basename, const char *symbol_name)
{
    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), module_basename, &dynh) != 0) return 0;
    char nid[12] = {0};
    nid_encode(symbol_name, nid);
    intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
    if (addr) {
        printf("  [+] %-32s -> 0x%lx\n", symbol_name, (unsigned long)addr);
    } else {
        printf("  [-] %-32s UNRESOLVED\n", symbol_name);
    }
    fflush(stdout);
    return addr;
}

extern const uint8_t  test_clip_mp4[];
extern const uint64_t test_clip_mp4_len;

static const uint8_t *g_cur_media_buf = NULL;
static uint64_t       g_cur_media_len = 0;
static int            g_cur_media_fd = -1;

static int cb_file_open(void *p, const char *filename)
{
    (void)p;
    g_cur_media_buf = NULL;
    g_cur_media_len = 0;
    if (g_cur_media_fd >= 0) { close(g_cur_media_fd); g_cur_media_fd = -1; }

    if (filename && strcmp(filename, "embedded.mp4") == 0) {
        g_cur_media_buf = test_clip_mp4;
        g_cur_media_len = test_clip_mp4_len;
        printf("  [File Open] Serving embedded MP4 clip (%llu bytes)\n", (unsigned long long)g_cur_media_len);
        fflush(stdout);
        return 0;
    }

    if (filename && access(filename, R_OK) == 0) {
        g_cur_media_fd = open(filename, O_RDONLY);
        if (g_cur_media_fd >= 0) {
            struct stat st;
            if (fstat(g_cur_media_fd, &st) == 0) g_cur_media_len = (uint64_t)st.st_size;
            printf("  [File Open] Opened from filesystem (fd=%d): %s (%llu bytes)\n",
                   g_cur_media_fd, filename, (unsigned long long)g_cur_media_len);
            fflush(stdout);
            return 0;
        }
    }

    /* Fallback to embedded */
    g_cur_media_buf = test_clip_mp4;
    g_cur_media_len = test_clip_mp4_len;
    printf("  [File Open] Falling back to embedded MP4 clip (%llu bytes)\n", (unsigned long long)g_cur_media_len);
    fflush(stdout);
    return 0;
}

static int cb_file_close(void *p)
{
    (void)p;
    if (g_cur_media_fd >= 0) { close(g_cur_media_fd); g_cur_media_fd = -1; }
    g_cur_media_buf = NULL;
    g_cur_media_len = 0;
    return 0;
}

static int cb_file_read_offset(void *p, uint8_t *buf, uint64_t pos, uint32_t len)
{
    (void)p;
    if (!buf || len == 0) return 0;
    if (pos >= g_cur_media_len) return 0;

    if (g_cur_media_buf) {
        uint64_t avail = g_cur_media_len - pos;
        if (avail > len) avail = len;
        memcpy(buf, g_cur_media_buf + pos, (size_t)avail);
        return (int)avail;
    }

    if (g_cur_media_fd >= 0) {
        ssize_t rd = pread(g_cur_media_fd, buf, len, (off_t)pos);
        return (int)(rd < 0 ? 0 : rd);
    }

    return 0;
}

static uint64_t cb_file_size(void *p)
{
    (void)p;
    return g_cur_media_len;
}

static volatile int g_last_event_id = -1;
static volatile int g_event_ready = 0;

static void cb_event_callback(void *obj, int eventId, int sourceId, void *eventData)
{
    (void)obj; (void)eventData;
    printf("  [AvPlayer Event] id=0x%x (source=%d)\n", eventId, sourceId);
    fflush(stdout);
    g_last_event_id = eventId;
    if (eventId == 0 || eventId == 2) {
        g_event_ready = 1;
    }
}

/* --------------------------------------------------------------------------
 * Core Hardware Decode Execution
 * -------------------------------------------------------------------------- */

static bool test_file_decode(const char *filepath)
{
    printf("\n==========================================================\n");
    printf("  Executing Hardware Video Decode on:\n  %s\n", filepath);
    printf("==========================================================\n");
    fflush(stdout);

    g_event_ready = 0;
    g_last_event_id = -1;

    SceAvPlayerInitData initData;
    memset(&initData, 0, sizeof(initData));
    initData.memoryReplacement.objectPointer    = (void*)0xa110c;
    initData.memoryReplacement.allocate         = cb_alloc_general;
    initData.memoryReplacement.deallocate       = cb_deallocate_general;
    initData.memoryReplacement.allocateTexture  = cb_alloc_texture;
    initData.memoryReplacement.deallocateTexture= cb_deallocate_texture;
    initData.fileReplacement.objectPointer      = (void*)0xf11e;
    initData.fileReplacement.open               = cb_file_open;
    initData.fileReplacement.close              = cb_file_close;
    initData.fileReplacement.readOffset         = cb_file_read_offset;
    initData.fileReplacement.size               = cb_file_size;
    initData.eventReplacement.objectPointer     = (void*)0xbeef;
    initData.eventReplacement.eventCallback     = cb_event_callback;
    initData.debugLevel                         = 3;
    initData.basePriority                       = 700;
    initData.numOutputVideoFrameBuffers         = 4;
    initData.autoStart                          = 0;

    void *playerHandle = p_sceAvPlayerInit(&initData);
    if (!playerHandle) {
        printf("  [!] sceAvPlayerInit failed\n");
        fflush(stdout);
        return false;
    }
    printf("  sceAvPlayerInit SUCCESS! Handle: %p\n", playerHandle);
    fflush(stdout);

    int add_rc = p_sceAvPlayerAddSource(playerHandle, filepath);
    printf("  sceAvPlayerAddSource() -> 0x%08x\n", add_rc);
    fflush(stdout);
    if (add_rc != 0) {
        p_sceAvPlayerClose(playerHandle);
        return false;
    }

    /* Enable continuous looping */
    if (p_sceAvPlayerSetLooping) {
        p_sceAvPlayerSetLooping(playerHandle, true);
    }

    /* Wait for demuxer thread to reach READY state (up to 3 seconds) */
    printf("  Waiting for container demux and READY event...\n");
    fflush(stdout);
    int stream_count = 0;
    for (int wait = 0; wait < 150; wait++) {
        stream_count = p_sceAvPlayerStreamCount(playerHandle);
        if (stream_count > 0 || g_event_ready) break;
        usleep(20000);
    }

    printf("  Stream Count: %d (ready=%d)\n", stream_count, g_event_ready);
    fflush(stdout);

    /* Enable Stream 0 (Video stream only - skip audio to prevent audio decoder failures) */
    printf("  Calling sceAvPlayerEnableStream(0) [Video]...\n");
    fflush(stdout);
    int en0 = p_sceAvPlayerEnableStream(playerHandle, 0);
    printf("  sceAvPlayerEnableStream(0) [Video] -> 0x%08x\n", en0);
    fflush(stdout);

    printf("  Calling sceAvPlayerStart()...\n");
    fflush(stdout);
    int start_rc = p_sceAvPlayerStart(playerHandle);
    printf("  sceAvPlayerStart() -> 0x%08x\n", start_rc);
    fflush(stdout);

    bool frame_captured = false;
    printf("  Polling for hardware decoded video frames (up to 40 seconds)...\n");
    fflush(stdout);

    uint8_t frame_raw[128];
    for (int poll = 0; poll < 1600; poll++) {
        if (p_sceAvPlayerGetVideoData) {
            memset(frame_raw, 0, sizeof(frame_raw));
            int ok = p_sceAvPlayerGetVideoData(playerHandle, frame_raw);
            uint8_t *pData = *(uint8_t**)frame_raw;
            if (ok && pData != NULL) {
                printf("\n**********************************************************\n");
                printf(">>> SUCCESS: HARDWARE DECODED FRAME CAPTURED (Poll %d)! <<<\n", poll);
                printf("**********************************************************\n");
                printf("  NV12 Plane Pointer   : %p\n", pData);
                for (int k = 0; k < 40; k += 8) {
                    printf("    +0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                           k, frame_raw[k], frame_raw[k+1], frame_raw[k+2], frame_raw[k+3],
                           frame_raw[k+4], frame_raw[k+5], frame_raw[k+6], frame_raw[k+7]);
                }
                fflush(stdout);

                save_nv12_to_bmp("/data/hw_decoded_frame.bmp", pData,
                                 1280, 720, 1280, 0, 0, 0, 0);
                save_nv12_to_bmp("/data/evoplayer/hw_decoded_frame.bmp", pData,
                                 1280, 720, 1280, 0, 0, 0, 0);
                save_nv12_to_bmp("/mnt/usb0/hw_decoded_frame.bmp", pData,
                                 1280, 720, 1280, 0, 0, 0, 0);

                evo_notify("EVO HW Decode SUCCESS!");
                frame_captured = true;
                break;
            }

            if (poll % 20 == 0) {
                printf("  [Poll %4d] GetVideoData -> %d (data=%p)\n", poll, ok, pData);
                fflush(stdout);
            }
        }

        usleep(25000); /* 25 ms */
    }

    if (!frame_captured) {
        printf("  [!] No frame returned within poll window\n");
        fflush(stdout);
    }

    p_sceAvPlayerStop(playerHandle);
    p_sceAvPlayerClose(playerHandle);

    return frame_captured;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n==========================================================\n");
    printf("  EVO Player — Hardware Video Decoding Capture (12.70)\n");
    printf("==========================================================\n");
    printf("Firmware : 0x%08x | PID: %d\n\n", kernel_get_fw_version(), getpid());
    fflush(stdout);

    /* Elevate process credentials to Sony Media Decoder family */
    pid_t mypid = getpid();
    uint64_t orig_authid = kernel_get_ucred_authid(mypid);
    uint8_t orig_caps[16] = {0};
    uint8_t privcaps[16];
    memset(privcaps, 0xFF, sizeof(privcaps));
    kernel_get_ucred_caps(mypid, orig_caps);

    printf("[Auth] Elevating ucred: 0x%016llx -> 0x4900000000000002, caps all-FF\n",
           (unsigned long long)orig_authid);
    kernel_set_ucred_authid(mypid, 0x4900000000000002ULL);
    kernel_set_ucred_caps(mypid, privcaps);
    fflush(stdout);

    evo_notify("EVO HW Decode: Initiating hardware frame extraction...");

    /* Pre-allocate a 2 MiB Direct Memory buffer for Access Units */
    g_mapped_au_buf = direct_alloc(0x4000, 2097152, "PreAlloc-AU", false);
    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (g_mem_slots[i].address == g_mapped_au_buf) {
            g_mapped_au_phys = g_mem_slots[i].phys;
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);

    /* Load Media Modules in exact dependency order */
    printf("[Phase 1] Loading Sony Media SPRX modules in dependency order...\n");
    static const char *const kModules[] = {
        "/system/common/lib/libSceIpmi.sprx",
        "/system/common/lib/libSceVideoArbitration.sprx",
        "/system/common/lib/libSceResourceArbitrator.sprx",
        "/system/common/lib/libSceGnmDriver.sprx",
        "/system/common/lib/libSceVdecCore.sprx",
        "/system/common/lib/libSceVideoDecoderArbitration.sprx",
        "/system/common/lib/libSceVideodec2.sprx",
        "/system/common/lib/libSceVdecwrap.sprx",
        "/system/common/lib/libSceVdecShevc.sprx",
        "/system/common/lib/libSceSysmodule.sprx",
        "/system/common/lib/libSceAjm.sprx",
        "/system/common/lib/libSceAudiodec.sprx",
        "/system/common/lib/libSceAudiodec2.sprx",
        "/system/common/lib/libSceAudioOut.sprx",
        "/system/common/lib/libSceAvPlayer.sprx",
    };

    for (size_t i = 0; i < sizeof(kModules)/sizeof(kModules[0]); i++) {
        int res = 0;
        int modid = sceKernelLoadStartModule(kModules[i], 0, NULL, 0, NULL, &res);
        const char *basename = strrchr(kModules[i], '/');
        basename = basename ? basename + 1 : kModules[i];
        printf("  [Load] %-36s modid=0x%x res=0x%x\n", basename, modid, res);
    }

    typedef int (*fn_SysmoduleLoadInternal)(uint32_t id);
    fn_SysmoduleLoadInternal p_sysmod_internal = (fn_SysmoduleLoadInternal)resolve_symbol("libSceSysmodule.sprx", "sceSysmoduleLoadModuleInternal");
    if (p_sysmod_internal) {
        printf("  [Sysmodule] Loading internal media codecs...\n");
        printf("  [Sysmodule] H.264 (0x80000036) -> 0x%08x\n", p_sysmod_internal(0x80000036));
        printf("  [Sysmodule] HEVC  (0x80000037) -> 0x%08x\n", p_sysmod_internal(0x80000037));
        printf("  [Sysmodule] AAC   (0x80000038) -> 0x%08x\n", p_sysmod_internal(0x80000038));
        printf("  [Sysmodule] MP3   (0x80000039) -> 0x%08x\n", p_sysmod_internal(0x80000039));
    }
    fflush(stdout);

    /* Resolve Entry Points */
    printf("\n[Phase 2] Resolving libSceAvPlayer entry points...\n");
    const char *avp_mod = "libSceAvPlayer.sprx";
    p_sceAvPlayerInit          = (fn_sceAvPlayerInit)resolve_symbol(avp_mod, "sceAvPlayerInit");
    p_sceAvPlayerAddSource     = (fn_sceAvPlayerAddSource)resolve_symbol(avp_mod, "sceAvPlayerAddSource");
    p_sceAvPlayerStart         = (fn_sceAvPlayerStart)resolve_symbol(avp_mod, "sceAvPlayerStart");
    p_sceAvPlayerStop          = (fn_sceAvPlayerStop)resolve_symbol(avp_mod, "sceAvPlayerStop");
    p_sceAvPlayerPause         = (fn_sceAvPlayerPause)resolve_symbol(avp_mod, "sceAvPlayerPause");
    p_sceAvPlayerResume        = (fn_sceAvPlayerResume)resolve_symbol(avp_mod, "sceAvPlayerResume");
    p_sceAvPlayerIsActive      = (fn_sceAvPlayerIsActive)resolve_symbol(avp_mod, "sceAvPlayerIsActive");
    p_sceAvPlayerClose         = (fn_sceAvPlayerClose)resolve_symbol(avp_mod, "sceAvPlayerClose");
    p_sceAvPlayerStreamCount   = (fn_sceAvPlayerStreamCount)resolve_symbol(avp_mod, "sceAvPlayerStreamCount");
    p_sceAvPlayerGetStreamInfo = (fn_sceAvPlayerGetStreamInfo)resolve_symbol(avp_mod, "sceAvPlayerGetStreamInfo");
    p_sceAvPlayerEnableStream  = (fn_sceAvPlayerEnableStream)resolve_symbol(avp_mod, "sceAvPlayerEnableStream");
    p_sceAvPlayerDisableStream = (fn_sceAvPlayerDisableStream)resolve_symbol(avp_mod, "sceAvPlayerDisableStream");
    p_sceAvPlayerGetVideoData  = (fn_sceAvPlayerGetVideoData)resolve_symbol(avp_mod, "sceAvPlayerGetVideoData");
    p_sceAvPlayerGetVideoDataEx= (fn_sceAvPlayerGetVideoDataEx)resolve_symbol(avp_mod, "sceAvPlayerGetVideoDataEx");
    p_sceAvPlayerGetAudioData  = (fn_sceAvPlayerGetAudioData)resolve_symbol(avp_mod, "sceAvPlayerGetAudioData");
    p_sceAvPlayerCurrentTime   = (fn_sceAvPlayerCurrentTime)resolve_symbol(avp_mod, "sceAvPlayerCurrentTime");
    p_sceAvPlayerSetLooping    = (fn_sceAvPlayerSetLooping)resolve_symbol(avp_mod, "sceAvPlayerSetLooping");

    p_sceVideodec2MapDirectMemory = (fn_MapDirectMemory)resolve_symbol("libSceVideodec2.sprx", "sceVideodec2MapDirectMemory");

    if (!p_sceAvPlayerInit || !p_sceAvPlayerAddSource) {
        printf("ERROR: Required symbols missing!\n");
        return EXIT_FAILURE;
    }

    /* Resolve Videodec2 symbols and install inline hooks for Query, Create, and Decode */
    void *target_query = (void*)resolve_symbol("libSceVideodec2.sprx", "sceVideodec2QueryDecoderMemoryInfo");
    void *target_create = (void*)resolve_symbol("libSceVideodec2.sprx", "sceVideodec2CreateDecoder");
    void *target_decode = (void*)resolve_symbol("libSceVideodec2.sprx", "sceVideodec2Decode");

    if (target_query && target_create && target_decode) {
        printf("\n[Phase 3] Installing inline hooks for Videodec2...\n");
        build_inline_hook(target_query, (void*)hook_QueryDecoderMemoryInfo, g_tramp_query, 16);
        build_inline_hook(target_create, (void*)hook_CreateDecoder, g_tramp_create, 16);
        build_inline_hook(target_decode, (void*)hook_Decode, g_tramp_decode, 20);
    }

    static const char *const kCandidateFiles[] = {
        "embedded.mp4",
        "/mnt/usb0/test_files_aud_vid/AAC 5.1.mp4",
        "/mnt/usb0/test.mp4",
    };

    bool overall_success = false;
    for (size_t i = 0; i < sizeof(kCandidateFiles)/sizeof(kCandidateFiles[0]); i++) {
        if (strcmp(kCandidateFiles[i], "embedded.mp4") == 0 || access(kCandidateFiles[i], R_OK) == 0) {
            if (test_file_decode(kCandidateFiles[i])) {
                overall_success = true;
                printf("\n  >>> HARDWARE DECODE CAPTURE SUCCEEDED ON: %s <<<\n", kCandidateFiles[i]);
                break;
            }
        }
    }

    printf("\n==========================================================\n");
    printf("  Final Outcome: %s\n", overall_success ? "SUCCESS — FRAME CAPTURED & SAVED TO /data/hw_decoded_frame.bmp" : "PROBE COMPLETE");
    printf("==========================================================\n\n");
    fflush(stdout);

    /* Restore credentials */
    kernel_set_ucred_authid(mypid, orig_authid);
    kernel_set_ucred_caps(mypid, orig_caps);

    return overall_success ? EXIT_SUCCESS : 0;
}
