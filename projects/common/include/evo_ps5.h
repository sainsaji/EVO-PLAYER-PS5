/* evo_ps5.h - PS5 system API declarations for EVO Player test projects.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The PS5 Payload SDK ships link-time *stubs* for the Sony modules
 * (sce_stubs/libSceVideoOut.c, libSceAudioOut.c, libkernel.c, ...) but its
 * public headers under include/ps5/ are only:
 *      kernel.h  klog.h  mdbg.h  nid.h  payload.h
 * There are no sceVideoOut or sceAudioOut prototypes anywhere in the SDK, so
 * every project has to declare the ones it calls. That is what this header is.
 *
 * Everything declared here was verified to exist as an exported symbol in the
 * SDK's stub sources, so it will link. The *signatures* and constants come
 * from the public PS4 SDK ABI, which VideoOut/AudioOut still follow on PS5 for
 * these entry points. Treat the constants as "known-good starting values" and
 * confirm against real hardware behaviour - see docs/validation.md.
 *
 * Link with the matching stub library, e.g. -lSceVideoOut -lSceAudioOut
 * -lSceUserService.
 */

#ifndef EVO_PS5_H
#define EVO_PS5_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* off_t, used by the direct-memory API */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Notifications (libkernel)                                                 */
/*                                                                           */
/* The most reliable way to get a message on screen from a payload. This is  */
/* exactly what the SDK's own samples/hello_world does.                      */
/* ------------------------------------------------------------------------ */

typedef struct evo_notify_request {
    char  useless1[45];
    char  message[3075];
} evo_notify_request_t;

int sceKernelSendNotificationRequest(int device,
                                     evo_notify_request_t *req,
                                     size_t size,
                                     int blocking);

/* Convenience wrapper implemented in common/src/evo_notify.c */
void evo_notify(const char *fmt, ...);

/* ------------------------------------------------------------------------ */
/* Kernel: direct memory + timing (libkernel)                                */
/* ------------------------------------------------------------------------ */

/* Direct memory types. Display buffers conventionally use WC_GARLIC, which is
 * write-combined and GPU-visible - correct for something the scanout engine
 * reads. WB_ONION is cached and better when the CPU must read back. */
#define SCE_KERNEL_WB_ONION      0
#define SCE_KERNEL_WC_GARLIC     3

/* Protection bits for sceKernelMapDirectMemory. */
#define SCE_KERNEL_PROT_CPU_READ   0x01
#define SCE_KERNEL_PROT_CPU_WRITE  0x02
#define SCE_KERNEL_PROT_CPU_RW     0x03
#define SCE_KERNEL_PROT_GPU_READ   0x10
#define SCE_KERNEL_PROT_GPU_WRITE  0x20
#define SCE_KERNEL_PROT_GPU_ALL    0x30

int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd,
                                  size_t len, size_t alignment,
                                  int memoryType, off_t *physAddrOut);

int sceKernelMapDirectMemory(void **addrInOut, size_t len, int prot,
                             int flags, off_t physAddr, size_t alignment);

int sceKernelReleaseDirectMemory(off_t start, size_t len);

size_t sceKernelGetDirectMemorySize(void);
int    sceKernelAvailableDirectMemorySize(off_t searchStart, off_t searchEnd,
                                          size_t alignment, off_t *physAddrOut,
                                          size_t *sizeOut);

int      sceKernelUsleep(unsigned int microseconds);
uint64_t sceKernelGetProcessTime(void);

/* Dynamic symbol resolution inside an already-loaded module. Together with
 * sceSysmoduleLoadModuleInternal this is the route used to reach modules the
 * SDK has no stub for - see projects/decoder_test. */
int sceKernelDlsym(int moduleHandle, const char *symbol, void **addrOut);
int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv,
                             unsigned int flags, void *opt, int *res);

/* ------------------------------------------------------------------------ */
/* Module introspection (libkernel)                                          */
/*                                                                           */
/* Verified present as exported symbols in the SDK's libkernel.so stub, so   */
/* these link. The STRUCT LAYOUTS below are the PS4 ones and are a           */
/* HYPOTHESIS on PS5 - sceKernelGetModuleInfo takes the caller's declared    */
/* size in st_size, so a wrong size is the likely failure mode. Probes that  */
/* use these should hexdump the raw struct alongside the parsed fields so    */
/* the layout can be confirmed rather than assumed.                          */
/* ------------------------------------------------------------------------ */

/* One loadable segment of a mapped module: where it landed and how big. */
typedef struct evo_module_segment {
    void    *address;
    uint32_t size;
    int32_t  prot;
} evo_module_segment_t;          /* 0x10 */

typedef struct evo_module_info {
    size_t               st_size;      /* IN: sizeof(evo_module_info_t) */
    char                 name[256];
    evo_module_segment_t segments[4];
    uint32_t             segment_count;
    uint8_t              fingerprint[20];
} evo_module_info_t;             /* 0x160 on PS4 */

/* sceKernelVirtualQuery: what is actually mapped at an address, independent
 * of any module's own bookkeeping. Useful as a cross-check on the above. */
typedef struct evo_virtual_query_info {
    void    *start;
    void    *end;
    int64_t  offset;
    int32_t  protection;
    int32_t  memory_type;
    uint32_t flags;        /* bit0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed */
    char     name[32];
} evo_virtual_query_info_t;      /* 0x48 */

int sceKernelGetModuleInfo(int handle, evo_module_info_t *info);
int sceKernelGetModuleInfoInternal(int handle, void *infoEx);
int sceKernelGetModuleList(int *handles, size_t max, size_t *count);
int sceKernelVirtualQuery(const void *addr, int flags, void *info, size_t infoSize);

/* ------------------------------------------------------------------------ */
/* System modules (libSceSysmodule)                                          */
/* ------------------------------------------------------------------------ */

int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleIsLoaded(uint16_t id);
int sceSysmoduleIsLoadedInternal(uint32_t id);
int sceSysmoduleUnloadModuleInternal(uint32_t id);

/* ------------------------------------------------------------------------ */
/* User service (libSceUserService)                                          */
/*                                                                           */
/* sceVideoOutOpen needs a user id. Get it from the initial (logged-in) user. */
/* ------------------------------------------------------------------------ */

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *userId);
int sceUserServiceGetLoginUserIdList(void *list);
/* NOTE: libSceUserService exports no sceUserServiceTerminate stub in SDK
 * v0.42 - do not call it, the link will fail. Verified against
 * sce_stubs/libSceUserService.c. */

/* ------------------------------------------------------------------------ */
/* VideoOut (libSceVideoOut)                                                 */
/* ------------------------------------------------------------------------ */

#define SCE_VIDEO_OUT_BUS_TYPE_MAIN               0

/* Pixel formats. The SRGB variants are what a CPU-filled BGRA framebuffer
 * wants. The FLOAT/BT2020 formats are the entry point for HDR work later. */
#define SCE_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SRGB  0x80000000
#define SCE_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB  0x80002200
#define SCE_VIDEO_OUT_PIXEL_FORMAT_A16R16G16B16_FLOAT 0xC1060000

#define SCE_VIDEO_OUT_TILING_MODE_TILE            0
#define SCE_VIDEO_OUT_TILING_MODE_LINEAR          1

#define SCE_VIDEO_OUT_ASPECT_RATIO_16_9           0

/* sceVideoOutSubmitFlip flip modes. */
#define SCE_VIDEO_OUT_FLIP_MODE_VSYNC             1  /* flip on next vblank  */
#define SCE_VIDEO_OUT_FLIP_MODE_HSYNC             2  /* flip immediately     */

typedef struct SceVideoOutBufferAttribute {
    uint32_t pixelFormat;
    uint32_t tilingMode;
    uint32_t aspectRatio;
    uint32_t width;
    uint32_t height;
    uint32_t pitchInPixel;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
} SceVideoOutBufferAttribute;

typedef struct SceVideoOutFlipStatus {
    uint64_t count;
    uint64_t processTime;
    uint64_t tsc;
    int64_t  flipArg;
    uint64_t submitTsc;
    uint64_t reserved0;
    int32_t  gcQueueNum;
    int32_t  flipPendingNum;
    int32_t  currentBuffer;
    uint32_t reserved1;
} SceVideoOutFlipStatus;

typedef struct SceVideoOutResolutionStatus {
    uint32_t fullWidth;
    uint32_t fullHeight;
    uint32_t paneWidth;
    uint32_t paneHeight;
    uint64_t refreshRate;
    float    screenSizeInInch;
    uint16_t flags;
    uint16_t reserved0;
    uint32_t reserved1[3];
} SceVideoOutResolutionStatus;

/* ---- PS5 native path (verified working on 12.70) -------------------------
 *
 * Use THESE, not the PS4-style calls below. Confirmed against the working
 * ps5-payload-dev/SDL video backend (src/video/ps5/SDL_ps5video.c) and by
 * running projects/videoout_test on a 12.70 console.
 *
 * Three things differ from the familiar PS4 sequence:
 *   1. Pass user id 0xff (system). Do NOT call sceUserServiceGetInitialUser -
 *      it returns 0x80940004 in an elfldr payload, which has no user session.
 *   2. Allocate with sceKernelAllocateMainDirectMemory, not
 *      sceKernelAllocateDirectMemory - the payload process has no direct
 *      memory budget of its own (sceKernelGetDirectMemorySize() reports 0).
 *   3. Describe and register buffers with the "2" variants. The pixel format
 *      is a 64-bit value, and pitch is implicit (= width).
 */

#define SCE_VIDEO_OUT_USER_ID_SYSTEM   0xff

/* 64-bit pixel format for the ...Attribute2 API. Memory layout is
 * SDL_PIXELFORMAT_ABGR8888, i.e. a uint32 of 0xAABBGGRR. */
#define SCE_VIDEO_OUT_PIXEL_FORMAT2_A8B8G8R8_SRGB  0x8000000022000000UL

/* Opaque, sized to match the ABI. Never inspect the fields. */
typedef struct SceVideoOutBuffer {
    void     *data;
    uint64_t  reserved[3];
} SceVideoOutBuffer;

typedef struct SceVideoOutBufferAttribute2 {
    uint8_t reserved[80];
} SceVideoOutBufferAttribute2;

void sceVideoOutSetBufferAttribute2(SceVideoOutBufferAttribute2 *attr,
                                    uint64_t pixelFormat, uint32_t tilingMode,
                                    uint32_t width, uint32_t height,
                                    uint64_t opt0, uint32_t opt1,
                                    uint64_t opt2);

int  sceVideoOutRegisterBuffers2(int32_t handle, int32_t startIndex,
                                 int32_t unk, SceVideoOutBuffer *buffers,
                                 int32_t bufferNum,
                                 SceVideoOutBufferAttribute2 *attr,
                                 int32_t unk2, void *unk3);

/* Direct memory from the *main* pool. Signature per the SDL backend:
 *   (length, alignment, memoryType, physAddrOut) */
int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment,
                                      int memoryType, intptr_t *physAddrOut);

int sceSystemServiceHideSplashScreen(void);

/* ---- shared between both paths ------------------------------------------ */

int  sceVideoOutOpen(int32_t userId, int32_t busType, int32_t index,
                     const void *param);
int  sceVideoOutClose(int32_t handle);

/* ---- PS4-style path: present in the stubs, but see the note above -------- */

void sceVideoOutSetBufferAttribute(SceVideoOutBufferAttribute *attribute,
                                   uint32_t pixelFormat, uint32_t tilingMode,
                                   uint32_t aspectRatio, uint32_t width,
                                   uint32_t height, uint32_t pitchInPixel);

int  sceVideoOutRegisterBuffers(int32_t handle, int32_t startIndex,
                                void * const *addresses, int32_t bufferNum,
                                const SceVideoOutBufferAttribute *attribute);
int  sceVideoOutUnregisterBuffers(int32_t handle, int32_t attributeIndex);

int  sceVideoOutSubmitFlip(int32_t handle, int32_t bufferIndex,
                           uint32_t flipMode, int64_t flipArg);
int  sceVideoOutGetFlipStatus(int32_t handle, SceVideoOutFlipStatus *status);
int  sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int  sceVideoOutWaitVblank(int32_t handle);
int  sceVideoOutGetResolutionStatus(int32_t handle,
                                    SceVideoOutResolutionStatus *status);

/* ------------------------------------------------------------------------ */
/* AudioOut (libSceAudioOut)                                                 */
/* ------------------------------------------------------------------------ */

#define SCE_AUDIO_OUT_PORT_TYPE_MAIN              0
#define SCE_AUDIO_OUT_PORT_TYPE_BGM               1

/* Parameter word passed to sceAudioOutOpen: format | (rate << 8). The classic
 * S16 stereo layout is format 0. */
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO       0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO     1
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH        2
#define SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_MONO     3
#define SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO   4

#define SCE_AUDIO_OUT_PARAM_ATTR_NONE             0

/* AudioOut runs at a fixed 48 kHz; the grain (frames per sceAudioOutOutput
 * call) must be one of the supported sizes, 256 being the common default. */
#define SCE_AUDIO_OUT_SAMPLE_RATE                 48000
#define SCE_AUDIO_OUT_GRAIN_DEFAULT               256

#define SCE_AUDIO_VOLUME_0DB                      32768

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t userId, int32_t type, int32_t index,
                    uint32_t length, uint32_t freq, uint32_t param);
int sceAudioOutClose(int32_t handle);
int sceAudioOutOutput(int32_t handle, const void *ptr);
int sceAudioOutSetVolume(int32_t handle, int32_t flag, const int32_t *vol);

/* System service (libSceSystemService): sceSystemServiceHideSplashScreen is
 * declared with the VideoOut group above, since that is where it is used. */

#ifdef __cplusplus
}
#endif

#endif /* EVO_PS5_H */
