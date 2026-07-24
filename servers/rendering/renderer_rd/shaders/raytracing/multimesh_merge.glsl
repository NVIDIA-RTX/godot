#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_ARB_gpu_shader_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Bakes the per-(MultiMesh, surface) merged BLAS inputs in a single dispatch:
//   * world-space positions (per instance) into the merged vertex buffer,
//   * rotated normals/tangents (if the source mesh has them),
//   * per-vertex attributes replicated N times into the merged attribute buffer,
//   * (MODE_INDEXED only) the index buffer replicated and offset by inst * V.
//
// Thread idx maps to a vertex (idx < N*V) and, for MODE_INDEXED, an index
// (idx < N*I) -- both run in the same invocation, no barrier needed because
// they target disjoint destination buffers.
//
// Source vertex/index buffers are immutable mesh data and accessed via BDA.
// Everything else (mm transforms, mesh attributes, all merged outputs) goes
// through descriptor sets so the draw graph tracks the dependencies for us.
layout(buffer_reference, std430) readonly buffer SrcFloatBuf {
	float v[];
};
layout(buffer_reference, std430) readonly buffer SrcUintBuf {
	uint v[];
};

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

layout(push_constant, std430) uniform PC {
	uint src_vtx_lo; // BDA lo of source vertex buffer (immutable)
	uint src_vtx_hi;
#ifdef MODE_INDEXED
	uint src_idx_lo; // BDA lo of source index buffer (immutable)
	uint src_idx_hi;
	uint index_count; // indices per instance
	uint src_is_16bit; // 1 if source is packed uint16
#endif
	uint vertex_count; // V (per instance)
	uint instance_count; // N
	uint pos_stride_words; // position stride in float words (3 for uncompressed)
	uint src_tbn_base_words; // word offset of TBN section in source vertex buffer
	uint src_tbn_stride_words; // 1 = normal only, 2 = normal + tangent (uint32 each)
	uint dst_tbn_base_words; // N * V * 3 (word offset of TBN section in output)
	uint mm_stride; // mm transform stride in floats
	uint mm_offset; // mm current instance offset (motion vectors double-buffer)
	uint has_tbn; // 1 if mesh has normals/tangents, 0 otherwise
	uint attr_stride_words; // attribute stride in uint words (0 = mesh has no attribs)
}
push;

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

void main() {
	uint idx = gl_GlobalInvocationID.x;
	uint total_v = push.vertex_count * push.instance_count;

	// --- Per-vertex section: bake position, replicate attributes, rotate TBN ---
	if (idx < total_v) {
		uint inst = idx / push.vertex_count;
		uint vtx = idx % push.vertex_count;

		SrcFloatBuf src = SrcFloatBuf(packUint2x32(uvec2(push.src_vtx_lo, push.src_vtx_hi)));
		vec3 p = vec3(
				src.v[vtx * push.pos_stride_words + 0u],
				src.v[vtx * push.pos_stride_words + 1u],
				src.v[vtx * push.pos_stride_words + 2u]);

		// MM buffer stores a row-major 3x4 transform per instance.
		// Read the three rows, then transpose so GLSL's column-major mat3
		// matches the source's rotation/scale and we can write `rot * v`.
		uint b = (push.mm_offset + inst) * push.mm_stride;
		vec4 row0 = vec4(mm_buf.v[b + 0u], mm_buf.v[b + 1u], mm_buf.v[b + 2u], mm_buf.v[b + 3u]);
		vec4 row1 = vec4(mm_buf.v[b + 4u], mm_buf.v[b + 5u], mm_buf.v[b + 6u], mm_buf.v[b + 7u]);
		vec4 row2 = vec4(mm_buf.v[b + 8u], mm_buf.v[b + 9u], mm_buf.v[b + 10u], mm_buf.v[b + 11u]);
		mat3 rot = transpose(mat3(row0.xyz, row1.xyz, row2.xyz));
		vec3 t = vec3(row0.w, row1.w, row2.w);

		vec3 wp = rot * p + t;
		uint pos_base = idx * 3u;
		dst_vtx.v[pos_base + 0u] = floatBitsToUint(wp.x);
		dst_vtx.v[pos_base + 1u] = floatBitsToUint(wp.y);
		dst_vtx.v[pos_base + 2u] = floatBitsToUint(wp.z);

		for (uint w = 0u; w < push.attr_stride_words; w++) {
			dst_attr.v[idx * push.attr_stride_words + w] = src_attr.v[vtx * push.attr_stride_words + w];
		}

		if (push.has_tbn != 0u) {
			SrcUintBuf src_u = SrcUintBuf(packUint2x32(uvec2(push.src_vtx_lo, push.src_vtx_hi)));
			uint tbn_src = push.src_tbn_base_words + vtx * push.src_tbn_stride_words;
			uint n_packed = src_u.v[tbn_src];

			vec2 n_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16u)) / 65535.0 * 2.0 - 1.0;
			vec3 rn = normalize(rot * oct_to_vec3(n_oct));
			vec2 rn_oct = vec3_to_oct(rn);
			uint rn_packed = uint(rn_oct.x * 32767.5 + 32767.5) | (uint(rn_oct.y * 32767.5 + 32767.5) << 16u);

			uint tbn_dst = push.dst_tbn_base_words + idx * push.src_tbn_stride_words;
			dst_vtx.v[tbn_dst] = rn_packed;

			if (push.src_tbn_stride_words >= 2u) {
				// Tangent encodes the bitangent sign in sign(t_raw.y); preserve it.
				uint t_packed = src_u.v[tbn_src + 1u];
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

#ifdef MODE_INDEXED
	// --- Per-index section: replicate and offset by inst * V ---
	uint total_i = push.index_count * push.instance_count;
	if (idx < total_i) {
		uint inst_i = idx / push.index_count;
		uint local_pos = idx % push.index_count;

		SrcUintBuf src_i = SrcUintBuf(packUint2x32(uvec2(push.src_idx_lo, push.src_idx_hi)));
		uint src_idx;
		if (push.src_is_16bit != 0u) {
			uint byte_off = local_pos * 2u;
			uint word = src_i.v[byte_off >> 2u];
			src_idx = ((byte_off & 2u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
		} else {
			src_idx = src_i.v[local_pos];
		}

		dst_idx.v[idx] = src_idx + inst_i * push.vertex_count;
	}
#endif
}
