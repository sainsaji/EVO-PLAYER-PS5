; solid_ps.s — solid vertex-colour pixel shader (GCN / RDNA2, gfx1030)
;
; out = vertexColour   (premultiplied, RmlUi ColourbPremultiplied)
;
; RmlUi's untextured case: rects, borders, and gradient meshes (gradients
; arrive as per-vertex-coloured geometry, no texture). Step 3 (#28).
;
; STATUS: hand-written, NOT run on hardware. Register conventions mirror
; ProsperoLight's pixel.text.linear-buffer.bin (see README.md). Assemble:
;   ./tools/build-shader.sh pp/shaders/solid_ps.s
;
; Header: reuses pp/blobs/pixel.header.bin verbatim. A solid PS is a strict
; structural subset of the NV12 PS (no images, no samplers, no CB) so the
; reused header over-describes rather than mis-describes — the first device
; check is that sceAgcCreateShader still accepts the pair.
;
; Inputs (from the linked VS / SPI setup, per the .header.bin):
;   v0, v1      barycentric I, J
;   m0          interpolation state (from s30)
;   attr1.xyzw  premultiplied vertex colour   (attr0 = UV, ignored here)
;
; Output:
;   exp mrt0    RGBA float4 -> the BGRA8 render target (channel order handled
;               by the render-target descriptor, as in #27's CxRenderTarget)

	s_mov_b32 m0, s30

	; interpolate vertex colour (attr1.xyzw) into v4..v7
	v_interp_p1_f32_e32 v4, v0, attr1.x
	v_interp_p2_f32_e32 v4, v1, attr1.x
	v_interp_p1_f32_e32 v5, v0, attr1.y
	v_interp_p2_f32_e32 v5, v1, attr1.y
	v_interp_p1_f32_e32 v6, v0, attr1.z
	v_interp_p2_f32_e32 v6, v1, attr1.z
	v_interp_p1_f32_e32 v7, v0, attr1.w
	v_interp_p2_f32_e32 v7, v1, attr1.w

	exp mrt0 v4, v5, v6, v7 done vm
	s_endpgm
