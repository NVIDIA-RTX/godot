#[raygen]

#version 460

#extension GL_EXT_control_flow_attributes : enable

#VERSION_DEFINES

#ifdef RT_TIMING_ENABLED
#extension GL_EXT_shader_realtime_clock : require
layout(set = 0, binding = 34, r32f) uniform writeonly image2D timing_image;

void store_path_timing(ivec2 pixel, uvec2 start_clock) {
	uvec2 end_clock = clockRealtime2x32EXT();
	// Subtract before converting to float, including the low-word borrow.
	// Device clocks remain coherent across trace calls and shader execution reordering.
	uvec2 elapsed = uvec2(end_clock.x - start_clock.x,
			end_clock.y - start_clock.y - uint(end_clock.x < start_clock.x));
	float ticks = float(elapsed.y) * 4294967296.0 + float(elapsed.x);
	imageStore(timing_image, pixel, vec4(ticks));
}
#endif

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
// clang-format on

#pragma shader_stage(raygen)
#extension GL_EXT_ray_tracing : enable
#ifdef USE_SER
#extension GL_EXT_shader_invocation_reorder : enable
#endif

#define GLSL 1
#define RT_STAGE_RAYGEN 1
#include "raytracing_common_inc.glsl"
#include "raytracing_samplers_inc.glsl"

layout(set = 0, binding = 0, rgba32f) uniform image2D image;
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 31) uniform accelerationStructureEXT tlas_transparent;
layout(set = 0, binding = 29) uniform texture2D prepass_depth_texture;
layout(set = 0, binding = 30) uniform texture2D prepass_color_texture;

layout(location = 0) rayPayloadEXT PathPayload payload;

struct ContinuationReservoir {
	PathState selected;
	float selected_weight;
	float total_weight;
	bool has_selection;
};

ContinuationReservoir continuation_reservoir_initial() {
	ContinuationReservoir r;
	r.selected_weight = 0.0;
	r.total_weight = 0.0;
	r.has_selection = false;
	return r;
}

void continuation_reservoir_add(inout ContinuationReservoir r, PathState candidate, inout uint rng_state) {
	if (is_path_terminated(candidate.packed_bounces_flags)) {
		return;
	}
	float weight = max(candidate.throughput.x, max(candidate.throughput.y, candidate.throughput.z));
	if (weight <= 1e-6) {
		return;
	}
	r.total_weight += weight;
	if (!r.has_selection || rand(rng_state) < weight / r.total_weight) {
		r.selected = candidate;
		r.selected_weight = weight;
		r.has_selection = true;
	}
}

// Russian roulette thresholds on the strongest throughput channel, per bounce; the last entry covers
// deeper bounces. The first bounce carries most of the visible indirect light, so it is cut least.
const float RT_RUSSIAN_ROULETTE_THRESHOLDS[2] = float[](0.1, 0.25);

// p_next_bounce is the segment about to be traced, 1 for the first bounce. Survivors are scaled back
// up to the threshold, never above it, so they cannot become fireflies.
bool russian_roulette_survives(uint p_next_bounce, inout PathState ps) {
	uint threshold_idx = min(p_next_bounce, uint(RT_RUSSIAN_ROULETTE_THRESHOLDS.length())) - 1u;
	float survival = max(ps.throughput.x, max(ps.throughput.y, ps.throughput.z)) / RT_RUSSIAN_ROULETTE_THRESHOLDS[threshold_idx];
	if (survival >= 1.0) {
		return true;
	}
	if (rand(ps.rng_state) >= survival) {
		return false;
	}
	ps.throughput /= survival;
	return true;
}

#ifdef DLSS_RR_ENABLED
// Maximum number of mirrors followed, including the primary one.
#define DLSS_RR_REPLACEMENT_MAX_MIRRORS 2u

// Fallback primary surface replacement, used when the path's own next hit couldn't provide it.
// Follows the mirror chain and writes the motion and albedo guides of the surface seen through it.
// The primary mirror isn't retraced; its normal is read back from the guides.
void dlss_rr_replace_primary_surface(vec3 p_origin, vec3 p_dir, float p_hit_t) {
	vec3 own_normal = imageLoad(dlss_rr_normal_roughness, ivec2(gl_LaunchIDEXT.xy)).xyz;
	mat3 image_xform = dlss_rr_householder(own_normal); // World to mirror-image space, linear part.
	float scene_length = p_hit_t;
	vec3 mirror_throughput = vec3(1.0); // Specular albedos of the mirrors after the primary one.
	vec3 ray_dir = reflect(p_dir, own_normal);
	vec3 ray_origin = offset_ray_origin(p_origin + p_dir * p_hit_t, ray_dir);

	PathState gs;
	gs.radiance = vec3(0.0);
	gs.throughput = vec3(0.0);
	gs.rng_state = 0u;
	gs.offset_normal = vec3(0.0, 0.0, 1.0);
	gs.next_ray_dir = vec3(0.0, 0.0, 1.0);
	for (uint mirrors = 1u; mirrors <= DLSS_RR_REPLACEMENT_MAX_MIRRORS; mirrors++) {
		// One bounce and no SAMPLE_ZERO keep the primary depth/velocity/guide writes away.
		gs.packed_bounces_flags = inc_total_bounce(0u) | GUIDE_RAY_FLAG |
				(mirrors == DLSS_RR_REPLACEMENT_MAX_MIRRORS ? GUIDE_LAST_FLAG : 0u);
		gs.hit_t = -1.0;
		path_pack(payload, gs);
		traceRayEXT(tlas, RT_RAY_FLAGS, RT_VIS_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
		if (payload.hit_t < 0.0 || (payload.packed_bounces_flags & GUIDE_RESULT_FLAG) == 0u) {
			return;
		}
		PathState result = path_unpack(payload);
		scene_length += result.hit_t;
		vec3 hit_diffuse = unpackUnorm4x8(payload.packed_rt[2]).rgb;
		vec3 hit_specular = unpackUnorm4x8(payload.rng_state).rgb;
		if ((result.packed_bounces_flags & GUIDE_MIRROR_FLAG) != 0u) {
			vec3 mirror_axis = ray_dir - result.next_ray_dir;
			if (dot(mirror_axis, mirror_axis) < 1e-10) {
				return;
			}
			mirror_throughput *= hit_specular;
			image_xform = image_xform * dlss_rr_householder(normalize(mirror_axis));
			ray_origin = offset_ray_origin(ray_origin + ray_dir * result.hit_t, result.offset_normal);
			ray_dir = result.next_ray_dir;
			continue;
		}

		vec3 motion = vec3(unpackHalf2x16(payload.packed_rt[0]), unpackHalf2x16(payload.packed_rt[1]).x);
		dlss_rr_write_replaced_guides(p_origin + p_dir * scene_length, image_xform * motion, mirror_throughput, hit_diffuse, hit_specular);
		return;
	}
}
#endif // DLSS_RR_ENABLED

void main() {
#ifdef RT_TIMING_ENABLED
	uvec2 start_clock = clockRealtime2x32EXT();
#endif
	uvec2 pixel = gl_LaunchIDEXT.xy;
	const vec2 pixel_center = vec2(pixel) + vec2(0.5);
	const vec2 in_uv = pixel_center / vec2(gl_LaunchSizeEXT.xy);
	vec2 d = in_uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	// Orthogonal cameras shoot parallel rays from the near plane; perspective ones fan out from the eye.
	const bool orthogonal = scene_data_block.data.projection_matrix[3][3] != 0.0;
	vec4 target = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, 1.0, 1.0);
	vec3 view_origin = orthogonal ? vec3(target.xy / target.w, 0.0) : vec3(0.0);
	vec3 view_direction = orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(target.xyz);
	vec4 origin = inv_view * vec4(view_origin, 1.0);
	vec4 direction = inv_view * vec4(view_direction, 0.0);

	// Depth composite against a co-tenant viewport (reverse-Z clear is 0.0): primary rays
	// stop at its depth, and on a primary miss its color/velocity/guides win the pixel.
	float composite_t_max = 10000.0;
	float composite_prepass_depth = 0.0;
	// The co-tenant renders the whole shared target before us, so on a primary miss
	// its color is the correct background whatever its depth. Testing the depth here
	// would exclude its sky, which sits at the reverse-Z far value of 0.0.
	bool composite_enabled = (RT_FLAGS & RT_FLAG_DEPTH_COMPOSITE_ENABLED) != 0u;
	if (composite_enabled) {
		composite_prepass_depth = texelFetch(sampler2D(prepass_depth_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0).r;
		if (composite_prepass_depth > 0.0) {
			vec4 prepass_pos = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, composite_prepass_depth, 1.0);
			composite_t_max = length(prepass_pos.xyz / prepass_pos.w - view_origin);
		}
	}

	// Sample count from specialization constant (or uniform in dynamic mode), frame index from uniform
	const uint samples_per_pixel = RT_GET_SAMPLE_COUNT();
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));

	// Accumulate multiple samples per pixel
	vec3 total_radiance = vec3(0.0);
	bool composite_passthrough = false;

#ifdef RT_DEBUG_ENABLED
	uint debug_tlayers = 0u; // VIS 23: primary-segment transparency hits
#endif

	const uint max_bounces = RT_GET_MAX_BOUNCES();

	// TODO: when we have a spp > 0 the first raycast is always identical,
	// we should move it out of the loop

	[[dont_unroll]] for (uint sample_idx = 0u; sample_idx < samples_per_pixel; sample_idx++) {
		// Every field must be defined: path_pack branches on the octahedral encodes,
		// and branching on an undefined value lets the driver drop the whole path.
		PathState ps;
		ps.radiance = vec3(0.0);
		ps.throughput = vec3(1.0);
		ps.packed_bounces_flags = (sample_idx == 0u) ? set_sample_zero(0u) : 0u;
		ps.rng_state = init_rng(pixel, frame_index, sample_idx);
		ps.hit_t = -1.0;
		ps.offset_normal = vec3(0.0, 0.0, 1.0);
		ps.next_ray_dir = vec3(0.0, 0.0, 1.0);

		vec3 ray_origin = origin.xyz;
		vec3 ray_dir = direction.xyz;
#ifdef DLSS_RR_ENABLED
		// Primary surface replacement this sample still owes (only sample 0 starts one).
		bool replace_pending = false;
		float replace_hit_t = 0.0;
#endif

		[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			vec3 throughput_entry = ps.throughput;
			vec3 radiance_pre = ps.radiance;
			uint flags_entry = ps.packed_bounces_flags;
			ps.hit_t = -1.0; // A miss must not retain the previous segment's hit distance.
			path_pack(payload, ps);
			float t_far = (bounce == 0u) ? composite_t_max : 10000.0;

#ifdef USE_SER
			hitObjectEXT hitObject;
			hitObjectTraceRayEXT(hitObject, tlas, RT_RAY_FLAGS, RT_VIS_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, t_far, 0);
			reorderThreadEXT(hitObject);
			hitObjectExecuteShaderEXT(hitObject, 0);
#else
			traceRayEXT(tlas, RT_RAY_FLAGS, RT_VIS_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, t_far, 0);
#endif

			ps = path_unpack(payload);
#ifdef DLSS_RR_ENABLED
			if (bounce == 0u && (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u) {
				replace_pending = true;
				replace_hit_t = ps.hit_t;
			} else if (bounce == 1u && replace_pending) {
				// The hit clears the flag once it wrote the replacement; a singular miss reflects the sky.
				replace_pending = (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u &&
						!(is_singular_path(flags_entry) && ps.hit_t < 0.0);
				ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
			}
#endif
			bool opaque_terminated = is_path_terminated(ps.packed_bounces_flags);

			// Transparency over this segment is bounded closest-hit peeling.
			// Non-scissored transparent instances are opaque to traversal, so
			// only the closest hit runs a shader on each layer.
			bool transparency_enabled = get_rt_param(RT_PARAM_TRANSPARENT_COUNT) > 0.0 &&
					get_rt_param(RT_PARAM_MAX_TRANSPARENCY_LAYERS) > 0.0 &&
					bounce <= uint(get_rt_param(RT_PARAM_TRANSPARENCY_MAX_BOUNCE));
#ifdef RT_DEBUG_ENABLED
			transparency_enabled = transparency_enabled && rt_debug_keeps_transparency(int(get_rt_param(RT_PARAM_VIS_MODE)));
#endif
			if (transparency_enabled) {
				// SCENE_DEPTH for peel hits: NDC depth of this segment's opaque
				// hit on the primary segment, far (0.0) on bounces.
				float scene_depth = 0.0;
				if (bounce == 0u) {
					if (!is_primary_miss(ps.packed_bounces_flags) && ps.hit_t > 0.0) {
						scene_depth = rt_ndc_depth(ray_origin + ray_dir * ps.hit_t);
					} else if (composite_prepass_depth > 0.0) {
						scene_depth = composite_prepass_depth;
					}
				}
				// Termination only means there is no next bounce. Unshaded surfaces
				// and hits at the bounce limit still occlude transparency behind them.
				float seg_end = ps.hit_t >= 0.0 ? min(ps.hit_t, t_far) : t_far;
				float indirect_range = get_rt_param(RT_PARAM_TRANSPARENCY_INDIRECT_RANGE);
				float indirect_fade = get_rt_param(RT_PARAM_TRANSPARENCY_INDIRECT_FADE);
				bool range_limited = bounce > 0u && !is_singular_path(flags_entry) && indirect_range > 0.0;
				if (range_limited) {
					seg_end = min(seg_end, indirect_range);
				}
				uint peel_hits = 0u;
				vec3 throughput_after_opaque = ps.throughput;
				vec3 radiance_after_opaque = ps.radiance;
				PathState opaque_continuation = ps;
				ContinuationReservoir continuation = continuation_reservoir_initial();
				uint continuation_rng = ps.rng_state;

				float T_total = 1.0;
				uint layer_param = bounce == 0u			? RT_PARAM_MAX_TRANSPARENCY_LAYERS
						: is_singular_path(flags_entry) ? RT_PARAM_MAX_TRANSPARENCY_LAYERS_REFLECTION
														: RT_PARAM_MAX_TRANSPARENCY_LAYERS_INDIRECT;
				uint layer_cap = min(uint(get_rt_param(layer_param)), 64u);
				float peel_tmin = 0.001;
				for (uint l = 0u; l < layer_cap && T_total > (1.0 / 256.0) && peel_tmin < seg_end; l++) {
					ps.hit_t = -1.0;
					ps.packed_bounces_flags = flags_entry | PEEL_RAY_FLAG;
					if (l == 0u) {
						ps.packed_bounces_flags |= PEEL_FIRST_LAYER_FLAG;
					}
					ps.rng_state = continuation_rng;
					ps.throughput = throughput_entry * T_total;
					vec3 radiance_before_layer = ps.radiance;
					path_pack_peel(payload, ps, scene_depth);
					traceRayEXT(tlas_transparent, RT_RAY_FLAGS, RT_TMASK_TRANSPARENT,
							0, 0, 0, ray_origin, peel_tmin, ray_dir, seg_end, 0);
					ps = path_unpack(payload);
					if (ps.hit_t < 0.0) {
						break;
					}
					continuation_rng = ps.rng_state;
					float layer_weight = 1.0;
					if (range_limited && indirect_fade > 0.0) {
						layer_weight = 1.0 - smoothstep(indirect_range - indirect_fade, indirect_range, ps.hit_t);
						ps.radiance = radiance_before_layer + (ps.radiance - radiance_before_layer) * layer_weight;
					}
					if ((ps.packed_bounces_flags & PEEL_HAS_CONTINUATION_FLAG) != 0u) {
						PathState layer_continuation = ps;
						layer_continuation.packed_bounces_flags &=
								~(PEEL_RAY_FLAG | PEEL_FIRST_LAYER_FLAG | PEEL_HAS_CONTINUATION_FLAG | PEEL_DEPTH_BARRIER_HIT_FLAG);
						layer_continuation.throughput *= layer_weight;
						continuation_reservoir_add(continuation, layer_continuation, continuation_rng);
					}
					float a = get_peel_alpha(ps.packed_bounces_flags) * layer_weight;
					T_total *= (1.0 - a);
					peel_hits++;
					// Advance by one representable positive float to avoid
					// re-hitting this layer without an identity any-hit shader.
					peel_tmin = uintBitsToFloat(floatBitsToUint(ps.hit_t) + 1u);
					if ((ps.packed_bounces_flags & PEEL_DEPTH_BARRIER_HIT_FLAG) != 0u) {
						break;
					}
				}

				if (peel_hits == 0u) {
					// Preserve primary-miss/composite state when the transparent
					// lane had no geometry. The peel payload starts from flags_entry.
					ps = opaque_continuation;
					opaque_terminated = is_path_terminated(ps.packed_bounces_flags);
				} else {
					vec3 peel_sum = ps.radiance - radiance_after_opaque;
					vec3 segment_radiance = radiance_after_opaque - radiance_pre;
					if (bounce == 0u && composite_enabled &&
							is_primary_miss(opaque_continuation.packed_bounces_flags)) {
						vec3 composite_color = texelFetch(
								sampler2D(prepass_color_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0)
													   .rgb;
						segment_radiance = throughput_entry * composite_color;
					}
					vec3 resolved_radiance = radiance_pre + segment_radiance * T_total + peel_sum;

					opaque_continuation.throughput = throughput_after_opaque * T_total;
					opaque_continuation.rng_state = continuation_rng;
					continuation_reservoir_add(continuation, opaque_continuation, continuation_rng);
					if (continuation.has_selection) {
						ps = continuation.selected;
						ps.throughput *= continuation.total_weight / continuation.selected_weight;
						ps.radiance = resolved_radiance;
						ps.rng_state = continuation_rng;
						opaque_terminated = false;
					} else {
						ps.radiance = resolved_radiance;
						ps.throughput = vec3(0.0);
						ps.packed_bounces_flags = set_path_terminated(flags_entry);
						ps.rng_state = continuation_rng;
						opaque_terminated = true;
					}
#ifdef DLSS_RR_ENABLED
					// Glass in front already composited onto the mirror's guides; replacing would drop it.
					if (bounce == 0u) {
						replace_pending = false;
						ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
					}
#endif
				}
#ifdef RT_DEBUG_ENABLED
				if (bounce == 0u && sample_idx == 0u) {
					debug_tlayers = peel_hits;
				}
#endif
			}

			if (opaque_terminated || !russian_roulette_survives(bounce + 1u, ps)) {
				break;
			}
			// Reconstruct the next ray origin from the current ray + hit distance, apply bias
			vec3 hit_pos = ray_origin + ray_dir * ps.hit_t;
			ray_origin = offset_ray_origin(hit_pos, ps.offset_normal);
			ray_dir = ps.next_ray_dir;
		}
#ifdef DLSS_RR_ENABLED
		if (replace_pending) {
			dlss_rr_replace_primary_surface(origin.xyz, direction.xyz, replace_hit_t);
		}
#endif

		total_radiance += ps.radiance;
		composite_passthrough = composite_passthrough || (composite_enabled && is_primary_miss(ps.packed_bounces_flags));
	}

#ifdef RT_DEBUG_ENABLED
	if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_TRANSPARENCY_LAYERS) {
		// Transparency cost heatmap: green 0 -> yellow 2 -> red 4+.
		float heat_x = clamp(float(debug_tlayers) * 0.25, 0.0, 1.0);
		vec3 heat = (heat_x < 0.5) ? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), heat_x * 2.0)
								   : mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat_x * 2.0 - 1.0);
		imageStore(image, ivec2(pixel), vec4(heat, 1.0));
		return;
	}
#endif

	if (composite_passthrough) {
		// The co-tenant pixel is nearer than anything traced: keep its color and depth,
		// leave its velocity and DLSS-RR guides untouched.
		imageStore(image, ivec2(pixel), texelFetch(sampler2D(prepass_color_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0));
		imageStore(rt_depth_image, ivec2(pixel), vec4(composite_prepass_depth));
#ifdef RT_TIMING_ENABLED
		store_path_timing(ivec2(pixel), start_clock);
#endif
		return;
	}

	vec3 final_radiance = total_radiance / float(samples_per_pixel);
	imageStore(image, ivec2(pixel), vec4(final_radiance, 1.0));
#ifdef RT_TIMING_ENABLED
	store_path_timing(ivec2(pixel), start_clock);
#endif
}

#[miss]

#version 460

#VERSION_DEFINES

#pragma shader_stage(miss)
#extension GL_EXT_ray_tracing : enable

#define GLSL 1
#define RT_STAGE_MISS 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
#include "raytracing_samplers_inc.glsl"
#include "raytracing_volumetric_fog_inc.glsl"
// clang-format on

layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 0, binding = 29) uniform texture2D prepass_depth_texture;
layout(set = 0, binding = 33) uniform texture2D sky_screen_texture;

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 7) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Array-backed octahedral radiance: roughness selects the prefiltered array
// layer (with a blend between adjacent layers). Ray/hit stages have no
// screen-space derivatives, so the intra-layer mip is fixed at 0.0; roughness is
// represented entirely by the layer selection.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	float layer;
	float blend = modf(clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer + 1.0), 0.0).rgb;
	return mix(a, b, blend);
}

#else

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Single-texture octahedral radiance: roughness maps to the mip LOD.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), p_oct_uv, clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
}

#endif // USE_RADIANCE_OCTMAP_ARRAY

void main() {
	// Transparency peel traces own their payload; the scene miss must not touch it.
	if ((payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		return;
	}

	PathState ps = path_unpack(payload);

#if !defined(USE_SER)
	// A shadow-ray miss returns its accumulated colored transmittance.
	if (is_shadow_ray(ps.packed_bounces_flags)) {
		ps.radiance = ps.throughput;
		path_pack(payload, ps);
		return;
	}
#endif

	// Miss always ends the path.
	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);

	bool composite_passthrough = false;
	if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
		ps.packed_bounces_flags = set_primary_miss(ps.packed_bounces_flags);
		// Matches raygen: a primary miss always yields the pixel to the co-tenant,
		// including where the co-tenant left far depth (its own sky).
		composite_passthrough = (RT_FLAGS & RT_FLAG_DEPTH_COMPOSITE_ENABLED) != 0u;
	}

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// Specular hit distance: the first-bounce closest_hit pre-seeds the
		// "no hit" color into radiance, so a missed reflection just keeps it.
		if (VIS_MODE == 13 && get_total_bounces(ps.packed_bounces_flags) > 0u) {
			path_pack(payload, ps);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED

	// Primary ray miss: write depth, velocity, and DLSS RR defaults (sample 0 only).
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags) && !composite_passthrough) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

			imageStore(rt_depth_image, pixel, vec4(0.0));

			// Sky velocity: reproject a far-plane point using unjittered VPs.
			{
				vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
				vec2 curr_uv = project_uv(far_world, curr_vp_unjittered);
				vec2 prev_uv = project_uv(far_world, prev_vp_unjittered);
				imageStore(rt_velocity_image, pixel, vec4(prev_uv - curr_uv, 0.0, 0.0));
#ifdef DLSS_RR_ENABLED
				dlss_rr_write_specular_motion_vector(prev_uv - curr_uv);
#endif
			}
		}
	}

	// A camera ray leaves along the pixel it was launched from, and blend-alpha
	// peels do not refract, so the launch ID still indexes the right texel even
	// behind transparency. The screen-space sky is the same shader the rasterizer
	// runs and comes out of sky.glsl already fogged and in render-buffer
	// luminance, so it takes neither the IBL normalization nor the fog mix below.
	vec3 sky_color;
	if (get_rt_param(RT_PARAM_SKY_SCREEN_ENABLED) > 0.5 && get_total_bounces(ps.packed_bounces_flags) == 0u) {
		sky_color = texelFetch(sampler2D(sky_screen_texture, radiance_sampler), ivec2(gl_LaunchIDEXT.xy), 0).rgb;
	} else {
		vec2 sky_uv = rt_sky_oct_uv(gl_WorldRayDirectionEXT);

		sky_color = radiance_octmap_sample(sky_uv, 0.0);
		sky_color *= scene_data_block.data.IBL_exposure_normalization;

		if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u) {
			vec3 fog_color = scene_data_block.data.fog_light_color;

			if (scene_data_block.data.fog_aerial_perspective > 0.0) {
				vec3 sky_fog = radiance_octmap_sample(sky_uv, 1.0 / MAX_ROUGHNESS_LOD);
				sky_fog *= scene_data_block.data.IBL_exposure_normalization;
				fog_color = mix(fog_color, sky_fog, scene_data_block.data.fog_aerial_perspective);
			}

			sky_color = mix(sky_color, fog_color, scene_data_block.data.fog_sky_affect);
		}

		if (get_total_bounces(ps.packed_bounces_flags) == 0u && rt_volumetric_fog_enabled()) {
			vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
			sky_color = rt_volumetric_fog_apply_sky(sky_color, project_uv(far_world, curr_vp_unjittered));
		}
	}

#ifdef DLSS_RR_ENABLED
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags) && !composite_passthrough) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
			imageStore(dlss_rr_diffuse_albedo, pixel, vec4(vec3(DLSSRR_SKY_DIFFUSE_ALBEDO), 1.0));
			imageStore(dlss_rr_specular_albedo, pixel, vec4(0.0));
			imageStore(dlss_rr_normal_roughness, pixel, vec4(0.0));
		}
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE == 20) {
			ps.radiance = vec3(1.0);
		} else if (VIS_MODE == RT_VIS_VOLUMETRIC_FOG || VIS_MODE == RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE) {
			vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
			ps.radiance = rt_volumetric_fog_debug(VIS_MODE, rt_volumetric_fog_sky_sample(project_uv(far_world, curr_vp_unjittered)));
		} else if (VIS_MODE == 0) {
			ps.radiance += ps.throughput * sky_color;
		} else {
			ps.radiance = sky_color;
		}
	}
#else
	ps.radiance += ps.throughput * sky_color;
#endif // RT_DEBUG_ENABLED

	path_pack(payload, ps);
}

#[closest_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(closest_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef USE_SER
#extension GL_EXT_shader_invocation_reorder : enable
#endif
#ifdef RT_RAY_CONE_POSITION_FETCH
#extension GL_EXT_ray_tracing_position_fetch : require
#endif

#define GLSL 1
#define RT_STAGE_CLOSEST_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"
#include "raytracing_lights_inc.glsl"

// clang-format off
layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 7) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Array-backed octahedral radiance: roughness selects the prefiltered array
// layer (with a blend between adjacent layers). Ray/hit stages have no
// screen-space derivatives, so the intra-layer mip is fixed at 0.0; roughness is
// represented entirely by the layer selection.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	float layer;
	float blend = modf(clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer + 1.0), 0.0).rgb;
	return mix(a, b, blend);
}

#else

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Single-texture octahedral radiance: roughness maps to the mip LOD.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), p_oct_uv, clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
}

#endif // USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 39) uniform texture2D rt_dfg;

// Same lookup as raster prefiltered_dfg(); the roughness axis is inverted.
vec2 rt_prefiltered_dfg(float p_roughness, float p_NdotV) {
	return textureLod(sampler2D(rt_dfg, SAMPLER_LINEAR_CLAMP), vec2(p_NdotV, 1.0 - p_roughness), 0.0).xy;
}

// clang-format off
#include "raytracing_material_eval_inc.glsl"
#include "raytracing_closest_hit_common_inc.glsl"
#include "raytracing_decals_inc.glsl"
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected by ShaderCompiler for HG1+)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

// ============================================================================
// MAIN
// ============================================================================
void main() {
	HitData h = compute_hit_data();
	write_primary_hit_depth(h.hit_pos);
	write_primary_hit_velocity(h.hit_pos);

#ifdef RT_CUSTOM_HIT_GROUP
	// Mesh UV / UV2 footprints; ShaderCompiler scales them per texture() call through the coordinate math.
	rt_ray_cone_gradients(h.uv_density, h.geometry_normal, rt_uv_ddx, rt_uv_ddy);
#ifdef RT_USES_UV2
	rt_ray_cone_gradients(h.uv2_density, h.geometry_normal, rt_uv2_ddx, rt_uv2_ddy);
#endif
	uint rt_geometry_idx = h.geometry_idx;
	vec3 rt_hit_pos = h.hit_pos;
	vec2 rt_uv = h.uv;
	vec2 rt_uv2 = h.uv2;
	vec4 rt_color = h.color;
	vec3 rt_normal = h.geometry_normal;
	vec3 rt_tangent = h.tangent;
	vec3 rt_bitangent = h.bitangent;
	bool rt_front_face = h.is_front_face;

#include "raytracing_custom_fragment_inc.glsl"

	// After the custom code: the footprint code reads material uniforms and overwrites rt_uv_ddx/ddy with the finest one.
	/* RT_CUSTOM_TEXTURE_FOOTPRINT */
	rt_texture_streaming_feedback(h.geometry_idx, rt_uv_ddx, rt_uv_ddy);

	// Build MaterialResult from fragment outputs.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = alpha;
	m.roughness = roughness;
	m.metalness = metallic;
	m.specular = specular;
	m.emissive = emission * scene_data_block.data.emissive_exposure_normalization;
	m.emission_indirect = emission_indirect;
	m.backlight = backlight;
	// view space -> world space
	mat3 view_to_world = mat3(inv_view_matrix);
	m.normal = normalize(view_to_world * normal);

	// The vertex stage may have rewritten TANGENT/BINORMAL (triplanar mapping
	// does), so the shading frame comes from the shader, not the mesh TBN.
	vec3 world_tangent = normalize(view_to_world * tangent);
	vec3 world_bitangent = normalize(view_to_world * binormal);

	// Apply normal map if it was written.
	vec3 ts_normal = vec3(0.0, 0.0, 1.0);
	if (normal_map != vec3(0.5, 0.5, 1.0)) {
		ts_normal.xy = normal_map.xy * 2.0 - 1.0;
		ts_normal.z = sqrt(max(0.0, 1.0 - dot(ts_normal.xy, ts_normal.xy)));
		vec3 mapped = world_tangent * ts_normal.x + world_bitangent * ts_normal.y + m.normal * ts_normal.z;
		m.normal = normalize(mix(m.normal, mapped, normal_map_depth));
	}

#ifdef RT_DECALS_ENABLED
	if (rt_decals_process(h, m)) {
		return;
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// VIS 23 (transparency heatmap) shades normally; raygen overrides the image.
		if (VIS_MODE != 0 && VIS_MODE != 23) {
			debug_visualize(VIS_MODE, debug_material_inputs(h, m, ts_normal, world_tangent, world_bitangent, h.uv, ao), m.alpha);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#else
	// HG0: StandardMaterial3D evaluation.
	MaterialData mat = rt_entry_material(h.geometry_idx);
	MaterialUV muv = material_uv_from_world(mat, geometries[h.geometry_idx], h.uv, h.hit_pos, h.geometry_normal);
	rt_material_uv_apply_cone(muv, material_uv1_channel(mat), h.uv_density, h);
	rt_texture_streaming_feedback(h.geometry_idx, muv.uv_ddx, muv.uv_ddy);

	// Triplanar mapping derives its own tangent frame from the blend normal.
	if (muv.triplanar) {
		material_uv_triplanar_tangents(muv, h.tangent, h.bitangent);
	}

	// Normal mapping.
	vec3 tangent_space_normal = vec3(0.0, 0.0, 1.0);
	vec3 final_normal = h.geometry_normal;
	if ((mat.flags & RT_MAT_FLAG_HAS_NORMAL_MAP) != 0u) {
		vec3 normal_sample = material_uv_sample(mat.normal_texture, muv, mat.flags).rgb;
		tangent_space_normal.xy = normal_sample.xy * 2.0 - 1.0;
		tangent_space_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_space_normal.xy, tangent_space_normal.xy)));
		final_normal = apply_normal_map(h, tangent_space_normal, mat.normal_map_depth);
	}

	// Texture sampling.
	vec4 albedo_tex = material_uv_sample(mat.albedo_texture, muv, mat.flags);
	vec3 albedo = albedo_tex.rgb * mat.albedo_color.rgb;
	vec2 roughness_metallic = material_roughness_metallic(mat, muv);
	float roughness = roughness_metallic.x;
	float metalness = roughness_metallic.y;

	MaterialUV emission_uv = muv;
	if ((mat.flags & RT_MAT_FLAG_EMISSION_UV2) != 0u) {
		MaterialUVChannel uv2_channel = material_uv2_channel(mat);
		emission_uv = material_uv_channel_from_world(uv2_channel, h.uv2, h.hit_pos, h.geometry_normal);
		rt_material_uv_apply_cone(emission_uv, uv2_channel, h.uv2_density, h);
		// Feedback keeps the smallest footprint per material, so the more detailed channel wins.
		rt_texture_streaming_feedback(h.geometry_idx, emission_uv.uv_ddx, emission_uv.uv_ddy);
	}
	vec3 emissive = material_emission(mat, emission_uv) * scene_data_block.data.emissive_exposure_normalization;

	// Build MaterialResult.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = albedo_tex.a * mat.albedo_color.a;
	m.roughness = roughness;
	m.metalness = metalness;
	m.specular = rt_material_surface_params(mat).z;
	m.emissive = emissive;
	m.emission_indirect = mat.emission_indirect;
	m.normal = final_normal;
	m.backlight = vec3(0.0); // TODO: wire up BaseMaterial3D's "Back Lighting" for RT (raster-only today).

	// Proximity/distance fade (BaseMaterial3D feature, mirrors raster).
	if ((mat.flags & (RT_MAT_FLAG_PROXIMITY_FADE | RT_MAT_FLAG_DISTANCE_FADE)) != 0u) {
		mat4 fade_view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 fade_view_pos = (fade_view_mat * vec4(h.hit_pos, 1.0)).xyz;
		if ((mat.flags & RT_MAT_FLAG_DISTANCE_FADE) != 0u) {
			vec2 distance_fade = unpackHalf2x16(mat.distance_fade);
			m.alpha *= clamp(smoothstep(distance_fade.x, distance_fade.y, length(fade_view_pos)), 0.0, 1.0);
		}
		if ((mat.flags & RT_MAT_FLAG_PROXIMITY_FADE) != 0u && (payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
			// Peel rays carry the segment's opaque NDC depth; 0.0 = far (no fade).
			float fade_scene_z = -1e19;
			float scene_depth = payload_peel_scene_depth(payload);
			if (scene_depth > 0.0) {
				vec4 fade_scene_pos = scene_data_block.data.inv_projection_matrix * vec4(0.0, 0.0, scene_depth, 1.0);
				fade_scene_z = fade_scene_pos.z / fade_scene_pos.w;
			}
			m.alpha *= clamp(1.0 - smoothstep(fade_scene_z + mat.proximity_fade_distance, fade_scene_z, fade_view_pos.z), 0.0, 1.0);
		}
	}

#ifdef RT_DECALS_ENABLED
	if (rt_decals_process(h, m)) {
		return;
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// VIS 23 (transparency heatmap) shades normally; raygen overrides the image.
		if (VIS_MODE != 0 && VIS_MODE != 23) {
			vec2 debug_uv = muv.triplanar ? muv.triplanar_pos.xy : muv.uv;
			uint albedo_index = rt_texture_index(mat.albedo_texture);
			if (VIS_MODE == RT_VIS_TEXTURE_MIP && albedo_index != 0u) { // 0 is the white default.
				rt_debug_texture_mip = rt_debug_texture_mip_level(textureSize(sampler2D(bindless_textures[nonuniformEXT(albedo_index)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), 0),
						textureQueryLevels(sampler2D(bindless_textures[nonuniformEXT(albedo_index)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT)), muv.uv_ddx, muv.uv_ddy);
			}
			debug_visualize(VIS_MODE, debug_material_inputs(h, m, tangent_space_normal, h.tangent, h.bitangent, debug_uv, material_ao(mat, muv)), m.alpha);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#endif
}

#[any_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(any_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_ANY_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(location = 0) rayPayloadInEXT PathPayload payload;

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
#include "raytracing_material_eval_inc.glsl"

layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected for per-HG any-hit)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

void main() {
	uint geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[geometry_idx];
	MaterialData mat = rt_entry_material(geometry_idx);
	bool transparent = (mat.flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
	bool alpha_scissor = (mat.flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u;
	if (!transparent && !alpha_scissor) {
		return; // Ordinary opaque shadow candidate: accept immediately.
	}

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

	float hit_alpha = 1.0;
	vec3 hit_tint = vec3(1.0);
	float scissor_threshold = -1.0;

#ifdef RT_CUSTOM_HIT_GROUP
	// Compute hit data inline (cannot include closest_hit_common_inc here).
	uint rt_geometry_idx = geometry_idx;
	vec2 rt_uv = fetch_uv(geom, i0, i1, i2, bary);
	vec2 rt_uv2 = fetch_uv2(geom, i0, i1, i2, bary);
	TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

	vec3 rt_normal = normalize(normal_matrix * ah_tbn.normal);
	vec3 rt_tangent = normalize(normal_matrix * ah_tbn.tangent);
	vec3 rt_bitangent = cross(rt_normal, rt_tangent) * ah_tbn.bitangent_sign;

	bool rt_front_face = rt_triangle_front_facing(geom);
	rt_normal = rt_side_checked_normal(geom, rt_normal, rt_front_face);

	vec3 rt_hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	vec4 rt_color = fetch_color(geom, i0, i1, i2, bary);

#include "raytracing_custom_fragment_inc.glsl"

	hit_alpha = alpha;
	hit_tint = clamp(albedo, vec3(0.0), vec3(1.0));
	scissor_threshold = alpha_scissor_threshold;
#else
	// HG0: StandardMaterial3D alpha evaluation.
	{
		vec3 world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		vec3 world_normal = vec3(0.0, 1.0, 0.0);
		if ((mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u) {
			// Only triplanar needs a normal, and fetching the frame is not free.
			TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);
			world_normal = normalize(mat3(gl_ObjectToWorldEXT) * ah_tbn.normal);
		}

		MaterialUV muv = material_uv_from_world(mat, geom, fetch_uv(geom, i0, i1, i2, bary), world_pos, world_normal);
		vec4 albedo_tex = material_uv_sample(mat.albedo_texture, muv, mat.flags);
		hit_alpha = albedo_tex.a * mat.albedo_color.a;
		hit_tint = clamp(albedo_tex.rgb * mat.albedo_color.rgb, vec3(0.0), vec3(1.0));
	}
#endif
	hit_alpha = rt_entry_alpha(mat, hit_alpha, scissor_threshold >= 0.0 ? scissor_threshold : 0.5);

	uint blend_class = (mat.flags & RT_MAT_BLEND_CLASS_MASK) >> RT_MAT_BLEND_CLASS_SHIFT;
	if (blend_class == RT_BLEND_CLASS_PREMULT && hit_alpha > 1e-6) {
		hit_tint = clamp(hit_tint / hit_alpha, vec3(0.0), vec3(1.0));
	}

	if ((mat.flags & RT_MAT_FLAG_INSTANCE_FADE) != 0u) {
		// rt_entry_alpha() zeroes scissored texels; the fade alone never reaches 0 (fully faded entries are culled).
		if (hit_alpha <= 0.0) {
			ignoreIntersectionEXT;
		}
	} else if (scissor_threshold >= 0.0) {
		if (hit_alpha < scissor_threshold) {
			ignoreIntersectionEXT;
		}
	} else if (!transparent && hit_alpha < 0.5) {
		ignoreIntersectionEXT;
	}
	// Alpha-0 texels of Mix/OIT/Add/Sub layers contribute nothing; skip them so
	// they don't spend a peel layer or take the first-layer depth/guide slot
	// from what's behind (glyph hull in front of its outline). Premult and Mul
	// carry light at alpha 0; depth_draw_always keeps raster's occlude-at-zero.
	if (transparent && hit_alpha < (1.0 / 255.0) &&
			blend_class != RT_BLEND_CLASS_PREMULT && blend_class != RT_BLEND_CLASS_MUL &&
			(mat.flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS) == 0u) {
		ignoreIntersectionEXT;
	}
	// Transparent shadow candidates use stochastic coverage. Surviving rays
	// accumulate a colored filter in the existing throughput payload.
	if (transparent && is_shadow_ray(payload.packed_bounces_flags)) {
		PathState shadow_ps = path_unpack(payload);
		float shadow_alpha = clamp(hit_alpha, 0.0, 1.0);
		bool blocks_light = rand(shadow_ps.rng_state) < shadow_alpha;
		if (!blocks_light) {
			shadow_ps.throughput *= mix(vec3(1.0), hit_tint, shadow_alpha);
		}
		path_pack(payload, shadow_ps);
		if (!blocks_light) {
			ignoreIntersectionEXT;
		}
	}
}

#[intersection]

#version 460

#VERSION_DEFINES

#pragma shader_stage(intersection)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_INTERSECTION 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

// Write all attributes and report the intersection. Transparently delta-compresses
// PREV_POSITION into spare .w bytes of packed_normal/tangent + prev_pos_delta_yz.
#define report_intersection(t_hit, kind) \
	{ \
		vec3 _obj_hit = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * (t_hit); \
		vec3 _delta = any(isnan(m_PREV_POSITION)) ? vec3(0.0) : (m_PREV_POSITION - _obj_hit); \
		uint _n4 = packSnorm4x8(vec4(m_HIT_NORMAL, 0.0)); \
		uint _t4 = packSnorm4x8(vec4(m_HIT_TANGENT, 0.0)); \
		uint _dx = packHalf2x16(vec2(_delta.x, 0.0)); \
		hit_attribs.bary_or_uv = m_HIT_UV; \
		hit_attribs.packed_normal = (_n4 & 0x00FFFFFFu) | ((_dx & 0xFFu) << 24u); \
		hit_attribs.packed_tangent = (_t4 & 0x00FFFFFFu) | (((_dx >> 8u) & 0xFFu) << 24u); \
		hit_attribs.prev_pos_delta_yz = packHalf2x16(vec2(_delta.y, _delta.z)); \
		reportIntersectionEXT(t_hit, kind); \
	}

#ifdef RT_CUSTOM_HIT_GROUP

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_custom_texture_inc.glsl"
#include "raytracing_samplers_inc.glsl"

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

/* RT_CUSTOM_TEXTURE_DEFINES */

// File-scope built-ins accessible from user helper functions in globals.
float global_time = scene_data_block.data.time;
float global_prev_time = 0.0;
mat4 read_model_matrix = mat4(0.0);
mat3 model_normal_matrix = mat3(0.0);
mat4 m_INV_MODEL_MATRIX = mat4(0.0);
mat4 read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
mat4 inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
mat4 projection_matrix = scene_data_block.data.projection_matrix;
mat4 inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
vec2 read_viewport_size = scene_data_block.data.viewport_size;
float m_Z_NEAR = scene_data_block.data.z_near;
float m_Z_FAR = scene_data_block.data.z_far;

uint64_t _rt_material_address;
#define material CustomMaterialUniforms(_rt_material_address)

/* RT_CUSTOM_INTERSECTION_GLOBALS */

#endif

void main() {
#ifdef RT_CUSTOM_HIT_GROUP
	// Writable outputs.
	vec2 m_HIT_UV = vec2(0.0);
	vec3 m_HIT_NORMAL = vec3(0.0, 1.0, 0.0);
	vec3 m_HIT_TANGENT = vec3(1.0, 0.0, 0.0);
	vec3 m_PREV_POSITION = vec3(uintBitsToFloat(0x7FC00000u)); // NaN sentinel = not set.

	// Per-invocation built-ins (require RT intrinsics, only available in main).
	vec3 m_ORIGIN = gl_ObjectRayOriginEXT;
	vec3 m_DIRECTION = gl_ObjectRayDirectionEXT;
	vec3 m_WORLD_ORIGIN = gl_WorldRayOriginEXT;
	vec3 m_WORLD_DIRECTION = gl_WorldRayDirectionEXT;
	float m_T_MIN = gl_RayTminEXT;
	float m_T_MAX = gl_RayTmaxEXT;
	global_prev_time = scene_data_block.prev_data.time;

	// Resolve custom material uniforms via BDA (assigns file-scope address).
	uint rt_geometry_idx = RT_HIT_GEOMETRY_IDX;
	MaterialData rt_mat = rt_entry_material(rt_geometry_idx);
	_rt_material_address = rt_mat.uniform_address;

	// Per-primitive AABB bounds (available when expose_aabb_bounds is enabled).
	GeometryData rt_geom = geometries[rt_geometry_idx];
	vec3 m_AABB_MIN = vec3(0.0);
	vec3 m_AABB_MAX = vec3(0.0);
	if (rt_geom.vertex_address != 0ul) {
		FloatBuffer aabb_buf = FloatBuffer(rt_geom.vertex_address);
		int base = int(gl_PrimitiveID) * 6;
		m_AABB_MIN = vec3(aabb_buf.v[base + 0], aabb_buf.v[base + 1], aabb_buf.v[base + 2]);
		m_AABB_MAX = vec3(aabb_buf.v[base + 3], aabb_buf.v[base + 4], aabb_buf.v[base + 5]);
	}

	read_model_matrix = mat4(gl_ObjectToWorldEXT);
	model_normal_matrix = mat3(read_model_matrix);
	m_INV_MODEL_MATRIX = mat4(gl_WorldToObjectEXT);

	/* RT_CUSTOM_INTERSECTION_CODE */

#else
	// Base-variant fallback: never executed at runtime (the intersection
	// stage is always rebuilt per-HG with RT_CUSTOM_HIT_GROUP defined).
	// Only touch unconditional HitAttribs fields so the base variant parses.
	hit_attribs.bary_or_uv = vec2(0.0);
	reportIntersectionEXT(gl_RayTminEXT, 0u);
#endif
}
