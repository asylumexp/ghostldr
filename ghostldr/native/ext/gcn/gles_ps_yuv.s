// ghostldr GLES on the GPU (gles_gpu.c): the pixel shader for SDL2's YUV and
// NV12/NV21 shaders: rgb = matrix * (yuv + offset), clamped, then times the
// vertex colour and the constant, alpha 1 (times the same).
//   user SGPRs: s[0:1] the draw's table: three T# + S# pairs (Y, U, V: for
//               NV12/21 the U and V descriptors are the interleaved plane's,
//               each selecting its channel into x), the constant (4 floats),
//               the offset (3), the matrix (9, column-major as GLSL's mat3)
//   s2: the primitive mask; v0, v1: perspective I, J
//   in: attr0 = (u, v), attr1 = colour; out: mrt0, FP16 ABGR
  s_mov_b32 m0, s2
  s_load_dwordx8 s[4:11], s[0:1], 0x0
  s_load_dwordx4 s[12:15], s[0:1], 0x8
  s_load_dwordx8 s[16:23], s[0:1], 0xc
  s_load_dwordx4 s[24:27], s[0:1], 0x14
  s_load_dwordx8 s[28:35], s[0:1], 0x18
  s_load_dwordx4 s[36:39], s[0:1], 0x20
  s_load_dwordx16 s[40:55], s[0:1], 0x24
  v_interp_p1_f32 v2, v0, attr0.x
  v_interp_p2_f32 v2, v1, attr0.x
  v_interp_p1_f32 v3, v0, attr0.y
  v_interp_p2_f32 v3, v1, attr0.y
  v_interp_p1_f32 v8, v0, attr1.x
  v_interp_p2_f32 v8, v1, attr1.x
  v_interp_p1_f32 v9, v0, attr1.y
  v_interp_p2_f32 v9, v1, attr1.y
  v_interp_p1_f32 v10, v0, attr1.z
  v_interp_p2_f32 v10, v1, attr1.z
  v_interp_p1_f32 v11, v0, attr1.w
  v_interp_p2_f32 v11, v1, attr1.w
  s_waitcnt lgkmcnt(0)
  image_sample_lz v12, v[2:3], s[4:11], s[12:15] dmask:0x1
  image_sample_lz v16, v[2:3], s[16:23], s[24:27] dmask:0x1
  image_sample_lz v20, v[2:3], s[28:35], s[36:39] dmask:0x1
  // the constant times the vertex colour
  v_mul_f32 v8, s40, v8
  v_mul_f32 v9, s41, v9
  v_mul_f32 v10, s42, v10
  v_mul_f32 v11, s43, v11
  s_waitcnt vmcnt(0)
  // yuv + offset (s44..s46)
  v_add_f32 v12, s44, v12
  v_add_f32 v16, s45, v16
  v_add_f32 v20, s46, v20
  // r = m0 y + m3 u + m6 v, g = m1 y + m4 u + m7 v, b = m2 y + m5 u + m8 v (m in s47..s55)
  v_mul_f32 v4, s47, v12
  v_mac_f32 v4, s50, v16
  v_mac_f32 v4, s53, v20
  v_mul_f32 v5, s48, v12
  v_mac_f32 v5, s51, v16
  v_mac_f32 v5, s54, v20
  v_mul_f32 v6, s49, v12
  v_mac_f32 v6, s52, v16
  v_mac_f32 v6, s55, v20
  // clamped to 0..1, as an 8-bit colour would be, before the modulation
  v_med3_f32 v4, v4, 0, 1.0
  v_med3_f32 v5, v5, 0, 1.0
  v_med3_f32 v6, v6, 0, 1.0
  v_mul_f32 v4, v4, v8
  v_mul_f32 v5, v5, v9
  v_mul_f32 v6, v6, v10
  v_cvt_pkrtz_f16_f32 v12, v4, v5
  v_cvt_pkrtz_f16_f32 v13, v6, v11
  exp mrt0 v12, v12, v13, v13 done compr vm
  s_endpgm
