#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Bakes the per-(MultiMesh, surface) merged BLAS inputs in a single dispatch:
//   * world-space positions (per instance) into the merged vertex buffer,
//   * rotated normals/tangents (if the source mesh has them),
//     or, with BAKE_CAPTURED, both copied from a vertex capture already in the merged layout,
//   * per-vertex attributes replicated N times into the merged attribute buffer,
//   * (MODE_INDEXED only) the index buffer replicated and offset by inst * V.
//
// Thread idx maps to a vertex (idx < N*V) and, for MODE_INDEXED, an index
// (idx < N*I) -- both run in the same invocation, no barrier needed because
// they target disjoint destination buffers.
//
// Every buffer goes through the descriptor set, so the draw graph orders the dispatch like any other.

layout(set = 0, binding = 0) restrict writeonly buffer DstVtxBuf {
	uint v[];
}
dst_vtx;
layout(set = 0, binding = 1) restrict readonly buffer MMBuf {
	float v[];
}
mm_buf;
layout(set = 0, binding = 2) restrict writeonly buffer DstAttrBuf {
	uint v[];
}
dst_attr;
layout(set = 0, binding = 3) restrict readonly buffer SrcAttrBuf {
	uint v[];
}
src_attr;

#ifdef MODE_INDEXED
layout(set = 0, binding = 4) restrict writeonly buffer DstIdxBuf {
	uint v[];
}
dst_idx;
#endif

#define BILLBOARD_NONE 0xFFFFFFFFu
#define BILLBOARD_MODE_DISABLED 0u
#define BILLBOARD_MODE_PARTICLES 3u

// Mirrors RTMergeBillboardParams; bases are columns in MultiMesh node space.
struct BillboardParams {
	vec4 facing[3];
	vec4 node_basis[3];
	vec4 fixed_size; // Scale at instance origin o: dot(xyz, o) + w.
	uint mode;
	uint keep_scale;
	uint fixed_size_enabled;
	uint custom_offset; // Float offset of INSTANCE_CUSTOM in an instance record, or BILLBOARD_NONE.
};

layout(set = 0, binding = 5, std430) restrict readonly buffer BillboardBuf {
	BillboardParams v[];
}
billboards;

// Vertex capture output (RTVertexCapture::Layout); only read with BAKE_CAPTURED.
layout(set = 0, binding = 6, std430) restrict readonly buffer CapturedBuf {
	uint v[];
}
captured;

// Source mesh vertices: [float3 positions x V] + [packed TBN x V].
layout(set = 0, binding = 7, std430) restrict readonly buffer SrcVtxBuf {
	uint v[];
}
src_vtx;

#ifdef MODE_INDEXED
layout(set = 0, binding = 8, std430) restrict readonly buffer SrcIdxBuf {
	uint v[];
}
src_idx_buf;
#endif

#define BAKE_TBN 1u
#define BAKE_CAPTURED 2u

layout(push_constant, std430) uniform PC {
	uint index_count; // indices per instance (MODE_INDEXED)
	uint src_is_16bit; // 1 if the source indices are packed uint16 (MODE_INDEXED)
	uint vertex_count; // V (per instance)
	uint instance_count; // N
	uint captured_base_words; // word offset of the capture in `captured` (BAKE_CAPTURED)
	uint src_tbn_base_words; // word offset of TBN section in source vertex buffer
	uint src_tbn_stride_words; // 1 = normal only, 2 = normal + tangent (uint32 each)
	uint dst_tbn_base_words; // N * V * 3 (word offset of TBN section in output)
	uint mm_stride; // mm transform stride in floats
	uint mm_offset; // mm current instance offset (motion vectors double-buffer)
	uint bake_flags; // BAKE_* bits
	uint attr_stride_words; // attribute stride in uint words (0 = mesh has no attribs)
	uint billboard_index; // entry in `billboards`, or BILLBOARD_NONE
	// Dead particles (zero basis, -INF origin) collapse here into zero-area triangles. Vulkan requires
	// primitives active at build time to stay active on refit, so NaN can't be used instead.
	// Also pads the block to the 16-byte multiple pipelines expect.
	float dead_x;
	float dead_y;
	float dead_z;
}
push;

// Node-space basis of a camera-dependent instance; mirrors RenderingBillboard::compute_world_transform.
mat3 billboard_basis(BillboardParams p_bb, mat3 p_rot, vec3 p_origin, uint p_record) {
	mat3 basis = p_rot;
	if (p_bb.mode != BILLBOARD_MODE_DISABLED) {
		basis = mat3(p_bb.facing[0].xyz, p_bb.facing[1].xyz, p_bb.facing[2].xyz);
		if (p_bb.mode == BILLBOARD_MODE_PARTICLES && p_bb.custom_offset != BILLBOARD_NONE) {
			float angle = mm_buf.v[p_record + p_bb.custom_offset];
			float c = cos(angle);
			float s = sin(angle);
			basis = basis * mat3(vec3(c, -s, 0.0), vec3(s, c, 0.0), vec3(0.0, 0.0, 1.0));
		}
		if (p_bb.keep_scale != 0u) {
			mat3 world = mat3(p_bb.node_basis[0].xyz, p_bb.node_basis[1].xyz, p_bb.node_basis[2].xyz) * p_rot;
			basis[0] *= length(world[0]);
			basis[1] *= length(world[1]);
			basis[2] *= length(world[2]);
		}
	}
	if (p_bb.fixed_size_enabled != 0u) {
		basis *= dot(p_bb.fixed_size.xyz, p_origin) + p_bb.fixed_size.w;
	}
	return basis;
}

vec2 vec3_to_oct(vec3 v) {
	float abs_sum = abs(v.x) + abs(v.y) + abs(v.z);
	v.xy /= abs_sum;
	if (v.z < 0.0) {
		v.xy = (1.0 - abs(v.yx)) * sign(v.xy);
	}
	return v.xy;
}

vec3 oct_to_vec3(vec2 e) {
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

// Point idx of a capture: its float3 position, and its TBN words that follow all positions like in dst_vtx.
void copy_captured(uint idx) {
	uint pos_base = idx * 3u;
	for (uint w = 0u; w < 3u; w++) {
		dst_vtx.v[pos_base + w] = captured.v[push.captured_base_words + pos_base + w];
	}
	if ((push.bake_flags & BAKE_TBN) != 0u) {
		uint tbn = push.dst_tbn_base_words + idx * push.src_tbn_stride_words;
		for (uint w = 0u; w < push.src_tbn_stride_words; w++) {
			dst_vtx.v[tbn + w] = captured.v[push.captured_base_words + tbn + w];
		}
	}
}

void bake_instanced(uint idx, uint inst, uint vtx) {
	vec3 p = uintBitsToFloat(uvec3(src_vtx.v[vtx * 3u + 0u], src_vtx.v[vtx * 3u + 1u], src_vtx.v[vtx * 3u + 2u]));

	// MM buffer stores a row-major 3x4 transform per instance.
	// Read the three rows, then transpose so GLSL's column-major mat3
	// matches the source's rotation/scale and we can write `rot * v`.
	uint b = (push.mm_offset + inst) * push.mm_stride;
	vec4 row0 = vec4(mm_buf.v[b + 0u], mm_buf.v[b + 1u], mm_buf.v[b + 2u], mm_buf.v[b + 3u]);
	vec4 row1 = vec4(mm_buf.v[b + 4u], mm_buf.v[b + 5u], mm_buf.v[b + 6u], mm_buf.v[b + 7u]);
	vec4 row2 = vec4(mm_buf.v[b + 8u], mm_buf.v[b + 9u], mm_buf.v[b + 10u], mm_buf.v[b + 11u]);
	mat3 rot = transpose(mat3(row0.xyz, row1.xyz, row2.xyz));
	vec3 t = vec3(row0.w, row1.w, row2.w);
	// Checked before the billboard, which would replace the dead particle's zero basis.
	bool dead = any(isinf(t)) || any(isnan(t));
	if (push.billboard_index != BILLBOARD_NONE && !dead) {
		rot = billboard_basis(billboards.v[push.billboard_index], rot, t, b);
	}

	vec3 wp = dead ? vec3(push.dead_x, push.dead_y, push.dead_z) : rot * p + t;
	uint pos_base = idx * 3u;
	dst_vtx.v[pos_base + 0u] = floatBitsToUint(wp.x);
	dst_vtx.v[pos_base + 1u] = floatBitsToUint(wp.y);
	dst_vtx.v[pos_base + 2u] = floatBitsToUint(wp.z);

	if ((push.bake_flags & BAKE_TBN) != 0u) {
		uint tbn_src = push.src_tbn_base_words + vtx * push.src_tbn_stride_words;
		uint n_packed = src_vtx.v[tbn_src];

		vec2 n_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16u)) / 65535.0 * 2.0 - 1.0;
		vec3 rn = normalize(rot * oct_to_vec3(n_oct));
		vec2 rn_oct = vec3_to_oct(rn);
		uint rn_packed = uint(rn_oct.x * 32767.5 + 32767.5) | (uint(rn_oct.y * 32767.5 + 32767.5) << 16u);

		uint tbn_dst = push.dst_tbn_base_words + idx * push.src_tbn_stride_words;
		dst_vtx.v[tbn_dst] = rn_packed;

		if (push.src_tbn_stride_words >= 2u) {
			// Tangent encodes the bitangent sign in sign(t_raw.y); preserve it.
			uint t_packed = src_vtx.v[tbn_src + 1u];
			vec2 t_raw = vec2(float(t_packed & 0xFFFFu), float(t_packed >> 16u)) / 65535.0 * 2.0 - 1.0;
			float bitan_sign = sign(t_raw.y);
			vec2 t_oct = vec2(t_raw.x, abs(t_raw.y) * 2.0 - 1.0);
			vec3 rt = normalize(rot * oct_to_vec3(t_oct));
			vec2 rt_oct = vec3_to_oct(rt);
			float t_raw_y = (abs(rt_oct.y) * 0.5 + 0.5) * bitan_sign;
			uint rt_packed = uint(rt_oct.x * 32767.5 + 32767.5) | (uint(t_raw_y * 32767.5 + 32767.5) << 16u);
			dst_vtx.v[tbn_dst + 1u] = rt_packed;
		}
	}
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	uint total_v = push.vertex_count * push.instance_count;

	// --- Per-vertex section: bake position and TBN, replicate attributes ---
	if (idx < total_v) {
		uint inst = idx / push.vertex_count;
		uint vtx = idx % push.vertex_count;

		if ((push.bake_flags & BAKE_CAPTURED) != 0u) {
			copy_captured(idx);
		} else {
			bake_instanced(idx, inst, vtx);
		}

		for (uint w = 0u; w < push.attr_stride_words; w++) {
			dst_attr.v[idx * push.attr_stride_words + w] = src_attr.v[vtx * push.attr_stride_words + w];
		}
	}

#ifdef MODE_INDEXED
	// --- Per-index section: replicate and offset by inst * V ---
	uint total_i = push.index_count * push.instance_count;
	if (idx < total_i) {
		uint inst_i = idx / push.index_count;
		uint local_pos = idx % push.index_count;

		uint src_idx;
		if (push.src_is_16bit != 0u) {
			uint byte_off = local_pos * 2u;
			uint word = src_idx_buf.v[byte_off >> 2u];
			src_idx = ((byte_off & 2u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
		} else {
			src_idx = src_idx_buf.v[local_pos];
		}

		dst_idx.v[idx] = src_idx + inst_i * push.vertex_count;
	}
#endif
}
