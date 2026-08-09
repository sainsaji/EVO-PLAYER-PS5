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

## The mesa + SDL2 route does NOT work — measured 2026-08-09

An earlier version of this document recommended SDL2 NV12 textures on the
grounds that the sysroot ships mesa and that mesa's `radeonsi` driver targets
the same RDNA2 hardware. **That was wrong**, and it was wrong in the one way
that matters: what is shipped is a *software* rasteriser.

Checked directly against the image:

```
libGL.so -> libOSMesa.so -> libOSMesa.so.8.0.0     (92 MB, Off-Screen Mesa)

gallium drivers linked inside it:
  radeonsi   0        <-- the hardware driver is absent
  llvmpipe   575
  softpipe   380
  swr        156

/dev/dri, renderD references:  none
no libradeonsi, libamdgpu, libgallium, libvulkan or DRI modules in the sysroot

SDL2 2.30.12 video drivers compiled in:  dummy, offscreen
                                         (no PS5, GNM or KMSDRM backend)
```

So `SDL_RenderCopy` on an NV12 texture would convert **on the CPU through
llvmpipe**, and SDL2 has no video backend that can present on this platform
anyway. It would be slower than `pp/src/pp_converter_fused.c`, which is
already multithreaded and writes straight into the PS5 tile layout.

`SDL_UpdateNVTexture` and `SDL_PIXELFORMAT_NV12` *do* exist in the headers
(SDL 2.30.12), which is presumably what made the original claim look safe.
Their presence says nothing about acceleration.

## What that leaves

There is no open hardware GL/Vulkan path on this SDK today. The options are:

1. **Keep improving the CPU converter.** `pp_converter_fused.c` is the real
   lever and it is measurable on the host — no console needed to benchmark a
   YUV→BGRA+swizzle kernel. Wider SIMD, fewer passes and better cache
   behaviour are all on the table.
2. **Raw GNM.** `libSceGnmDriver.so` resolves the submit entry points, but you
   must hand-assemble the PM4 stream and supply precompiled shader binaries,
   with no headers, no Gnmx and no PSSL compiler. Large, speculative.
3. **Wait for the ecosystem.** If a radeonsi/DRI port lands in pacbrew, this
   reopens cheaply — re-run the checks above to find out.

Do not re-scaffold `projects/yuv_gpu_test` against SDL2 without re-checking
the driver list first. That is the assumption that failed.

## HDR

`sceVideoOutSetHdrMetadata` and `sceVideoOutSysSetHdrMetadata` are present in
the VideoOut stub, and the pixel-format constants include
`A16R16G16B16_FLOAT`. So the *plumbing* exists. Whether HDR output can actually
be driven from a payload — as opposed to from a licensed title — is unverified
and should be treated as an open research question, not a planned feature.

Investigate in this order: get 10-bit HEVC decoding to P010, present it as SDR
with correct tone mapping, and only then experiment with HDR metadata.
