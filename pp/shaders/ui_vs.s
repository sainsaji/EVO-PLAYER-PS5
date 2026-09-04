; ui_vs.s — 2D-ortho UI vertex/primitive shader (GCN / RDNA2 NGG, gfx1030)
;
; Transforms a structured RmlUi vertex by an ortho MVP and passes UV + colour.
; Step 3 (#28).
;
;   in  vertex { float2 pos; float4 colour; float2 uv; }   (32 B, see note)
;   in  b0     { row_major float4x4 mvp; }
;   out pos0   = mvp * float4(pos, 0, 1)
;   out param0 = uv.xyyy
;   out param1 = colour.xyzw   (premultiplied, interpolated for gradients)
;
; ============================================================================
;  HARDWARE RESULT (2026-09-04, pp_agc_probe_ui_shaders, --agc-probe)
; ============================================================================
; sceAgcCreateShader(this, geometry.header.bin) = 0x0  — ACCEPTED.
; sceAgcLinkShaders(this, solid_ps, TriangleList) = 0x0 — LINKS.
;
; So the reused geometry.header.bin tolerates this VS's 3 vertex fetches + 2
; param exports (contrary to the earlier "needs its own header" worry). What is
; NOT yet verified is whether it actually FETCHES correctly when drawn — the
; attribute-descriptor selection math below is elided (see the buffer_load_format
; lines). Phase 1's first real draw confirms/fixes that against
; geometry.text.bin's exact sequence.
;
; Textured pixel shaders are the real wall: any PS that touches a memory
; resource (image_sample OR buffer_load_format) fails sceAgcCreateShader
; 0x8a6c001f — it validates the code against the header's "sl00" metadata and
; ProsperoLight only ships NV12/P010 PS headers. Text/icons/art need the
; GLSL -> SPIR-V -> AMD ISA toolchain (agc-implementation.md §7). The solid
; path (this VS + solid_ps) is unblocked.
; ============================================================================
;
; Inputs (Sony fetch-shader convention, mirrored from geometry.text.bin):
;   s[8:11]    b0 constant-buffer V#   (MVP, 16 dwords)
;   s[12:13]   vertex-attribute descriptor-table base
;   s14/s15/.. packed per-attribute fetch info (stride / format / table index)
;   v5         S_VERTEX_ID (wave-relative), v8 = 0 helper
;
; STATUS: CreateShader + LinkShaders verified on hardware; vertex fetch not yet
; exercised by a real draw. Assemble:  ./tools/build-shader.sh pp/shaders/ui_vs.s

	s_inst_prefetch 0x3

	; --- NGG primitive alloc + prim export (verbatim shape from ref) ---
	s_bfe_u32 vcc_hi, s3, 0x80008
	s_lshl_b32 vcc_lo, vcc_hi, 12
	s_and_b32 s0, s3, 0xff
	s_or_b32 m0, s0, vcc_lo
	s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
	s_sub_i32 vcc_lo, 64, vcc_hi
	s_lshr_b64 exec, -1, vcc_lo
	exp prim v0, off, off, off done
	s_sub_i32 vcc_lo, 64, s0
	s_waitcnt expcnt(0)
	s_lshr_b64 exec, -1, vcc_lo

	; --- load MVP (b0, 16 dwords) into s[16:31] ---
	s_buffer_load_dwordx16 s[16:31], s[8:11], null
	s_waitcnt lgkmcnt(0)

	; --- fetch attribute 0: position (xy) -> v[0:1] ---
	;   (attribute-descriptor selection math elided — see geometry.text.bin;
	;    a real build resolves s[12:13] + packed info like the ref does)
	v_mov_b32_e32 v6, 0
	buffer_load_format_xy  v[0:1], v5, s[0:3], s14 idxen
	; --- fetch attribute 1: uv (xy) -> v[3:4] ---
	buffer_load_format_xy  v[3:4], v5, s[0:3], s15 idxen
	; --- fetch attribute 2: colour (xyzw) -> v[9:12] ---
	buffer_load_format_xyzw v[9:12], v5, s[0:3], s16 idxen
	s_waitcnt vmcnt(0)

	; --- pos = mvp * (x, y, 0, 1)   (row-major, s[16:31]) ---
	v_mul_f32_e32       v13, s16, v0
	v_fmac_f32_e32      v13, s20, v1
	v_add_f32_e32       v13, s28, v13          ; + w term (z=0)
	v_mul_f32_e32       v14, s17, v0
	v_fmac_f32_e32      v14, s21, v1
	v_add_f32_e32       v14, s29, v14
	v_mul_f32_e32       v15, s18, v0
	v_fmac_f32_e32      v15, s22, v1
	v_add_f32_e32       v15, s30, v15
	v_mul_f32_e32       v16, s19, v0
	v_fmac_f32_e32      v16, s23, v1
	v_add_f32_e32       v16, s31, v16

	exp pos0 v13, v14, v15, v16 done
	s_waitcnt expcnt(0)

	; param0 = uv, param1 = colour
	exp param0 v3, v4, v4, v4
	exp param1 v9, v10, v11, v12
	s_endpgm
