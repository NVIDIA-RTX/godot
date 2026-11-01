// Path tracer vertex capture: stores the final vertex() output of every point into the capture arena,
// in the deformed-mesh layout the ray tracing hit shaders decode (float3 positions, then packed TBN).

// Mirrors RTVertexCapture::DrawParams.
struct RTCaptureDraw {
	vec4 view_to_local[3];
	uint position_offset; // In uints.
	uint tbn_offset; // In uints.
	uint vertex_count;
	uint flags;
	vec4 dead_position; // xyz in capture space.
};

#define RT_CAPTURE_FLAG_NORMAL (1u << 0)
#define RT_CAPTURE_FLAG_TANGENT (1u << 1)

// Indexed by draw_call.instance_index so the push constant layout stays untouched.
layout(set = RT_CAPTURE_UNIFORM_SET, binding = 0, std430) restrict readonly buffer RTCaptureParams {
	RTCaptureDraw data[];
}
rt_capture_params;

layout(set = RT_CAPTURE_UNIFORM_SET, binding = 1, std430) restrict writeonly buffer RTCaptureOutput {
	uint data[];
}
rt_capture_output;

uint rt_capture_pack_unorm2(vec2 p_value) {
	uvec2 v = uvec2(clamp(ivec2(p_value * 65535.0), ivec2(0), ivec2(0xFFFF)));
	return v.x | (v.y << 16);
}

vec2 rt_capture_sign_not_zero(vec2 p_v) {
	return mix(vec2(-1.0), vec2(1.0), greaterThanEqual(p_v, vec2(0.0)));
}

// Octahedral encoding in [0, 1], matching the mesh vertex format.
vec2 rt_capture_vec3_to_oct(vec3 p_n) {
	vec2 o = p_n.xy / (abs(p_n.x) + abs(p_n.y) + abs(p_n.z));
	o = p_n.z >= 0.0 ? o : (1.0 - abs(o.yx)) * rt_capture_sign_not_zero(o);
	return o * 0.5 + 0.5;
}

// Tangent octahedral encoding with the binormal sign folded into y, matching the mesh vertex format.
vec2 rt_capture_tangent_to_oct(vec3 p_t, float p_sign) {
	vec2 o = rt_capture_vec3_to_oct(p_t);
	o.y = o.y * 0.5 + 0.5;
	o.y = p_sign >= 0.0 ? o.y : 1.0 - o.y;
	return o;
}

vec3 rt_capture_transform_dir(RTCaptureDraw p_draw, vec3 p_dir) {
	return normalize(vec3(dot(p_draw.view_to_local[0].xyz, p_dir), dot(p_draw.view_to_local[1].xyz, p_dir), dot(p_draw.view_to_local[2].xyz, p_dir)));
}

bool rt_capture_is_non_finite(vec3 p_v) {
	return any(isinf(p_v)) || any(isnan(p_v));
}

// p_culled is set for instances the particle system hid with a non-finite transform. Their points collapse to
// dead_position: Vulkan requires primitives active at BLAS build time to stay active on refit, so they can't be NaN.
void rt_capture_store(vec3 p_vertex, vec3 p_normal, vec3 p_tangent, vec3 p_binormal, bool p_culled) {
	RTCaptureDraw draw = rt_capture_params.data[draw_call.instance_index];
	uint idx = uint(INSTANCE_INDEX) * draw.vertex_count + uint(VERTEX_INDEX);

	vec4 v = vec4(p_vertex, 1.0);
	vec3 local = vec3(dot(draw.view_to_local[0], v), dot(draw.view_to_local[1], v), dot(draw.view_to_local[2], v));
	if (p_culled || rt_capture_is_non_finite(local)) {
		local = draw.dead_position.xyz;
	}
	uint pos = draw.position_offset + idx * 3u;
	rt_capture_output.data[pos + 0u] = floatBitsToUint(local.x);
	rt_capture_output.data[pos + 1u] = floatBitsToUint(local.y);
	rt_capture_output.data[pos + 2u] = floatBitsToUint(local.z);

	if (bool(draw.flags & RT_CAPTURE_FLAG_NORMAL)) {
		bool has_tangent = bool(draw.flags & RT_CAPTURE_FLAG_TANGENT);
		uint tbn = draw.tbn_offset + idx * (has_tangent ? 2u : 1u);
		vec3 n = rt_capture_transform_dir(draw, p_normal);
		rt_capture_output.data[tbn] = rt_capture_pack_unorm2(rt_capture_vec3_to_oct(n));
		if (has_tangent) {
			vec3 t = rt_capture_transform_dir(draw, p_tangent);
			vec3 b = rt_capture_transform_dir(draw, p_binormal);
			float binormal_sign = dot(cross(n, t), b) >= 0.0 ? 1.0 : -1.0;
			rt_capture_output.data[tbn + 1u] = rt_capture_pack_unorm2(rt_capture_tangent_to_oct(t, binormal_sign));
		}
	}

	gl_PointSize = 1.0;
}
