# Vendored — RmlUi OpenGL 3 backend

These three files are **verbatim copies** from RmlUi's `Backends/` directory
(the same tree checked out at `build/rmlui-host/RmlUi/`):

- `RmlUi_Renderer_GL3.cpp`
- `RmlUi_Renderer_GL3.h`
- `RmlUi_Include_GL3.h`  (glad 2.0 GL 3.3 core loader, header-only)

Do **not** edit them. EVO's additions live in the subclass
`EvoRenderInterfaceGL` (`../evo_rmlui_render_gl.{h,cpp}`) — texture loading from
the embedded bundle / `evo:mem/` namespace, premultiplied-alpha convention, and
the FBO→BGRA readback bracket. Everything else (geometry, stencil clip masks,
MVP transforms, scissor, layers, filters) is inherited unchanged.

To update: re-copy from a newer RmlUi checkout and re-verify
`tools/uiview.sh --all` + `UIVIEW_GL=1`. Render-overhaul GL-2 (#78).
