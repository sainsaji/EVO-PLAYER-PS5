/* EVO Player - yuv_gpu_test
 *
 * FUTURE MILESTONE - placeholder. The brief says explicitly that this must
 * not block initial environment setup, so it is scaffolded and no more.
 *
 * TARGET PIPELINE
 *     Y plane + UV plane  ->  GPU shader  ->  RGB  ->  VideoOut
 *
 * WHY THIS MATTERS
 *   Software YUV->RGB (libswscale) is the single biggest CPU cost in 4K
 *   playback. Moving it to the GPU is what makes 4K SDR and HEVC 10-bit
 *   realistic. It is also the natural place to implement HDR tone mapping
 *   later, since the conversion already touches every pixel.
 *
 * WHY NOT RAW GNM
 *   See projects/gpu_test: the SDK exposes GNM *stubs* but no headers, no
 *   Gnmx helpers and no shader compiler, so raw GNM means hand-writing PM4
 *   packets. The pacbrew sysroot already provides mesa and SDL2, so the
 *   productive route is an SDL2/OpenGL texture upload with a fragment shader
 *   doing the colour conversion. Start from SDL_CreateTexture with
 *   SDL_PIXELFORMAT_NV12 and only drop to a custom shader if that is not
 *   flexible enough for 10-bit.
 *
 * PREREQUISITE: videoout_test must present correctly first.
 */

#include <stdio.h>
#include <stdlib.h>

#include "evo_ps5.h"

int
main(void)
{
    printf("=== EVO Player yuv_gpu_test ===\n");
    printf("Not implemented yet - future milestone.\n");
    printf("Planned: Y/UV planes -> GPU shader -> RGB -> VideoOut,\n");
    printf("built on SDL2 + mesa rather than raw GNM. See docs/gpu-notes.md.\n");

    evo_notify("EVO yuv_gpu_test: placeholder (not implemented)");
    return EXIT_SUCCESS;
}
