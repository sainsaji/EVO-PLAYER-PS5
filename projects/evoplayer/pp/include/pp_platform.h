/*
 * Slim platform decls for pp_videoout / tile_copy.
 * Product tree — no probe logging surface.
 */
#ifndef PP_PLATFORM_H
#define PP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_VO_ATTR_TILED_BGRA 0x8000000022000000ULL
/* #27: ProsperoLight's SDR framebuffer attribute (linear, no EVO CPU-tiler
 * swizzle). pp_videoout_init registers the VO with this instead of
 * PP_VO_ATTR_TILED_BGRA when the sceAgc GPU present path is armed - the ported
 * render_frame's render-target bits are tuned to this layout. */
#define PP_VO_ATTR_SDR_LINEAR 0x8000000000000000ULL
#define PP_ALIGN_128K         0x20000
#define PP_MEM_1080P_DOUBLE   0x2000000

typedef struct PP_VideoBuf {
    void *data;
    uint64_t junk0[3];
} PP_VideoBuf;

typedef struct PP_VideoAttr {
    uint8_t junk0[80];
} PP_VideoAttr;

int sceKernelAllocateMainDirectMemory(size_t, size_t, int, intptr_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, intptr_t, size_t);
int sceKernelReleaseDirectMemory(intptr_t, size_t);
int sceKernelMunmap(void *, size_t);
int sceKernelCreateEqueue(void **, const char *);
int sceKernelDeleteEqueue(void *);
int sceVideoOutOpen(int, int, int, const void *);
int sceVideoOutClose(int);
int sceVideoOutAddFlipEvent(void *, int, void *);
int sceVideoOutSetFlipRate(int, int);
int sceVideoOutSubmitFlip(int, int, uint32_t, int64_t);
void sceVideoOutSetBufferAttribute2(PP_VideoAttr *, uint64_t, uint32_t,
                                    uint32_t, uint32_t, uint64_t, uint32_t,
                                    uint64_t);
int sceVideoOutRegisterBuffers2(int, int, int, PP_VideoBuf *, int,
                                PP_VideoAttr *, int, void *);
int sceVideoOutUnregisterBuffers(int, int);

/* EVO: real flip-completion reporting.
 *
 * Upstream retired display buffers on a time heuristic ("32 ms have passed,
 * assume the flip finished"), which is why motion tore: the next frame was
 * written into a buffer that could still be scanning out. flipArg carries the
 * argument of the LAST COMPLETED flip, so comparing it against the value we
 * passed to sceVideoOutSubmitFlip tells us exactly when a buffer is free. */
typedef struct PP_VideoOutFlipStatus {
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
} PP_VideoOutFlipStatus;

int sceVideoOutGetFlipStatus(int, PP_VideoOutFlipStatus *);

void pp_draw_pixels_as_tiles(uint32_t *src, uint32_t *dst, int frame_width,
                             int frame_height);

#ifdef __cplusplus
}
#endif

#endif /* PP_PLATFORM_H */
