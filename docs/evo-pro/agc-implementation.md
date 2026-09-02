# AGC implementation guide — GPU rendering Step 2/3

> Companion to [gpu-rendering-plan.md](gpu-rendering-plan.md). That doc is the
> *why* and the ladder; this is the *how* — the ProsperoLight AGC path read
> line by line, the shader blobs disassembled, and the concrete EVO port.
>
> **Status: host analysis, 2026-09-02.** No AGC call has run from EVO yet. The
> reachability gate (`package-app.sh --agc-probe`) is pending a console session.
> Everything here is written so the port can be built now and tested the moment
> the gate passes.

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
RDNA2 ISA for `--triple=amdgcn--amdpal --mcpu=gfx1030`** — verified 2026-09-02:

```bash
llvm-mc-18 --assemble --triple=amdgcn--amdpal --mcpu=gfx1030 --filetype=obj -o out.o in.s
llvm-objdump -d --mcpu=gfx1030 out.o          # round-trips
```

So the real situation is:

| Need | Have it? |
|---|---|
| NV12→RGB fullscreen convert PS | ✅ `pixel.text.linear-buffer.bin` (ProsperoLight) |
| P010 (10-bit HDR) convert PS | ✅ `pixel.text.p010-passthrough.bin` |
| Fullscreen-quad VS | ✅ `geometry.text.bin` |
| **RGBA-passthrough PS** (for compositing the cached RmlUi surface) | ❌ — hand-write ~15 instrs of GCN, assemble with `llvm-mc-18` |
| Solid-colour / textured-triangle PS+VS (Step 3) | ❌ — larger hand-written set |

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

### New files

```
pp/src/pp_agc.h            public: pp_agc_available(), pp_agc_init(w,h,hdr),
                           pp_agc_present_nv12(y_uv, pitch, vis_w, vis_h, marker),
                           pp_agc_shutdown()
pp/src/pp_agc.c            the port: AGC ABI (runtime-resolved NIDs), the
                           shader_memory layout, initialize + render_frame,
                           stripped of HUD/keyboard/telemetry
pp/src/agc_blobs.S         .incbin the 5 ProsperoLight .bin assets
pp/src/shaders/rgba_ps.s   hand-written RGBA-passthrough PS (later)
pp/src/shaders/*.sb        assembled output (build step)
```

### Wiring

- `pp/src/pp_videoout.c` — on `__PROSPERO__` + `pp_agc_available()`, the present
  path calls `pp_agc_present_nv12()` instead of the swizzle + `SubmitFlip`.
- `pp/src/pp_playback.c` — the converter stage (`pp_converter_fused` etc.) is
  skipped; the decoder's NV12 frame goes straight to `pp_agc_present_nv12`.
  (FFmpeg `sws` may still be needed if the decoder emits YUV420P planar, not
  NV12 — a cheap plane interleave, or request NV12 from the decoder.)
- **Settings toggle** — `Playback → Renderer: Auto / CPU / GPU`. `Auto` =
  `pp_agc_available()`. Persist in `evo_settings_t`.
- Host preview (`tools/uiview*`, `uiplay`) — `pp_agc.c` is `#ifdef __PROSPERO__`
  compiled to a stub returning "unavailable"; the SDL/CPU path is untouched.

### AGC symbol resolution

The app-module SDK has **no `libSceAgc` stub** (like the payload SDK). Resolve
at runtime the same way `evo_agc_probe.c` does: `sceKernelLoadStartModule(
"/system/common/lib/libSceAgc.sprx")` + `kernel_dynlib_resolve` over
`nid_encode()` of each name. Cache the ~15 function pointers in a struct. If any
fail to resolve → `pp_agc_available()` returns 0, player falls back to CPU.
The full list is in `native_agc_present.cpp:191–209`.

### Panic discipline

- Bring AGC up **after** VideoOut is otherwise idle; tear it down **before**
  reconfiguring VO (carried from [videodec2-abi.md](videodec2-abi.md) §6).
- The AGC submit runs on the render thread; watchdog it like the decode thread
  ([hardware-decode-review.md](../hardware-decode-review.md) §7) — a hung
  `sceAgcDriverSubmitDcb` must not wedge the app slot (see the `agc_probe`
  watchdog for the pattern).
- `flush_gpu_data` before every submit is **mandatory** — the scratch is CPU
  WB memory the GPU reads.

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

## 7. Sequencing

1. **Console:** `package-app.sh --agc-probe` → deploy → launch. Read the
   `EVO agc:` notification. **Gate.**
2. If viable: write `pp_agc.c` (§4) — most of it is a mechanical strip of
   `native_agc_present.cpp`. Build with `package-app.sh`.
3. First device test: `pp_agc_init` + one `render_frame` of a test-pattern
   NV12 buffer → a picture on the TV via the GPU. Attribute each stage from
   the `report_agc_receipt`-style notifications.
4. Feed real decoded frames; A/B the plane hash.
5. RGBA-passthrough PS → GPU OSD composite.
6. Step 3, maybe.
