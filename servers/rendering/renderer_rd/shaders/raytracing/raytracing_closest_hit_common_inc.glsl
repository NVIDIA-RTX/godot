// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_entry_material_inc.glsl,
//   raytracing_lights_inc.glsl, raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image, rt_prefiltered_dfg() (rt_dfg),
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped on back faces of cull_front / cull_disabled materials (raster DO_SIDE_CHECK).
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec2 uv2; // Raw UV2 (no shader scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

// Fetch vertex attributes and transform to world space.
// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[h.geometry_idx];

	// Custom hit groups may reference vertex color in their fragment code, so
	// pull all attributes; default HGs can skip color for perf.
#ifdef RT_CUSTOM_HIT_GROUP
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
#else
	uint uv2_fetch = (rt_entry_material_flags(h.geometry_idx) & RT_MAT_FLAG_EMISSION_UV2) != 0u ? FETCH_UV2 : 0u;
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_UV | FETCH_TBN | uv2_fetch);
#endif
	h.uv = attrs.uv;
	h.uv2 = attrs.uv2;
	h.color = attrs.color;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
		h.uv2 = h.uv;
		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		h.geometry_normal = normalize(normal_matrix * obj_normal);
		h.tangent = normalize(normal_matrix * obj_tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent);

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	} else
#endif
	{
		// Triangle hit: reuse `attrs` from the top-level fetch (already has
		// UV / TBN from FETCH_UV | FETCH_TBN or FETCH_ALL).
		h.geometry_normal = normalize(normal_matrix * attrs.normal);
		h.tangent = normalize(normal_matrix * attrs.tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

		h.is_front_face = rt_triangle_front_facing(geom);
		h.geometry_normal = rt_side_checked_normal(geom, h.geometry_normal, h.is_front_face);
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

	return h;
}

// ============================================================================
// HELPERS
// ============================================================================

// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

void rt_store_ndc_depth(vec3 hit_pos) {
	imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(rt_ndc_depth(hit_pos)));
}

// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags) &&
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) == 0u) {
		rt_store_ndc_depth(hit_pos);
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

// Previous-frame object-space position of a triangle point: deformed meshes read
// their previous vertex buffer, everything else is static in object space.
vec3 rt_prev_triangle_object_pos(GeometryData geom, uint p_primitive_id, vec2 p_bary, vec3 p_obj_pos) {
	uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
	if ((geom.flags & FLAG_DEFORMED) == 0u || prev_addr == 0ul) {
		return p_obj_pos;
	}
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, p_primitive_id, i0, i1, i2);
	vec3 bary = vec3(1.0 - p_bary.x - p_bary.y, p_bary.x, p_bary.y);
	FloatBuffer prev_vb = FloatBuffer(prev_addr);
	uint stride_floats = geom.position_stride >> 2;
	vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u],
			prev_vb.v[i0 * stride_floats + 1u],
			prev_vb.v[i0 * stride_floats + 2u]);
	vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u],
			prev_vb.v[i1 * stride_floats + 1u],
			prev_vb.v[i1 * stride_floats + 2u]);
	vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u],
			prev_vb.v[i2 * stride_floats + 1u],
			prev_vb.v[i2 * stride_floats + 2u]);
	return bary.x * p0 + bary.y * p1 + bary.z * p2;
}

// Previous model matrix: compact motion entry if the instance moved, current transform otherwise.
vec3 rt_prev_world_pos(uint p_geometry_idx, vec3 p_prev_obj_pos, mat4 p_object_to_world) {
	int mi = motion_indices[p_geometry_idx];
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : p_object_to_world;
	return (prev_model * vec4(p_prev_obj_pos, 1.0)).xyz;
}

// Previous-frame world position of the current closest-hit point.
vec3 rt_hit_prev_world_pos(vec3 hit_pos) {
	uint geom_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[geom_idx];
	vec3 prev_obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif
#ifdef RT_HIT_ATTRIBS_DECLARED
	prev_obj_pos = rt_prev_triangle_object_pos(geom, gl_PrimitiveID, attribs, prev_obj_pos);
#endif

	return rt_prev_world_pos(geom_idx, prev_obj_pos, mat4(gl_ObjectToWorldEXT));
}

void rt_store_velocity(vec3 hit_pos) {
	imageStore(rt_velocity_image, ivec2(gl_LaunchIDEXT.xy), vec4(rt_motion_vector(hit_pos, rt_hit_prev_world_pos(hit_pos)), 0.0, 0.0));
}

// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags) ||
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		return;
	}
	rt_store_velocity(hit_pos);
}

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

vec3 fog_get_directional_color(uint index) {
	return lights_get(index).emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_light_buffer[index].position);
	mat3 view_rot = transpose(mat3(
			scene_data_block.data.view_matrix[0].xyz,
			scene_data_block.data.view_matrix[1].xyz,
			scene_data_block.data.view_matrix[2].xyz));
	return view_rot * world_dir;
}

#define FOG_HAS_RADIANCE

vec3 fog_sample_radiance(vec3 vertex, float mip_level) {
	vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 cube_uv = vec3_to_oct_with_border(cube_view, border);
	// mip_level is a normalized roughness (0..1); radiance_octmap_sample maps it
	// onto the prefiltered roughness array layers.
	return radiance_octmap_sample(cube_uv, mip_level);
}

#include "../fog_inc.glsl"
#include "raytracing_volumetric_fog_inc.glsl"

// Analytic and, on camera segments, froxel fog at a hit, as premultiplied
// inscatter and transmittance.
vec4 rt_fog_at(vec3 world_pos, vec3 view_vertex, bool camera_segment) {
	bool use_fog = (RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u;
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
	if (use_fog) {
		fog = fog_process(scene_data_block.data, view_vertex);
		fog = vec4(fog.rgb * fog.a, 1.0 - fog.a);
	}
	if (camera_segment && rt_volumetric_fog_enabled()) {
		fog = rt_combine_fog(fog, use_fog, rt_volumetric_fog_at(world_pos, view_vertex));
	}
	return fog;
}

vec3 rt_world_to_view(vec3 p_world) {
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	return (view_mat * vec4(p_world, 1.0)).xyz;
}

// Apply environment fog for the ray segment that was just traversed.
// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(float segment_dist, uint mat_flags, bool camera_segment, inout vec3 radiance, inout vec3 throughput) {
	if ((mat_flags & RT_MAT_FLAG_FOG_DISABLED) != 0u) {
		return;
	}
	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u && !(camera_segment && rt_volumetric_fog_enabled())) {
		return;
	}

	vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * segment_dist;
	vec4 fog = rt_fog_at(world_hit, rt_world_to_view(world_hit), camera_segment);
	radiance += throughput * fog.rgb;
	throughput *= fog.a;
}

// ============================================================================
// BRDF MATERIAL SETUP
// ============================================================================

// BRDF inputs for a surface. 'p_mat_flags' supplies the Godot light model and
// 'p_NdotV' the rasterizer's DFG energy compensation.
MaterialProperties material_properties(vec3 p_albedo, float p_metallic, float p_roughness, float p_specular,
		vec3 p_backlight, uint p_mat_flags, float p_NdotV) {
	MaterialProperties material;
	material.baseColor = p_albedo;
	material.metalness = p_metallic;
	material.roughness = p_roughness;
	material.dielectricF0 = material_specular_to_f0(p_specular);
	material.emissive = vec3(0.0);
	material.transmissivness = 0.0;
	material.opacity = 1.0;
	vec3 f0 = baseColorToSpecularF0(p_albedo, p_metallic, material.dielectricF0);
	float dfg_y = rt_prefiltered_dfg(p_roughness, clamp(p_NdotV, 0.0001, 1.0)).y;
	material.energyCompensation = 1.0 + f0 * (1.0 / dfg_y - 1.0);
	material.backlight = p_backlight;
	material.diffuseMode = (p_mat_flags >> RT_MAT_DIFFUSE_MODE_SHIFT) & RT_MAT_SHADING_MODE_MASK;
	material.specularMode = (p_mat_flags >> RT_MAT_SPECULAR_MODE_SHIFT) & RT_MAT_SHADING_MODE_MASK;
	return material;
}

MaterialProperties material_properties_from_result(HitData h, MaterialResult m, vec3 N, vec3 V) {
	MaterialProperties material = material_properties(m.albedo, m.metalness, m.roughness, m.specular, m.backlight,
			rt_entry_material_flags(h.geometry_idx), dot(N, V));
	material.emissive = m.emissive;
	return material;
}

// Shadow-caster groups p_geometry_idx receives shadows from: only SELF_SHADOWING receivers keep the SELF_SHADOWING group.
uint rt_receiver_shadow_mask(uint p_geometry_idx) {
	if ((geometries[p_geometry_idx].flags & FLAG_SELF_SHADOWING) != 0u) {
		return 0xFFu;
	}
	return ~uint(get_rt_param(RT_PARAM_SELF_SHADOW_MASK)) & 0xFFu;
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
#define RT_VIS_DIRECTIONAL_SHADOW_BLOCKER 27

bool debug_is_camera_visible(uint p_geometry_idx, vec3 p_origin, vec3 p_direction, float p_t) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_VIS_MASK, p_origin, max(p_t - 0.01, 0.0), p_direction, p_t + 0.01);
	while (rayQueryProceedEXT(rq)) {
		if (RT_RQ_GEOMETRY_IDX(rq, false) == p_geometry_idx) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// White: sun visible. Yellow: only transparent casters (scaled by transmittance).
// Red: opaque blocker the camera cannot see (raster ignores it). Blue: opaque blocker the camera sees.
// Darker means a farther blocker. Gray: facing away from the sun. Magenta: no directional light.
vec3 debug_directional_shadow_blocker(vec3 p_geometry_normal, uint p_receiver_shadow_mask) {
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	int sun = -1;
	for (uint i = 0u; i < light_count; i++) {
		if (lights_get_type(i) == RT_LIGHT_TYPE_DIRECTIONAL) {
			sun = int(i);
			break;
		}
	}
	if (sun < 0) {
		return vec3(1.0, 0.0, 1.0);
	}
	vec3 to_sun = -normalize(rt_light_buffer[sun].position);
	if (dot(p_geometry_normal, to_sun) <= 0.0) {
		return vec3(0.1);
	}

	vec3 origin = offset_ray_origin(gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT, p_geometry_normal);
	vec3 transmittance = vec3(1.0);
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_SHADOW_GROUPS_MASK & p_receiver_shadow_mask, origin, 0.001, to_sun, 10000.0);
	while (rayQueryProceedEXT(rq)) {
		uint geometry_idx = RT_RQ_GEOMETRY_IDX(rq, false);
		uint mat_flags = rt_entry_material_flags(geometry_idx);
		if ((mat_flags & (RT_MAT_FLAG_TRANSPARENT | RT_MAT_FLAG_ALPHA_SCISSOR)) == 0u) {
			rayQueryConfirmIntersectionEXT(rq);
			continue;
		}
		bool transparent = (mat_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
		if (transparent && (mat_flags & (RT_MAT_FLAG_NO_RAY_QUERY_ALPHA | RT_MAT_FLAG_INSTANCE_FADE)) == RT_MAT_FLAG_NO_RAY_QUERY_ALPHA) {
			continue;
		}
		vec4 surface = ray_query_surface(geometry_idx, rayQueryGetIntersectionPrimitiveIndexEXT(rq, false), rayQueryGetIntersectionBarycentricsEXT(rq, false), 0.0);
		if (transparent) {
			// Expected value of ray_query_shadow_transmittance's stochastic coverage.
			transmittance *= (1.0 - surface.a) * mix(vec3(1.0), surface.rgb, surface.a);
		} else if (surface.a >= 0.5) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return transmittance == vec3(1.0) ? vec3(1.0) : vec3(1.0, 1.0, 0.0) * luminance(transmittance);
	}
	float t = rayQueryGetIntersectionTEXT(rq, true);
	uint blocker = RT_RQ_GEOMETRY_IDX(rq, true);
	float brightness = clamp(1.0 - log2(1.0 + t) / 8.0, 0.25, 1.0);
	vec3 color = debug_is_camera_visible(blocker, origin, to_sun, t) ? vec3(0.0, 0.3, 1.0) : vec3(1.0, 0.0, 0.0);
	return color * brightness;
}

// Must match RTDynamicGeometryDebug.
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MAX 7u
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MASK 15u
#define RT_DYNAMIC_GEOMETRY_REST_SHIFT 4u
#define RT_DYNAMIC_GEOMETRY_AGE_SHIFT 8u
#define RT_DYNAMIC_GEOMETRY_FADE_FRAMES 60.0

// Indexed by RTDynamicGeometryDebug::Category: static, moved, refit, build, captured refit, captured build,
// built idle, captured built idle.
const vec3 RT_DYNAMIC_GEOMETRY_COLORS[8] = vec3[](vec3(0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.05, 0.15, 1.0), vec3(1.0, 0.0, 1.0),
		vec3(0.25, 0.015, 0.015), vec3(0.22, 0.015, 0.22));
const float RT_DYNAMIC_GEOMETRY_STRENGTH[8] = float[](0.0, 0.4, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0);

// Category color of the packed category at p_shift, over p_base.
vec3 debug_dynamic_geometry_tint(uint p_state, uint p_shift, vec3 p_base, float p_recency) {
	uint category = min((p_state >> p_shift) & RT_DYNAMIC_GEOMETRY_CATEGORY_MASK, RT_DYNAMIC_GEOMETRY_CATEGORY_MAX);
	return mix(p_base, RT_DYNAMIC_GEOMETRY_COLORS[category], RT_DYNAMIC_GEOMETRY_STRENGTH[category] * p_recency);
}

// Category color over the rest category's color, itself over dim grayscale shading. Work done this frame
// (age 0) is opaque; earlier work starts well below that and fades out over RT_DYNAMIC_GEOMETRY_FADE_FRAMES.
vec3 debug_dynamic_geometry(uint p_state, vec3 p_albedo, float p_NdotV) {
	uint age = p_state >> RT_DYNAMIC_GEOMETRY_AGE_SHIFT;
	vec3 gray = vec3((0.02 + 0.13 * luminance(p_albedo)) * (0.3 + 0.7 * p_NdotV));
	vec3 rest = debug_dynamic_geometry_tint(p_state, RT_DYNAMIC_GEOMETRY_REST_SHIFT, gray, 1.0);
	float recency = age == 0u ? 1.0 : mix(0.6, 0.1, float(age) / RT_DYNAMIC_GEOMETRY_FADE_FRAMES);
	return debug_dynamic_geometry_tint(p_state, 0u, rest, recency);
}

// Opacity range of transparent layers: faint particles stay visible, near-opaque ones still show what's behind.
#define RT_DYNAMIC_GEOMETRY_MIN_OPACITY 0.35
#define RT_DYNAMIC_GEOMETRY_MAX_OPACITY 0.75

// Reports a transparency peel layer of p_color, like shade_and_bounce does: raygen composites it over what's behind.
void debug_dynamic_geometry_peel(inout PathState r_ps, vec3 p_color, float p_alpha) {
	float opacity = clamp(p_alpha, RT_DYNAMIC_GEOMETRY_MIN_OPACITY, RT_DYNAMIC_GEOMETRY_MAX_OPACITY);
	r_ps.hit_t = gl_HitTEXT;
	r_ps.radiance += r_ps.throughput * opacity * p_color;
	r_ps.packed_bounces_flags = set_peel_alpha(r_ps.packed_bounces_flags, opacity) & ~PEEL_HAS_CONTINUATION_FLAG;
	path_pack(payload, r_ps);
}

// Debug inputs of a closest hit. p_tangent_space_normal is the unpacked normal map sample.
MaterialDebugInputs debug_material_inputs(HitData p_h, MaterialResult p_m, vec3 p_tangent_space_normal, vec3 p_tangent, vec3 p_bitangent, vec2 p_uv, float p_ao) {
	MaterialDebugInputs inputs;
	inputs.geometry_normal = p_h.geometry_normal;
	inputs.normal = p_m.normal;
	inputs.view_normal = normalize(mat3(scene_data_block.data.inv_view_matrix) * p_m.normal);
	inputs.tangent_space_normal = p_tangent_space_normal;
	inputs.tangent = p_tangent;
	inputs.bitangent = p_bitangent;
	inputs.uv = p_uv;
	inputs.albedo = p_m.albedo;
	inputs.ao = p_ao;
	inputs.roughness = p_m.roughness;
	inputs.metallic = p_m.metalness;
	inputs.specular = p_m.specular;
	inputs.emission = p_m.emissive;
	inputs.NdotV = max(dot(p_m.normal, -gl_WorldRayDirectionEXT), 0.0001);
	inputs.front_facing = p_h.is_front_face;
	inputs.depth = gl_HitTEXT / (scene_data_block.data.z_far - scene_data_block.data.z_near);
	return inputs;
}

void debug_visualize(int vis_mode, MaterialDebugInputs dbg, float alpha) {
	PathState ps = path_unpack(payload);
	vec3 V = -gl_WorldRayDirectionEXT;

	if (material_debug_is_material_mode(vis_mode)) {
		ps.radiance = material_debug_color(vis_mode, dbg);
	} else if (vis_mode == 1) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ps.hit_t = gl_HitTEXT;
			ps.offset_normal = dbg.geometry_normal;
			ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, dbg.geometry_normal);
			path_pack(payload, ps);
			return;
		} else {
			ps.radiance = dbg.geometry_normal * 0.5 + 0.5;
		}
	} else if (vis_mode == 13) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			if (dbg.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
				ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
				ps.radiance = vec3(0.1, 0.1, 0.4);
				ps.hit_t = gl_HitTEXT;
				ps.offset_normal = dbg.normal;
				ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, dbg.normal);
				path_pack(payload, ps);
				return;
			} else {
				ps.radiance = vec3(0.1, 0.1, 0.4);
			}
		} else {
			float spec_hit_t = gl_HitTEXT;
			float v = clamp(log(spec_hit_t + 1.0) / log(1000.0), 0.0, 1.0);
			vec3 color;
			if (v < 0.33) {
				color = mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), v * 3.0);
			} else if (v < 0.66) {
				color = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), (v - 0.33) * 3.0);
			} else {
				color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), (v - 0.66) * 3.0);
			}
			ps.radiance = color;
		}
	} else if (vis_mode == RT_VIS_DIRECTIONAL_SHADOW_BLOCKER) {
		ps.radiance = debug_directional_shadow_blocker(dbg.geometry_normal, rt_receiver_shadow_mask(RT_HIT_GEOMETRY_IDX));
	} else if (vis_mode == RT_VIS_DYNAMIC_GEOMETRY) {
		vec3 color = debug_dynamic_geometry(geometries[RT_HIT_GEOMETRY_IDX].debug_state, dbg.albedo, dbg.NdotV);
		if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
			debug_dynamic_geometry_peel(ps, color, alpha);
			return;
		}
		ps.radiance = color;
	} else if (vis_mode == RT_VIS_VOLUMETRIC_FOG || vis_mode == RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE) {
		vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		ps.radiance = rt_volumetric_fog_debug(vis_mode, rt_volumetric_fog_at(world_hit, rt_world_to_view(world_hit)));
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		//
		// Color legend (ordered cleanest -> most biased):
		//   green   = no rejection in either pass (no fallback needed)
		//   blue    = clamp alone resolves rejection (energy preserving)
		//   yellow  = clamp + mirror (one lobe still mirrored after clamp)
		//   red     = clamp + mirror (both lobes mirrored - most biased pixel)
		//
		// Production output is never black anymore -- yellow/red just indicate
		// where the cheaper Layer-1 clamp could not catch the bump and the
		// biased Layer-2 mirror had to step in. Raise
		// RT_SHADING_NORMAL_CLAMP_THRESHOLD if yellow/red dominate.
		vec3 final_normal = dbg.normal;
		vec3 geometry_normal = dbg.geometry_normal;
		MaterialProperties dbg_mat = material_properties(dbg.albedo, dbg.metallic, dbg.roughness, dbg.specular,
				vec3(0.0), rt_entry_material_flags(RT_HIT_GEOMETRY_IDX), dbg.NdotV);

		vec3 dbg_dir;
		vec3 dbg_weight;
		// Draw both random pairs up front so each (specular/diffuse) test in
		// the un-clamped and clamped pass uses the exact same numbers.
		vec2 u_spec_rng = rand2(ps.rng_state);
		vec2 u_diff_rng = rand2(ps.rng_state);

		bool u_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, final_normal, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool u_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, final_normal, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		// Uses the same threshold as shade_and_bounce so the visualization
		// stays in sync with production sampling.
		vec3 N_clamped = clampShadingNormal(final_normal, geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		bool c_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, N_clamped, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool c_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, N_clamped, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		bool all_uncl_ok = u_spec_ok && u_diff_ok;
		bool all_cl_ok = c_spec_ok && c_diff_ok;
		bool any_cl_ok = c_spec_ok || c_diff_ok;

		if (all_uncl_ok) {
			ps.radiance = vec3(0.0, 1.0, 0.0);
		} else if (all_cl_ok) {
			ps.radiance = vec3(0.0, 0.4, 1.0);
		} else if (any_cl_ok) {
			ps.radiance = vec3(1.0, 1.0, 0.0);
		} else {
			ps.radiance = vec3(1.0, 0.0, 0.0);
		}
	}

	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	path_pack(payload, ps);
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

bool surface_is_unshaded(uint geometry_idx) {
#ifdef MODE_UNSHADED
	return true;
#else
	return (rt_entry_material_flags(geometry_idx) & RT_MAT_FLAG_UNSHADED) != 0u;
#endif
}

// Ambient and reflection lighting for max_bounces == 0, which never traces the
// sky. Mirrors the rasterizer (scene_forward_clustered.glsl): ambient source,
// sky contribution, reflection source, specular occlusion and DFG weighting.
vec3 ambient_approx(vec3 N, vec3 V, MaterialResult m, MaterialProperties material) {
	uint flags = scene_data_block.data.flags;
	vec4 ambient_color_energy = scene_data_block.data.ambient_light_color_energy;
	float sky_scale = scene_data_block.data.IBL_exposure_normalization * ambient_color_energy.a;
	float roughness = material.roughness;

	vec3 ambient = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) != 0u) {
		ambient = ambient_color_energy.rgb;
		if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP) != 0u) {
			vec3 sky_ambient = radiance_octmap_sample(rt_sky_oct_uv(N), 1.0) * sky_scale;
			ambient = mix(ambient, sky_ambient, scene_data_block.data.ambient_color_sky_mix);
		}
	}

	vec3 specular = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP) != 0u) {
		vec3 ref_vec = mix(reflect(-V, N), N, roughness * roughness);
		float horizon = min(1.0 + dot(ref_vec, N), 1.0);
		specular = radiance_octmap_sample(rt_sky_oct_uv(ref_vec), sqrt(roughness)) * sky_scale * horizon * horizon;

		float specular_occlusion = min(dot(ambient, vec3(0.3, 0.59, 0.11)) * 8.0, 1.0);
		float reflective_f = (1.0 - roughness) * material.metalness;
		specular_occlusion = max(min(reflective_f * specular_occlusion * 10.0, 1.0), specular_occlusion);

		specular *= specular_occlusion;
		if (material.diffuseMode == GODOT_DIFFUSE_TOON) {
			specular *= m.specular * material.metalness * material.baseColor * 2.0;
		} else {
			vec3 f0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.dielectricF0);
			vec2 env_brdf = rt_prefiltered_dfg(roughness, clamp(dot(N, V), 0.0001, 1.0));
			float f90 = clamp(50.0 * f0.g, material.metalness, 1.0);
			specular *= material.energyCompensation * ((f90 - f0) * env_brdf.x + f0 * env_brdf.y);
		}
	}

	return baseColorToDiffuseReflectance(material.baseColor, material.metalness) * ambient + specular;
}

// p_is_indirect: the path made a diffuse bounce, scaling lights by indirect_energy and emission by emission_indirect.
vec3 shade_local_surface(HitData h, MaterialResult m, vec3 N, vec3 V, bool p_is_indirect, inout uint rng_state) {
	float emission_weight = p_is_indirect ? m.emission_indirect : 1.0;
	if (surface_is_unshaded(h.geometry_idx)) {
		return (m.albedo + m.emissive) * emission_weight;
	}
	vec3 lit = m.emissive * emission_weight;
	MaterialProperties material = material_properties_from_result(h, m, N, V);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	vec3 hit_pos_offset = offset_ray_origin(h.hit_pos, h.geometry_normal);
	uint layers = geometries[h.geometry_idx].layers;
	uint shadow_mask = rt_receiver_shadow_mask(h.geometry_idx);
	if (RT_GET_MAX_BOUNCES() == 0u) {
		// Direct only: one ray per light group, plus the rasterizer's ambient.
		lit += lights_evaluate_direct_lighting_per_group(hit_pos_offset, N, V, material,
				rng_state, p_is_indirect, light_count, layers, shadow_mask);
		lit += ambient_approx(N, V, m, material);
	} else {
		lit += lights_evaluate_direct_lighting(hit_pos_offset, N, V, material,
				rng_state, p_is_indirect, light_count, layers, shadow_mask);
	}
	return lit;
}

bool sample_surface_continuation(HitData h, MaterialResult m, vec3 N, vec3 V, inout PathState ps) {
	if (surface_is_unshaded(h.geometry_idx)) {
		return false;
	}
	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		return false;
	}

	MaterialProperties material = material_properties_from_result(h, m, N, V);
	vec3 specular_f0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.dielectricF0);
	vec3 diffuse_reflectance = baseColorToDiffuseReflectance(material.baseColor, material.metalness);
	float specular_lum = luminance(specular_f0);
	float diffuse_lum = luminance(diffuse_reflectance);

	int brdf_type;
	if (diffuse_lum < 0.0001) {
		brdf_type = SPECULAR_TYPE;
	} else if (specular_lum < 0.0001) {
		brdf_type = DIFFUSE_TYPE;
	} else {
		float brdf_probability = clamp(specular_lum / (specular_lum + diffuse_lum), 0.01, 0.99);
		if (rand(ps.rng_state) < brdf_probability) {
			brdf_type = SPECULAR_TYPE;
			ps.throughput /= brdf_probability;
		} else {
			brdf_type = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdf_probability);
		}
	}

	vec3 next_dir;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(rand2(ps.rng_state), N, h.geometry_normal, V, material, brdf_type, next_dir, brdf_weight, vec4(0.0))) {
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			return false;
		}
		next_dir = recovered_dir;
	}

	ps.throughput *= brdf_weight;
	ps.packed_bounces_flags = (brdf_type == DIFFUSE_TYPE)
			? inc_diffuse_bounce(ps.packed_bounces_flags) | LAST_BOUNCE_DIFFUSE_FLAG
			: inc_total_bounce(ps.packed_bounces_flags) & ~LAST_BOUNCE_DIFFUSE_FLAG;
	if (brdf_type == DIFFUSE_TYPE || material.roughness > SINGULAR_ROUGHNESS_MAX) {
		ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
	}
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	return true;
}

#ifdef DLSS_RR_ENABLED
// Specular motion vector guide (rt_velocity_image convention). Treats the reflector as a plane:
// the mirror hit's virtual image sits on the view ray at the reflection distance, moved by the
// reflector's motion plus the reflected point's motion mirrored through the plane.
// Rough surfaces use the surface motion. Deterministic: RR guides must never carry sampling noise.
// p_plane_normal must be the unbumped geometry normal: normal-map detail scatters the virtual
// image per pixel and DLSS-RR warps highlights along that noise.
vec2 dlss_rr_specular_motion_vector(vec3 p_hit_pos, vec3 p_prev_hit_pos, vec3 p_plane_normal, vec3 V, float p_roughness, uint p_mask) {
	if (p_roughness >= MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
		return rt_motion_vector(p_hit_pos, p_prev_hit_pos);
	}
	vec3 N = dot(p_plane_normal, V) < 0.0 ? -p_plane_normal : p_plane_normal;
	vec3 spec_dir = reflect(-V, N);
	vec3 spec_origin = offset_ray_origin(p_hit_pos, spec_dir);
	rayQueryEXT spec_rq;
	// A/B: TerminateOnFirstHit stops at whichever hit traversal finds first, not the closest.
	rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT, p_mask, spec_origin, 0.001, spec_dir, 10000.0);
	while (rayQueryProceedEXT(spec_rq)) {
		if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(
						RT_RQ_GEOMETRY_IDX(spec_rq, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
						rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
			rayQueryConfirmIntersectionEXT(spec_rq);
		}
	}
	if (rayQueryGetIntersectionTypeEXT(spec_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		// Reflected sky is at infinity: only camera rotation moves it, like the primary sky velocity.
		vec3 far_world = p_hit_pos - V * 10000.0;
		return rt_motion_vector(far_world, far_world);
	}

	float spec_t = rayQueryGetIntersectionTEXT(spec_rq, true);
	vec3 reflected_pos = spec_origin + spec_dir * spec_t;
	uint reflected_geom_idx = RT_RQ_GEOMETRY_IDX(spec_rq, true);
	vec3 reflected_obj_pos = rt_prev_triangle_object_pos(geometries[reflected_geom_idx],
			uint(rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, true)),
			rayQueryGetIntersectionBarycentricsEXT(spec_rq, true),
			(mat4(rayQueryGetIntersectionWorldToObjectEXT(spec_rq, true)) * vec4(reflected_pos, 1.0)).xyz);
	vec3 reflected_prev_pos = rt_prev_world_pos(reflected_geom_idx, reflected_obj_pos,
			mat4(rayQueryGetIntersectionObjectToWorldEXT(spec_rq, true)));

	vec3 reflector_motion = p_prev_hit_pos - p_hit_pos;
	vec3 relative_motion = reflected_prev_pos - reflected_pos - reflector_motion;
	vec3 virtual_pos = p_hit_pos - V * spec_t;
	vec3 virtual_prev_pos = virtual_pos + reflector_motion + reflect(relative_motion, N);
	return rt_motion_vector(virtual_pos, virtual_prev_pos);
}

vec3 dlss_rr_compute_specular_albedo(MaterialResult m, float NdotV) {
	return DLSSRR_computeSpecularAlbedo(m.albedo, m.metalness, material_specular_to_f0(m.specular), m.roughness, NdotV);
}

void dlss_rr_store_albedo_guides(MaterialResult m, float NdotV) {
	dlss_rr_write_albedo_guides(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness), dlss_rr_compute_specular_albedo(m, NdotV));
}

// Primary hits at least this metallic take the reflection's motion as their own.
#define DLSS_RR_SPECULAR_ONLY_METALNESS 0.9
// Material-only test: an albedo/Fresnel based share flips per texel and with view angle, tearing
// replaced and own guides apart on textured dielectrics.
bool dlss_rr_is_replaceable_mirror(HitData h, MaterialResult m) {
	return m.roughness <= DLSS_RR_REPLACEMENT_ROUGHNESS_MAX && m.metalness >= DLSS_RR_SPECULAR_ONLY_METALNESS &&
			!surface_is_unshaded(h.geometry_idx);
}

// Guide ray hit, returned to raygen in the payload. Every hit: hit_t, diffuse albedo (packed_rt[2])
// and specular albedo (rng_state) as unorm8. A replaceable mirror adds its exact reflection
// (next_ray_dir); any other surface its world motion (packed_rt[0], packed_rt[1].x). Raygen blends
// albedos and maps motion through the chain.
void dlss_rr_guide_hit(HitData h, MaterialResult m, vec3 N, vec3 V, float NdotV, uint p_flags) {
	bool mirror = (p_flags & GUIDE_LAST_FLAG) == 0u && dlss_rr_is_replaceable_mirror(h, m);
	PathState gs;
	gs.radiance = vec3(0.0);
	gs.throughput = vec3(0.0);
	gs.packed_bounces_flags = p_flags | GUIDE_RESULT_FLAG | (mirror ? GUIDE_MIRROR_FLAG : 0u);
	gs.rng_state = 0u;
	gs.hit_t = gl_HitTEXT;
	gs.offset_normal = h.geometry_normal;
	gs.next_ray_dir = mirror ? reflect(-V, N) : N;
	path_pack(payload, gs);
	vec3 motion = mirror ? vec3(0.0) : rt_hit_prev_world_pos(h.hit_pos) - h.hit_pos;
	payload.packed_rt[0] = packHalf2x16(motion.xy);
	payload.packed_rt[1] = packHalf2x16(vec2(motion.z, 0.0));
	payload.packed_rt[2] = packUnorm4x8(vec4(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness), 0.0));
	payload.rng_state = packUnorm4x8(vec4(dlss_rr_compute_specular_albedo(m, NdotV), 0.0));
}

// Composites a peel layer's RR guides over the ones stored for this pixel. p_cover is
// how much of what is behind the layer it hides. A premultiplied layer adds its albedos
// unscaled and takes over normal and roughness by its share of the
// specular albedo, so its guides stay continuous at any alpha.
void dlss_rr_composite_peel_guides(HitData h, MaterialResult m, vec3 N, vec3 V, float NdotV, float p_cover, bool p_premultiplied) {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	float source_weight = p_premultiplied ? 1.0 : p_cover;
	float behind = 1.0 - p_cover;

	vec3 diffuse = imageLoad(dlss_rr_diffuse_albedo, pixel).rgb * behind +
			DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness) * source_weight;
	vec3 layer_specular = source_weight * dlss_rr_compute_specular_albedo(m, NdotV);
	vec3 specular = imageLoad(dlss_rr_specular_albedo, pixel).rgb * behind + layer_specular;
	dlss_rr_write_albedo_guides(diffuse, specular);

	float share = p_premultiplied ? luminance(layer_specular) / max(luminance(specular), 1e-4) : p_cover;
	vec4 prev_nr = imageLoad(dlss_rr_normal_roughness, pixel);
	// Sky stores a zero normal, so the blend can be zero when share is 0.
	vec3 blended_n = mix(prev_nr.xyz, N, share);
	blended_n = dot(blended_n, blended_n) > 0.0 ? normalize(blended_n) : blended_n;
	imageStore(dlss_rr_normal_roughness, pixel, vec4(blended_n, mix(prev_nr.w, m.roughness, share)));

	// Blended motion vectors would point at neither surface, so the larger share owns them.
	if (share >= 0.5) {
		vec2 specular_mv = dlss_rr_specular_motion_vector(h.hit_pos, rt_hit_prev_world_pos(h.hit_pos), h.geometry_normal, V, m.roughness, RT_VIS_MASK | RT_TMASK_TRANSPARENT);
		dlss_rr_write_specular_motion_vector(specular_mv);
	}
}
#endif // DLSS_RR_ENABLED

// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	// Any-hit already rejected alpha-scissored texels.
	m.alpha = rt_entry_alpha(rt_entry_material(h.geometry_idx), m.alpha, 0.0);
#ifdef RT_DEBUG_ENABLED
	if (rt_peel_scene_depth_lost) {
		m.albedo = vec3(0.0);
		m.emissive = vec3(50.0, 0.0, 25.0); // Hot pink, bright enough to survive exposure and tonemapping.
		m.alpha = 1.0;
	}
#endif
	PathState ps = path_unpack(payload);
	// Every surface bounds this ray segment, even if it cannot continue the path.
	ps.hit_t = gl_HitTEXT;

	vec3 V = -gl_WorldRayDirectionEXT;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

#ifdef DLSS_RR_ENABLED
	if ((ps.packed_bounces_flags & GUIDE_RAY_FLAG) != 0u) {
		dlss_rr_guide_hit(h, m, N, V, NdotV, ps.packed_bounces_flags);
		return;
	}
#endif

	if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		// Peel layer: shade like an opaque hit, apply its blend operation, no bounce.
		// Raygen pre-scales throughput by the running transmittance and owns
		// path continuation; this only reports (t, alpha) and its own light.
		uint mat_flags = rt_entry_material_flags(h.geometry_idx);
		uint bclass = (mat_flags & RT_MAT_BLEND_CLASS_MASK) >> RT_MAT_BLEND_CLASS_SHIFT;
		bool depth_barrier = (mat_flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS) != 0u;
		float material_alpha = clamp(m.alpha, 0.0, 1.0);
		float peel_alpha;
		float continuation_alpha = 0.0;
		vec3 fogged_throughput = ps.throughput;
		vec3 branch_fog = vec3(0.0);
		apply_segment_fog(gl_HitTEXT, mat_flags, get_total_bounces(ps.packed_bounces_flags) == 0u, branch_fog, fogged_throughput);
		if (bclass == RT_BLEND_CLASS_MUL) {
			// Mul contributes no light; its color folds into the scalar T.
			peel_alpha = clamp(1.0 - luminance(m.albedo), 0.0, 1.0);
		} else {
			// Add/Sub scale their source by alpha but do not attenuate the
			// destination. Premultiplied alpha already includes alpha in RGB.
			peel_alpha = (bclass == RT_BLEND_CLASS_ADD || bclass == RT_BLEND_CLASS_SUB) ? 0.0 : material_alpha;
			ps.throughput = fogged_throughput;
			path_pack(payload, ps);
			vec3 lit = shade_local_surface(h, m, N, V,
					get_diffuse_bounces(ps.packed_bounces_flags) > 0u, ps.rng_state);
			float source_alpha = (bclass == RT_BLEND_CLASS_PREMULT) ? 1.0 : material_alpha;
			vec3 contribution = ps.throughput * source_alpha * lit;
			if (bclass == RT_BLEND_CLASS_SUB) {
				ps.radiance -= contribution;
			} else {
				ps.radiance += contribution;
			}
			// Add/Sub don't attenuate the destination (peel_alpha above), but they
			// still need a chance at their own indirect bounce so they can pick up
			// GI/sky ambient like every other blend class does. Premult adds its
			// direct light unscaled by alpha, so its bounce must be unscaled too.
			continuation_alpha = (bclass == RT_BLEND_CLASS_PREMULT) ? 1.0 : material_alpha;
			if (bclass != RT_BLEND_CLASS_ADD && bclass != RT_BLEND_CLASS_SUB) {
				ps.radiance += branch_fog * material_alpha;
			}

			// Dominant surface: the first peel layer on the primary segment
			// with alpha >= the dominant layer alpha owns depth/velocity and
			// blends into RR guides. depth_draw_always owns depth regardless of its alpha.
			// Premult reflects at full strength at any alpha, so it always
			// composites into the RR guides.
			float dominant_alpha = get_rt_param(RT_PARAM_TRANSPARENCY_DOMINANT_LAYER_ALPHA);
			bool primary_first_layer = get_total_bounces(ps.packed_bounces_flags) == 0u && is_sample_zero(ps.packed_bounces_flags) &&
					(ps.packed_bounces_flags & PEEL_FIRST_LAYER_FLAG) != 0u;
			bool dominant = depth_barrier || (dominant_alpha > 0.0 && peel_alpha >= dominant_alpha);
			if (primary_first_layer && dominant) {
				rt_store_ndc_depth(h.hit_pos);
				rt_store_velocity(h.hit_pos);
			}
#ifdef DLSS_RR_ENABLED
			bool premultiplied = bclass == RT_BLEND_CLASS_PREMULT;
			if (primary_first_layer && (dominant || premultiplied)) {
				dlss_rr_composite_peel_guides(h, m, N, V, NdotV, depth_barrier ? material_alpha : peel_alpha, premultiplied);
			}
#endif
		}
		ps.packed_bounces_flags = set_peel_alpha(ps.packed_bounces_flags, peel_alpha);
		if (depth_barrier) {
			ps.packed_bounces_flags |= PEEL_DEPTH_BARRIER_HIT_FLAG;
		}
		ps.packed_bounces_flags &= ~PEEL_HAS_CONTINUATION_FLAG;
		// Continue with probability continuation_alpha rather than scaling throughput by
		// it, so faint layers rarely spawn a full indirect path.
		if (rand(ps.rng_state) < continuation_alpha && sample_surface_continuation(h, m, N, V, ps)) {
			ps.packed_bounces_flags |= PEEL_HAS_CONTINUATION_FLAG;
		}
		path_pack(payload, ps);
		return;
	}

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);

	// Environment fog for this ray segment (before surface contribution).
	apply_segment_fog(gl_HitTEXT, rt_entry_material_flags(h.geometry_idx), total_bounces == 0u, ps.radiance, ps.throughput);

	path_pack(payload, ps);
	ps.radiance += ps.throughput * shade_local_surface(h, m, N, V, diffuse_bounces > 0u, ps.rng_state);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
		dlss_rr_store_albedo_guides(m, NdotV);
		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, m.roughness));
		vec2 specular_mv = dlss_rr_specular_motion_vector(h.hit_pos, rt_hit_prev_world_pos(h.hit_pos), h.geometry_normal, V, m.roughness, RT_VIS_MASK);
		dlss_rr_write_specular_motion_vector(specular_mv);
		// Metals show nothing of their own, so primary motion follows the reflection (which is the
		// surface motion again past the spec MV roughness limit).
		if (m.metalness >= DLSS_RR_SPECULAR_ONLY_METALNESS) {
			imageStore(rt_velocity_image, pixel, vec4(specular_mv, 0.0, 0.0));
		}
		// The path's next hit (or raygen's fallback walk) replaces these guides with the surface seen through the mirror.
		if (dlss_rr_is_replaceable_mirror(h, m)) {
			ps.packed_bounces_flags |= DLSS_RR_REPLACE_PENDING_FLAG;
		}
	} else if (total_bounces == 1u && (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u &&
			is_sample_zero(ps.packed_bounces_flags) && is_singular_path(ps.packed_bounces_flags) &&
			!dlss_rr_is_replaceable_mirror(h, m)) {
		// Singular bounce off the replaceable primary mirror: this hit's image across the mirror plane
		// stands in for the mirror. Exact even though the bounce direction was sampled. A second
		// mirror leaves the flag set for raygen's walk.
		vec4 mirror_normal_roughness = imageLoad(dlss_rr_normal_roughness, ivec2(gl_LaunchIDEXT.xy));
		mat3 mirror_xform = dlss_rr_householder(mirror_normal_roughness.xyz);
		vec3 mirror_pos = gl_WorldRayOriginEXT;
		dlss_rr_write_replaced_guides(mirror_pos + mirror_xform * (h.hit_pos - mirror_pos),
				mirror_xform * (rt_hit_prev_world_pos(h.hit_pos) - h.hit_pos), vec3(1.0),
				DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness), dlss_rr_compute_specular_albedo(m, NdotV));
		ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
	}
#endif

	if (!sample_surface_continuation(h, m, N, V, ps)) {
		ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	}
	path_pack(payload, ps);
}
