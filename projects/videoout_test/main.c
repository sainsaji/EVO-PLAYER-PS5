/* EVO Player - videoout_test
 *
 * Milestone 3. Exercises the display path end to end:
 *
 *     user id -> sceVideoOutOpen -> buffer attribute -> direct memory
 *             -> sceVideoOutRegisterBuffers -> flip -> present
 *
 * Two scenes, selected with a command line argument:
 *     (default)   solid colour, cycling through a few flat fills
 *     pattern     an RGB test pattern (colour bars, gradient, grey ramp)
 *
 * This is the foundation the GPU YUV renderer eventually replaces: once a
 * decoded frame can be converted to BGRA and shown here, playback works, and
 * only then is it worth moving the conversion onto the GPU.
 *
 * NOT YET RUN ON HARDWARE by the author of this scaffold - the constants come
 * from the documented VideoOut ABI and every symbol used is present in the
 * SDK's libSceVideoOut stub. Treat the first console run as the real test and
 * record the result in docs/validation.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_ps5.h"

/* 1080p. Deliberately not 4K: this test is about proving the pipeline, and a
 * 1080p buffer is a quarter of the memory and flips on every panel. 4K is a
 * later milestone driven by sceVideoOutConfigureOutputMode. */
#define FB_WIDTH   1920
#define FB_HEIGHT  1080
#define FB_COUNT   2            /* double buffered */

/* VideoOut requires the pitch to be a multiple of 64 pixels. 1920 already is;
 * this macro keeps that true if you change the width. */
#define ALIGN_UP(v, a)   (((v) + ((a) - 1)) & ~((a) - 1))
#define FB_PITCH   ALIGN_UP(FB_WIDTH, 64)

#define FB_BYTES   ((size_t)FB_PITCH * FB_HEIGHT * 4)

/* Display buffers are allocated from one direct-memory block, 2 MiB aligned
 * (the alignment the scanout path expects for a linear surface). */
#define DMEM_ALIGN  (2u * 1024u * 1024u)

typedef struct fb {
    int32_t  handle;
    void    *base;              /* mapped CPU pointer to the whole block  */
    off_t    phys;              /* physical address of the block          */
    size_t   total;             /* bytes actually allocated               */
    void    *buf[FB_COUNT];     /* per-buffer pointers into base          */
} fb_t;

static uint32_t
bgra(uint8_t r, uint8_t g, uint8_t b)
{
    /* SCE_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SRGB, little-endian 32-bit word. */
    return (uint32_t)0xff000000u | ((uint32_t)r << 16)
         | ((uint32_t)g << 8) | (uint32_t)b;
}

static void
fill_solid(uint32_t *px, uint32_t colour)
{
    for (unsigned y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = px + (size_t)y * FB_PITCH;
        for (unsigned x = 0; x < FB_WIDTH; x++)
            row[x] = colour;
    }
}

/* Colour bars on top, an RGB gradient in the middle, a grey ramp underneath.
 * Enough to spot wrong pixel order, wrong pitch and wrong colour range at a
 * glance - the three things that actually go wrong first. */
static void
fill_pattern(uint32_t *px)
{
    const uint8_t bar_rgb[8][3] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255},   {255,0,0},   {0,0,255},   {0,0,0}
    };

    const unsigned bar_h  = FB_HEIGHT / 2;
    const unsigned grad_h = FB_HEIGHT / 4;

    for (unsigned y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = px + (size_t)y * FB_PITCH;

        if (y < bar_h) {
            for (unsigned x = 0; x < FB_WIDTH; x++) {
                unsigned i = (x * 8) / FB_WIDTH;
                if (i > 7) i = 7;
                row[x] = bgra(bar_rgb[i][0], bar_rgb[i][1], bar_rgb[i][2]);
            }
        } else if (y < bar_h + grad_h) {
            /* Horizontal red->green->blue sweep. */
            for (unsigned x = 0; x < FB_WIDTH; x++) {
                unsigned t = (x * 255) / (FB_WIDTH - 1);
                row[x] = bgra((uint8_t)(255 - t), (uint8_t)t,
                              (uint8_t)((t * 2) % 256));
            }
        } else {
            /* Grey ramp - reveals banding and any range/limited-range issue. */
            for (unsigned x = 0; x < FB_WIDTH; x++) {
                uint8_t v = (uint8_t)((x * 255) / (FB_WIDTH - 1));
                row[x] = bgra(v, v, v);
            }
        }
    }
}

static int
fb_init(fb_t *fb)
{
    int rc;
    int32_t userId = 0;

    /* sceVideoOutOpen needs the id of the logged-in user. */
    rc = sceUserServiceInitialize(NULL);
    if (rc < 0 && rc != 0) {
        /* Already-initialised is common and benign when another payload ran
         * first; only a hard failure matters. */
        printf("sceUserServiceInitialize -> 0x%08x (continuing)\n", rc);
    }

    rc = sceUserServiceGetInitialUser(&userId);
    if (rc < 0) {
        printf("sceUserServiceGetInitialUser failed: 0x%08x\n", rc);
        return -1;
    }
    printf("initial user id     : 0x%08x\n", userId);

    fb->handle = sceVideoOutOpen(userId, SCE_VIDEO_OUT_BUS_TYPE_MAIN, 0, NULL);
    if (fb->handle < 0) {
        printf("sceVideoOutOpen failed: 0x%08x\n", fb->handle);
        return -1;
    }
    printf("videoout handle     : %d\n", fb->handle);

    /* Report what the panel is actually doing. */
    SceVideoOutResolutionStatus res;
    memset(&res, 0, sizeof res);
    if (sceVideoOutGetResolutionStatus(fb->handle, &res) == 0) {
        printf("panel resolution    : %ux%u @ %lu\n",
               res.fullWidth, res.fullHeight, (unsigned long)res.refreshRate);
    }

    /* -- allocate one block big enough for every buffer ------------------- */
    fb->total = ALIGN_UP(FB_BYTES * FB_COUNT, DMEM_ALIGN);

    rc = sceKernelAllocateDirectMemory(
            0,                                  /* searchStart              */
            (off_t)sceKernelGetDirectMemorySize(),
            fb->total,
            DMEM_ALIGN,
            SCE_KERNEL_WC_GARLIC,               /* write-combined, GPU side */
            &fb->phys);
    if (rc < 0) {
        printf("sceKernelAllocateDirectMemory(%zu) failed: 0x%08x\n",
               fb->total, rc);
        sceVideoOutClose(fb->handle);
        return -1;
    }

    fb->base = NULL;
    rc = sceKernelMapDirectMemory(&fb->base, fb->total,
                                  SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_ALL,
                                  0, fb->phys, DMEM_ALIGN);
    if (rc < 0) {
        printf("sceKernelMapDirectMemory failed: 0x%08x\n", rc);
        sceKernelReleaseDirectMemory(fb->phys, fb->total);
        sceVideoOutClose(fb->handle);
        return -1;
    }
    printf("framebuffer memory  : %zu bytes at %p (phys 0x%lx)\n",
           fb->total, fb->base, (unsigned long)fb->phys);

    for (int i = 0; i < FB_COUNT; i++)
        fb->buf[i] = (uint8_t *)fb->base + (size_t)i * FB_BYTES;

    /* -- describe the surface and register it ----------------------------- */
    SceVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof attr);
    sceVideoOutSetBufferAttribute(&attr,
                                  SCE_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SRGB,
                                  SCE_VIDEO_OUT_TILING_MODE_LINEAR,
                                  SCE_VIDEO_OUT_ASPECT_RATIO_16_9,
                                  FB_WIDTH, FB_HEIGHT, FB_PITCH);

    rc = sceVideoOutRegisterBuffers(fb->handle, 0, fb->buf, FB_COUNT, &attr);
    if (rc < 0) {
        printf("sceVideoOutRegisterBuffers failed: 0x%08x\n", rc);
        sceKernelReleaseDirectMemory(fb->phys, fb->total);
        sceVideoOutClose(fb->handle);
        return -1;
    }
    printf("registered %d buffers (%ux%u, pitch %u)\n",
           FB_COUNT, FB_WIDTH, FB_HEIGHT, FB_PITCH);

    /* 0 = flip as fast as vblank allows (60Hz on a 60Hz panel). */
    sceVideoOutSetFlipRate(fb->handle, 0);
    return 0;
}

static void
fb_present(fb_t *fb, int index)
{
    if (sceVideoOutSubmitFlip(fb->handle, index,
                              SCE_VIDEO_OUT_FLIP_MODE_VSYNC, index) < 0) {
        printf("sceVideoOutSubmitFlip(%d) failed\n", index);
        return;
    }
    sceVideoOutWaitVblank(fb->handle);
}

static void
fb_teardown(fb_t *fb)
{
    sceVideoOutUnregisterBuffers(fb->handle, 0);
    sceVideoOutClose(fb->handle);
    sceKernelReleaseDirectMemory(fb->phys, fb->total);
}

int
main(int argc, char **argv)
{
    int pattern = (argc > 1 && strcmp(argv[1], "pattern") == 0);
    fb_t fb;

    memset(&fb, 0, sizeof fb);

    printf("=== EVO Player videoout_test (%s) ===\n",
           pattern ? "RGB test pattern" : "solid colours");

    if (fb_init(&fb) != 0) {
        evo_notify("EVO videoout_test: initialisation FAILED (see klog)");
        return EXIT_FAILURE;
    }

    evo_notify("EVO videoout_test: %s, %dx%d",
               pattern ? "test pattern" : "solid colour", FB_WIDTH, FB_HEIGHT);

    if (pattern) {
        /* Static image; hold it long enough to photograph. */
        for (int i = 0; i < FB_COUNT; i++)
            fill_pattern((uint32_t *)fb.buf[i]);

        for (int frame = 0; frame < 60 * 10; frame++)
            fb_present(&fb, frame % FB_COUNT);
    } else {
        const uint8_t rgb[4][3] = {
            {200, 30,  30},   /* red    */
            {30,  200, 30},   /* green  */
            {30,  30,  200},  /* blue   */
            {200, 200, 200}   /* white  */
        };

        /* Two seconds per colour, flipping every vblank so we also prove the
         * flip queue keeps up. */
        for (unsigned c = 0; c < 4; c++) {
            for (int frame = 0; frame < 120; frame++) {
                int idx = frame % FB_COUNT;
                if (frame < FB_COUNT)
                    fill_solid((uint32_t *)fb.buf[idx],
                               bgra(rgb[c][0], rgb[c][1], rgb[c][2]));
                fb_present(&fb, idx);
            }
        }
    }

    fb_teardown(&fb);

    printf("videoout_test done\n");
    evo_notify("EVO videoout_test: finished cleanly");
    return EXIT_SUCCESS;
}
