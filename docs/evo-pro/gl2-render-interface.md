# GL-2 (#78) — RmlUi OpenGL render interface, host-proven

> Story GL-2 of the [OpenGL render overhaul](opengl-render-overhaul.md).
> **Blocked by GL-1 (#77)** (done — GO). Host only; the device cutover is GL-3.

## What landed

RmlUi's upstream `RenderInterface_GL3` (GL 3.3 core) is now a real render path
in EVO, proven in the host preview harness against the CPU coverage rasteriser
(`evo_rmlui_render.cpp`). The CPU path stays the default and the device path;
GL is opt-in on the host via `EVO_RML_GL=1` / `UIVIEW_GL=1`.

| File | Role |
|---|---|
| `projects/evoplayer/ui_rml/src/rmlui_gl3/RmlUi_Renderer_GL3.{cpp,h}` + `RmlUi_Include_GL3.h` | Vendored verbatim from RmlUi `Backends/` (see `VENDORED.md` there). Not edited. |
| `projects/evoplayer/ui_rml/include/evo_rmlui_render_bridge.h` | `EvoRenderBridge` — the seam that lets `EvoRmlApp` hold either interface. Both implement it. `FrameBegin`/`FrameEnd` bracket every `Context::Render()`. |
| `projects/evoplayer/ui_rml/src/evo_rmlui_render_gl.{cpp}` + `include/evo_rmlui_render_gl.h` | `EvoRenderInterfaceGL : RenderInterface_GL3, EvoRenderBridge`. Overrides `LoadTexture` (embedded bundle + `evo:mem/` + premultiply, identical candidate list to the CPU path) and `SetMemoryTexture`/`DropMemoryTexture`. Geometry, stencil clip masks, MVP transforms, scissor, layers, filters all inherited unchanged. |
| `projects/evoplayer/ui_rml/src/evo_gl_context_host.cpp` + `include/evo_gl_context.h` | Headless EGL/GL 3.3 context. `eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)` + a 1920x1080 pbuffer backbuffer, `RmlGL3::Initialize()`. `#ifndef EVO_APP_MODULE`. The device impl (`evo_gl_context_device.cpp`) is GL-3. |

Changed: `Dockerfile` (Mesa/EGL host packages), `evo_rmlui_app.{h,cpp}`
(`m_render` behind the bridge, GL selection, `FrameBegin/FrameEnd` in
`EVO_PROF_CTX_RENDER()` and `RenderToast`), `evo_rmlui_render.{h}`
(`EvoRenderInterface` also derives the bridge), `tools/uiview_playback_rml.{cpp,sh}`.

## Render model on the host

The rest of EVO still hands the interface a plain BGRA `uint32_t*`:

1. `SetFramebuffer(fb)` / `SetDimensions(w,h)` — cache the target + viewport.
2. `FrameBegin()` — clear the pbuffer (fb 0) to transparent, then RmlUi
   `BeginFrame()` (binds + clears its own MSAA layer stack).
3. `Context::Render()` — RmlUi draws through the GL3 backend.
4. `FrameEnd()` — RmlUi `EndFrame()` resolves MSAA and blits the composite to
   fb 0; then `glReadPixels(GL_RGBA)` (bytes R,G,B,A == EVO's `0xAABBGGRR` in
   memory, premultiplied) and a premultiplied-over composite onto the caller's
   framebuffer, y-flipped.

The fb-0 clear in step 2 matters: `RenderInterface_GL3::EndFrame()` composites
onto fb 0 with premultiplied blending and never clears it, so an overlay pass
(toast/dialog over a menu) that leaves most of the frame transparent would
otherwise show the previous frame's fb 0.

## Parity (host, `UIVIEW_GL=1`, llvmpipe, threshold 8)

- Full menu/settings/theme screens: **~0.12–0.15%** of pixels differ, scattered
  along text and curved edges — GL's MSAA vs the CPU coverage accumulator. This
  is the expected improvement, not a regression.
- Overlay screens (toast, dialog, subtitle picker): composite correctly over
  the screen beneath once fb 0 is cleared per frame.

`diff_*` / `*_gl.png` land in `output/uiview/`.

## Next: GL-3 (#79) — device cutover

The GL-2 investigation settled GL-3's architecture (see the
[#79 alignment comment](https://github.com/sainsaji/EVO-PLAYER-PS5/issues/79#issuecomment-5605332303)):
**`ps5-opengl` owns `sceVideoOut` for the whole session** — no VO handoff,
`pp_videoout` / `pp_agc` present removed not run alongside. Because of that
single-owner constraint the player frame's *present* also moves into GL-3 (a
GL texture blit of `pp/`'s already-CPU-converted RGBA frame); `pp/` keeps
decode + convert + clock. GL-4 (#80) then replaces that CPU convert with a
GLSL shader.

Staged B1 (persistent device GL context + `--gl` flag + boot cutover) → B2
(menu screens through GL) → B3 (player present via GL texture, kills the #32
overlay machine) → B4 (delete the dual path, retire routes #2/#3) → B5 (#49
seam cleanup). Each is its own hardware-verified commit.

## Not done here (later stories)

Device GL (GL-3, #79), GLSL YUV→RGB + CPU-converter deletion (GL-4, #80), the
strays — subtitles font, keyboard, image viewer, FPS overlay (GL-5), deleting
`evo_rmlui_render.cpp` / `pp_agc.c` the files (GL-6).
