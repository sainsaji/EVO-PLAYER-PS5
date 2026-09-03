; texbuf_ps.s — textured PS via TYPED BUFFER load (no image_sample / sampler)
;               (GCN / RDNA2, gfx1030)
;
; Probe (#28 Phase 1, option B): does sceAgcCreateShader accept a pixel shader
; that reads a texel with buffer_load_format from a V# in s[0:3] - the same
; primitive the working geometry.text.bin VS uses for vertex fetch - instead of
; image_sample from a T#/S#? image_sample triggers the "sl00" resource-metadata
; validation that rejects every hand-written textured shader (0x8a6c001f).
;
; If this passes: UI text/icons render point-sampled from a single fixed-width
; RGBA8 atlas buffer, UV pre-scaled to texel space by CompileGeometry, row
; stride a compile-time constant. No sampler, no mips - fine for 1:1 UI.
;
; STATUS: probe. Assemble: ./tools/build-shader.sh pp/shaders/texbuf_ps.s
;
; Inputs:
;   s[0:3]      atlas buffer descriptor (V#)  — pre-loaded from the SH block
;   v0, v1      barycentric I, J
;   m0          interpolation state (from s30)
;   attr0.xy    UV in TEXEL units (CompileGeometry already multiplied by dims
;               and offset to the sub-rect origin)
;   attr1.xyzw  premultiplied vertex colour
;
; ATLAS_ROW_BYTES = atlas_width(2048) * 4  = 8192  (compile-time)

	s_mov_b32 m0, s30

	; interp texel-space UV -> v2 (u), v3 (v)
	v_interp_p1_f32_e32 v2, v0, attr0.x
	v_interp_p2_f32_e32 v2, v1, attr0.x
	v_interp_p1_f32_e32 v3, v0, attr0.y
	v_interp_p2_f32_e32 v3, v1, attr0.y

	; interp vertex colour -> v8..v11
	v_interp_p1_f32_e32 v8,  v0, attr1.x
	v_interp_p2_f32_e32 v8,  v1, attr1.x
	v_interp_p1_f32_e32 v9,  v0, attr1.y
	v_interp_p2_f32_e32 v9,  v1, attr1.y
	v_interp_p1_f32_e32 v10, v0, attr1.z
	v_interp_p2_f32_e32 v10, v1, attr1.z
	v_interp_p1_f32_e32 v11, v0, attr1.w
	v_interp_p2_f32_e32 v11, v1, attr1.w

	; texel_x = (int)u ; texel_y = (int)v   (UV >= 0, truncate = floor)
	v_cvt_i32_f32_e32 v2, v2
	v_cvt_i32_f32_e32 v3, v3

	; byte offset = texel_y * 8192 + texel_x * 4
	v_lshlrev_b32_e32 v3, 13, v3
	v_lshlrev_b32_e32 v2, 2, v2
	v_add_nc_u32_e32 v20, v3, v2

	; load one RGBA8 texel (V# format handles the 8_8_8_8 unpack)
	buffer_load_format_xyzw v[4:7], v20, s[0:3], 0 offen
	s_waitcnt vmcnt(0)

	; modulate by premultiplied vertex colour
	v_mul_f32_e32 v4, v4, v8
	v_mul_f32_e32 v5, v5, v9
	v_mul_f32_e32 v6, v6, v10
	v_mul_f32_e32 v7, v7, v11

	exp mrt0 v4, v5, v6, v7 done vm
	s_endpgm
