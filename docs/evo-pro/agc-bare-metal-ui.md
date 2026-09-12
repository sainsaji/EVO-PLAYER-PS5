# Bare-metal AGC UI — the RmlUi renderer on `sceAgc` directly

**Status: WORKING, hardware-verified 2026-09-12.** The RmlUi interface renders
through hand-built `sceAgc` command buffers — no Mesa, no GL, no CPU rasteriser.
Snappy, vsync-paced, no visual artefacts.

This supersedes the "the `#27`/`#28` sceAgc work is history" framing in
[status.md](status.md): bare-metal AGC is the UI path in `--agc` builds.

---

## Why

The UI was slow, and the cause was not API overhead — the UI was being
**rasterised on the CPU**. `evo_rmlui_app.cpp` defaults `m_gl_blit_mode` to
true, selecting `EvoRenderInterface` (`evo_rmlui_render.cpp`, ~1450 lines of
coverage rasteriser) and blitting the result as one GL texture. RmlUi's native
GL3 path measures **~1.2 s/frame** on ps5-opengl, because that driver's
render-to-texture is synchronous with a full CPU surface copy per draw.

Neither existing option was fast: one rasterises on the CPU, the other is a
driver limitation. Going straight to AGC was the only route to a fast UI.

## What runs on the GPU today

| Subsystem | Path | GPU? |
|---|---|---|
| RmlUi UI (menus, text, icons, thumbnails) | `EvoRenderInterfaceAGC` -> `sceAgc` DCB | **yes** |
| Video decode | `sceVideodec2` resident decoders | **yes** (dedicated block) |
| Video present | 4 AGC video pipelines (NV12 / planar / P010 PQ / P010 HLG) | **yes** |
| Playback OSD | RmlUi geometry, re-dispatched after the video quad | **yes** |
| CPU coverage rasteriser | still compiled; unused once AGC constructs | dormant |

Only the UI pipeline is mandatory at init — a video pipeline that fails to
compile disables that path and logs it rather than aborting the runtime. Build
`--agc` without `EVO_AGC_HAVE_VIDEO_PIPES` to bring the runtime up UI-only.

The OSD ordering is worth knowing: `RenderPlaybackOSD()` draws through the AGC
interface as GPU geometry (it does **not** fill `gl_scratch` the way the GL path
does), and `main.c` dispatches it *before* the video blit — which then paints
over it with `BLEND_NONE`. It is therefore re-dispatched after the blit. That
costs one redundant OSD pass; the clean fix is reordering the loop so video
draws first, which means untangling `_osd_changed`/`_osd_was`.

---

## The shader toolchain

Shaders are **not** hand-assembled and their registers are **not** guessed.

```
projects/evoplayer/shaders/agc/*.pipe        AMD LLPC pipeline source
  -> amdllpc -gfxip=10.1.3 -o=X.pal.elf      (gfx1013 = the PS5 GPU)
  -> llvm-objcopy --dump-section=.text       slice at _amdgpu_gs_main /
  -> llvm-readelf --symbols                     _amdgpu_ps_main extents
  -> llvm-readelf --notes                    AMDGPU Metadata (PAL) YAML
  -> tools/build_agc_pipes.py                derive AGC registers
  -> projects/evoplayer/shaders/agc/*_pipe.h ISA blobs + register tables
  -> evo_agc_shader_header.c                 build the 0x148 arena
  -> sceAgcCreateShader + sceAgcLinkShaders
```

`.pipe` is AMD's public LLPC format: GLSL 450 in `[VsGlsl]`/`[FsGlsl]` plus a
`[ResourceMapping]` block declaring the user-data layout **beside the shader
source**. That placement is the point — see "Why the toolchain changed".

Ported from `ps5-xash3d-halflife` (`tools/build_shader.py`,
`tools/generate_agc_metadata.py`, `src/ps5_shader_header.c`), which drives a full
3D game on this firmware through the same chain. That project publishes its
source but **no compiled shaders**, so it is a structural reference only — it
cannot be run, and there is no working command stream to diff against.

### The scripts, and what each one owns

Three Python files carry this toolchain, and **none** of them run in the normal
`ps5-dev` container. Two need the **amdllpc image**; the third runs inside the
Docker image build itself.

| File | Runs | Owns |
|---|---|---|
| `tools/build_agc_pipes.py` | amdllpc image | `.pipe` -> `*_pipe.h`. The whole compile chain. |
| `tools/gen_video_pipes.py` | amdllpc image | Writes the four video `.pipe` files from one shared vertex stage. |
| `tools/patches/llpc-add-gfx1013.py` | `Dockerfile.amdllpc`, at image build | Adds gfx1013 to LLPC's GPU table. |
| `tools/build_agc_shaders.py` | **deleted** | The old opengnm-psbc wrapper. Gone; do not resurrect. |

**`tools/build_agc_pipes.py`** — the only thing that turns a `.pipe` into
something the runtime can load. Per pipeline it runs `amdllpc -gfxip=10.1.3`,
dumps `.text` with `llvm-objcopy`, slices it at the `_amdgpu_gs_main` /
`_amdgpu_ps_main` symbol extents from `llvm-readelf --symbols`, decodes the PAL
metadata YAML out of the AMDGPU note, and *derives* every AGC register from it
(`derive()` is a port of ps5-xash3d's `generate_agc_metadata.py`). It writes
`<name>_pipe.h` plus an `evo_agc_pipes.h` that includes them all, and a
`build/agc_pipes/<name>.manifest.json` with the raw metadata for debugging.

It refuses to emit a header rather than emit a wrong one:

- the pipeline must be a relocation-free gfx1013 **NGG** pipeline;
- the ELF must carry no unresolved relocations;
- every required `[ResourceMapping]` node must have a user-SGPR slot. The
  *vertex-buffer* table is allowed to be absent (`vtx=-1`) because the video
  pipelines build their quad from `gl_VertexIndex`, but a missing constant-buffer
  or texture table is fatal — that is precisely the silent failure the psbc
  chain shipped, where a shader read a resource through an unset pointer, drew
  nothing, and faulted nothing.

Its one-line-per-pipeline output is the receipt worth reading:

```
ui_screen_2d: gs=352B ps=164B esgs_itemsize=1 draw_modifier=0x5
              user_dwords(const=1,vtx=2,tex=1) write(vs=3,ps=2)
```

`esgs_itemsize` must be **1**, `draw_modifier` **0x5**, and the `user_dwords`
are what the runtime writes into the user-SGPR block. If any of those move, the
runtime's expectations move with them — they are read at runtime through
`evo_agc_runtime_get_user_data_layout()`, never hardcoded.

**`tools/gen_video_pipes.py`** — the four video pipelines are the same fullscreen
quad with four different fragment stages, so the vertex shader and the
`[ResourceMapping]` skeleton live in the generator instead of being copy-pasted.
Two things it encodes that are easy to get wrong by hand: the vertex stage takes
**no vertex buffer** (hence no `IndirectUserDataVaPtr` and no
`[VertexInputState]`), and the fragment samplers must sit at **set 1**, because
the vertex stage already owns set 0 binding 0 for its uniform block. Edit this
file and re-run it; never edit the generated `.pipe` files.

**`tools/patches/llpc-add-gfx1013.py`** — runs once inside the image build, not
by hand. See "gfx1013 is not in public AMDVLK" below.

### Building the compiler

```
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml build ps5-dev
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml \
  run --rm ps5-dev python3 tools/build_agc_pipes.py
```

One-time, LLVM-scale (~1 h at `-j4`), cached in the image. Two non-obvious
requirements, both explained in `Dockerfile.amdllpc`:

- **`dxc`.** LLPC's `gfxruntime` compiles an HLSL "advanced blend" library and
  hard-fails configure without Microsoft's DirectX compiler. There is no skip
  option, unlike `gpurt` (turned off with `-DVKI_RAY_TRACING=OFF`).
- **`-j4`, link jobs 1.** The AMDGPU CodeGen translation units peak at 2-4 GB of
  GCC each; one per core exhausted the Docker VM at object ~2080/2383 twice,
  once taking the whole Docker engine down with it.

### gfx1013 is not in public AMDVLK

`amdllpc -gfxip=10.1.3` fails with `Invalid gfxip: gfx1013`. The chain is
`amdllpc.cpp` -> `LgcContext::isGpuNameValid` -> `TargetInfo::setTargetInfo`,
whose `gpuNameMap` lists gfx1010/1011/1012 and 1030+ and **omits 1013** — AMD
does not ship the PS5's semi-custom part. It is not behind a build flag.

LLVM's own AMDGPU backend knows the target (`llc-18 -march=amdgcn -mcpu=help`
lists `gfx1013`), so only that table is in the way.
`tools/patches/llpc-add-gfx1013.py` clones `setGfx1011Info` (Navi12, the closest
GFX10.1 sibling, including the integer-dot caps that separate it from 1010) and
registers `{"gfx1013", "Navi10Lite", ...}`. The Dockerfile `grep -q`s afterwards,
so a silently-failed patch cannot produce a compiler-less image.

> If a shader ever miscompiles in a way that smells like a missing hardware
> erratum workaround, that cloned workaround table is the first place to look.

---

## Why the toolchain changed

The previous chain (`tools/build_agc_shaders.py`, now deleted) was a hand-written
wrapper around `opengnm-psbc` that reimplemented ps5-opengl's Gallium caller but
not its overrides. It shipped two silent, hardware-only defects:

1. **`VGT_ESGS_RING_ITEMSIZE` packaged as 4 instead of 1.** For NGG whose source
   stage is vertex, non-passthrough multiplies every vertex index by this value
   before using it for primitive exports and LDS addressing. The GPU wedged on
   the first DCB containing a real draw, `sceAgcSuspendPoint()` never returned,
   and the OS killed the process ~55 s later. ps5-opengl's `ps5_agc_package.c`
   hardcodes the override; its Gallium caller passes 4 and lets the builder
   correct it. The wrapper copied the caller and not the correction.
2. **No `--descriptor-binding` for the vertex stage.** Both vertex shaders
   declare `layout(set=0, binding=0) uniform ...`, and only the *fragment*
   shaders were given bindings. The compiler was never told set 0's layout for
   that stage, so the shader read its projection matrix from a descriptor slot
   nobody had described. Draws executed, retired, faulted nothing, and produced
   **exactly zero fragments**.

Neither was detectable without hardware, and neither is expressible in `.pipe`:
`[ResourceMapping]` lives beside the shader and every register is derived from
PAL metadata. That is the whole argument for the migration.

### User-SGPR slots are derived, never hardcoded

`build_agc_pipes.py` reads PAL's `.user_data_reg_map` and fails the build if any
`[ResourceMapping]` node has no slot. For `ui_screen_2d` the vertex stage is:

```
slot 0 = 0x10000000 GlobalTable          (driver-supplied)
slot 1 = 0          -> const-buffer table
slot 2 = 0x1000000f VertexBufferTable    (from IndirectUserDataVaPtr)
slot 3 = 0x10000003 BaseVertex           (supplied by the draw packet)
slot 4 = 0x10000004 BaseInstance         (supplied by the draw packet)
```

The vertex-buffer table is a PAL **special**, not resource-mapping index 1 —
`IndirectUserDataVaPtr` is hardware-managed, so searching for the node's own
index finds nothing. LLPC's layout also differs from psbc's (`const=1 vtx=2
tex=1` against psbc's `vtx=0 base=1 const=2`), so any hardcoded guess would have
been silently wrong again. The runtime reads the layout off the compiled
pipeline through `evo_agc_runtime_get_user_data_layout()`.

---

## Runtime bugs fixed along the way

Every one produced a *silent* wrong result, which is why they cost hardware
round-trips. Recorded because the failure signatures are not obvious from code.

| Symptom on screen | Cause | Fix |
|---|---|---|
| GPU wedges, process killed ~55 s later | `VGT_ESGS_RING_ITEMSIZE` = 4 | derive from PAL (= 1) |
| Draws retire, **zero** pixels written | vertex stage compiled with no descriptor binding | `.pipe` `[ResourceMapping]` |
| UI draws once then freezes; navigation dead | `seal(slot, token)` then `begin(slot, 0, 1)` gives `TOKEN_MISMATCH`; the return was ignored, so every slot stayed sealed and all allocations failed | store the seal token, hand it back once the fence proves completion; log begin failures |
| Blank screen, every frame discarded | discarding a frame called `_seal()` (needs a token only a submit can clear) instead of `_abort()` | `evo_agc_transient_ring_abort()` |
| Some glyph atlases garbled, others perfect | textures were 64-byte aligned; a GFX10 image descriptor stores `address >> 8` and needs **256** | over-allocate and align; `build_tsharp_2d_internal` now *rejects* a misaligned base |
| Whole UI colour-swapped (blue theme -> gold) | the scanout is BGRA-ordered; the CB was storing RGBA | `COMP_SWAP = ALT` in `CB_COLOR0_INFO` |
| Black band creeping down the picture while loading | `sceVideoOutSubmitFlip` is async and was never waited on; with 2 buffers the CPU clear wiped the **live** framebuffer | poll `sceVideoOutGetFlipStatus` until `status[3] == flip_arg` |
| Square corners on rounded containers; masked overlays in the wrong place | `EnableClipMask`/`RenderToClipMask` were never implemented, so RmlUi's non-rectangular clipping hit base-class no-ops (rectangular clipping still worked - that goes through the scissor) | stencil buffer + the two overrides |
| UI flashing, alternating with blank | frames with no draws were still cleared and flipped | skip present when `frame_has_draws == 0` |

Two pieces that were always required and simply missing:

- **End-of-pipe cache protocol.** `FLUSH_AND_INV_CB_DATA_TS` (event 45, GCR 12)
  then `CACHE_FLUSH_AND_INV_TS` (event 40, GCR `0x30c` = GLV/GL1/GL2 invalidate
  **plus GL2 writeback**) via `sceAgcCbReleaseMem`. Without both, the colour
  block and L2 hold the frame while the display — not a coherent client — scans
  out stale DRAM.
- **Init-time cache flush** of `shader_storage` (the ISA the GPU fetches) and
  `gpu_regs` (register arrays the CP DMA-reads, including the shader entry
  address). Written once by the CPU into write-back memory, read by the GPU for
  the life of the process.

---

## Verifying a build

`evo.log` carries everything needed to separate a good run from a bad one:

```
agc pipe ui_screen_2d cx=53 sh=12 uc=3 modifier=0x5 gs_pgm=0286e120:00000000 ...
agc pipe ui_screen_2d user_data vs_n=3 ps_n=2 const=1 vtx=2 tex=1
agc color_target rc=0/0 base0=0x27a8000 info=0x8828 (comp_swap=1) ...
agc health frame=240 dcb=11872/524288 peak=12178 ring_fail=0 tex_fail=0 \
           direct_mem=6249472/67108864 peak=6257664 allocs=347
agc health presents=120 dcb_min_presented=2845 flip_waits=120 timeouts=0
```

- `gs_pgm` / `ps_pgm` **non-zero** — `sceAgcCreateShader` resolved the entries.
- `comp_swap=1` — the CB stores BGRA. Without it the whole UI is R-B swapped.
- `ring_fail` / `tex_fail` **0** — nothing is silently dropped. These counters
  exist because `RenderGeometry` has several early-returns that otherwise make
  content vanish with no error anywhere.
- `flip_waits == presents` — exactly one vblank per present, correctly paced.
  `timeouts` must be 0.
- `direct_mem` / `allocs` stable across windows — no leak.

**Check the build marker before trusting any of it.** Two debugging rounds were
wasted reading a stale `.ffpfsc`; a log line whose format predates the change
under test means the deploy did not land.

`agc_dump_scanout()` reads the framebuffer back once at frame 40 and logs a
luminance thumbnail plus the most common colours. That is what settled the
colour bug: the buffer held `ffedbe00` (correct cyan for `#00cdff`) while the
panel showed gold, proving the swap happened *after* the framebuffer. Reach for
it before theorising about channel order.

---

## Clip masks

RmlUi clips to non-rectangular shapes - rounded containers, masked overlays -
through `EnableClipMask()` / `RenderToClipMask()`, backed here by an 8 MB S8
stencil surface. Depth is deliberately left disabled (`DB_Z_INFO` format 0):
nothing needs a Z test, so no depth buffer is allocated at all. The DB register
layout follows ps5-opengl's `append_depth_target_state()`.

`RenderToClipMask` mirrors RmlUi's own GL3 backend - stencil to write mode with
`CB_TARGET_MASK = 0` so the mask cannot touch colour, draw the geometry through
the normal path, then switch to testing against it.

**One deliberate divergence.** RmlUi's reference clears the stencil buffer before
every `Set`. Here that would be a multi-megabyte fill per mask, many times a
frame. Instead the buffer is zeroed once per frame with a CP-synced DMA fill and
each `Set` claims the next unused value (1, 2, 3...), with the test comparing
against it; an untouched texel is always 0 and so can never collide with a live
mask. `Intersect` still increments, so only texels already carrying the previous
value reach `ref + 1`. `SetInverse` stamps the same way but tests NOTEQUAL.

This is the one piece with no reference implementation to copy the state machine
from - the registers come from ps5-opengl, the mask-value scheme does not. If
corners come back square the DB binding did not take (`agc stencil base=...
s_info=0x20000181` in the log says whether it did); if clipping is inverted or
content vanishes entirely, the compare function or the `ref` bookkeeping is wrong.

## Not done

- **The CPU coverage rasteriser** is still compiled in and remains the UI
  renderer for non-`--agc` (GL) builds, which are still the shipping default. It
  cannot be deleted until `--agc` is the only build.
- **Scanout resolution is hardcoded 1920x1080** (`WIDTH`/`HEIGHT` in `main.c`);
  nothing calls `sceVideoOutGetResolutionStatus`, so VideoOut upscales to the
  panel's actual mode. Rendering natively needs the RmlUi context sized to match
  as well, and the RCSS is authored entirely in `px` (1388 uses, zero `dp`), so
  the layout would shrink unless it moves to density-independent units first.
- The GPU-side `SetFlip` packet in the DCB is unused; the CPU flip is the proven
  path. Revisiting it would save a CPU round trip per frame.
