# SharpProspero — AGC GPU reference for the EVO rendering track

> **What this is:** a study of `SvenGDK/SharpProspero`'s `sceAgc` GPU path,
> cloned to `third_party/SharpProspero/` (git-ignored, commit `e24e25e`,
> https://github.com/SvenGDK/SharpProspero). It complements
> [gpu-rendering-plan.md](gpu-rendering-plan.md), whose primary reference is the
> **hardware-proven C++** `third_party/ProsperoLight/src/native_agc_present.cpp`.
>
> **Roles:** ProsperoLight = the implementation that has run on a real 12.70
> console. SharpProspero = the **cleaner, fully-commented ABI + method
> reference** — every `libSceAgc` entry point with a described signature, the
> command-buffer book-keeping layout, the render-target register model, and a
> clean-room GPU tiling (swizzle) library. SharpProspero's *own* AGC render loop
> is not documented as hardware-verified; treat its ABI as reference, its
> sequencing as a strong hint, and validate against ProsperoLight + hardware.

---

## 1. TL;DR — what SharpProspero gives EVO that we didn't have

| Piece | Where in the clone | Why it matters for EVO |
|---|---|---|
| **Full `libSceAgc` API** — every command builder, every `*GetSize`, shader create/link, register defaults, with described args + packet sizes | `src/SharpProspero/Interop/Agc/SceAgc.cs`, `SceAgcDriver.cs` | The reverse-engineering EVO would otherwise redo by hand. These are plain exported-symbol calls (`[LibraryImport("libSceAgc")]`), so they map 1:1 to C. |
| **DCB book-keeping struct** — the block the flat-C builders read/advance | `Graphics/Agc/DrawCommandBuffer.cs` `State` (offsets 0x00 Bottom / 0x08 Top / 0x10 UpCursor / 0x18 DownCursor / 0x20 Callback / 0x28 UserData / 0x30 ReservedDwords) | Lets EVO build its own DCB wrapper without guessing the struct the builders mutate. |
| **A documented single-draw frame loop** | `Graphics/Renderer3D.cs`, `docs/graphics-gpu.md` | The exact call order: wait-for-display → assemble Cx/Sh/Uc register blocks in direct memory → `sceAgcLinkShaders` → `sceAgcDcbSet{Cx,Sh,Uc}RegistersIndirect` → bind descriptors into shader user-data → draw → `sceAgcDcbSetFlip` (flip on the GPU timeline) → `sceAgcDriverSubmitDcb` → wait-on-screen → rotate N per-frame sets. |
| **Render-target-into-tiled-scanout** | `Graphics/Agc/CxRenderTarget.cs`, `AgcRenderTargetSetup.cs`, `RegisterDefaults.cs`, `DisplayDevice.cs` | The 16-register colour-target block filled from a `RenderTargetSpec`, pointing straight at the `sceVideoOut` back-buffer. **This is what removes EVO's CPU swizzle** — the GPU writes the tiled layout directly; the CPU "rearrange in rows" pass is skipped entirely for GPU-drawn frames. |
| **Clean-room GPU tiling / swizzle** | `Graphics/Agc/AgcSurfaceTiling.cs`, `AgcTiler.cs`, `AgcSurface.cs`, `AgcTilingTables.cs` | A from-scratch reimplementation of the AMD address library: block dims, mip-tail tables, the `offset = (b_y·W_b + b_x)·S_block + e(x,y)` element swizzle, `Tile`/`Detile`. Directly relevant to [converter-perf.md](../converter-perf.md) even if EVO stays on the CPU path a while longer. |
| **Shader-binary container format** | `Graphics/Agc/ShaderBinary.cs`, `AgcShader.cs` | `.sb` = an ELF container with `.shader_header` (magic `0x34333231` = "1234", program-type byte @90, ctx-reg-count @91, sh-reg-count @92) + `.shader_text` microcode. Both copied to GPU-readable direct memory, then `sceAgcCreateShader(&handle, header, code)`. Header carries the register writes the program needs — `AgcShader.ContextRegisters` / `ShaderRegisters` read them straight out. |
| **`DisplayDevice`** — VideoOut open + `SetBufferAttribute2(Bgra8Srgb, tiling)` + `RegisterBuffers2`, tiled vs linear, flip/vblank pacing, the "pitch field must stay 0" gotcha, the size whitelist (1920×1080 the only universally-accepted mode) | `Graphics/DisplayDevice.cs` | Cross-check against `pp/src/pp_videoout.c`; the SRGB-BGRA + `kAlt` channel-order note explains EVO's `0xAABBGGRR` framebuffer rule from the other direction. |
| **Compute dispatch is in the ABI** | `sceAgcCbDispatch` / `sceAgcDcbDispatchIndirect` / `sceAgcAcbDispatchIndirect` in `SceAgc.cs` | A YUV→BGRA **compute** shader (rather than a fullscreen-quad pixel shader) is a viable Step-2 route. |

## 2. What it does NOT give us

- **No PSSL→`.sb` compiler.** SharpProspero *embeds* pre-built `mesh_vs.sb` /
  `mesh_ps.sb` (the `.pssl` sources sit beside them in
  `Graphics/Agc/Shaders/`) and its `shader` tool only *inspects* a blob. This is
  the same gate [gpu-rendering-plan.md](gpu-rendering-plan.md) §2/§Step-2
  already calls out: **EVO needs its own precompiled shader blobs** (a
  fullscreen-triangle VS + a NV12/YUV420→BGRA sample PS, or one convert CS),
  built off-device once with Sony's Prospero PSSL compiler or an equivalent.
  ProsperoLight's embedded NV12 shaders remain the closest ready-made set.
- **No hardware-verification claim for its render loop.** Its multi-firmware
  offset tables and "signatures recovered from the module" show real RE, but
  nothing in the repo says the `Renderer3D` path has been run on 12.70. Trust
  ProsperoLight for "this actually presents a frame."
- **Its media sample is CPU-composited too.** `samples/prospero-media` does
  `frame.RenderTo(surface, …)` — a CPU YUV→RGB blit into a CPU `Surface`, same
  shape as EVO today. Only `Renderer3D` (mesh) uses the GPU. So SharpProspero
  does not hand us a working YUV-on-GPU sample; it hands us the pieces to build
  one.
- **`libSceAgc` / `libSceAgcDriver` link stubs** — not here as C; ProsperoLight
  ships them (`third_party/ProsperoLight/vendor/ps5/sdk/stubs/agc_link_stub.c`,
  `agc_driver_link_stub.c`).

## 3. The frame loop, distilled (from `Renderer3D.cs` + `graphics-gpu.md`)

```
once:
  sceAgcInit(state, defaultsRevision = 8)
  open DisplayDevice: sceVideoOutOpen → SetFlipRate(0)
    → AgcSurface.Compute(desc) for the tiled RenderTarget layout
    → allocate N framebuffers from 2 MiB-aligned direct memory
    → sceVideoOutSetBufferAttribute2(Bgra8Srgb, tiling=Tiled)   // row pitch MUST be left 0
    → sceVideoOutRegisterBuffers2
  prepare shaders: ShaderBinary.Load(.sb) → copy header+code to direct memory
    → sceAgcCreateShader(&h, header, code)   // once per shader
  allocate, per frame-in-flight (= framebuffer count, min 2):
    DrawCommandBuffer (GPU-readable direct memory, ~256 KB)
    Cx / Sh / Uc register-state regions + any constant/descriptor buffers

per frame (slot rotates with the framebuffer):
  constants (matrices / convert coefficients) written into this slot's region
  build Cx block in direct memory:
     CxRenderTarget.Init(RegisterDefaults.RenderTargetBlock())
       → AgcRenderTargetSetup.Initialize(rt, RenderTargetSpec(
             k8_8_8_8, kUNorm, kAlt,               // kAlt = framebuffer carries blue first
             width, height, backBufferGpuAddr,
             tiled ? kRenderTarget : kLinear))
     AgcViewport.SetViewport(0,0,w,h) → WriteTo(block)
     CB_TARGET_MASK (offset 0x008E) = 0xF
     reserve 34 regs, then sceAgcLinkShaders(linkageSlot, ucPrimStateRegion,
             null, vsHandle, psHandle, PRIM_TRIANGLE_LIST=4)
     append vs.ContextRegisters, ps.ContextRegisters
  build Sh block: vs.ShaderRegisters, ps.ShaderRegisters
  dcb.Reset()
  dcb.WaitUntilSafeForDisplay(outHandle, currentBufferIndex)
  sceAgcDcbSetCxRegistersIndirect(dcb, cxRegion, cxCount)
  sceAgcDcbSetShRegistersIndirect(dcb, shRegion, shCount)
  sceAgcDcbSetUcRegistersIndirect(dcb, ucRegion, 3)
  for each shader resource: write 4-dword descriptor at
     GsUserDataBaseOffset(0x008C) + slotDwordOffset   via sceAgcCbSetShRegisterRangeDirect
  dcb.SetIndexSize / SetIndexBuffer / SetIndexCount / DrawIndex     // or DrawIndexAuto(3) for a fullscreen triangle
  sceAgcDcbSetFlip(dcb, outHandle, bufferIndex, VSyncMode=1, frameIndex)
  sceAgcDriverSubmitDcb(&{words, wordCount, flag})                  // wordCount ≤ 0xFFFFF
  sceAgcSuspendPoint()
  wait until sceVideoOutGetFlipStatus.FlipArg ≥ frameIndex (sceVideoOutWaitVblank between polls)
  advance buffer index + frame counter + register-set slot
```

Register-block rhythm (every typed block): `block.Init(driver defaults from
RegisterDefaults)` → typed setters → copy `block.Registers` into the combined
Cx region → one indirect set packet loads them all. Offsets and reset values
come from the driver at runtime, never baked in.

## 4. How this reshapes the gpu-rendering-plan Step 2

[gpu-rendering-plan.md](gpu-rendering-plan.md) Step 2 ("AGC present + composite")
already targets `native_agc_present.cpp`. SharpProspero narrows the unknowns:

1. **The command-layer ABI is now fully specced** — `SceAgc.cs` is effectively
   the header EVO's `pp/src/pp_agc_present.c` needs. Transcribe the ~40 calls it
   actually uses into a C header, link ProsperoLight's stubs.
2. **The render-target-to-scanout path is spelled out** — `CxRenderTarget` +
   `AgcRenderTargetSetup` + `RegisterDefaults.RenderTargetBlock()` +
   `DisplayDevice.Open(tiling: Tiled)`. Adopting this is what lets EVO delete
   the CPU swizzle in `pp_compute_pipeline.c` / `pp_converter_*` on target.
3. **Tiling math is available clean-room** — `AgcSurfaceTiling.cs` /
   `AgcTilingTables.cs` port to C if EVO ever needs to compute a surface layout
   or (host side) detile for `bench.sh` plane-hash parity.
4. **Shape of the shader work is confirmed**: two tiny programs
   (fullscreen-triangle VS, YUV-sample PS) *or* one convert CS. Still gated on
   an off-device PSSL compile — unchanged, and still the single biggest
   open artifact.

**Recommendation:** keep `native_agc_present.cpp` as the implementation
skeleton for `pp/src/pp_agc_present.c`; use SharpProspero's `SceAgc.cs` +
`Renderer3D.cs` + `graphics-gpu.md` as the annotated spec while writing it, and
its `CxRenderTarget` / `AgcRenderTargetSetup` / tiling code as the reference for
the scanout render target that kills the CPU swizzle. Do **Step 1 (dirty-flag
the UI surface)** first regardless — it needs none of this and the profile
(§6 of the plan) may show it's enough.

## 5. Files worth reading in the clone

```
third_party/SharpProspero/
  src/SharpProspero/Interop/Agc/SceAgc.cs         every libSceAgc call, described
  src/SharpProspero/Interop/Agc/SceAgcDriver.cs   submit / queue / flip / wait
  src/SharpProspero/Graphics/Agc/DrawCommandBuffer.cs   DCB State struct + record calls
  src/SharpProspero/Graphics/Agc/AgcShader.cs           shader header field offsets, user-data slots
  src/SharpProspero/Graphics/Agc/ShaderBinary.cs        .sb container parser
  src/SharpProspero/Graphics/Agc/CxRenderTarget.cs      16-reg colour-target block
  src/SharpProspero/Graphics/Agc/AgcRenderTargetSetup.cs  fill it from a spec
  src/SharpProspero/Graphics/Agc/RegisterDefaults.cs    driver reset values
  src/SharpProspero/Graphics/Agc/CxBlend.cs             alpha-blend register block (for UI-over-video)
  src/SharpProspero/Graphics/Agc/AgcSurfaceTiling.cs    the swizzle address library
  src/SharpProspero/Graphics/Agc/Shaders/mesh_{vs,ps}.pssl + .sb   worked example
  src/SharpProspero/Graphics/Renderer3D.cs              the whole frame loop, one draw
  src/SharpProspero/Graphics/DisplayDevice.cs           VideoOut open/register/flip
  docs/graphics-gpu.md                                  prose for all of the above
  samples/prospero-3d/Program.cs                        Renderer3D in use
```

Also relevant: `samples/prospero-payload-unjail/` is a second reference for
[phase-1b-app-module.md](phase-1b-app-module.md) task 7 (`projects/sandbox_unjail`).
