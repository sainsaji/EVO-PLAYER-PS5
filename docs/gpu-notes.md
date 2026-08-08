# GPU on the PS5 payload SDK

Findings from the v0.42 audit, and what they mean for the GPU YUV renderer.

## What exists

`sce_stubs/libSceGnmDriver.c` gives link-time symbols for the GNM driver:

```
sceGnmSubmitCommandBuffers          sceGnmSubmitAndFlipCommandBuffers
sceGnmSubmitCommandBuffersForWorkload   sceGnmSubmitDone
sceGnmAreSubmitsAllowed             sceGnmRequestFlipAndSubmitDone
sceGnmInsertWaitFlipDone            sceGnmValidateOnSubmitEnabled
```

`libSceGnmDriverForNeoMode.c` also exists — that is the PS4-Pro-mode variant
and **not** what a PS5 payload wants.

`projects/gpu_test` probes all of this at run time and reports what resolves.

## What does not exist

- **No GNM headers.** `include/ps5/` has `kernel.h`, `klog.h`, `mdbg.h`,
  `nid.h`, `payload.h` and nothing else.
- **No Gnmx.** Sony's C++ helper library that builds command buffers for you is
  proprietary and is not reproduced anywhere open.
- **No shader compiler.** There is no open PSSL compiler producing PS5 shader
  binaries.

So you can *call* `sceGnmSubmitCommandBuffers`, but you must hand-assemble the
PM4 packet stream and supply pre-compiled shader binaries to have anything
worth submitting. That is a large reverse-engineering project on its own.

## The productive route: mesa + SDL2

The pacbrew sysroot already ships **mesa** and **SDL2** (both in
`ci-libs.sh`'s package list, both present in the image). Mesa's `radeonsi`
driver targets the same RDNA2 hardware through an open stack, and SDL2 sits on
top of it.

For YUV→RGB specifically, start here before writing any shader:

```c
SDL_Texture *tex = SDL_CreateTexture(renderer,
                                     SDL_PIXELFORMAT_NV12,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     width, height);
SDL_UpdateNVTexture(tex, NULL, yPlane, yPitch, uvPlane, uvPitch);
SDL_RenderCopy(renderer, tex, NULL, NULL);
```

SDL2 performs the colour conversion on the GPU for NV12/IYUV textures. If that
is sufficient, the entire "GPU YUV converter" milestone collapses into a few
lines. Only drop to a custom fragment shader when it is not — most likely for
**10-bit HEVC** (`P010`), where SDL2's format support is thinner, and for HDR
tone mapping.

This is why `projects/yuv_gpu_test` is scaffolded against SDL2 rather than GNM.

## Ordering

1. `videoout_test` must present correctly first — a working scanout path is the
   prerequisite for everything else.
2. Get software `libswscale` conversion working end to end in the player. Slow
   is fine; correct is the point.
3. Swap in `SDL_PIXELFORMAT_NV12` and measure. This is where the 4K win is.
4. Only then consider a custom shader, for P010 / 10-bit / HDR.
5. Raw GNM is a last resort, and probably never.

## HDR

`sceVideoOutSetHdrMetadata` and `sceVideoOutSysSetHdrMetadata` are present in
the VideoOut stub, and the pixel-format constants include
`A16R16G16B16_FLOAT`. So the *plumbing* exists. Whether HDR output can actually
be driven from a payload — as opposed to from a licensed title — is unverified
and should be treated as an open research question, not a planned feature.

Investigate in this order: get 10-bit HEVC decoding to P010, present it as SDR
with correct tone mapping, and only then experiment with HDR metadata.
