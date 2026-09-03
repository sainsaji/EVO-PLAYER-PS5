; glyph_ps.s — single-channel (coverage) atlas × vertex-colour pixel shader
;              (GCN / RDNA2, gfx1030)
;
; cov = texture(atlas, uv).x            ; 1-channel coverage / alpha
; out = vertexColour * cov              ; premultiplied colour scaled by coverage
;
; RmlUi text: the font engine hands GenerateTexture an already-premultiplied
; RGBA atlas today, but a coverage-only atlas (R8) is the efficient GPU form
; and this shader is also what samples the R8 clip-mask target
; (RenderToClipMask, Phase 2). Step 3 (#28).
;
; STATUS: hand-written, NOT run on hardware. Register conventions mirror
; ProsperoLight's pixel.text.linear-buffer.bin (see README.md). Assemble:
;   ./tools/build-shader.sh pp/shaders/glyph_ps.s
;
; Header: reuses pp/blobs/pixel.header.bin verbatim (1 image + 1 sampler + 1
; CB is a subset of the NV12 PS's 2 images + 2 samplers + 1 CB).
;
; Inputs (from the linked VS / SPI setup, per the .header.bin):
;   s[28:29]    descriptor-table base
;   s[0:7]      tex0 image descriptor (T#)   — the R8 atlas / mask
;   s[16:19]    tex0 sampler (S#)
;   v0, v1      barycentric I, J
;   m0          interpolation state (from s30)
;   attr0.xy    UV
;   attr1.xyzw  premultiplied vertex colour
;
; Output:
;   exp mrt0    RGBA float4

	s_mov_b32 m0, s30

	; interpolate UV (attr0.xy) into v2, v3
	v_interp_p1_f32_e32 v2, v0, attr0.x
	v_interp_p2_f32_e32 v2, v1, attr0.x
	v_interp_p1_f32_e32 v3, v0, attr0.y
	v_interp_p2_f32_e32 v3, v1, attr0.y

	; interpolate vertex colour (attr1.xyzw) into v8..v11
	v_interp_p1_f32_e32 v8,  v0, attr1.x
	v_interp_p2_f32_e32 v8,  v1, attr1.x
	v_interp_p1_f32_e32 v9,  v0, attr1.y
	v_interp_p2_f32_e32 v9,  v1, attr1.y
	v_interp_p1_f32_e32 v10, v0, attr1.z
	v_interp_p2_f32_e32 v10, v1, attr1.z
	v_interp_p1_f32_e32 v11, v0, attr1.w
	v_interp_p2_f32_e32 v11, v1, attr1.w

	; load tex0 image + sampler descriptors
	s_load_dwordx8 s[0:7],   s[28:29], 0x0
	s_load_dwordx4 s[16:19], s[28:29], 0x20
	s_waitcnt lgkmcnt(0)

	; sample coverage -> v12 (single channel)
	image_sample v12, v[2:3], s[0:7], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)

	; modulate premultiplied colour by coverage
	v_mul_f32_e32 v4, v8,  v12
	v_mul_f32_e32 v5, v9,  v12
	v_mul_f32_e32 v6, v10, v12
	v_mul_f32_e32 v7, v11, v12

	exp mrt0 v4, v5, v6, v7 done vm
	s_endpgm
