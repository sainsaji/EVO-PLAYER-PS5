/* EVO Player - videoout_test
 *
 * Milestone 3. Exercises the display path end to end:
 *
 *     sceVideoOutOpen -> main direct memory -> map
 *                     -> SetBufferAttribute2 -> RegisterBuffers2
 *                     -> SubmitFlip -> present
 *
 * Two scenes, run back to back (elfldr passes no argv):
 *     solid     red / green / blue / white, ~2 s each
 *     pattern   horizontal colour bands, one scanout tile tall each
 *
 * ---------------------------------------------------------------------------
 * THIS IS THE PS5 PATH, NOT THE PS4 ONE.
 *
 * The familiar PS4 sequence does not work from an ps5-payload-elfldr payload.
 * Measured on firmware 12.70:
 *
 *   sceUserServiceGetInitialUser()  -> 0x80940004
 *       A payload has no user session. Pass user id 0xff (system) to
 *       sceVideoOutOpen instead and skip UserService entirely.
 *
 *   sceKernelGetDirectMemorySize()  -> 0
 *       The payload's host process has no direct memory budget, so deriving
 *       an allocator search range from it yields an empty range. Use
 *       sceKernelAllocateMainDirectMemory, which draws from the main pool.
 *
 *   sceVideoOutSetBufferAttribute / RegisterBuffers
 *       Superseded on PS5 by the "2" variants, which take a 64-bit pixel
 *       format and an array of buffer descriptors rather than raw pointers.
 *       Pitch is implicit (= width).
 *
 * The working sequence below matches ps5-payload-dev/SDL's video backend
 * (src/video/ps5/SDL_ps5video.c), which is known-good on this platform.
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_ps5.h"

#define FB_WIDTH   1920
#define FB_HEIGHT  1080
#define FB_COUNT   2                    /* double buffered */

/* PS5 scanout tile geometry, from ps5-payload-dev/SDL (SDL_ps5tilemap.inc). */
#define PS5_TILE_WIDTH   512
#define PS5_TILE_HEIGHT  128

/* Matches the SDL backend: one 64 MiB block, second buffer at the halfway
 * point. A 1920x1080x4 frame is ~8 MiB, so this is comfortably oversized and
 * keeps both buffers well clear of each other. */
#define FB_MEMSIZE  0x4000000u          /* 64 MiB total   */
#define FB_ALIGN    0x20000u            /* 128 KiB        */
#define FB_MEMTYPE  3                   /* WC_GARLIC      */
#define FB_PROT     0x33                /* CPU RW | GPU all */

/* Tiling mode passed to sceVideoOutSetBufferAttribute2.
 *
 * ps5-payload-dev/SDL passes 0 here and then swizzles every pixel through a
 * generated lookup table (SDL_ps5tilemap.inc, PS5_DrawPixelsAsTiles) - i.e.
 * mode 0 is a TILED surface, and writing to it linearly scrambles any image
 * that is not a flat colour.
 *
 * 1 requests a LINEAR surface, which would be far more convenient for a video
 * player. TESTED ON 12.70 AND REJECTED - the console replies:
 *
 *   [VideoOut] Tiling Mode Error: Linear format is only valid with
 *   "Enhanced Display Buffer Attribute" enabled (at Debug Settings)
 *   sceVideoOutRegisterBuffers2 -> 0x80290007
 *
 * That is a devkit debug-menu option, so on a retail console TILING IS
 * MANDATORY. Any non-uniform image must therefore be swizzled through the
 * tile map (ps5-payload-dev/SDL generates one in SDL_ps5tilemap.inc).
 * Tile geometry: 512 x 128 pixels.
 *
 * Override at build time:  make FB_TILING=0 */
#ifndef FB_TILING
#define FB_TILING   0                   /* 0 = tiled (required on retail), 1 = linear */
#endif

typedef struct fb {
    int32_t                     handle;
    void                       *base;
    intptr_t                    paddr;
    size_t                      memsize;
    SceVideoOutBuffer           buf[FB_COUNT];
    SceVideoOutBufferAttribute2 attr;
} fb_t;

/* The framebuffer is ABGR8888: a uint32 laid out 0xAABBGGRR. Getting this
 * backwards shows up immediately as swapped red and blue. */
static inline uint32_t
abgr(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* Fill the WHOLE buffer region, not just width*height pixels.
 *
 * Observed on hardware: filling exactly 1920*1080 left a black wedge in the
 * bottom-right corner. The scanout surface is padded beyond width*height (it
 * is tiled - see the note on FB_TILING below), so the tail was never written.
 * A solid colour is invariant under any pixel permutation, so covering the
 * entire region is both correct and the simplest fix. */
static void
fill_solid(uint32_t *px, size_t bytes, uint32_t colour)
{
    for (size_t i = 0; i < bytes / sizeof *px; i++)
        px[i] = colour;
}

/* HORIZONTAL colour bands, one tile-row tall each.
 *
 * Why bands and not the usual vertical colour bars: the PS5 scanout surface is
 * TILED (512x128 pixels per tile - see FB_TILING above), so a linear write of
 * a non-uniform image comes out scrambled. Rendering an arbitrary image at
 * this level needs the full swizzle table that ps5-payload-dev/SDL generates
 * in SDL_ps5tilemap.inc.
 *
 * A band that is exactly one tile tall and spans the full width makes every
 * tile it touches a single flat colour. Flat tiles are invariant under any
 * intra-tile permutation, so the bands display correctly with no swizzle
 * table at all - while still proving channel order and vertical geometry.
 *
 * Vertical bars cannot do this: 1920 / 512 = 3.75, so bar edges would fall
 * inside tiles and scramble.
 *
 * The buffer region is filled to the end so no padding is left black. */
static void
fill_pattern(uint32_t *px, size_t bytes)
{
    /* Deliberately ordered so a red/blue channel swap is unmistakable: pure
     * red sits directly above pure blue. */
    static const uint8_t band[][3] = {
        {255, 255, 255},   /* white   */
        {255, 255,   0},   /* yellow  */
        {  0, 255, 255},   /* cyan    */
        {  0, 255,   0},   /* green   */
        {255,   0, 255},   /* magenta */
        {255,   0,   0},   /* RED     */
        {  0,   0, 255},   /* BLUE    */
        {128, 128, 128},   /* grey    */
        {  0,   0,   0},   /* black   */
    };
    const size_t nband = sizeof band / sizeof *band;
    const size_t rowpx = FB_WIDTH;
    const size_t total = bytes / sizeof *px;

    for (size_t i = 0; i < total; i++) {
        size_t y = i / rowpx;
        size_t b = (y / PS5_TILE_HEIGHT) % nband;
        px[i] = abgr(band[b][0], band[b][1], band[b][2]);
    }
}

static int
fb_init(fb_t *fb)
{
    int rc;
    void *vaddr = NULL;

    memset(fb, 0, sizeof *fb);
    fb->memsize = FB_MEMSIZE;

    /* Get the PS5 logo/splash out of the way before drawing. */
    sceSystemServiceHideSplashScreen();

    /* 0xff = system user. A payload has no logged-in user session. */
    fb->handle = sceVideoOutOpen(SCE_VIDEO_OUT_USER_ID_SYSTEM, 0, 0, NULL);
    if (fb->handle < 0) {
        printf("sceVideoOutOpen failed: 0x%08x\n", fb->handle);
        return -1;
    }
    printf("videoout handle     : %d\n", fb->handle);

    /* Direct memory is the awkward part in a payload process.
     *
     * ps5-payload-elfldr spawns the payload with dmem#0 - no direct memory
     * budget at all (visible in klog as "FMEM x/y 0.0/0.0 payload.elf"), so
     * the 64 MiB the SDL backend asks for comes back EAGAIN (0x80020023).
     *
     * Rather than guess, try progressively smaller sizes and both allocators,
     * reporting each result. Two 1920x1080x4 buffers need 16 MiB, so anything
     * from 16 MiB up is enough.
     *
     * Measured on 12.70 under elfldr: 64 MiB (the value SDL's backend uses)
     * fails with EAGAIN, 32 MiB succeeds. So start at 32. */
    static const size_t sizes[] = {
        0x2000000,   /* 32 MiB - works under elfldr on 12.70 */
        0x1000000,   /* 16 MiB - exactly two 1080p buffers   */
    };

    rc = -1;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes && rc; i++) {
        fb->memsize = sizes[i];

        rc = sceKernelAllocateMainDirectMemory(fb->memsize, FB_ALIGN,
                                               FB_MEMTYPE, &fb->paddr);
        printf("AllocateMainDirectMemory(%8zu KiB) -> 0x%08x\n",
               fb->memsize / 1024, rc);
        if (!rc) break;

        /* The non-"Main" allocator draws from a different pool; worth a try
         * when the main one is exhausted. 16 GiB search range because
         * sceKernelGetDirectMemorySize() reports 0 here. */
        rc = sceKernelAllocateDirectMemory(0, (off_t)16 * 1024 * 1024 * 1024,
                                           fb->memsize, FB_ALIGN,
                                           FB_MEMTYPE, (off_t *)&fb->paddr);
        printf("AllocateDirectMemory    (%8zu KiB) -> 0x%08x\n",
               fb->memsize / 1024, rc);
    }

    if (rc) {
        printf("\nno direct memory available to this payload.\n"
               "The elfldr host process is spawned with dmem#0. A payload that\n"
               "needs a framebuffer has to run in a process with a graphics\n"
               "budget - launch it through ps5-payload-websrv instead.\n");
        sceVideoOutClose(fb->handle);
        return -1;
    }
    printf("direct memory       : %zu bytes at phys 0x%lx\n",
           fb->memsize, (unsigned long)fb->paddr);

    rc = sceKernelMapDirectMemory(&vaddr, fb->memsize, FB_PROT, 0,
                                  fb->paddr, FB_ALIGN);
    if (rc) {
        printf("sceKernelMapDirectMemory failed: 0x%08x\n", rc);
        sceKernelReleaseDirectMemory(fb->paddr, fb->memsize);
        sceVideoOutClose(fb->handle);
        return -1;
    }
    fb->base = vaddr;
    printf("mapped at           : %p\n", fb->base);

    fb->buf[0].data = (uint8_t *)vaddr;
    fb->buf[1].data = (uint8_t *)vaddr + (fb->memsize / 2);

    sceVideoOutSetFlipRate(fb->handle, 0);

    printf("tiling mode         : %d (%s)\n", FB_TILING,
           FB_TILING ? "linear" : "tiled");

    sceVideoOutSetBufferAttribute2(&fb->attr,
                                   SCE_VIDEO_OUT_PIXEL_FORMAT2_A8B8G8R8_SRGB,
                                   FB_TILING, FB_WIDTH, FB_HEIGHT, 0, 0, 0);

    rc = sceVideoOutRegisterBuffers2(fb->handle, 0, 0, fb->buf, FB_COUNT,
                                     &fb->attr, 0, NULL);
    if (rc) {
        printf("sceVideoOutRegisterBuffers2 failed: 0x%08x\n", rc);
        printf("  tiling mode %d rejected - rebuild with FB_TILING=%d\n",
               FB_TILING, !FB_TILING);
        sceKernelReleaseDirectMemory(fb->paddr, fb->memsize);
        sceVideoOutClose(fb->handle);
        return -1;
    }
    printf("registered %d buffers (%ux%u)\n", FB_COUNT, FB_WIDTH, FB_HEIGHT);
    return 0;
}

static void
fb_present(fb_t *fb, int index, int64_t frame_id)
{
    /* flip mode 1 = on next vblank */
    int rc = sceVideoOutSubmitFlip(fb->handle, index, 1, frame_id);
    if (rc) {
        printf("sceVideoOutSubmitFlip(%d) failed: 0x%08x\n", index, rc);
        return;
    }
    sceVideoOutWaitVblank(fb->handle);
}

static void
fb_teardown(fb_t *fb)
{
    sceVideoOutClose(fb->handle);
    sceKernelReleaseDirectMemory(fb->paddr, fb->memsize);
}

int
main(int argc, char **argv)
{
    fb_t fb;

    /* ps5-payload-elfldr passes no arguments (argv[0] is always
     * "payload.elf"), so the scene cannot be selected from the command line
     * the way it can under websrv. Run BOTH scenes in sequence instead - one
     * deploy then shows everything worth looking at.
     *
     * Passing "solid" or "pattern" still works when a loader does supply
     * argv. */
    int want_solid   = 1;
    int want_pattern = 1;
    if (argc > 1) {
        want_solid   = (strcmp(argv[1], "solid")   == 0);
        want_pattern = (strcmp(argv[1], "pattern") == 0);
    }

    printf("=== EVO Player videoout_test ===\n");

    if (fb_init(&fb) != 0) {
        evo_notify("EVO videoout_test: init FAILED (see klog / stdout)");
        return EXIT_FAILURE;
    }

    evo_notify("EVO videoout_test: %dx%d, watch the TV", FB_WIDTH, FB_HEIGHT);

    int64_t frame_id = 0;

    if (want_solid) {
        static const uint8_t rgb[4][3] = {
            {200,  30,  30},   /* red   */
            { 30, 200,  30},   /* green */
            { 30,  30, 200},   /* blue  */
            {200, 200, 200}    /* white */
        };

        for (unsigned c = 0; c < 4; c++) {
            printf("solid colour %u: rgb(%u,%u,%u)\n",
                   c, rgb[c][0], rgb[c][1], rgb[c][2]);
            for (int f = 0; f < 120; f++) {          /* ~2 s at 60 Hz */
                int idx = f % FB_COUNT;
                if (f < FB_COUNT)
                    fill_solid((uint32_t *)fb.buf[idx].data, fb.memsize / FB_COUNT,
                               abgr(rgb[c][0], rgb[c][1], rgb[c][2]));
                fb_present(&fb, idx, frame_id++);
            }
        }
    }

    if (want_pattern) {
        printf("RGB test pattern (bars / gradient / grey ramp)\n");
        evo_notify("EVO videoout_test: RGB test pattern");
        for (int i = 0; i < FB_COUNT; i++)
            fill_pattern((uint32_t *)fb.buf[i].data, fb.memsize / FB_COUNT);

        /* Hold it long enough to look at and photograph. */
        for (int f = 0; f < 60 * 8; f++)
            fb_present(&fb, f % FB_COUNT, frame_id++);
    }

    fb_teardown(&fb);

    printf("videoout_test done (%lld frames)\n", (long long)frame_id);
    evo_notify("EVO videoout_test: finished cleanly");
    return EXIT_SUCCESS;
}
