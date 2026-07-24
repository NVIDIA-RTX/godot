// Shared utilities for hit shaders (closest_hit, any_hit)
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, oct_inc.glsl, raytracing_inc.glsl

#include "raytracing_data_inc.glsl"

// ============================================================================
// VERTEX ATTRIBUTES
// ============================================================================
struct VertexAttributes {
	vec2 uv;
	vec3 normal;
	vec3 tangent;
	float bitangent_sign;
	vec4 color;
	bool has_uv;
	bool has_normal;
	bool has_tangent;
	bool has_color;
};

struct TBNResult {
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	float bitangent_sign;
};

// ============================================================================
// TRIANGLE INDEX FETCHING
// ============================================================================

/// Core implementation taking an explicit primitive ID (works in any shader stage).
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

/// Convenience wrapper using gl_PrimitiveID (hit shaders only).
void get_triangle_indices(in GeometryData geom, out uint i0, out uint i1, out uint i2) {
	get_triangle_indices_ex(geom, gl_PrimitiveID, i0, i1, i2);
}

// ============================================================================
// UV FETCHING
// ============================================================================
vec2 fetch_uv(in GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	if (geom.uv_byte_offset == OFFSET_NONE || geom.attribute_address == 0ul) {
		return vec2(0.0);
	}

	uint stride_words = geom.attribute_stride >> 2;
	uint uv_word_off = geom.uv_byte_offset >> 2;
	bool compressed = (geom.flags & FLAG_COMPRESSED) != 0u;

	vec2 uv0, uv1, uv2;

	if (compressed) {
		Uint32Buffer attr = Uint32Buffer(geom.attribute_address);
		uint p0 = attr.v[i0 * stride_words + uv_word_off];
		uint p1 = attr.v[i1 * stride_words + uv_word_off];
		uint p2 = attr.v[i2 * stride_words + uv_word_off];

		uv0 = vec2(float(p0 & 0xFFFFu), float(p0 >> 16)) / 65535.0;
		uv1 = vec2(float(p1 & 0xFFFFu), float(p1 >> 16)) / 65535.0;
		uv2 = vec2(float(p2 & 0xFFFFu), float(p2 >> 16)) / 65535.0;

		vec2 scale = unpackHalf2x16(geom.uv_scale_packed);
		if (scale.x != 0.0 || scale.y != 0.0) {
			uv0 = (uv0 - 0.5) * scale;
			uv1 = (uv1 - 0.5) * scale;
			uv2 = (uv2 - 0.5) * scale;
		}
	} else {
		FloatBuffer attr_f = FloatBuffer(geom.attribute_address);
		uint stride_floats = geom.attribute_stride >> 2;
		uint uv_float_off = geom.uv_byte_offset >> 2;

		uv0 = vec2(attr_f.v[i0 * stride_floats + uv_float_off],
				attr_f.v[i0 * stride_floats + uv_float_off + 1u]);
		uv1 = vec2(attr_f.v[i1 * stride_floats + uv_float_off],
				attr_f.v[i1 * stride_floats + uv_float_off + 1u]);
		uv2 = vec2(attr_f.v[i2 * stride_floats + uv_float_off],
				attr_f.v[i2 * stride_floats + uv_float_off + 1u]);
	}

	return bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
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
#define FETCH_ALL (FETCH_UV | FETCH_TBN | FETCH_COLOR)

#ifdef RT_HIT_ATTRIBS_DECLARED
VertexAttributes fetch_vertex_attributes(in GeometryData geom, vec2 hit_attribs, uint fetch_flags) {
	VertexAttributes attrs;
	attrs.uv = vec2(0.0);
	attrs.normal = vec3(0.0, 1.0, 0.0);
	attrs.tangent = vec3(1.0, 0.0, 0.0);
	attrs.bitangent_sign = 1.0;
	attrs.color = vec4(1.0);
	attrs.has_uv = false;
	attrs.has_normal = false;
	attrs.has_tangent = false;
	attrs.has_color = false;

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);

	vec3 bary = vec3(1.0 - hit_attribs.x - hit_attribs.y, hit_attribs.x, hit_attribs.y);

	if ((fetch_flags & FETCH_UV) != 0u && geom.uv_byte_offset != OFFSET_NONE) {
		attrs.uv = fetch_uv(geom, i0, i1, i2, bary);
		attrs.has_uv = true;
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
		attrs.has_normal = true;
		attrs.has_tangent = true;
	}

	return attrs;
}
#endif
