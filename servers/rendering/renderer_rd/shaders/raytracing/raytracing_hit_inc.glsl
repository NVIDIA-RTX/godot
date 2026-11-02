// Shared utilities for hit shaders (closest_hit, any_hit)
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, oct_inc.glsl, raytracing_inc.glsl

#include "raytracing_data_inc.glsl"

// Reads the vec4 at bit `shift` of instance_data_layout from the hit instance's record.
vec4 fetch_instance_field(GeometryData geom, uint primitive_id, uint shift, vec4 fallback) {
	uint offset = (geom.instance_data_layout >> shift) & 0xffu;
	if (geom.instance_data_address == 0ul || offset == 0xffu) {
		return fallback;
	}
	uint divisor = geom.instance_data_primitive_divisor;
	uint instance = divisor == 0u ? 0u : primitive_id / divisor;
	uint stride = geom.instance_data_layout & 0xffu;
	FloatBuffer data = FloatBuffer(geom.instance_data_address + uint64_t(instance * stride + offset) * 4ul);
	return vec4(data.v[0], data.v[1], data.v[2], data.v[3]);
}

vec4 fetch_instance_color(GeometryData geom, uint primitive_id) {
	return fetch_instance_field(geom, primitive_id, 8u, vec4(1.0));
}

vec4 fetch_instance_custom(GeometryData geom, uint primitive_id) {
	return fetch_instance_field(geom, primitive_id, 16u, vec4(0.0));
}

// ============================================================================
// VERTEX ATTRIBUTES
// ============================================================================
struct VertexAttributes {
	vec2 uv;
	vec2 uv_edge1; // uv[i1] - uv[i0], with FETCH_UV.
	vec2 uv_edge2; // uv[i2] - uv[i0], with FETCH_UV.
	uvec3 indices; // Triangle vertex indices.
	vec2 uv2;
	vec3 normal;
	vec3 tangent;
	float bitangent_sign;
	vec4 color;
	vec3 normal_delta1; // Object-space normal[i1] - normal[i0], with FETCH_TBN.
	vec3 normal_delta2; // Object-space normal[i2] - normal[i0], with FETCH_TBN.
	bool has_uv;
	bool has_uv2;
	bool has_normal;
	bool has_tangent;
	bool has_color;
};

struct TBNResult {
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	float bitangent_sign;
	vec3 normal_delta1; // normal[i1] - normal[i0]; only set by fetch_tbn.
	vec3 normal_delta2; // normal[i2] - normal[i0]; only set by fetch_tbn.
};

// ============================================================================
// TRIANGLE INDEX FETCHING
// ============================================================================

// Core implementation taking an explicit primitive ID (works in any shader stage).
void get_triangle_indices_ex(in GeometryData geom, uint primitive_id, out uint i0, out uint i1, out uint i2) {
	if (geom.index_format == 2u) {
		i0 = primitive_id * 3u;
		i1 = primitive_id * 3u + 1u;
		i2 = primitive_id * 3u + 2u;
	} else if (geom.index_address != 0ul) {
		Uint32Buffer idx = Uint32Buffer(geom.index_address);
		if (geom.index_format == 0u) {
			uint byte_off = primitive_id * 6u;
			uint word0 = idx.v[byte_off >> 2];
			uint word1 = idx.v[(byte_off >> 2) + 1u];
			if ((byte_off & 3u) == 0u) {
				i0 = word0 & 0xFFFFu;
				i1 = word0 >> 16;
				i2 = word1 & 0xFFFFu;
			} else {
				i0 = word0 >> 16;
				i1 = word1 & 0xFFFFu;
				i2 = word1 >> 16;
			}
		} else {
			i0 = idx.v[primitive_id * 3u];
			i1 = idx.v[primitive_id * 3u + 1u];
			i2 = idx.v[primitive_id * 3u + 2u];
		}
	} else {
		i0 = i1 = i2 = 0u;
	}
}

#if defined(RT_STAGE_CLOSEST_HIT) || defined(RT_STAGE_ANY_HIT) || defined(RT_STAGE_INTERSECTION)
// Convenience wrapper using gl_PrimitiveID (hit shaders only).
void get_triangle_indices(in GeometryData geom, out uint i0, out uint i1, out uint i2) {
	get_triangle_indices_ex(geom, gl_PrimitiveID, i0, i1, i2);
}
#endif

// ============================================================================
// FACING
// ============================================================================
#if defined(RT_STAGE_CLOSEST_HIT) || defined(RT_STAGE_ANY_HIT)
// Raster FRONT_FACING of a triangle hit: the winding of the triangle vertex() output, as seen from the ray origin.
bool rt_triangle_front_facing(in GeometryData geom) {
	return (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT) != ((geom.flags & FLAG_FACING_INVERTED) != 0u);
}
#endif

// The shading normal p_normal, flipped on back faces of materials raster flips it for (DO_SIDE_CHECK).
vec3 rt_side_checked_normal(in GeometryData geom, vec3 p_normal, bool p_front_facing) {
	return (!p_front_facing && (geom.flags & FLAG_SIDE_CHECK) != 0u) ? -p_normal : p_normal;
}

// ============================================================================
// POSITION FETCHING
// ============================================================================
// Object-space position of vertex p_index. Compressed positions are unorm16 within the surface AABB.
vec3 fetch_position_vertex(in GeometryData geom, uint p_index) {
	if ((geom.flags & FLAG_COMPRESSED) != 0u) {
		Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
		uint xy = vb.v[p_index * 2u];
		uint z = vb.v[p_index * 2u + 1u] & 0xFFFFu;
		vec3 unorm = vec3(float(xy & 0xFFFFu), float(xy >> 16), float(z)) / 65535.0;
		return vec3(geom.aabb_pos_x, geom.aabb_pos_y, geom.aabb_pos_z) + unorm * vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z);
	}
	FloatBuffer vb = FloatBuffer(geom.vertex_address);
	uint base = p_index * (geom.position_stride >> 2);
	return vec3(vb.v[base], vb.v[base + 1u], geom.position_stride >= 12u ? vb.v[base + 2u] : 0.0);
}

// ============================================================================
// UV FETCHING
// ============================================================================
vec2 fetch_uv_vertex(in GeometryData geom, uint byte_offset, uint scale_packed, uint index) {
	uint stride_words = geom.attribute_stride >> 2;
	uint uv_word_offset = byte_offset >> 2;

	if ((geom.flags & FLAG_COMPRESSED_UV) != 0u) {
		Uint32Buffer attributes = Uint32Buffer(geom.attribute_address);
		uint packed_uv = attributes.v[index * stride_words + uv_word_offset];
		vec2 uv = vec2(float(packed_uv & 0xFFFFu), float(packed_uv >> 16)) / 65535.0;
		vec2 scale = unpackHalf2x16(scale_packed);
		return (scale.x != 0.0 || scale.y != 0.0) ? (uv - 0.5) * scale : uv;
	}

	FloatBuffer attributes = FloatBuffer(geom.attribute_address);
	uint offset = index * stride_words + uv_word_offset;
	return vec2(attributes.v[offset], attributes.v[offset + 1u]);
}

vec2 fetch_uv_at(in GeometryData geom, uint byte_offset, uint scale_packed, uint i0, uint i1, uint i2, vec3 bary) {
	if (byte_offset == OFFSET_NONE || geom.attribute_address == 0ul) {
		return vec2(0.0);
	}

	vec2 uv0 = fetch_uv_vertex(geom, byte_offset, scale_packed, i0);
	vec2 uv1 = fetch_uv_vertex(geom, byte_offset, scale_packed, i1);
	vec2 uv2 = fetch_uv_vertex(geom, byte_offset, scale_packed, i2);

	return bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
}

vec2 fetch_uv(in GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	return fetch_uv_at(geom, geom.uv_byte_offset, geom.uv_scale_packed, i0, i1, i2, bary);
}

vec2 fetch_uv2(in GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	return fetch_uv_at(geom, geom.uv2_byte_offset, geom.uv2_scale_packed, i0, i1, i2, bary);
}

// ============================================================================
// VERTEX COLOR FETCHING (RGBA8 packed in uint32)
// ============================================================================
vec4 fetch_color(in GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	if (geom.color_byte_offset == OFFSET_NONE || geom.attribute_address == 0ul) {
		return vec4(1.0);
	}

	Uint32Buffer attr = Uint32Buffer(geom.attribute_address);
	uint stride_words = geom.attribute_stride >> 2;
	uint color_word_off = geom.color_byte_offset >> 2;

	vec4 col0 = unpackUnorm4x8(attr.v[i0 * stride_words + color_word_off]);
	vec4 col1 = unpackUnorm4x8(attr.v[i1 * stride_words + color_word_off]);
	vec4 col2 = unpackUnorm4x8(attr.v[i2 * stride_words + color_word_off]);

	return bary.x * col0 + bary.y * col1 + bary.z * col2;
}

// ============================================================================
// TBN FETCHING
// ============================================================================
void axis_angle_to_tbn(vec3 axis, float angle, out vec3 tangent, out vec3 binormal, out vec3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	vec3 omc_axis = (1.0 - c) * axis;
	vec3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + vec3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
}

TBNResult fetch_tbn_compressed_vertex(in GeometryData geom, uint idx) {
	TBNResult result;
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);

	uint norm_base = geom.normal_byte_offset >> 2;
	uint norm_stride = geom.normal_stride >> 2;
	uint n_packed = vb.v[norm_base + idx * norm_stride];
	vec2 axis_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16)) / 65535.0 * 2.0 - 1.0;
	vec3 axis = oct_to_vec3(axis_oct);

	uint pos_word1 = vb.v[idx * 2u + 1u];
	float angle_raw = float(pos_word1 >> 16) / 65535.0;

	result.bitangent_sign = angle_raw > 0.5 ? 1.0 : -1.0;
	float angle = abs(angle_raw * 2.0 - 1.0) * PI;

	axis_angle_to_tbn(axis, angle, result.tangent, result.binormal, result.normal);
	result.binormal *= result.bitangent_sign;

	return result;
}

TBNResult fetch_tbn_uncompressed_vertex(in GeometryData geom, uint idx) {
	TBNResult result;
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);

	uint norm_base = geom.normal_byte_offset >> 2;
	uint norm_stride = geom.normal_stride >> 2;

	uint n_packed = vb.v[norm_base + idx * norm_stride];
	vec2 n_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16)) / 65535.0 * 2.0 - 1.0;
	result.normal = oct_to_vec3(n_oct);

	if (geom.normal_stride >= 8u) {
		uint t_packed = vb.v[norm_base + idx * norm_stride + 1u];
		vec2 t_raw = vec2(float(t_packed & 0xFFFFu), float(t_packed >> 16)) / 65535.0 * 2.0 - 1.0;

		vec2 t_oct = vec2(t_raw.x, abs(t_raw.y) * 2.0 - 1.0);
		result.tangent = oct_to_vec3(t_oct);
		result.bitangent_sign = sign(t_raw.y);
		result.binormal = normalize(cross(result.normal, result.tangent) * result.bitangent_sign);
	} else {
		result.tangent = vec3(1.0, 0.0, 0.0);
		result.binormal = vec3(0.0, 0.0, 1.0);
		result.bitangent_sign = 1.0;
	}

	return result;
}

TBNResult fetch_tbn(in GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	TBNResult result;
	result.normal = vec3(0.0, 1.0, 0.0);
	result.tangent = vec3(1.0, 0.0, 0.0);
	result.binormal = vec3(0.0, 0.0, 1.0);
	result.bitangent_sign = 1.0;
	result.normal_delta1 = vec3(0.0);
	result.normal_delta2 = vec3(0.0);

	if (geom.normal_byte_offset == OFFSET_NONE || geom.vertex_address == 0ul) {
		return result;
	}

	bool compressed = (geom.flags & FLAG_COMPRESSED) != 0u;

	TBNResult t0, t1, t2;
	if (compressed) {
		t0 = fetch_tbn_compressed_vertex(geom, i0);
		t1 = fetch_tbn_compressed_vertex(geom, i1);
		t2 = fetch_tbn_compressed_vertex(geom, i2);
	} else {
		t0 = fetch_tbn_uncompressed_vertex(geom, i0);
		t1 = fetch_tbn_uncompressed_vertex(geom, i1);
		t2 = fetch_tbn_uncompressed_vertex(geom, i2);
	}

	result.normal_delta1 = t1.normal - t0.normal;
	result.normal_delta2 = t2.normal - t0.normal;
	result.normal = normalize(bary.x * t0.normal + bary.y * t1.normal + bary.z * t2.normal);
	result.tangent = normalize(bary.x * t0.tangent + bary.y * t1.tangent + bary.z * t2.tangent);
	result.binormal = normalize(bary.x * t0.binormal + bary.y * t1.binormal + bary.z * t2.binormal);
	result.bitangent_sign = sign(bary.x * t0.bitangent_sign + bary.y * t1.bitangent_sign + bary.z * t2.bitangent_sign);

	return result;
}

// ============================================================================
// VERTEX ATTRIBUTE FETCHING (requires hitAttributeEXT vec2 attribs to be declared)
// ============================================================================
#define FETCH_UV (1u << 0)
#define FETCH_TBN (1u << 1)
#define FETCH_COLOR (1u << 2)
#define FETCH_UV2 (1u << 3)
#define FETCH_ALL (FETCH_UV | FETCH_UV2 | FETCH_TBN | FETCH_COLOR)

#ifdef RT_HIT_ATTRIBS_DECLARED
VertexAttributes fetch_vertex_attributes(in GeometryData geom, vec2 hit_attribs, uint fetch_flags) {
	VertexAttributes attrs;
	attrs.uv = vec2(0.0);
	attrs.uv_edge1 = vec2(0.0);
	attrs.uv_edge2 = vec2(0.0);
	attrs.uv2 = vec2(0.0);
	attrs.normal = vec3(0.0, 1.0, 0.0);
	attrs.tangent = vec3(1.0, 0.0, 0.0);
	attrs.bitangent_sign = 1.0;
	attrs.color = vec4(1.0);
	attrs.normal_delta1 = vec3(0.0);
	attrs.normal_delta2 = vec3(0.0);
	attrs.has_uv = false;
	attrs.has_uv2 = false;
	attrs.has_normal = false;
	attrs.has_tangent = false;
	attrs.has_color = false;

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	attrs.indices = uvec3(i0, i1, i2);

	vec3 bary = vec3(1.0 - hit_attribs.x - hit_attribs.y, hit_attribs.x, hit_attribs.y);

	if ((fetch_flags & FETCH_UV) != 0u && geom.uv_byte_offset != OFFSET_NONE && geom.attribute_address != 0ul) {
		vec2 uv0 = fetch_uv_vertex(geom, geom.uv_byte_offset, geom.uv_scale_packed, i0);
		attrs.uv_edge1 = fetch_uv_vertex(geom, geom.uv_byte_offset, geom.uv_scale_packed, i1) - uv0;
		attrs.uv_edge2 = fetch_uv_vertex(geom, geom.uv_byte_offset, geom.uv_scale_packed, i2) - uv0;
		attrs.uv = uv0 + bary.y * attrs.uv_edge1 + bary.z * attrs.uv_edge2;
		attrs.has_uv = true;
	}

	if ((fetch_flags & FETCH_UV2) != 0u && geom.uv2_byte_offset != OFFSET_NONE) {
		attrs.uv2 = fetch_uv2(geom, i0, i1, i2, bary);
		attrs.has_uv2 = true;
	}

	if ((fetch_flags & FETCH_COLOR) != 0u && geom.color_byte_offset != OFFSET_NONE) {
		attrs.color = fetch_color(geom, i0, i1, i2, bary);
		attrs.has_color = true;
	}

	if ((fetch_flags & FETCH_TBN) != 0u && geom.normal_byte_offset != OFFSET_NONE) {
		TBNResult tbn = fetch_tbn(geom, i0, i1, i2, bary);
		attrs.normal = tbn.normal;
		attrs.tangent = tbn.tangent;
		attrs.bitangent_sign = tbn.bitangent_sign;
		attrs.normal_delta1 = tbn.normal_delta1;
		attrs.normal_delta2 = tbn.normal_delta2;
		attrs.has_normal = true;
		attrs.has_tangent = true;
	}

	return attrs;
}
#endif
