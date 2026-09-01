/* =============================================================================
 * PS5 Native Hardware Video Decoder Test (libSceVideodec2)
 *
 * Implements the verified native PS5 Videodec2 hardware decoding pipeline
 * based on the ProsperoLight hardware stream architecture.
 * =============================================================================
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>
#include "evo_ps5.h"

#define ALIGN_16K(x) (((x) + 0x3FFFull) & ~0x3FFFull)
#define ALIGN_256(x) (((x) + 0xFFull) & ~0xFFull)

/* --- VideoOut Display Setup (1080p Double-Buffered) --- */
#define FB_WIDTH   1920
#define FB_HEIGHT  1080
#define FB_COUNT   2
#define FB_MEMSIZE 0x4000000u /* 64 MiB total */

typedef struct DisplayCtx {
    int32_t                     handle;
    void                       *base;
    intptr_t                    paddr;
    SceVideoOutBuffer           buf[FB_COUNT];
    SceVideoOutBufferAttribute2 attr;
    int32_t                     current_buf;
} DisplayCtx;

static inline uint32_t abgr(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static int init_display(DisplayCtx *d) {
    memset(d, 0, sizeof(*d));
    d->handle = sceVideoOutOpen(0xff, SCE_VIDEO_OUT_BUS_TYPE_MAIN, 0, NULL);
    if (d->handle < 0) {
        printf("[-] sceVideoOutOpen failed: 0x%08x\n", d->handle);
        fflush(stdout);
        return -1;
    }

    sceSystemServiceHideSplashScreen();

    int ret = sceKernelAllocateMainDirectMemory(FB_MEMSIZE, 0x20000, SCE_KERNEL_WC_GARLIC, &d->paddr);
    if (ret != 0) {
        printf("[-] FB alloc direct memory failed: 0x%08x\n", ret);
        fflush(stdout);
        return -1;
    }

    ret = sceKernelMapDirectMemory(&d->base, FB_MEMSIZE,
                                   SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_ALL,
                                   0, d->paddr, 0x20000);
    if (ret != 0 || !d->base) {
        printf("[-] FB map direct memory failed: 0x%08x\n", ret);
        fflush(stdout);
        return -1;
    }

    size_t half = FB_MEMSIZE / 2;
    d->buf[0].data = d->base;
    d->buf[1].data = (uint8_t *)d->base + half;

    sceVideoOutSetBufferAttribute2(&d->attr,
                                   SCE_VIDEO_OUT_PIXEL_FORMAT2_A8B8G8R8_SRGB,
                                   0, /* Tiled mode */
                                   FB_WIDTH, FB_HEIGHT,
                                   0, 0, 0);

    ret = sceVideoOutRegisterBuffers2(d->handle, 0, 0, d->buf, FB_COUNT, &d->attr, 0, NULL);
    if (ret != 0) {
        printf("[-] sceVideoOutRegisterBuffers2 failed: 0x%08x\n", ret);
        fflush(stdout);
        return -1;
    }

    printf("[+] Display initialized successfully (1080p)\n");
    fflush(stdout);
    return 0;
}

static void fill_solid(DisplayCtx *d, uint8_t r, uint8_t g, uint8_t b) {
    if (!d->base) return;
    uint32_t color = abgr(r, g, b);
    uint32_t *fb = (uint32_t *)d->buf[d->current_buf].data;
    size_t half = FB_MEMSIZE / 2;
    size_t num_pixels = half / sizeof(uint32_t);

    for (size_t i = 0; i < num_pixels; i++) {
        fb[i] = color;
    }

    sceVideoOutSubmitFlip(d->handle, d->current_buf, SCE_VIDEO_OUT_FLIP_MODE_VSYNC, 0);
    d->current_buf ^= 1;
}

/* Fast planar NV12 to BGRA converter & blitter */
static void blit_nv12_to_display(DisplayCtx *d, const uint8_t *nv12_data, int width, int height, int pitch) {
    if (!d->base || !nv12_data) return;
    uint32_t *dst = (uint32_t *)d->buf[d->current_buf].data;

    int render_w = width < FB_WIDTH ? width : FB_WIDTH;
    int render_h = height < FB_HEIGHT ? height : FB_HEIGHT;

    const uint8_t *y_plane = nv12_data;
    const uint8_t *uv_plane = nv12_data + pitch * height;

    for (int y = 0; y < render_h; y++) {
        const uint8_t *py = y_plane + y * pitch;
        const uint8_t *puv = uv_plane + (y / 2) * pitch;
        uint32_t *pdst = dst + y * FB_WIDTH;

        for (int x = 0; x < render_w; x++) {
            int Y = py[x];
            int U = puv[(x & ~1)] - 128;
            int V = puv[(x & ~1) + 1] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;

            pdst[x] = abgr((uint8_t)R, (uint8_t)G, (uint8_t)B);
        }
    }

    sceVideoOutSubmitFlip(d->handle, d->current_buf, SCE_VIDEO_OUT_FLIP_MODE_VSYNC, 0);
    d->current_buf ^= 1;
}

static void close_display(DisplayCtx *d) {
    if (d->handle >= 0) {
        sceVideoOutClose(d->handle);
    }
    if (d->paddr) {
        sceKernelReleaseDirectMemory(d->paddr, FB_MEMSIZE);
    }
}

/* --- Direct Memory Allocator Helper (ProsperoLight MT 12) --- */
static int32_t allocate_direct(size_t size, int protection, int64_t *start, void **address) {
    size_t aligned_size = ALIGN_16K(size);
    int64_t limit = sceKernelGetDirectMemorySize();
    off_t phys = 0;
    int32_t result = sceKernelAllocateDirectMemory(0, limit, aligned_size, 0x4000, 12, &phys);
    if (result == 0) {
        if (start) *start = phys;
        result = sceKernelMapDirectMemory(address, aligned_size, protection, 0, phys, 0x4000);
    }
    return result;
}

/* =============================================================================
 * Exact ProsperoLight SceVideodec2 ABI Structs & Function Pointers
 * =============================================================================
 */

typedef struct videodec2_decoder_config {
    uint64_t size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t  max_width, max_height, max_dpb_frames;
    uint32_t pipeline_depth;
    uint64_t compute_queue, cpu_affinity;
    int32_t  cpu_priority;
    uint32_t optimize_progressive, check_memory_type, reserved;
} videodec2_decoder_config_t;

typedef struct videodec2_decoder_memory {
    uint64_t size, cpu_size;
    void    *cpu;
    uint64_t gpu_size;
    void    *gpu;
    uint64_t cpu_gpu_size;
    void    *cpu_gpu;
    uint64_t max_frame_size;
    uint32_t frame_alignment, reserved;
} videodec2_decoder_memory_t;

typedef struct videodec2_compute_config {
    uint64_t size;
    uint16_t pipe_id, queue_id;
    uint8_t  check_memory_type, reserved0;
    uint16_t reserved1;
} videodec2_compute_config_t;

typedef struct videodec2_compute_memory {
    uint64_t size, cpu_gpu_size;
    void    *cpu_gpu;
} videodec2_compute_memory_t;

typedef struct videodec2_input {
    uint64_t size;
    void    *au;
    uint64_t au_size, pts, dts, attached;
} videodec2_input_t;

typedef struct videodec2_frame {
    uint64_t size;
    void    *buffer;
    uint64_t buffer_size;
    uint32_t accepted, reserved;
} videodec2_frame_t;

typedef struct videodec2_output {
    uint64_t size;
    uint8_t  valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void    *buffer;
    uint64_t buffer_size;
    uint32_t frame_format, pitch_bytes;
} videodec2_output_t;

typedef int32_t (*fn_sceVideodec2QueryComputeMemoryInfo)(videodec2_compute_memory_t *memory);
typedef int32_t (*fn_sceVideodec2AllocateComputeQueue)(const videodec2_compute_config_t *config,
                                                      const videodec2_compute_memory_t *memory,
                                                      void **queue);
typedef int32_t (*fn_sceVideodec2ReleaseComputeQueue)(void *queue);
typedef int32_t (*fn_sceVideodec2QueryDecoderMemoryInfo)(const videodec2_decoder_config_t *config,
                                                        videodec2_decoder_memory_t *memory);
typedef int32_t (*fn_sceVideodec2CreateDecoder)(const videodec2_decoder_config_t *config,
                                               const videodec2_decoder_memory_t *memory,
                                               void **decoder);
typedef int32_t (*fn_sceVideodec2DeleteDecoder)(void *decoder);
typedef int32_t (*fn_sceVideodec2Reset)(void *decoder);
typedef int32_t (*fn_sceVideodec2Decode)(void *decoder, videodec2_input_t *input,
                                        videodec2_frame_t *frame, videodec2_output_t *output);
typedef int32_t (*fn_sceVideodec2Flush)(void *decoder, videodec2_frame_t *frame,
                                       videodec2_output_t *output);

typedef struct Videodec2Api {
    fn_sceVideodec2QueryComputeMemoryInfo QueryComputeMemoryInfo;
    fn_sceVideodec2AllocateComputeQueue  AllocateComputeQueue;
    fn_sceVideodec2ReleaseComputeQueue   ReleaseComputeQueue;
    fn_sceVideodec2QueryDecoderMemoryInfo QueryDecoderMemoryInfo;
    fn_sceVideodec2CreateDecoder         CreateDecoder;
    fn_sceVideodec2DeleteDecoder         DeleteDecoder;
    fn_sceVideodec2Reset                 Reset;
    fn_sceVideodec2Decode                Decode;
    fn_sceVideodec2Flush                 Flush;
} Videodec2Api;

static void preload_media_modules(void) {
    printf("[*] Preloading video decoder modules...\n");
    fflush(stdout);

    static const char *const kMods[] = {
        "/system/common/lib/libSceVdecCore.sprx",
        "/system/common/lib/libSceVideoDecoderArbitration.sprx",
        "/system/common/lib/libSceVideodec2.sprx",
        "/system/common/lib/libSceVdecwrap.sprx",
        "/system/common/lib/libSceVdecSavc2.sprx",
        "/system/common/lib/libSceVdecShevc.sprx",
    };

    for (size_t i = 0; i < sizeof(kMods)/sizeof(kMods[0]); i++) {
        int res = 0;
        int modid = sceKernelLoadStartModule(kMods[i], 0, NULL, 0, NULL, &res);
        printf("  load %-42s modid=0x%x res=0x%x\n", kMods[i], modid, res);
        fflush(stdout);
    }
}

static int resolve_videodec2(Videodec2Api *api) {
    pid_t pid = getpid();
    uint32_t handle = 0;

    preload_media_modules();
    sceSysmoduleLoadModule(207); /* 207 = SceVideodec2 sysmodule */

    if (kernel_dynlib_handle(pid, "libSceVideodec2.sprx", &handle) != 0) {
        printf("[-] Failed to find libSceVideodec2.sprx in process\n");
        fflush(stdout);
        return -1;
    }

    printf("[+] libSceVideodec2.sprx handle: 0x%08x\n", handle);
    fflush(stdout);

    api->QueryComputeMemoryInfo = (fn_sceVideodec2QueryComputeMemoryInfo)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2QueryComputeMemoryInfo");
    api->AllocateComputeQueue   = (fn_sceVideodec2AllocateComputeQueue)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2AllocateComputeQueue");
    api->ReleaseComputeQueue    = (fn_sceVideodec2ReleaseComputeQueue)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2ReleaseComputeQueue");
    api->QueryDecoderMemoryInfo = (fn_sceVideodec2QueryDecoderMemoryInfo)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2QueryDecoderMemoryInfo");
    api->CreateDecoder          = (fn_sceVideodec2CreateDecoder)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2CreateDecoder");
    api->DeleteDecoder          = (fn_sceVideodec2DeleteDecoder)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2DeleteDecoder");
    api->Reset                  = (fn_sceVideodec2Reset)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2Reset");
    api->Decode                 = (fn_sceVideodec2Decode)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2Decode");
    api->Flush                  = (fn_sceVideodec2Flush)(uintptr_t)kernel_dynlib_dlsym(pid, handle, "sceVideodec2Flush");

    if (!api->QueryDecoderMemoryInfo || !api->CreateDecoder || !api->Decode) {
        printf("[-] Essential Videodec2 symbols missing!\n");
        fflush(stdout);
        return -1;
    }

    printf("[+] All essential Videodec2 symbols resolved.\n");
    fflush(stdout);
    return 0;
}

int main(void) {
    printf("\n==================================================\n");
    printf("  PS5 Native Hardware Video Decoder Test (Videodec2)\n");
    printf("==================================================\n\n");
    fflush(stdout);

    DisplayCtx display;
    if (init_display(&display) == 0) {
        fill_solid(&display, 20, 40, 100); /* Deep Blue */
    }

    evo_notify("Starting Videodec2 native test...");

    Videodec2Api api;
    memset(&api, 0, sizeof(api));

    if (resolve_videodec2(&api) != 0) {
        printf("[-] Videodec2 symbol resolution failed.\n");
        fflush(stdout);
        fill_solid(&display, 180, 20, 20);
        evo_notify("Videodec2 Test: FAILED (Symbols)");
        sceKernelUsleep(5000000);
        close_display(&display);
        return 1;
    }

    /* 1. Configure & Query Decoder Memory */
    printf("[*] Step 1: Configure & Query H.264 Decoder Memory...\n");
    fflush(stdout);

    int32_t res = 0;
    videodec2_decoder_config_t dec_cfg;
    memset(&dec_cfg, 0, sizeof(dec_cfg));
    dec_cfg.size = sizeof(dec_cfg);
    dec_cfg.resource_type = 1; /* 1 */
    dec_cfg.codec_type = 1;    /* 1 = H.264 AVC */
    dec_cfg.profile = 100;     /* 100 = High Profile (matches Big Buck Bunny) */
    dec_cfg.max_level = 51;    /* Level 5.1 */
    dec_cfg.max_width = 1920;
    dec_cfg.max_height = 1088;
    dec_cfg.max_dpb_frames = 4;
    dec_cfg.pipeline_depth = 1;
    dec_cfg.compute_queue = 0;
    dec_cfg.cpu_affinity = 0x3f;
    dec_cfg.cpu_priority = 700;
    dec_cfg.optimize_progressive = 1;

    videodec2_decoder_memory_t dec_mem;
    memset(&dec_mem, 0, sizeof(dec_mem));
    dec_mem.size = sizeof(dec_mem);

    res = api.QueryDecoderMemoryInfo(&dec_cfg, &dec_mem);
    printf("    QueryDecoderMemoryInfo -> 0x%08x\n", res);
    printf("      CPU Workspace:    %llu bytes\n", (unsigned long long)dec_mem.cpu_size);
    printf("      GPU Workspace:    %llu bytes\n", (unsigned long long)dec_mem.gpu_size);
    printf("      Shared Workspace: %llu bytes\n", (unsigned long long)dec_mem.cpu_gpu_size);
    printf("      Max Frame Size:   %llu bytes\n", (unsigned long long)dec_mem.max_frame_size);
    printf("      Frame Alignment:  %u bytes\n", dec_mem.frame_alignment);
    fflush(stdout);

    if (res != 0) {
        fill_solid(&display, 180, 20, 20);
        evo_notify("Videodec2 Test: FAILED (Query)");
        sceKernelUsleep(5000000);
        close_display(&display);
        return 1;
    }

    /* 3. Allocate Decoder Workspaces & Direct Memory Pools */
    printf("[*] Step 3: Allocate Decoder Workspaces & Direct Memory Pools...\n");
    fflush(stdout);

    size_t cpu_size = ALIGN_16K((size_t)dec_mem.cpu_size);
    dec_mem.cpu = malloc(cpu_size > 0 ? cpu_size : 0x10000);
    memset(dec_mem.cpu, 0, cpu_size > 0 ? cpu_size : 0x10000);

    size_t gpu_size = ALIGN_16K((size_t)dec_mem.gpu_size);
    int64_t gpu_start = 0;
    allocate_direct(gpu_size, 0x32, &gpu_start, &dec_mem.gpu);
    dec_mem.gpu_size = gpu_size;

    if (dec_mem.cpu_gpu_size > 0) {
        size_t cpu_gpu_size = ALIGN_16K((size_t)dec_mem.cpu_gpu_size);
        int64_t cpu_gpu_start = 0;
        allocate_direct(cpu_gpu_size, 0x33, &cpu_gpu_start, &dec_mem.cpu_gpu);
        dec_mem.cpu_gpu_size = cpu_gpu_size;
    }

    size_t frame_size = ALIGN_16K((size_t)dec_mem.max_frame_size > 0 ? (size_t)dec_mem.max_frame_size : 0x400000);
    int64_t frame_start = 0;
    void *frame_buffer = NULL;
    allocate_direct(frame_size, 0x32, &frame_start, &frame_buffer);

    size_t input_size = 0x400000; /* 4 MiB AU buffer for 1080p keyframes */
    int64_t input_start = 0;
    void *input_buffer = NULL;
    allocate_direct(input_size, 0x32, &input_start, &input_buffer);

    printf("    CPU @ %p, GPU @ %p, FrameBuffer @ %p, InputBuffer @ %p\n",
           dec_mem.cpu, dec_mem.gpu, frame_buffer, input_buffer);
    fflush(stdout);

    /* 4. Create Decoder */
    void *decoder = NULL;
    res = api.CreateDecoder(&dec_cfg, &dec_mem, &decoder);
    printf("    CreateDecoder -> 0x%08x (decoder: %p)\n", res, decoder);
    fflush(stdout);

    if (res != 0 || !decoder) {
        fill_solid(&display, 180, 20, 20);
        evo_notify("Videodec2 Test: FAILED (CreateDecoder)");
        sceKernelUsleep(5000000);
        close_display(&display);
        return 1;
    }

    if (api.Reset) {
        res = api.Reset(decoder);
        printf("    Reset -> 0x%08x\n", res);
        fflush(stdout);
    }

    /* 5. Load H.264 Test Stream and Decode Frames */
    printf("[*] Step 5: Loading /data/test_au.bin and feeding Access Units...\n");
    fflush(stdout);

    FILE *f = fopen("/data/test_au.bin", "rb");
    if (!f) {
        printf("[-] Failed to open /data/test_au.bin!\n");
        fflush(stdout);
        fill_solid(&display, 180, 20, 20);
        evo_notify("Videodec2: /data/test_au.bin not found");
        sceKernelUsleep(5000000);
        close_display(&display);
        return 1;
    }

    int frame_idx = 0;
    int frames_decoded = 0;
    uint32_t be_len = 0;

    while (fread(&be_len, 1, 4, f) == 4 && frames_decoded < 60) {
        uint32_t au_sz = ((be_len >> 24) & 0xff) |
                         ((be_len >> 8) & 0xff00) |
                         ((be_len << 8) & 0xff0000) |
                         ((be_len << 24) & 0xff000000);

        if (au_sz > input_size) au_sz = input_size;
        fread(input_buffer, 1, au_sz, f);

        printf("  [AU %d] size: %u bytes, feeding to decoder...\n", frame_idx, au_sz);
        fflush(stdout);

        videodec2_input_t input;
        memset(&input, 0, sizeof(input));
        input.size = sizeof(input);
        input.au = input_buffer;
        input.au_size = au_sz;
        input.pts = frame_idx * 33333ULL;
        input.dts = UINT64_MAX;

        videodec2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.size = sizeof(frame);
        frame.buffer = frame_buffer;
        frame.buffer_size = frame_size;

        videodec2_output_t output;
        memset(&output, 0, sizeof(output));
        output.size = sizeof(output);

        res = api.Decode(decoder, &input, &frame, &output);
        printf("    Decode -> 0x%08x (valid=%u, error=%u, accepted=%u)\n",
               res, output.valid, output.error, frame.accepted);
        fflush(stdout);

        if (!output.valid && api.Flush) {
            memset(&output, 0, sizeof(output));
            output.size = sizeof(output);
            res = api.Flush(decoder, &frame, &output);
            printf("    Flush -> 0x%08x (valid=%u, error=%u, %ux%u pitch=%u)\n",
                   res, output.valid, output.error, output.width, output.height, output.pitch);
            fflush(stdout);
        }

        if (output.valid && output.buffer) {
            if (frames_decoded == 0) {
                printf("\n======================================================\n");
                printf("  SUCCESS! PS5 Hardware Decoded Frame Received!\n");
                printf("  Dimensions: %ux%u (pitch: %u)\n", output.width, output.height, output.pitch);
                printf("  Frame Buffer Pointer: %p\n", output.buffer);
                printf("======================================================\n\n");
                fflush(stdout);
                evo_notify("SUCCESS: Hardware Decoded Frame Displayed!");
            }

            int pitch = output.pitch > 0 ? output.pitch : output.width;
            blit_nv12_to_display(&display, (const uint8_t *)output.buffer, output.width, output.height, pitch);
            frames_decoded++;
            sceKernelUsleep(33000); /* 30 fps */
        }
        frame_idx++;
    }
    fclose(f);

    printf("[+] Total Hardware Frames Decoded & Rendered: %d\n", frames_decoded);
    fflush(stdout);

    if (frames_decoded > 0) {
        printf("[+] Holding last decoded frame on TV for 10 seconds...\n");
        fflush(stdout);
        sceKernelUsleep(10000000);
    } else {
        printf("[-] No valid frame output received.\n");
        fflush(stdout);
        fill_solid(&display, 180, 20, 20);
        evo_notify("Videodec2: No valid frame output");
        sceKernelUsleep(5000000);
    }

    /* Cleanup */
    if (api.DeleteDecoder) {
        api.DeleteDecoder(decoder);
    }
    close_display(&display);

    printf("[+] Test finished.\n");
    fflush(stdout);
    return 0;
}
