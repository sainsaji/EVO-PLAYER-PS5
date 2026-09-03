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
;  HEADER RISK — READ BEFORE ITERATING ON HARDWARE
; ============================================================================
; The video path (#27) reuses ProsperoLight's geometry.header.bin verbatim and
; swaps only geometry.text.bin. That works because its VS is a strict fetch of
; { float3 pos; float2 uv } with ONE exported param — a structural match.
;
; This UI VS needs a THIRD fetched attribute (per-vertex colour) and a SECOND
; exported param. That is a header change, not a subset: the reused
; geometry.header.bin's vertex-input descriptor table (2 entries) and its
; SPI_VS_OUT_CONFIG / param-export count will not describe this program.
;
; Options, in order of preference (decide at the Phase-1 hardware session):
;   1. Smuggle colour through the existing 2-slot fetch: upload the UI vertex as
;      { float2 pos; u32 colour; float2 uv } and fetch it as { float3; float2 }.
;      Colour rides in "pos.z". Requires the colour param export to be FLAT
;      (S_MODE / interp mode in the header) AND unpacking 4x u8 in the PS. Loses
;      per-triangle colour interpolation — acceptable for solid fills, WRONG for
;      RmlUi gradient meshes. Ship a separate non-interpolated gradient path or
;      accept banding.
;   2. Hand-build a matching header from the RDNA2 ISA reference (doc 70648) +
;      the .sb parsers in KytyPS5 / shadPS5 / SharpProspero ShaderInfo.cs
;      (agc-implementation.md §7). Highest effort, fully correct.
;   3. Add glslang + SPIRV-LLVM-Translator to the container and compile a real
;      VS from GLSL — the compiler emits a correct header. (Question 3 was
;      answered "hand-write GCN now"; revisit if 1 and 2 both stall.)
;
; The body below is written for option 2/3 (3 real attributes, 2 params). It
; assembles but has NOT run on hardware and has no matching header yet.
; ============================================================================
;
; Inputs (Sony fetch-shader convention, mirrored from geometry.text.bin):
;   s[8:11]    b0 constant-buffer V#   (MVP, 16 dwords)
;   s[12:13]   vertex-attribute descriptor-table base
;   s14/s15/.. packed per-attribute fetch info (stride / format / table index)
;   v5         S_VERTEX_ID (wave-relative), v8 = 0 helper
;
; STATUS: hand-written, NOT run on hardware, NO matching header. Assemble:
;   ./tools/build-shader.sh pp/shaders/ui_vs.s

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
