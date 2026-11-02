// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0, bindless_textures[].
// Note: raytracing_shadow_query_inc.glsl requires GL_EXT_ray_query.

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 64 bytes, std430)
// ============================================================================

#define RT_LIGHT_NO_PROJECTOR 0u // Bindless default white texture.

// Bitfield words of C++ RT_LightData; see it for the layout. Positional-only fields are zero for directional lights.
struct RTLightPacked {
	vec3 position; // World pos (omni/spot) or normalized direction (directional).
	float range; // Omni/spot range, 0 = infinite.
	uint emission_rg; // fp16 r, g.
	uint emission_b_radius; // fp16 b, radius.
	uint energy; // fp16 specular, indirect.
	uint masks; // cull_mask : 20, type : 2, unused : 2, shadow_ray_mask : 8.
	uint attenuation_shadow; // fp16 attenuation, shadow opacity.
	uint spot; // fp16 1 - cos(spot angle), 1 / spot attenuation.
	uint orientation_xy; // snorm16 quaternion x, y.
	uint orientation_zw; // snorm16 quaternion z, w.
	uint projector_texture; // index : 24, unused : 8.
	uint reserved0; // Free for future fields, keeps the 64-byte stride.
	uint reserved1;
	uint reserved2;
};

struct RTLightData {
	vec3 position; // World pos (omni/spot) or direction (directional).
	uint type; // RT_LIGHT_TYPE_*.
	vec3 emission; // HDR emission color (color * energy).
	float radius; // Light size (omni/spot) or angular radius (directional).
	float attenuation; // Attenuation exponent (2=inverse-square, 1=linear).
	float inv_max_range; // 1/range (-1 = infinite).
	float max_range_squared; // range^2 (0 = infinite).
	float specular_amount; // Godot specular multiplier [0..1].
	float indirect_energy; // Godot indirect energy multiplier.
	float shadow_opacity; // 0 skips the shadow ray; otherwise blends traced transmittance towards 1.
	float one_minus_cos_spot; // 1 - cosine of the spot cone half-angle.
	float inv_spot_attenuation; // Spot cone softness.
	uint light_masks; // Bits 0-19: light_cull_mask (lighting test). Bits 24-31: TLAS shadow-ray mask.
	uint projector_texture; // Bindless sRGB index, RT_LIGHT_NO_PROJECTOR when unset.
	vec4 orientation; // Unit rotation quaternion (omni/spot); the spot cone points along local -Z.
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightPacked rt_light_buffer[];
};

layout(set = 0, binding = 40) uniform sampler rt_light_projector_sampler;

uint lights_get_type(uint idx) {
	return bitfieldExtract(rt_light_buffer[idx].masks, 20, 2);
}

RTLightData lights_get(uint idx) {
	RTLightPacked p = rt_light_buffer[idx];
	RTLightData light;
	light.position = p.position;
	light.type = bitfieldExtract(p.masks, 20, 2);
	vec2 emission_b_radius = unpackHalf2x16(p.emission_b_radius);
	light.emission = vec3(unpackHalf2x16(p.emission_rg), emission_b_radius.x);
	light.radius = emission_b_radius.y;
	vec2 energy = unpackHalf2x16(p.energy);
	light.specular_amount = energy.x;
	light.indirect_energy = energy.y;
	vec2 attenuation_shadow = unpackHalf2x16(p.attenuation_shadow);
	light.attenuation = attenuation_shadow.x;
	light.shadow_opacity = attenuation_shadow.y;
	light.inv_max_range = p.range > 0.0 ? 1.0 / p.range : -1.0;
	light.max_range_squared = p.range * p.range;
	vec2 spot = unpackHalf2x16(p.spot);
	light.one_minus_cos_spot = spot.x;
	light.inv_spot_attenuation = spot.y;
	light.light_masks = p.masks;
	light.projector_texture = bitfieldExtract(p.projector_texture, 0, 24);
	vec4 q = vec4(unpackSnorm2x16(p.orientation_xy), unpackSnorm2x16(p.orientation_zw));
	light.orientation = q * inversesqrt(max(dot(q, q), 1e-8));
	return light;
}

// Rotates p_v by the unit quaternion p_q.
vec3 lights_rotate(vec4 p_q, vec3 p_v) {
	return p_v + 2.0 * cross(p_q.xyz, cross(p_q.xyz, p_v) + p_q.w * p_v);
}

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	// Cone axis.
	vec3 sphere_axis = to_light * inv_dist;
	vec3 dir_axis = -normalize(light.position);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

// ============================================================================
// Attenuation
// ============================================================================

// Godot-style windowed distance attenuation, identical to the rasterizer's get_omni_attenuation().
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay). Not clamped: lights closer than
// 1 unit must brighten past 1.0, which small-range fixture lights rely on.
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = pow(max(distance, 0.0001), -decay);

	// Windowed falloff if range is finite (inv_max_range >= 0).
	if (inv_max_range >= 0.0) {
		float nd = distance * inv_max_range;
		nd *= nd;
		nd *= nd; // nd^4
		nd = max(1.0 - nd, 0.0);
		nd *= nd; // nd^2 window
		return atten * nd;
	}

	return atten;
}

// Per-light specular multiplier.
float lights_get_specular_multiplier(float specular_amount, float roughness) {
	if (specular_amount >= 0.0) {
		return specular_amount;
	} else {
		float r3 = roughness * roughness * roughness;
		return mix(0.0, r3, -specular_amount);
	}
}

// Projector tint for light-to-surface direction p_dir, using the rasterizer's mapping. 1.0 without a projector.
vec3 lights_projector_color(RTLightData light, vec3 p_dir) {
	if (light.projector_texture == RT_LIGHT_NO_PROJECTOR) {
		return vec3(1.0);
	}
	vec3 local = lights_rotate(vec4(-light.orientation.xyz, light.orientation.w), p_dir);
	vec2 uv;
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		// Perspective onto [-1, 1] along -Z with the rasterizer's fov = 2 * spot angle, clamped below 90 degrees.
		// The texture's V axis points down.
		float omc = light.one_minus_cos_spot;
		float cot_spot = max(1.0 - omc, 0.0017453) * inversesqrt(max(omc * (2.0 - omc), 1e-8));
		vec2 ndc = local.xy * cot_spot / max(-local.z, 1e-4);
		uv = vec2(ndc.x, -ndc.y) * 0.5 + 0.5;
	} else {
		// Equirectangular panorama, as the rasterizer's decal atlas converts it to dual paraboloid: -Z at u = 0, down at v = 0.
		uv = vec2(fract(atan(-local.x, -local.z) * (0.5 / PI)), acos(clamp(-local.y, -1.0, 1.0)) * (1.0 / PI));
	}
	vec4 projector = textureLod(sampler2D(bindless_textures[nonuniformEXT(light.projector_texture)], rt_light_projector_sampler), uv, 0.0);
	return projector.rgb * projector.a;
}

#include "raytracing_shadow_query_inc.glsl"

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

// Returns colored light transmittance.
// Uses SkipClosestHitShader so only any_hit (alpha test) and miss are invoked.
// TerminateOnFirstHit exits on the first opaque or stochastic alpha blocker.
// p_shadow_mask is the light's 8-bit shadow-caster group mask.
vec3 lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, uint p_shadow_mask, inout uint rng_state) {
#ifdef USE_RAY_QUERY_SHADOWS
	// Ray queries are faster but approximate custom or animated material outputs
	// with a direct albedo texture sample.
	return ray_query_shadow_transmittance(origin, direction, 0.001, max_dist - 0.001, p_shadow_mask, 0.0, rng_state);
#elif defined(USE_SER)
	PathPayload saved_payload = payload;
	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	shadow_ps = path_unpack(payload);
	rng_state = shadow_ps.rng_state;
	payload = saved_payload;
	return hitObjectIsHitEXT(hitObject) ? vec3(0.0) : shadow_ps.throughput;
#else
	// The miss shader copies accumulated transmittance to radiance.
	// If an opaque hit occurs, miss is never called and radiance stays vec3(0.0).
	// Save full payload, set up shadow ray, then restore after trace.
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	// Unpack the colored visibility written by the miss shader.
	shadow_ps = path_unpack(payload);
	vec3 transmittance = shadow_ps.radiance;
	rng_state = shadow_ps.rng_state;

	payload = saved_payload;

	return transmittance;
#endif
}

// Shadow transmittance from origin towards the light, blended towards 1 by the light's shadow opacity.
// Skips the trace when the path tracer draws no shadow for this light. p_receiver_shadow_mask filters the light's caster groups.
vec3 lights_shadow_transmittance(RTLightData light, vec3 origin, vec3 direction, float max_dist, uint p_receiver_shadow_mask, inout uint rng_state) {
	if (light.shadow_opacity <= 0.001) {
		return vec3(1.0);
	}
	vec3 traced = lights_trace_shadow_ray(origin, direction, max_dist, (light.light_masks >> 24u) & p_receiver_shadow_mask, rng_state);
	return mix(vec3(1.0), traced, light.shadow_opacity);
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

// gather_lights() packs every directional light before the positional ones.
uint lights_directional_count(uint light_count) {
	uint count = 0u;
	while (count < light_count && lights_get_type(count) == RT_LIGHT_TYPE_DIRECTIONAL) {
		count++;
	}
	return count;
}

// Mini-batch reservoir selection over rt_light_buffer[first, first + count): draws k
// candidates and keeps one valid light uniformly. Returns false if none is valid.
bool lights_select(vec3 hit_pos, uint first, uint count, uint instance_layers, inout uint rng_state,
		out uint r_idx, out float r_select_pdf) {
	r_idx = first;
	r_select_pdf = 1.0;
	if (count == 0u) {
		return false;
	}

	const uint k = min(uint(RT_LIGHT_RESERVOIR_SIZE), count);
	uint valid_found = 0u;

	for (uint i = 0u; i < k; i++) {
		uint idx = first + min(uint(rand(rng_state) * float(count)), count - 1u);
		RTLightData test_light = lights_get(idx);

		// light_cull_mask: the light only affects intersecting layers.
		bool is_valid = (test_light.light_masks & instance_layers & 0xFFFFFu) != 0u;

		// Range check for positional lights.
		bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
		if (is_valid && is_positional) {
			vec3 to_l = test_light.position - hit_pos;
			float d2 = dot(to_l, to_l);
			is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
		}

		if (is_valid) {
			valid_found++;
			if (rand(rng_state) < 1.0 / float(valid_found)) {
				r_idx = idx;
			}
		}
	}

	if (valid_found == 0u) {
		return false;
	}

	// Estimate valid count from sample ratio, PDF = 1/validCount.
	float valid_count_estimate = float(count) * (float(valid_found) / float(k));
	r_select_pdf = 1.0 / max(valid_count_estimate, 1.0);
	return true;
}

// Unweighted contribution of one omni or spot light, one shadow ray.
vec3 lights_evaluate_positional(RTLightData light, vec3 hit_pos, vec3 N, vec3 V, MaterialProperties material,
		inout uint rng_state, bool is_indirect_bounce, uint p_receiver_shadow_mask) {
	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);

	// Early out: outside max range.
	if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
		return vec3(0.0);
	}

	float dist = sqrt(dist_sq);
	vec3 L;
	float shadow_dist;

	if (light.radius <= 0.01) {
		// True point light: exact direction.
		L = to_light / max(dist, 0.0001);
		shadow_dist = dist;
	} else {
		// Sphere light: cone sampling for soft shadows.
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		L = lights_sample_cone(ls, rand2(rng_state), light_pdf);
		float t_center = dot(to_light, L);
		vec3 perp = to_light - t_center * L;
		float perp_sq = dot(perp, perp);
		float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
		shadow_dist = max(0.0, t_center - dt);
	}

	// Spot cone early-out.
	float spot_atten = 1.0;
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float one_minus_scos = 1.0 + dot(L, lights_rotate(light.orientation, vec3(0.0, 0.0, -1.0)));
		if (one_minus_scos >= light.one_minus_cos_spot) {
			return vec3(0.0);
		}
		float spot_rim = max(1e-4, one_minus_scos / light.one_minus_cos_spot);
		spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
	}

	// From the light center, as in raster, so area lights keep a sharp projection.
	vec3 projector_color = lights_projector_color(light, -to_light / max(dist, 0.0001));
	if (all(lessThanEqual(projector_color, vec3(0.0)))) {
		return vec3(0.0);
	}

	vec3 shadow_transmittance = lights_shadow_transmittance(light, hit_pos, L, shadow_dist, p_receiver_shadow_mask, rng_state);

	// Evaluate BRDF (diffuse incl. backlight, specular separately for specular_amount control).
	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

	// Distance attenuation.
	LightSample ls_atten;
	ls_atten.distance_sq = dist_sq;
	float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

	float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
	vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

	float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

	// NdotL is already included in brdf_value.
	return brdf_value * light.emission * projector_color * shadow_transmittance * atten * indirect_mul;
}

// Unweighted contribution of one directional light, one shadow ray.
vec3 lights_evaluate_directional(RTLightData light, vec3 hit_pos, vec3 N, vec3 V, MaterialProperties material,
		inout uint rng_state, bool is_indirect_bounce, uint p_receiver_shadow_mask) {
	LightSample ls = lights_prepare_sample(hit_pos, light);
	float light_pdf;
	vec3 L = lights_sample_cone(ls, rand2(rng_state), light_pdf);

	if (dot(N, L) <= 0.0 && !godotShadesBackfacingLight(material)) {
		return vec3(0.0);
	}

	vec3 shadow_transmittance = lights_shadow_transmittance(light, hit_pos, L, ls.max_distance, p_receiver_shadow_mask, rng_state);

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

	float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
	vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

	float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

	return brdf_value * light.emission * shadow_transmittance * indirect_mul;
}

// NEE with one shadow ray to a single light selected among all lights.
vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint instance_layers,
		uint p_receiver_shadow_mask) {
	uint idx;
	float select_pdf;
	if (!lights_select(hit_pos, 0u, light_count, instance_layers, rng_state, idx, select_pdf)) {
		return vec3(0.0);
	}

	RTLightData light = lights_get(idx);
	vec3 contribution;
	if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		contribution = lights_evaluate_directional(light, hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask);
	} else {
		contribution = lights_evaluate_positional(light, hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask);
	}
	return contribution / max(select_pdf, 1e-10);
}

// NEE with up to two shadow rays: one to a directional light and one to a
// positional light, each selected within its own group.
vec3 lights_evaluate_direct_lighting_per_group(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint instance_layers,
		uint p_receiver_shadow_mask) {
	vec3 result = vec3(0.0);
	uint directional_count = lights_directional_count(light_count);

	uint idx;
	float select_pdf;
	if (lights_select(hit_pos, 0u, directional_count, instance_layers, rng_state, idx, select_pdf)) {
		vec3 contribution = lights_evaluate_directional(lights_get(idx), hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask);
		result += contribution / max(select_pdf, 1e-10);
	}
	if (lights_select(hit_pos, directional_count, light_count - directional_count, instance_layers, rng_state, idx, select_pdf)) {
		vec3 contribution = lights_evaluate_positional(lights_get(idx), hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask);
		result += contribution / max(select_pdf, 1e-10);
	}
	return result;
}
