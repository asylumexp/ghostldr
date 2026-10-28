// ghostldr GLES on the GPU (gles_gpu.c): the vertex shader, GCN gfx7 (PS4).
// The CPU has already transformed the vertices (the same code as the software
// path), so this only fetches them and passes them on.
//   user SGPRs: s[0:3] the vertex ring's V# (stride 32 bytes: x y u v r g b a,
//               x and y in clip space, w = 1), s4 the draw's first vertex
//   v0: the vertex index (auto-generated, from 0)
//   out: pos0 = (x, y, 0, 1), param0 = (u, v, 0, 1), param1 = (r, g, b, a)
  v_add_i32 v0, vcc, s4, v0
  buffer_load_dwordx4 v[1:4], v0, s[0:3], 0 idxen
  buffer_load_dwordx4 v[5:8], v0, s[0:3], 0 idxen offset:16
  s_waitcnt vmcnt(0)
  v_mov_b32 v9, 0
  v_mov_b32 v10, 1.0
  exp param0 v3, v4, v9, v10
  exp param1 v5, v6, v7, v8
  exp pos0 v1, v2, v9, v10 done
  s_endpgm
