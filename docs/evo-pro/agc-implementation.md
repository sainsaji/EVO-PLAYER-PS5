# AGC implementation guide — GPU rendering Step 2/3

> Companion to [gpu-rendering-plan.md](gpu-rendering-plan.md). That doc is the
> *why* and the ladder; this is the *how* — the ProsperoLight AGC path read
> line by line, the shader blobs disassembled, and the concrete EVO port.
>
> **Status: 2026-09-03.** Gate PASSED on hardware — `sceAgcInit` +
> `sceAgcGetRegisterDefaults` work from `PPSA99039` via positional PRX import
> stubs. `pp_agc_init` (§3 `initialize_presenter` first half — shader scratch,
> blob copy, `CreateShader` ×2, `LinkShaders`) is **hardware-verified**
> (`create=0x0 link=0x0`). **`agc_render_frame` (§3) is now ported into
> `pp/src/pp_agc.c` and the present path is wired into `pp_playback.c`'s V8
> branch — builds green (app-module + host), not yet run on hardware.** What
> remains is a device run + the deltas in §4a below.

---

## 0. What the references actually prove

| Reference | GPU pipeline it demonstrates | Hardware-verified? |
|---|---|---|
| **ProsperoLight** `native_agc_present.cpp` | Fullscreen textured quads: NV12/P010→RGB sample, a second quad for the overlay composite, `sceAgcDcbSetFlip`. Render target points straight at the `sceVideoOut` back-buffer. | ✅ **yes** — this is "what runs" |
| **SharpProspero** `Graphics/Agc/` (`Renderer3D.DrawMesh`, `MeshBuffer`, `CxRenderTarget`, `AgcRenderTargetSetup`, `AgcViewport`, `AgcBufferDescriptor`) | The **full** pipeline: arbitrary structured vertex buffer + 32-bit index buffer, constant buffers, `sceAgcLinkShaders(…, PrimitiveTriangleList=4)`, `sceAgcDcbSetIndexBuffer` + `DrawIndex`, viewport, target write-mask, blend. Its `prospero-3d` sample draws an indexed cube mesh. | ⚠️ **clean-room, ABI-solid, render loop not claimed on device** (managed C#/.NET — the *code* isn't liftable, the *register model + ABI* is) |

**ProsperoLight renders its own RmlUi UI on the CPU** — `main.cpp:972`
`SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"software")`, `:977`
`SDL_CreateSoftwareRenderer`, `SdlRenderInterface`→`SDL_RenderGeometry` into an
`SDL_Surface`. The GPU only touches the decoded video frame, the loading screen,
and compositing that CPU-drawn surface. **ProsperoLight = EVO's Step 1 + Step 2.
It is not a Step 3 reference — SharpProspero is.**

Consequence for sequencing: Step 2 rides ProsperoLight's *proven* fullscreen
path. Step 3's arbitrary-geometry draw is only *structurally* proven
(SharpProspero) — so do it after Step 2 has the shared plumbing (shaders, DCB
submit, VideoOut, panic behaviour) known-good on device.

`sceAgcDcbDrawIndex` / `DrawIndexAuto` take any vertex+index buffer — nothing
restricts the GPU to fullscreen quads. RmlUi's
`RenderGeometry(vertices, indices, texture, translation)` is a triangle list
with position + `ColourbPremultiplied` + UV per vertex → maps 1:1 onto
`Renderer3D`'s `Vertex(pos, normal, uv, colour)` (drop the normal), a 2D-ortho
MVP, and a texture bind.

---

## 1. The one blocker, and the way around it

There is **no PSSL→`.sb` compiler** in the SDK or anywhere reachable. Sony's
`orbis-wave-psslc` is proprietary. ProsperoLight ships **prebuilt** shader
binaries and never compiles one.

**But `llvm-mc-18` (in the dev container) assembles and disassembles AMD GCN /
RDNA2 ISA for `--triple=amdgcn--amdpal --mcpu=gfx1030`.** Full workflow wired +
verified 2026-09-02 as **`tools/build-shader.sh`**:

```bash
./tools/build-shader.sh pp/shaders/rgba_ps.s   # .s -> raw .text blob + disasm
```

It assembles with `llvm-mc-18`, then `llvm-objcopy-18 -O binary
--only-section=.text` strips the ELF wrapper to the bare code half of the
`(header, code)` pair `sceAgcCreateShader` wants. `pp/shaders/rgba_ps.s`
(112-byte blob) round-trips cleanly.

> The container has `llvm-mc-18` / `llvm-objcopy-18` / `llc-18` / `clang-18`
> with the **amdgcn target**, but **no** `glslang` / `spirv-tools` /
> `llvm-spirv` / Mesa / RGA, and the image is unprivileged (no apt). A nicer
> **GLSL → SPIR-V → RDNA2 ISA** path (LLVM AMDGPU backend / Radeon GPU Analyzer
> — see §7) would need a Dockerfile change; hand-written GCN is what works today
> and the shaders are small enough that it is fine.

So the real situation is:

| Need | Have it? |
|---|---|
| NV12→RGB fullscreen convert PS | ✅ `pixel.text.linear-buffer.bin` (ProsperoLight) |
| P010 (10-bit HDR) convert PS | ✅ `pixel.text.p010-passthrough.bin` |
| Fullscreen-quad VS | ✅ `geometry.text.bin` |
| **RGBA-passthrough PS** (for compositing the cached RmlUi surface) | 🚧 written — `pp/shaders/rgba_ps.s`, assembles, **not run on hardware** |
| Solid-colour / textured-triangle PS+VS (Step 3) | ❌ — larger hand-written set, same workflow |

The hard part of a hand-written shader is **not the ISA** — it is the Sony
**shader *header*** blob that `sceAgcCreateShader(shader, header, code)` consumes
(SPI register config, VGPR/SGPR counts, the resource-table layout that
`render_frame` reads back at byte offsets +24/+32/+91/+92 of the shader object).
Mitigation: an RGBA-passthrough PS has the *same input signature* as the NV12
one (fullscreen quad, sample image(s), write MRT0) and uses **fewer** resources,
so **reuse `pixel.header.bin` verbatim and swap only the `.text`**. Over-
allocated GPRs waste a little occupancy; they do not break correctness. This is
the first thing to validate on device.

---

## 2. Shader blobs — what's in them

`third_party/ProsperoLight/assets/private/` (git-ignored clone). Each shader is
a `.header.bin` + `.text.bin` pair; `sceAgcCreateShader` binds them together.

### `pixel.text.linear-buffer.bin` (2304 B) — the video convert PS

Disassembled (`llvm-objdump -d --mcpu=gfx1030`), the body is unmistakable:

```
v_interp_p1/p2_f32  v2, v0/v1, attr0.x       ; UV.x from the quad
v_interp_p1/p2_f32  v3, v0/v1, attr0.y       ; UV.y
image_sample  v4,     v[2:3], s[0:7],  s[16:19]  dmask:0x1   ; Y  plane (1 ch)
image_sample  v[0:1], v[2:3], s[8:15], s[20:23]  dmask:0x3   ; UV plane (2 ch)
... v_fma / v_mul  against s[24:27] (the constant buffer)      ; YUV→RGB matrix
exp mrt0 ...                                                   ; RGB out
```

- `s[0:7]` / `s[8:15]` — image descriptors for the **Y** and **interleaved UV**
  planes → **NV12**. Built at runtime by `bind_pixel_source()`.
- `s[16:19]` / `s[20:23]` — bilinear samplers (`kNativeAgcBilinearSamplerWord`).
- `s[24:27]` — the pixel constant buffer. `render_frame` fills it from
  `pixel_constants[16]`:
  ```
  {1.0, 1.0, 1.0, 0,  1.855(0x3fed844d), -0.187(0xbe3fd0d0), 0, 0,
   0, -0.468(0xbeefad6d), 1.575(0x3fc9930c), 0,  <vis_w>, <vis_h>, <pitch>, <pitch/2>}
  ```
  That is a **BT.709 limited-range YUV→RGB matrix** (`prepare_resources` also
  patches limited-range offset/scale words at `resources+0x500/0x600/0x700`),
  plus the visible dimensions and plane pitches appended as words 12–15.

### `pixel.text.p010-passthrough.bin` (2304 B) — 10-bit HDR variant

Same shape, `bind_main10_source()` builds P010 descriptors (16-bit components,
different tiling word `0x90000204`/`0x9000022c`). `hdr_pixel_constants` carries
a BT.2020 matrix.

### `geometry.text.bin` (736 B) — the VS

A minimal fullscreen-quad vertex program. Reads a structured vertex buffer
(indexed by `S_VERTEX_ID`), transforms by the MVP in constant buffer b0
(`geometry_constants[16]` — a 4×4 that maps the Netflix quad's native bounds to
the AGC viewport), passes UV to the PS. `sceAgcDcbDrawIndexAuto(&cmd, 4, 2)`
draws it — **4 vertices, primitive type 2** (a quad / tri-strip).

Whether this VS generalises to arbitrary RmlUi geometry (Step 3): **partly.**
Its input is already "structured vertex buffer + index + MVP" (see
SharpProspero `Graphics/Agc/Shaders/mesh_vs.pssl` for the same shape with
`float3 pos; float3 normal; float2 uv; uint color`). The blocker is that its PS
counterpart samples a *specific* NV12 texture; a UI triangle needs per-vertex
colour + an optional texture + scissor. So Step 3 needs **new pixel shaders**
(solid, textured, modulated) and possibly a wider vertex struct — hand-written,
assembled, each with a hand-adapted header. Bounded but real work; defer until
Step 2 proves the toolchain end to end.

### `netflix-video-resources.bin` (2816 B) — "NGR1" descriptor table

Magic `NGR1`. Header words: `[1]` = offset of a relocation table (2 entries),
`[3]` = offset of a second table (8 entries), `[2]` = vertex-buffer offset.
`prepare_resources()` walks both tables and rebases every entry to absolute
addresses (`entry[0] += (uintptr_t)resources`), and for SDR overwrites the
limited-range constants at `+0x500/+0x600/+0x700` with the SDR set. This blob is
the pre-baked GPU resource layout the recovered Netflix shaders expect; EVO
reuses it as-is for the video path.

---

## 3. `render_frame()` — the DCB, annotated

`native_agc_present.cpp:571`. One call = one presented frame. Scratch lives in
the mapped `shader_memory` (`SHADER_MEMORY_BYTES = 0xD0000`, direct-memory type
12, `MAP_PROTECTION = 0x33`). Fixed layout inside it:

| offset | contents |
|---|---|
| `0x0000` | geometry (VS) header |
| `0x1000` | pixel (PS) header |
| `0x2000` | pixel code (`.text`) |
| `0x3700` | geometry code (`.text`) |
| `0x5000` | `sceAgcLinkShaders` scratch A |
| `0x6000` | `sceAgcLinkShaders` scratch B / UC regs (3) |
| `0x6800` | combined SH register block |
| `0x7000` | CX register array (built per frame) |
| `0x7800` | geometry constant buffer (MVP) |
| `0x7900` | pixel constant buffer (YUV matrix + dims) |
| `0x7a00` | HUD pixel CB |
| `0x7b00` | HUD CX state |
| `0x8000` | the DCB word stream (`0x4000` words) |
| `0xc000` | resources (`NGR1`, rebased) |

Per-frame sequence:

1. **CX registers** — start from `sceAgcGetRegisterDefaults()`, copy the 16
   render-target / viewport / blend entries at `target_offsets[]`, then patch:
   `cx[0]/cx[10]` = render-target base (`target >> 8`, `>> 40`), `cx[14]` =
   `(h-1) | ((w-1)<<14)`, blend/format bits in `cx[2]/cx[4]/cx[15]`. Then
   `ADD_REG` appends the viewport scale/offset (`0x10f–0x114`), depth
   (`0x2fa–0x2fd`), scan rect (`0x090/0x091`), guardband.
2. **Constant buffers** — `memcpy` `geometry_constants` → `0x7800`;
   `pixel_constants` → `0x7900`, then poke words 12–15 with
   `visible_width/height/pitch/pitch2`.
3. `sceAgcDriverWaitUntilSafeForRendering(&cmd.up, size, 0, video, buffer_index)`
   — stall the builder until the GPU is done with this buffer.
4. **Link** — read `vs/ps` CX+SH register blocks out of the shader objects
   (`*(void**)(shader+24)` = cx ptr, `+32` = sh ptr, `shader[91]` = cx count,
   `shader[92]` = sh count), concatenate into `cx[]` / `combined_sh[]`, emit via
   `sceAgcDcbSetCxRegistersIndirect` / `SetUcRegistersIndirect(…, 3)` /
   `SetShRegistersIndirect`.
5. **Vertex resources** — `shader_resource_offset(vertex_shader, 3, &slot)`,
   then `sceAgcCbSetShRegisterRangeDirect(&cmd, 0x8c + slot, descriptor, 8)`
   with the geometry CB pointer + the `NGR1` table/vertex pointers.
6. **Pixel source** — `bind_pixel_source()` (SDR) or `bind_main10_source()`
   (HDR): writes a 30-word descriptor at SH register `0x0c` — Y image desc, UV
   image desc, two sampler words, the pixel CB pointer, the resource-table
   pointer.
7. `sceAgcDcbDrawIndexAuto(&cmd, 4, 2)` — the video quad.
8. **Overlay (optional)** — second draw: `sceAgcDcbSetCxRegistersIndirect` with
   an 11-entry state block that re-points the viewport to the overlay rect and
   sets constant-alpha blend `0x1e0 = 0x40001413` (`src·a + dst·(1−a)`), then
   `bind_pixel_source` on the overlay surface + `DrawIndexAuto(4, 2)`.
9. `sceAgcDcbSetFlip(&cmd, video, buffer_index, 1, render_marker)`.
10. `flush_gpu_data(memory, SHADER_STATIC_BYTES)` — `clflush` + `mfence` the
    CPU-written scratch so the GPU sees it.
11. `submit.words = words; submit.word_count = cmd.up - words;`
    `sceAgcDriverSubmitDcb(&submit)` then `sceAgcSuspendPoint()`.

`initialize_presenter()` (`:990`) does this once: `sceAgcInit(&state, 8)` →
allocate+map `shader_memory` → `copy_asset` the 5 blobs into place →
`prepare_resources` → `sceAgcCreateShader` ×2(+1 HDR) → `sceAgcLinkShaders(…, 6)`
→ `sceVideoOutOpen(0xff,0,0,NULL)` → allocate+map a 2× framebuffer pool
(`FRAMEBUFFER_ALIGNMENT = 0x200000`) → `sceVideoOutSetBufferAttribute2` (format
`0x8000000000000000` SDR / `0x8100070422000000` HDR) → `sceVideoOutRegisterBuffers2`.

---

## 4. The EVO port

### Scope — do NOT boil the ocean

| Path | Step 2 approach | Why |
|---|---|---|
| **4K / 1080p video playback** | GPU: NV12→RGB + scale + present + flip via `render_frame`. `source` = the FFmpeg (or native) decoder's NV12 output. Deletes `pp_converter_*` **and** the CPU swizzle **and** `sceVideoOutSubmitFlip` from the hot path. | This is the ~11 fps case and the whole point. NV12 shader already in hand. |
| **Playback OSD over video** | Draw the RmlUi OSD to a small **BGRA** surface (already how `RenderPlaybackOSD` works after nothing — it renders to the fb). Composite it as `render_frame`'s "overlay" draw — but that path samples NV12. **Needs the RGBA-passthrough PS** (§1). Until that exists: composite the OSD on the CPU into the RGB frame *before* the AGC present (one 1080p-region alpha blend, ~1 ms — the OSD is small and only redraws on state change thanks to Step 1's dirty flag). | Ship CPU-composite first, GPU-composite when the RGBA shader lands. |
| **Menus (launch/settings/browser/…)** | **Keep Step 1's CPU path.** `RenderCachedScreen` already hits 60 fps with a 0.5 ms memcpy. No AGC. | Zero benefit, real risk. |

### 4a. As-built (2026-09-03) — deltas from the plan above

| Planned | As built |
|---|---|
| `pp/src/shaders/` new dir | not needed for Step 2 — the 6 vendored blobs in `pp/blobs/` + `pp/src/agc_blobs.S` cover NV12. Hand-written shaders wait for Step 3 / the OSD composite. |
| runtime-resolved NIDs | **none** — positional PRX stubs (see below). `pp_agc.c` just `extern`s. |
| `pp_agc_present_nv12(y_uv, pitch, vis_w, vis_h, marker)` | `pp_agc_present_nv12(int vout_handle, uint32_t buf_idx, void *gpu_target, const void *nv12, uint32_t pitch, uint32_t coded_h, uint32_t vis_w, uint32_t vis_h, uint32_t out_w, uint32_t out_h, int64_t marker)` — takes EVO's VO handle + acquired plane. |
| `source` = decoder NV12 straight in | the decoder's NV12 copy is **flexible memory** (`malloc` → `sceKernelMapNamedFlexibleMemory`, `PROT_RW`) — **not GPU-samplable**. `pp_agc_present_nv12` stages each frame into a **per-VO-buffer direct-memory scratch** (`prot 0x33`, `PP_VO_MAX_BUFFERS` slots keyed by `buf_idx`; `pp_videoout_acquire`'s retire guarantee covers reuse). ~12 MB × 3 at 4K, lazy, every rc checked. |
| `pp_videoout.c` calls `pp_agc_present_nv12` | the call is in **`pp_playback.c`'s V8 branch** (it already holds the acquired buffer). `pp_videoout.c` gained `pp_videoout_adopt_flip(vo, idx, marker)` — records the DCB-queued flip as in-flight with **no** `SubmitFlip`, so `retire_old_inflight` frees the buffer when `flipArg` reaches `marker`. `main.c`'s V8 `present_pre_tiled` is skipped for AGC frames (they set no `pending_present`). |
| FFmpeg planar → cheap interleave | `evo_vdec_native.c` `ro_harvest` emits **straight NV12** (skips its NV12→I420 de-interleave) when `pp_agc_available()`; `pp_frame` gained `coded_height`. `pp_playback.c` de-interleaves NV12→YUV420P as a fallback for every other path (host, disabled AGC, 1080/V3). |
| guard `__PROSPERO__` | guard is `EVO_APP_MODULE` (matches the rest of the app-module code). |
| watchdog the submit thread | **done (#27 plan B, `feat/27-agc-submit-watchdog`).** The whole `agc_render_frame` (incl. `WaitUntilSafeForRendering` + `SubmitDcb` + `SuspendPoint`) runs on a dedicated `agc_submit_worker` thread; `pp_agc_present_nv12` `pthread_cond_timedwait`s 250 ms. On timeout the worker is abandoned (it's stuck in the GPU syscall — never joined), `g_agc.submit_wedged` latches, `pp_agc_available()` goes false, and `pp_agc_present_nv12` returns **`-2`** → the V8 branch `adopt_flip`s the buffer (the abandoned worker may still queue its flip) and drops to the CPU path for the rest of the session. The first-frame `sigsetjmp` guard moved onto the worker (the thread that faults). |
| Settings row `Renderer: Auto/CPU/GPU` | **not done** — separate task, coordinate with **#37**'s `Video decoder` row (same settings screen, same fscanf-append; land decoder-append first). `pp_agc_init` is currently called unconditionally from `main()` (app module) so the bare build arms the path; `pp_agc_available()` is the de-facto Auto. |

**Open hardware unknowns** (first device run answers these): RT format/tiling —
**plan A landed:** `pp_videoout_init` now registers the VO with ProsperoLight's
`0x8000000000000000` (linear SDR) instead of `0x8000000022000000` whenever
`pp_agc_available()`, so `render_frame`'s CX render-target bits
(`cx[2]/cx[4]/cx[15]`) meet the layout they were tuned for. If the picture is
still wrong the fallback is adjusting those `cx` bits (garbled / channel-swapped,
**not** a crash — and the plan-B watchdog now contains a crash into a dropped
frame anyway); `sceAgcDcbSetFlip` against a handle whose buffers `pp_videoout`
registered; `0xAABBGGRR` order vs the
shader's MRT0 export; type-12 direct-memory headroom (0xD0000 + ~36 MB staging
alongside #31's resident decoder).

### New files (as built)

```
pp/include/pp_agc.h   pp_agc_available(), pp_agc_init(w,h,hdr),
                      pp_agc_present_nv12(...11 args...), pp_agc_shutdown()
pp/src/pp_agc.c       the port: extern sceAgc*, agc_register_t /
                      agc_command_buffer_t / agc_submit_description_t,
                      bind_pixel_source, shader_resource_offset, flush_gpu_data,
                      agc_render_frame (verbatim strip, no overlay),
                      pp_agc_present_nv12 (staging + fault guard)
pp/src/agc_blobs.S    .incbin the 6 pp/blobs/*.bin (vendored ProsperoLight)
pp/blobs/*.bin        geometry.header/.text, pixel.header/.text.linear-buffer,
                      pixel.text.p010-passthrough, netflix-video-resources
```

### AGC symbol resolution — SOLVED (#31)

**No runtime resolution.** As of #31 `libSceAgc` + `libSceAgcDriver` are
**positional PRX import stubs** (`tools/native-app/stubs/prx/libSceAgc*.syms`
→ `package-app.sh` step 6b, **unconditional for `MODE == player`** — they're
already `DT_NEEDED` because `libSceVideodec2`'s own GPU imports pull them in).
The loader auto-loads the `.sprx` at process start and `sceAgc*` resolve as
ordinary imports. So `pp_agc.c` just `extern`s and calls them, like
`evo_vdec_native.c` / the rewritten `evo_agc_probe.c` — the
`sceKernelLoadStartModule` + `nid_encode` + `sceKernelDlsym` machinery is
gone. To add a symbol `pp_agc.c` needs: append it to `libSceAgc.syms` (a link
error names the missing one). `pp_agc_available()` = "did `sceAgcInit`
succeed", checked once at init; on failure the player falls back to the Step 1
CPU path. The old "`libSceAgc.sprx` load FAILED" gate is dead.

### Panic discipline

- Bring AGC up **after** VideoOut is otherwise idle; tear it down **before**
  reconfiguring VO (carried from [videodec2-abi.md](videodec2-abi.md) §6).
- The AGC submit runs on a **dedicated `agc_submit_worker` thread** (#27 plan B),
  not the playback push thread. `pp_agc_present_nv12` dispatches one
  `agc_render_frame` at a time via a single-slot mailbox and
  `pthread_cond_timedwait`s 250 ms; a hung `sceAgcDriverSubmitDcb` is abandoned
  (`g_agc.submit_wedged`, worker never joined), `pp_agc` goes permanently
  unavailable, and playback returns to the CPU converter — the app slot is
  never wedged. First-frame `sigsetjmp` guard lives on the worker now.
- `flush_gpu_data` before every submit is **mandatory** — the scratch is CPU
  WB memory the GPU reads. Ditto the NV12 staging copy (`flush_gpu_data(stage,
  need)` after the memcpy).
- Check **every** AGC alloc rc — a silent alloc fail leaving stale state is
  what black-screened #31. `pp_agc.c` logs each to `evo_boot.log`.

---

## 5. Step 3 — complete UI on the GPU (committed scope)

Not optional — the destination is a **single GPU path**: UI triangles + video
convert + composite + flip all on `sceAgc`, `evo_rmlui_render.cpp`'s ~2000-line
CPU coverage rasteriser deleted. Sequenced after Step 2 because Step 2 is the
minimum pipeline that de-risks the shared infrastructure (toolchain,
`CreateShader`/`LinkShaders`, DCB submit, VideoOut integration, panic
discipline) on the simplest case.

**Feasible, and not speculative** — SharpProspero's `Renderer3D.DrawMesh`
(`Graphics/Agc/`) is exactly this primitive: arbitrary structured vertex buffer
+ 32-bit index buffer + constant buffer + `sceAgcLinkShaders(…, triangle list)`
+ `DrawIndex`, render target on the VideoOut back-buffer. Its `CxRenderTarget` /
`AgcRenderTargetSetup` / `AgcViewport` / `AgcBufferDescriptor` are the
clean-room C# register model to transcribe to C (the managed code itself isn't
liftable). `llvm-mc-18` assembles the shaders; `geometry.text.bin`'s VS is
already the right shape. Remaining shaders are small.

**Work:**

1. **Shaders** (hand-write `.s`, assemble with `llvm-mc-18`, adapt a header
   from ProsperoLight's):
   - RGBA passthrough — textured quad (also serves the Step 2 OSD composite)
   - solid-colour triangle
   - textured + per-vertex-colour-modulated triangle (RmlUi's common case)
   - each with scissor (rect test in the PS, or a HW scissor register)
2. **`ui_rml/src/evo_rmlui_render_agc.cpp`** — an `Rml::RenderInterface` that
   emits AGC draw calls into a per-frame DCB instead of CPU-rasterising:
   - `CompileGeometry` → upload a GPU vertex/index buffer, return a handle
   - `RenderGeometry` / `RenderCompiledGeometry` → bind shader + texture(s) +
     translation constant → `sceAgcDcbDrawIndexAuto`
   - `EnableScissorRegion` / `SetScissorRegion` → scissor register
   - `RenderToClipMask` / `EnableClipMask` → stencil buffer or a coverage
     texture sampled by the PS
   - `SetTransform` → MVP constant buffer
   - `GenerateTexture` / `LoadTexture` → GPU texture upload (the glyph atlas
     and icons become GPU textures once, not per frame)
3. **`evo_rmlui_app.cpp`** — pick `EvoRenderInterfaceAgc` when
   `pp_agc_available()`, else the CPU `EvoRenderInterface` (Step 1). Runtime
   switch, no `#ifdef` in the app layer.
4. **`pp_agc.c` `render_frame`** gains a UI pass between the video quad and the
   flip — one DCB does video + UI + composite + flip.
5. **Delete** the CPU coverage rasteriser from `evo_rmlui_render.cpp` once
   plane-hash parity holds on every screen.

The Step 1 CPU path is kept as the fallback (AGC probe fails, or host preview).

---

## 6. Validation

- Composited-output plane-hash parity vs. the CPU path (`tools/bench.sh`,
  [validation.md](../validation.md)). The AGC output and the CPU output must
  match within the YUV→RGB rounding tolerance.
- 4K60 HEVC Main10 holds VSync cadence (the [rmlui-integration-guide.md](../rmlui-integration-guide.md) §7 target).
- No new panic vector — probe → watchdog → fallback, all three.
- Host preview byte-identical (`__PROSPERO__` guard).

---

## 7. Shader toolchain — status & the nicer path

**Working now (`tools/build-shader.sh`):** hand-written GCN `.s` →
`llvm-mc-18` → `llvm-objcopy-18 --only-section=.text` → raw `.text` blob.
`pp/shaders/rgba_ps.s` is the first shader (RGBA × vertex colour, MRT0 export),
register conventions mirrored from ProsperoLight's disassembled NV12 PS
(`pp/shaders/README.md`). Assembles + round-trips; **unrun**.

**Still to solve for a shader we author:** the `.header.bin` half.
`sceAgcCreateShader(shader, header, code)` needs SPI config + GPR counts +
resource layout. Plan: reuse ProsperoLight's `pixel.header.bin` with our
`.text` (structural subset). Fallback: build the header from the RDNA2 ISA
reference (doc 70648) + the `.sb` parsers in KytyPS5 / shadPS5 / SharpProspero's
`ShaderInfo.cs`.

**The nicer path (needs a Dockerfile change, not blocking):**
GLSL → SPIR-V → RDNA2 ISA with open tooling, so Step 3's shader set can be
written in GLSL (port RmlUi's own GL3/Vulkan backend shaders) instead of
assembly:

- add `glslang` + `spirv-tools` + `SPIRV-LLVM-Translator` (matching LLVM 18)
  to the container, OR
- `clang-18 -target amdgcn-amd-amdpal` / `llc-18 -mtriple=amdgcn-amd-amdpal
  -mcpu=gfx1030` (both present) fed from SPIR-V or LLVM IR, OR
- AMD's [Radeon GPU Analyzer](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer)
  (GLSL/Vulkan/HLSL → AMD ISA, offline, single binary).

The header wrapping is still bespoke either way; only the code generation gets
easier. Verify on host before committing to it.

---

## 8. Sequencing

1. ~~**Console:** `--agc-probe` → gate.~~ ✅ PASSED (2026-09-03).
2. ~~Write `pp_agc.c` (§4).~~ ✅ `pp_agc_init` hw-verified; `agc_render_frame`
   + `pp_agc_present_nv12` + `pp_playback` wiring done, builds green (§4a).
3. ~~**Plan A+B** — submit watchdog thread + linear VO attr.~~ ✅ landed
   (`feat/27-agc-submit-watchdog`), builds green, not run on hardware.
4. **◀ NEXT — first device run (plan C).** `tools/evo-remote.sh build --agc-probe`
   → launch `PPSA99039` from the Games row → `evo-remote.sh boot` →
   `evo-remote.sh play <4K H.264>`. Watch `evo_boot.log` for
   `pp_agc: render_frame rc=…`. The watchdog means a wrong guess now costs a
   dropped frame + a log line (`011_AGC_SUBMIT_WEDGED` / `pp_agc: SUBMIT WEDGED`),
   not a console cycle. Expect one of: a correct picture; a garbled/channel-
   swapped picture (RT format/tiling — adjust the `cx` bits, plan A's attr is
   already applied); a first-frame fault or a wedge (logged, AGC disabled, CPU
   fallback). No test-pattern harness yet — the native 4K decoder's NV12 output
   is the first real input.
5. A/B the composited plane hash vs the CPU converter (`tools/bench.sh`,
   [validation.md](../validation.md)); then remove `pp_converter_fused` / the
   CPU swizzle from the 4K hot path.
6. Settings row `Playback → Renderer: Auto/CPU/GPU`;
   P010/HDR present (`bind_main10_source` + `pixel.text.p010-passthrough`).
7. `rgba_ps` + reused header → `sceAgcCreateShader` accepts it? → GPU OSD
   composite over 4K video (deferred; AGC frames present with no overlay today).
8. Step 3 (#28) — the shader set + `evo_rmlui_render_agc.cpp` + delete the CPU
   rasteriser.
