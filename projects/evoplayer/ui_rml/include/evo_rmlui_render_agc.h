#pragma once
/*
 * evo_rmlui_render_agc.h - #28 Phase 4: GPU geometry sink.
 *
 * The RmlUi UI runs on the CPU coverage rasteriser (evo_rmlui_render.cpp). The
 * held-scroll cost is that rasteriser chewing through rounded rects + gradients
 * - which RmlUi emits as vertex-coloured *untextured* triangles. This sink
 * collects that solid-geometry stream during a Context::Render() pass and hands
 * it to pp_agc_present_geo() (SharpProspero mesh shaders -> one DCB -> flip).
 * Text and icons are textured draws; those stay on the CPU blitter and are
 * composited afterwards (see gpu-rendering-plan.md Step 3).
 *
 * This is NOT an Rml::RenderInterface - RmlUi 6 binds the interface at context
 * creation and cannot swap it. Instead EvoRenderInterface owns an optional
 * pointer to one of these; when set, its RenderGeometry() diverts untextured,
 * untransformed draws here instead of CPU-rasterising them.
 *
 * Everything here is host-safe (plain std::vector); the GPU present is behind
 * EVO_APP_MODULE + --agc-probe + /mnt/usb0/evo_agc_ui in pp_agc.c.
 */
#include <RmlUi/Core/Vertex.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Matrix4.h>
#include <cstdint>
#include <vector>

extern "C" {
#include "pp_agc.h"   /* pp_agc_geo_vertex_t / pp_agc_geo_draw_t */
}

class EvoAgcGeoSink {
public:
    /* Reset for a new frame. w/h are the target (VO output) dimensions. */
    void Begin(int w, int h);

    /* Record one untextured RmlUi geometry batch. `verts`/`inds` are the
     * compiled geometry; `translation` is RmlUi's per-draw offset; the scissor
     * is the pixel rect currently enabled (scissor_enabled == false -> whole
     * target). `transform` is RmlUi's active element transform (e.g. a focus
     * `scale(1.04)`) in pixel space, or nullptr for none - it is folded into the
     * vertex positions here so the GPU still draws the batch (rather than the
     * whole thing dropping to the CPU rasteriser). Called from
     * EvoRenderInterface::RenderGeometry. */
    void Add(const std::vector<Rml::Vertex>& verts,
             const std::vector<int>& inds,
             Rml::Vector2f translation,
             bool scissor_enabled,
             Rml::Rectanglei scissor,
             const Rml::Matrix4f* transform = nullptr);

    bool Empty() const { return m_draws.empty(); }
    int  Width()  const { return m_w; }
    int  Height() const { return m_h; }

    const std::vector<pp_agc_geo_vertex_t>& Vertices() const { return m_verts; }
    const std::vector<uint32_t>&            Indices()  const { return m_indices; }
    const std::vector<pp_agc_geo_draw_t>&   Draws()    const { return m_draws; }

    /* Diagnostics: total vertices/indices/draws in the last frame. */
    size_t VertexCount() const { return m_verts.size(); }

private:
    int m_w = 0, m_h = 0;
    std::vector<pp_agc_geo_vertex_t> m_verts;
    std::vector<uint32_t>            m_indices;
    std::vector<pp_agc_geo_draw_t>   m_draws;
};
