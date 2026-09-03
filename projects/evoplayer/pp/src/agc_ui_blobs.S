/*
 * agc_ui_blobs.S - the Step-3 (#28) UI shader blobs, embedded.
 *
 * Companion to agc_blobs.S (the #27 video shaders). These are the hand-written
 * GCN/RDNA2 pixel/vertex shaders for rendering the RmlUi UI on the GPU:
 *
 *   ui_vs      2D-ortho UI vertex/primitive shader   (pp/shaders/ui_vs.s)
 *   solid_ps   solid vertex-colour pixel shader      (pp/shaders/solid_ps.s)
 *   glyph_ps   coverage-atlas x colour pixel shader  (pp/shaders/glyph_ps.s)
 *   rgba_ps    textured x colour pixel shader         (pp/shaders/rgba_ps.s)
 *
 * Assembled by tools/build-shader.sh --all (llvm-mc-18, gfx1030). SHA256s in
 * pp/shaders/SHA256SUMS. See docs/evo-pro/agc-implementation.md section 5.
 *
 * The .text (code) half is checked in under pp/shaders/ as .bin. The header:
 *   - pixel shaders reuse ProsperoLight's pixel.header.bin verbatim (each is a
 *     structural subset of the NV12 PS - fewer images/samplers/CBs).
 *   - ui_vs has NO matching header yet - see the risk note at the top of
 *     pp/shaders/ui_vs.s. Until it is resolved, ui_vs_header points at
 *     geometry.header.bin as a placeholder and the GPU UI path stays gated
 *     behind --agc-probe / pp_agc_ui_ready() == 0.
 *
 * .incbin paths are relative to make's CWD (projects/evoplayer/):
 *   pp/blobs         vendored ProsperoLight headers (this module)
 *   ../../pp/shaders  hand-written shader code (repo root)
 */
#define BLOB(sym, file) \
    .section .rodata            ; \
    .global sym##_start         ; \
sym##_start:                    ; \
    .incbin file                ; \
    .global sym##_end           ; \
sym##_end:                      ; \
    .text

/* headers */
BLOB(pp_agc_ui_vs_header,   "pp/blobs/geometry.header.bin")   /* placeholder - see note */
BLOB(pp_agc_ui_ps_header,   "pp/blobs/pixel.header.bin")

/* code (hand-written, tools/build-shader.sh --all) */
BLOB(pp_agc_ui_vs_code,     "../../pp/shaders/ui_vs.bin")
BLOB(pp_agc_solid_ps_code,  "../../pp/shaders/solid_ps.bin")
BLOB(pp_agc_glyph_ps_code,  "../../pp/shaders/glyph_ps.bin")
BLOB(pp_agc_rgba_ps_code,   "../../pp/shaders/rgba_ps.bin")
