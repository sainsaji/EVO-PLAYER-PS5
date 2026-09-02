#pragma once

/*
 * RmlUi frame profiler — compiled in ONLY when EVO_RML_PROFILE is defined.
 *
 * No build defines it by default: scripts/build-evoplayer.sh, the app-module
 * package, tools/uiview*.sh and tools/uiplay.sh all leave it off, so this
 * header collapses to nothing and the render path is byte-identical to before.
 * tools/prof_rmlui.sh is the only consumer and passes -DEVO_RML_PROFILE.
 *
 * Attribution model (see docs/evo-pro/gpu-rendering-plan.md §6):
 *   update_ms  — Rml::Context::Update()  (style + layout walk)
 *   render_ms  — Rml::Context::Render()  (everything below is a subset of this)
 *     geo_quad_ms — RenderGeometry calls served entirely by the fast AA-quad blitter
 *     geo_tri_ms  — RenderGeometry calls that fell through to the coverage rasterizer
 *     clip_ms     — RenderToClipMask (rounded-corner / overflow:hidden cutouts)
 *     gentex_ms   — GenerateTexture (glyph atlas (re)builds; 0 == pure atlas hits)
 *     loadtex_ms  — LoadTexture (icon PNG decode + premultiply, or evo:mem lookup)
 */

#ifdef EVO_RML_PROFILE

#include <chrono>

struct EvoRmlProf {
    double geo_quad_ms, geo_tri_ms, clip_ms, gentex_ms, loadtex_ms;
    double update_ms, render_ms;
    long   geo_quad_n,  geo_tri_n,  clip_n,  gentex_n,  loadtex_n;
    long   update_n,    render_n;
    long   gentex_px;
};

extern EvoRmlProf g_evo_rml_prof;

inline double evo_prof_now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

inline void evo_prof_reset() { g_evo_rml_prof = EvoRmlProf{}; }

#endif /* EVO_RML_PROFILE */
