/*
 * EVO Player - sony_player_test
 *
 * Clean implementation using Sony's official libSceAvPlayer engine with
 * proper video arbitration bring-up, H.264 codec loading, and on-screen notifications.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

#define DEFAULT_TARGET_FILE "/mnt/usb0/Big_Buck_Bunny_1080_10s_30MB.mp4"
#define LOG_PATH            "/mnt/usb0/sony_player_log.txt"
#define OUTPUT_BMP_USB      "/mnt/usb0/decoded_bbb_frame.bmp"
#define OUTPUT_BMP_DATA     "/data/decoded_bbb_frame.bmp"

static FILE *g_log_file = NULL;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log_file) {
        va_start(ap, fmt);
        vfprintf(g_log_file, fmt, ap);
        va_end(ap);
        fflush(g_log_file);
    }
}

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
 * Video Decoder Arbitration
 * -------------------------------------------------------------------------- */

#define ARB_PARAMS_SIZE 0x18

typedef struct {
    uint64_t thisSize;   /* +0x00  0x18 */
    uint32_t priority;   /* +0x08  256..767 */
    uint32_t pad0c;
    uint64_t count;      /* +0x10  1..127 */
} SceVideoDecoderArbitrationParams;

typedef int (*arb_init_fn)(const SceVideoDecoderArbitrationParams *);
typedef int (*arb_enable_fn)(void *mustBeNull, void *callback);
typedef int (*arb_accept_fn)(unsigned int);

static volatile unsigned g_arb_events = 0;
static void arbitration_callback(void) { g_arb_events++; }

/* --------------------------------------------------------------------------
 * libSceAvPlayer ABI Definitions
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
typedef bool  (*fn_sceAvPlayerGetAudioData)(void *handle, SceAvPlayerFrameInfo *info);
typedef uint64_t (*fn_sceAvPlayerCurrentTime)(void *handle);
typedef int   (*fn_sceAvPlayerSetLooping)(void *handle, bool loop);

static fn_sceAvPlayerInit          p_sceAvPlayerInit = NULL;
static fn_sceAvPlayerAddSource     p_sceAvPlayerAddSource = NULL;
static fn_sceAvPlayerStart         p_sceAvPlayerStart = NULL;
static fn_sceAvPlayerStop          p_sceAvPlayerStop = NULL;
static fn_sceAvPlayerPause         p_sceAvPlayerPause = NULL;
static fn_sceAvPlayerResume        p_sceAvPlayerResume = NULL;
static fn_sceAvPlayerIsActive      p_sceAvPlayerIsActive = NULL;
static fn_sceAvPlayerClose         p_sceAvPlayerClose = NULL;
static fn_sceAvPlayerStreamCount   p_sceAvPlayerStreamCount = NULL;
static fn_sceAvPlayerGetStreamInfo p_sceAvPlayerGetStreamInfo = NULL;
static fn_sceAvPlayerEnableStream  p_sceAvPlayerEnableStream = NULL;
static fn_sceAvPlayerDisableStream p_sceAvPlayerDisableStream = NULL;
static fn_sceAvPlayerGetVideoData  p_sceAvPlayerGetVideoData = NULL;
static fn_sceAvPlayerGetAudioData  p_sceAvPlayerGetAudioData = NULL;
static fn_sceAvPlayerCurrentTime   p_sceAvPlayerCurrentTime = NULL;
static fn_sceAvPlayerSetLooping    p_sceAvPlayerSetLooping = NULL;

/* --------------------------------------------------------------------------
 * Direct Memory Management
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
    int memType = SCE_KERNEL_WB_ONION; /* Use WB_ONION for full CPU cacheability and direct DMA support */
    int ret = sceKernelAllocateMainDirectMemory(bytes, align, memType, &phys);
    if (ret != 0) {
        LOG("  [%s] sceKernelAllocateMainDirectMemory FAILED: bytes=%zu, align=%u, type=%d, ret=0x%x\n",
            tag, bytes, align, memType, ret);
        return NULL;
    }

    void *mapped = NULL;
    ret = sceKernelMapDirectMemory(&mapped, bytes, 0x33, 0, phys, align);
    if (ret != 0 || mapped == NULL) {
        LOG("  [%s] sceKernelMapDirectMemory FAILED: ret=0x%x\n", tag, ret);
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
            LOG("  [%s] DirectMem mapped: %p (bytes=%zu, phys=0x%lx, is_tex=%d)\n",
                tag, mapped, bytes, (unsigned long)phys, is_tex);
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
    if (size == 0) return NULL;
    if (size >= 65536 || align >= 4096) {
        void *ptr = direct_alloc(align, size, "AllocGeneral-Direct", false);
        if (ptr) return ptr;
    }
    if (align < 64) align = 64;
    void *ptr = NULL;
    if (posix_memalign(&ptr, align, size) != 0) {
        ptr = malloc(size);
    }
    LOG("  [alloc-heap] align=%u size=%u -> %p\n", align, size, ptr);
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
    if (size == 0) return NULL;
    void *ptr = direct_alloc(align, size, "AllocTexture-Direct", true);
    if (ptr) return ptr;

    if (align < 64) align = 64;
    if (posix_memalign(&ptr, align, size) != 0) {
        ptr = malloc(size);
    }
    LOG("  [alloc-texture-heap] align=%u size=%u -> %p\n", align, size, ptr);
    return ptr;
}

static void cb_deallocate_texture(void *obj, void *ptr)
{
    cb_deallocate_general(obj, ptr);
}

/* --------------------------------------------------------------------------
 * BMP Frame Saver (NV12 -> 24-bit RGB BMP)
 * -------------------------------------------------------------------------- */

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void save_nv12_to_bmp(const char *filepath, const uint8_t *nv12_data,
                             int width, int height, int pitch)
{
    if (width <= 0 || height <= 0 || !nv12_data) return;

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        LOG("  [BMP] Failed to open %s for writing\n", filepath);
        return;
    }

    int row_stride = (width * 3 + 3) & ~3;
    uint32_t image_size = (uint32_t)row_stride * height;
    uint32_t file_size = 54 + image_size;

    uint8_t header[54] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        (uint8_t)(width), (uint8_t)(width >> 8), (uint8_t)(width >> 16), (uint8_t)(width >> 24),
        (uint8_t)(height), (uint8_t)(height >> 8), (uint8_t)(height >> 16), (uint8_t)(height >> 24),
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

    for (int y = height - 1; y >= 0; y--) {
        const uint8_t *y_line = y_plane + (size_t)y * pitch;
        const uint8_t *uv_line = uv_plane + (size_t)(y / 2) * pitch;

        for (int x = 0; x < width; x++) {
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
        for (int p = width * 3; p < row_stride; p++) {
            row_buf[p] = 0;
        }
        fwrite(row_buf, 1, row_stride, fp);
    }

    free(row_buf);
    fclose(fp);
    LOG("  [BMP] Successfully saved decoded frame: %s (%dx%d, %u bytes)\n",
        filepath, width, height, file_size);
}

/* --------------------------------------------------------------------------
 * File Replacement Callbacks
 * -------------------------------------------------------------------------- */

static int      g_cur_media_fd = -1;
static uint64_t g_cur_media_len = 0;

static int cb_file_open(void *p, const char *filename)
{
    (void)p;
    g_cur_media_len = 0;
    if (g_cur_media_fd >= 0) { close(g_cur_media_fd); g_cur_media_fd = -1; }

    if (filename && access(filename, R_OK) == 0) {
        g_cur_media_fd = open(filename, O_RDONLY);
        if (g_cur_media_fd >= 0) {
            struct stat st;
            if (fstat(g_cur_media_fd, &st) == 0) g_cur_media_len = (uint64_t)st.st_size;
            LOG("  [File Open] Opened file: %s (fd=%d, %llu bytes)\n",
                filename, g_cur_media_fd, (unsigned long long)g_cur_media_len);
            return 0;
        }
    }
    LOG("  [File Open] FAILED to open file: %s\n", filename ? filename : "NULL");
    return -1;
}

static int cb_file_close(void *p)
{
    (void)p;
    if (g_cur_media_fd >= 0) { close(g_cur_media_fd); g_cur_media_fd = -1; }
    g_cur_media_len = 0;
    return 0;
}

static int cb_file_read_offset(void *p, uint8_t *buf, uint64_t pos, uint32_t len)
{
    (void)p;
    if (!buf || len == 0 || g_cur_media_fd < 0) return 0;
    if (pos >= g_cur_media_len) return 0;

    ssize_t rd = pread(g_cur_media_fd, buf, len, (off_t)pos);
    return (int)(rd < 0 ? 0 : rd);
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
    LOG("  [AvPlayer Event] eventId=0x%x (sourceId=%d)\n", eventId, sourceId);
    g_last_event_id = eventId;
    if (eventId == 0 || eventId == 2) {
        g_event_ready = 1;
        evo_notify("2/5: Video Demuxed! 1080p H.264 Stream Detected");
    }
}

/* --------------------------------------------------------------------------
 * Dynamic Symbol Resolution Helper
 * -------------------------------------------------------------------------- */

static intptr_t resolve_symbol(const char *module_basename, const char *symbol_name)
{
    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), module_basename, &dynh) != 0) return 0;
    char nid[12] = {0};
    nid_encode(symbol_name, nid);
    intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
    if (addr) {
        LOG("  [+] %-32s -> 0x%lx\n", symbol_name, (unsigned long)addr);
    } else {
        LOG("  [-] %-32s UNRESOLVED\n", symbol_name);
    }
    return addr;
}

static void spoof_titleid(pid_t pid, const char *new_titleid)
{
    intptr_t p = kernel_get_proc(pid);
    if (!p) {
        LOG("  [Spoof] kernel_get_proc failed\n");
        return;
    }
    LOG("  [Spoof] Kernel proc addr: 0x%lx\n", (unsigned long)p);

    uint8_t buf[0x800];
    if (kernel_copyout(p, buf, sizeof(buf)) != 0) {
        LOG("  [Spoof] Failed to read proc struct\n");
        return;
    }

    /* Search for known launcher titleids: FAKE, NPXS, EVOP, CUSA, PPSA */
    for (size_t off = 0; off + 16 < sizeof(buf); off++) {
        if (memcmp(buf + off, "FAKE", 4) == 0 ||
            memcmp(buf + off, "NPXS40106", 9) == 0 ||
            memcmp(buf + off, "EVOP10001", 9) == 0) {
            char old[32] = {0};
            memcpy(old, buf + off, 16);
            LOG("  [Spoof] Found TitleID \"%s\" at proc + 0x%zx -> Overwriting with \"%s\"\n",
                old, off, new_titleid);

            char new_tid[16] = {0};
            strncpy(new_tid, new_titleid, sizeof(new_tid) - 1);
            kernel_copyin(new_tid, p + off, sizeof(new_tid));
        }
    }
}

/* --------------------------------------------------------------------------
 * Main Entry Point
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *target_file = DEFAULT_TARGET_FILE;
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        target_file = argv[1];
    }

    g_log_file = fopen(LOG_PATH, "w");

    LOG("\n==========================================================\n");
    LOG("  EVO Player — Sony AvPlayer Target Test (Media Slot Spoof)\n");
    LOG("==========================================================\n");
    LOG("Target Video : %s\n", target_file);
    LOG("Firmware     : 0x%08x | PID: %d\n\n", kernel_get_fw_version(), getpid());

    evo_notify("1/5: Loading Media Modules & Video: %s", strrchr(target_file, '/') ? strrchr(target_file, '/') + 1 : target_file);

    /* 1. Elevate process credentials & Spoof Media App Title ID */
    pid_t mypid = getpid();
    uint64_t orig_authid = kernel_get_ucred_authid(mypid);
    uint8_t orig_caps[16] = {0};
    uint8_t privcaps[16];
    memset(privcaps, 0xFF, sizeof(privcaps));
    kernel_get_ucred_caps(mypid, orig_caps);

    uint8_t orig_attrs[32] = {0};
    kernel_get_ucred_attrs(mypid, orig_attrs);

    LOG("[Auth] Elevating ucred: 0x%016llx -> 0x4900000000000002, caps all-FF\n",
        (unsigned long long)orig_authid);
    kernel_set_ucred_authid(mypid, 0x4900000000000002ULL);
    kernel_set_ucred_caps(mypid, privcaps);

    /* Safe default title identity */
    spoof_titleid(mypid, "FAKE00000");

    /* 2. Initialize User Service */
    int uinit = sceUserServiceInitialize(NULL);
    LOG("sceUserServiceInitialize -> 0x%08x\n", uinit);

    /* 3. Load Sony Media SPRX modules in exact dependency order */
    LOG("\n[Phase 1] Loading Sony Media SPRX modules in order...\n");
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
        "/system/common/lib/libSceVdecSavc.sprx",
        "/system/common/lib/libSceVdecSavc2.sprx",
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
        LOG("  [Load] %-36s modid=0x%x res=0x%x\n", basename, modid, res);
    }

    typedef int (*fn_SysmoduleLoadInternal)(uint32_t id);
    fn_SysmoduleLoadInternal p_sysmod_internal = (fn_SysmoduleLoadInternal)resolve_symbol("libSceSysmodule.sprx", "sceSysmoduleLoadModuleInternal");
    if (p_sysmod_internal) {
        LOG("  [Sysmodule] Loading internal media codecs...\n");
        LOG("  [Sysmodule] H.264 (0x80000036) -> 0x%08x\n", p_sysmod_internal(0x80000036));
        LOG("  [Sysmodule] HEVC  (0x80000037) -> 0x%08x\n", p_sysmod_internal(0x80000037));
        LOG("  [Sysmodule] AAC   (0x80000038) -> 0x%08x\n", p_sysmod_internal(0x80000038));
        LOG("  [Sysmodule] MP3   (0x80000039) -> 0x%08x\n", p_sysmod_internal(0x80000039));
    }

    /* 4. Bring up Video Decoder Arbitration before AvPlayer initialization */
    LOG("\n[Phase 2] Bringing up Video Decoder Arbitration...\n");
    uint32_t dynh_arb = 0;
    if (kernel_dynlib_handle(getpid(), "libSceVideoDecoderArbitration.sprx", &dynh_arb) == 0) {
        arb_init_fn   arb_init   = (arb_init_fn)resolve_symbol("libSceVideoDecoderArbitration.sprx", "sceVideoDecoderArbitrationInitialize");
        arb_enable_fn arb_enable = (arb_enable_fn)resolve_symbol("libSceVideoDecoderArbitration.sprx", "sceVideoDecoderArbitrationEnable");
        arb_accept_fn arb_accept = (arb_accept_fn)resolve_symbol("libSceVideoDecoderArbitration.sprx", "sceVideoDecoderArbitrationAcceptEvent");

        if (arb_init && arb_enable && arb_accept) {
            SceVideoDecoderArbitrationParams p;
            memset(&p, 0, sizeof(p));
            p.thisSize = ARB_PARAMS_SIZE;
            p.priority = 700;
            p.count    = 4;

            int rc = arb_init(&p);
            LOG("  Arbitration Initialize -> 0x%08x\n", rc);
            rc = arb_enable(NULL, (void*)arbitration_callback);
            LOG("  Arbitration Enable     -> 0x%08x\n", rc);
            rc = arb_accept(0);
            LOG("  Arbitration Accept(0)  -> 0x%08x\n", rc);
            rc = arb_accept(1);
            LOG("  Arbitration Accept(1)  -> 0x%08x\n", rc);
            evo_notify("2/5: Video Decoder Arbitration Online!");
        }
    }

    /* 5. Resolve entry points */
    LOG("\n[Phase 3] Resolving libSceAvPlayer entry points...\n");
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
    p_sceAvPlayerGetAudioData  = (fn_sceAvPlayerGetAudioData)resolve_symbol(avp_mod, "sceAvPlayerGetAudioData");
    p_sceAvPlayerCurrentTime   = (fn_sceAvPlayerCurrentTime)resolve_symbol(avp_mod, "sceAvPlayerCurrentTime");
    p_sceAvPlayerSetLooping    = (fn_sceAvPlayerSetLooping)resolve_symbol(avp_mod, "sceAvPlayerSetLooping");

    if (!p_sceAvPlayerInit || !p_sceAvPlayerAddSource || !p_sceAvPlayerStart) {
        LOG("ERROR: Critical libSceAvPlayer symbols missing!\n");
        return EXIT_FAILURE;
    }

    /* 6. Initialize SceAvPlayer */
    LOG("\n[Phase 4] Initializing SceAvPlayer instance...\n");
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
    initData.numOutputVideoFrameBuffers         = 2;
    initData.autoStart                          = 0;

    void *playerHandle = p_sceAvPlayerInit(&initData);
    if (!playerHandle) {
        LOG("ERROR: sceAvPlayerInit failed!\n");
        return EXIT_FAILURE;
    }
    LOG("sceAvPlayerInit SUCCESS! Handle: %p\n", playerHandle);

    /* 7. Add Source */
    int add_rc = p_sceAvPlayerAddSource(playerHandle, target_file);
    LOG("sceAvPlayerAddSource(\"%s\") -> 0x%08x\n", target_file, add_rc);
    if (add_rc != 0) {
        LOG("ERROR: sceAvPlayerAddSource failed with 0x%08x\n", add_rc);
        p_sceAvPlayerClose(playerHandle);
        return EXIT_FAILURE;
    }

    if (p_sceAvPlayerSetLooping) {
        p_sceAvPlayerSetLooping(playerHandle, true);
    }

    /* 8. Wait for demuxer to parse streams */
    LOG("\n[Phase 5] Waiting for container demux and READY event...\n");
    int stream_count = 0;
    for (int wait = 0; wait < 500; wait++) {
        stream_count = p_sceAvPlayerStreamCount(playerHandle);
        if (stream_count > 0 && g_event_ready) break;
        usleep(20000); /* 20 ms */
    }
    LOG("Stream Count: %d (ready event=%d)\n", stream_count, g_event_ready);

    for (int s = 0; s < stream_count && s < 4; s++) {
        SceAvPlayerStreamInfo sinfo;
        memset(&sinfo, 0, sizeof(sinfo));
        if (p_sceAvPlayerGetStreamInfo) {
            int irc = p_sceAvPlayerGetStreamInfo(playerHandle, (uint32_t)s, &sinfo);
            LOG("  Stream %d Info: type=%u duration=%llu (rc=0x%08x)\n",
                s, sinfo.type, (unsigned long long)sinfo.duration, irc);
        }
    }

    /* 9. Enable Video Stream (Stream 0) */
    LOG("Enabling Video Stream 0...\n");
    int en0 = p_sceAvPlayerEnableStream(playerHandle, 0);
    LOG("sceAvPlayerEnableStream(0) -> 0x%08x\n", en0);
    evo_notify("3/5: Video Stream 0 Enabled (rc=0x%08x)!", en0);

    /* 10. Start Playback */
    LOG("\n[Phase 6] Starting AvPlayer playback...\n");
    int start_rc = p_sceAvPlayerStart(playerHandle);
    LOG("sceAvPlayerStart() -> 0x%08x\n", start_rc);
    evo_notify("4/5: AvPlayer Playback Started (rc=0x%08x)!", start_rc);

    /* 11. Poll for decoded video frame */
    LOG("\n[Phase 7] Polling for decoded video frame (up to 30 seconds)...\n");
    bool frame_captured = false;
    uint8_t frame_raw[128];

    for (int poll = 0; poll < 1200; poll++) {
        if (p_sceAvPlayerGetVideoData) {
            memset(frame_raw, 0, sizeof(frame_raw));
            int ok = p_sceAvPlayerGetVideoData(playerHandle, (SceAvPlayerFrameInfo*)frame_raw);
            uint8_t *pData = *(uint8_t**)frame_raw;

            if (ok && pData != NULL) {
                LOG("\n**********************************************************\n");
                LOG(">>> SUCCESS: HARDWARE DECODED FRAME CAPTURED (Poll %d)! <<<\n", poll);
                LOG("**********************************************************\n");
                LOG("  NV12 Plane Pointer: %p\n", pData);

                /* Save to USB and local data */
                save_nv12_to_bmp(OUTPUT_BMP_USB, pData, 1920, 1080, 1920);
                save_nv12_to_bmp(OUTPUT_BMP_DATA, pData, 1920, 1080, 1920);

                /* Send on-screen system notification */
                evo_notify("5/5 SUCCESS! HW Decoded Frame Captured from Big Buck Bunny!");

                frame_captured = true;
                break;
            }

            if (poll % 40 == 0) {
                LOG("  [Poll %4d] GetVideoData -> %d (data=%p)\n", poll, ok, pData);
            }
        }
        usleep(25000); /* 25 ms */
    }

    if (!frame_captured) {
        LOG("  [!] No decoded frame returned within polling window.\n");
    }

    /* 12. Cleanup */
    LOG("\n[Phase 8] Tearing down AvPlayer...\n");
    p_sceAvPlayerStop(playerHandle);
    p_sceAvPlayerClose(playerHandle);

    /* Restore credentials */
    kernel_set_ucred_authid(mypid, orig_authid);
    kernel_set_ucred_caps(mypid, orig_caps);

    LOG("\n=== Final Status ===\n");
    LOG("Frame Captured: %s\n", frame_captured ? "YES (SUCCESS)" : "NO");
    LOG("Log written to: %s\n", LOG_PATH);

    if (g_log_file) fclose(g_log_file);

    return frame_captured ? EXIT_SUCCESS : EXIT_FAILURE;
}
