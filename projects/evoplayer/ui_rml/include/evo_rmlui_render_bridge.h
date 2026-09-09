#pragma once
#include <cstdint>
#include <string>

namespace Rml { class RenderInterface; }
class EvoAgcGeoSink;   /* #28 geo sink - non-owning, retired in GL-3 */

/*
 * The seam that lets EvoRmlApp hold either render interface.
 *
 * EVO has two Rml::RenderInterface implementations that share no base beyond
 * Rml::RenderInterface itself:
 *
 *   - EvoRenderInterface    - the CPU coverage rasteriser (evo_rmlui_render.cpp),
 *                             writes premultiplied BGRA straight into a caller
 *                             framebuffer. The only path on device today.
 *   - EvoRenderInterfaceGL  - RmlUi's upstream RenderInterface_GL3 with EVO's
 *                             texture adapters (evo_rmlui_render_gl.cpp), renders
 *                             to a GL FBO. Host preview only in GL-2 (#78);
 *                             becomes the device path in GL-3.
 *
 * EvoRmlApp talks to whichever is active through this interface. All EVO-
 * specific entry points the app calls (SetFramebuffer / SetDimensions /
 * SetMemoryTexture / ...) live here; RmlUi itself is handed AsRml().
 *
 * FrameBegin/FrameEnd bracket each Rml::Context::Render(). The CPU interface
 * needs no bracket (it composites directly), so its hooks are no-ops and the
 * render path stays byte-identical when GL is off. The GL interface binds its
 * FBO in FrameBegin and reads the result back into the target framebuffer in
 * FrameEnd, so the rest of EVO keeps seeing a plain BGRA buffer.
 */
class EvoRenderBridge {
public:
    virtual ~EvoRenderBridge() = default;

    /* The object to hand to Rml::SetRenderInterface(). */
    virtual Rml::RenderInterface* AsRml() = 0;

    virtual void SetFramebuffer(uint32_t* fb) = 0;
    virtual void SetDimensions(int w, int h) = 0;

    virtual void SetMemoryTexture(const std::string& key, const uint32_t* bgra,
                                  int w, int h) = 0;
    virtual void DropMemoryTexture(const std::string& key) = 0;

    /* #28 Phase 4 geometry divert. No-op on the GL interface (GL-3 deletes the
     * sink entirely); kept on the bridge so call sites compile unchanged. */
    virtual void SetAgcSink(EvoAgcGeoSink* sink) = 0;

    virtual void FrameBegin() {}
    virtual void FrameEnd() {}
};
