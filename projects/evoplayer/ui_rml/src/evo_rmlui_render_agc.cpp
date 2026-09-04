/*
 * evo_rmlui_render_agc.cpp - see evo_rmlui_render_agc.h.
 *
 * Expands RmlUi's premultiplied 2D vertices into SharpProspero's 36-byte mesh
 * vertex layout ({float3 pos; float3 normal; float2 uv; uint32 color}) and
 * batches the draws for pp_agc_present_geo(). Pixel space is kept as-is
 * (y-down, top-left origin); pp_agc.c's MVP maps it to clip space.
 */
#include "evo_rmlui_render_agc.h"

#include <algorithm>
#include <cstring>

void EvoAgcGeoSink::Begin(int w, int h)
{
    m_w = w;
    m_h = h;
    m_verts.clear();
    m_indices.clear();
    m_draws.clear();
}

void EvoAgcGeoSink::Add(const std::vector<Rml::Vertex>& verts,
                        const std::vector<int>& inds,
                        Rml::Vector2f translation,
                        bool scissor_enabled,
                        Rml::Rectanglei scissor)
{
    if (verts.empty() || inds.empty())
        return;

    const uint32_t base = static_cast<uint32_t>(m_verts.size());
    const uint32_t first_index = static_cast<uint32_t>(m_indices.size());

    m_verts.reserve(m_verts.size() + verts.size());
    for (const Rml::Vertex& v : verts) {
        pp_agc_geo_vertex_t out;
        out.x = v.position.x + translation.x;
        out.y = v.position.y + translation.y;
        out.z = 0.0f;
        /* mesh_ps light: lit = colour * (0.25 + 0.75 * saturate(dot(N, -lightDir))),
         * lightDir = normalize(0.4,-1,-0.6). Setting the normal to -lightDir's
         * direction makes dot == 1 -> lit == colour (exact passthrough). */
        out.nx = -0.4f;
        out.ny =  1.0f;
        out.nz =  0.6f;
        out.u = v.tex_coord.x;
        out.v = v.tex_coord.y;
        /* mesh_vs unpacks colour as (A<<24 | R<<16 | G<<8 | B). Rml::ColourbPremultiplied
         * is bytes {r,g,b,a}, so repack rather than memcpy (which would swap R/B). */
        const uint8_t* c = reinterpret_cast<const uint8_t*>(&v.colour);
        out.color = (uint32_t(c[3]) << 24) | (uint32_t(c[0]) << 16) |
                    (uint32_t(c[1]) << 8)  |  uint32_t(c[2]);
        m_verts.push_back(out);
    }

    m_indices.reserve(m_indices.size() + inds.size());
    for (int idx : inds)
        m_indices.push_back(base + static_cast<uint32_t>(idx));

    pp_agc_geo_draw_t d;
    d.first_index = first_index;
    d.index_count = static_cast<uint32_t>(inds.size());
    if (scissor_enabled) {
        int l = std::max(0, scissor.Left());
        int t = std::max(0, scissor.Top());
        int r = std::min(m_w, scissor.Right());
        int b = std::min(m_h, scissor.Bottom());
        d.scissor_x = l;
        d.scissor_y = t;
        d.scissor_w = r > l ? (r - l) : 0;
        d.scissor_h = b > t ? (b - t) : 0;
        if (d.scissor_w == 0 || d.scissor_h == 0)
            return;   /* fully clipped */
    } else {
        d.scissor_x = d.scissor_y = 0;
        d.scissor_w = d.scissor_h = -1;   /* whole target */
    }
    m_draws.push_back(d);
}
