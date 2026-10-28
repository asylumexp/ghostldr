// ghostldr GLES on the GPU (gles_gpu.c): the pixel shader for SDL2's solid and
// texture shaders: colour = texture(u, v) * vertex colour * constant.
//   user SGPRs: s[0:1] the draw's table: T# (8 dwords), S# (4), constant (4 floats)
//               (a solid draw samples a 1x1 white texture; the texture's
//               channel swaps and "alpha = 1" are in its T#'s DST_SEL)
//   s2: the primitive mask (after the user SGPRs); v0, v1: perspective I, J
//   in: attr0 = (u, v), attr1 = colour
//   out: mrt0, FP16 ABGR (SPI_SHADER_COL_FORMAT 4)
  s_mov_b32 m0, s2
  s_load_dwordx8 s[4:11], s[0:1], 0x0
  s_load_dwordx4 s[12:15], s[0:1], 0x8
  s_load_dwordx4 s[16:19], s[0:1], 0xc
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
  image_sample_lz v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf
  v_mul_f32 v8, s16, v8
  v_mul_f32 v9, s17, v9
  v_mul_f32 v10, s18, v10
  v_mul_f32 v11, s19, v11
  s_waitcnt vmcnt(0)
  v_mul_f32 v4, v4, v8
  v_mul_f32 v5, v5, v9
  v_mul_f32 v6, v6, v10
  v_mul_f32 v7, v7, v11
  v_cvt_pkrtz_f16_f32 v12, v4, v5
  v_cvt_pkrtz_f16_f32 v13, v6, v7
  exp mrt0 v12, v12, v13, v13 done compr vm
  s_endpgm
