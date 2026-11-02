// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant (bits 0-25: flags, 26-27: sample mode, 28-31: bounces).
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define SHADER_IS_SRGB false

#define RT_FLAG_DLSS_RR_ENABLED (1u << 1)
#define RT_FLAG_FOG_ENABLED (1u << 2)
#define RT_FLAG_DEPTH_COMPOSITE_ENABLED (1u << 5)
#define RT_FLAG_VOLUMETRIC_FOG_ENABLED (1u << 7)
#define RT_FLAG_RAY_CONES_ENABLED (1u << 9)

// Unified TLAS masks: bit 0 = opaque camera lane, bit 1 = transparent camera
// lane, bits 2-7 = shadow-caster groups.
#define RT_VIS_MASK 0x01u
#define RT_TMASK_TRANSPARENT 0x02u
#define RT_SHADOW_GROUPS_MASK 0xFCu

// Sample mode: 0/1/2 = 1/2/4 samples (compile-time), 3 = dynamic (RT_PARAM_SAMPLE_COUNT).
#define RT_SAMPLE_MODE_SHIFT 26u
#define RT_SAMPLE_MODE_MASK 0x3u
#define RT_SAMPLE_MODE_DYNAMIC 3u
#define RT_GET_SAMPLE_MODE() ((RT_FLAGS >> RT_SAMPLE_MODE_SHIFT) & RT_SAMPLE_MODE_MASK)
#define RT_GET_SAMPLE_COUNT() (RT_GET_SAMPLE_MODE() == RT_SAMPLE_MODE_DYNAMIC \
				? max(1u, uint(get_rt_param(RT_PARAM_SAMPLE_COUNT))) \
				: (1u << RT_GET_SAMPLE_MODE()))

#define RT_MAX_BOUNCES_SHIFT 28u
#define RT_MAX_BOUNCES_MASK 0xFu
#define RT_GET_MAX_BOUNCES() ((RT_FLAGS >> RT_MAX_BOUNCES_SHIFT) & RT_MAX_BOUNCES_MASK)

// Cull back faces by default; double-sided instances override via CULL_DISABLE flag.
#define RT_RAY_FLAGS gl_RayFlagsCullBackFacingTrianglesEXT

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 14, std430) readonly buffer GlobalShaderUniformData {
	vec4 data[256];
}
global_shader_uniforms;

// Octahedral radiance-map UV (with border) for a world-space direction.
vec2 rt_sky_oct_uv(vec3 p_world_dir) {
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * mat3(scene_data_block.data.inv_view_matrix);
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	return vec3_to_oct_with_border(world_to_sky * p_world_dir, border);
}

#ifndef RT_STAGE_ANY_HIT

layout(set = 0, binding = 6, std140) uniform RaytracingParams {
	vec4 rt_params[RT_PARAM_COUNT / 4];
	mat4 prev_vp_unjittered;
	mat4 curr_vp_unjittered;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
}

// Project a world-space point to UV through an unjittered VP.
vec2 project_uv(vec3 world_pos, mat4 vp) {
	vec4 clip = vp * vec4(world_pos, 1.0);
	return clip.xy / clip.w * 0.5 + 0.5;
}

// Uses unjittered VP matrices matching the raster motion_vectors_store convention.
vec2 rt_motion_vector(vec3 p_pos, vec3 p_prev_pos) {
	return project_uv(p_prev_pos, prev_vp_unjittered) - project_uv(p_pos, curr_vp_unjittered);
}

float rt_ndc_depth(vec3 p_world_pos) {
	mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 clip_pos = scene_data_block.data.projection_matrix * vec4((view_mat * vec4(p_world_pos, 1.0)).xyz, 1.0);
	return clip_pos.z / clip_pos.w;
}

#ifdef DLSS_RR_ENABLED
layout(set = 0, binding = 9, rgba16f) uniform image2D dlss_rr_diffuse_albedo;
layout(set = 0, binding = 10, rgba16f) uniform image2D dlss_rr_specular_albedo;
layout(set = 0, binding = 11, rgba16f) uniform image2D dlss_rr_normal_roughness;
layout(set = 0, binding = 12, rg16f) uniform image2D dlss_rr_specular_motion_vectors;

// Share of a surface's reflectance leaving through the specular lobe, by Rec. 709 luminance.
float dlss_rr_specular_share(vec3 p_diffuse, vec3 p_specular) {
	const vec3 rec709 = vec3(0.2126, 0.7152, 0.0722);
	float specular = dot(p_specular, rec709);
	return specular / max(specular + dot(p_diffuse, rec709), 0.0001);
}

void dlss_rr_write_specular_motion_vector(vec2 p_mv) {
	imageStore(dlss_rr_specular_motion_vectors, ivec2(gl_LaunchIDEXT.xy), vec4(p_mv, 0.0, 0.0));
}

// DLSS-RR handles near-black albedo; unlike NRD's 0.04 floor, a metal's diffuse must stay ~0 or RR
// filters part of its reflection as diffuse. Only both guides at zero is invalid (RTXPT floor).
#define DLSS_RR_MIN_ALBEDO 0.0001
void dlss_rr_write_albedo_guides(vec3 p_diffuse, vec3 p_specular) {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	vec3 diffuse = clamp(p_diffuse, vec3(0.0), vec3(1.0));
	vec3 specular = clamp(p_specular, vec3(0.0), vec3(1.0));
	if (dot(diffuse + specular, vec3(1.0 / 3.0)) < DLSS_RR_MIN_ALBEDO) {
		diffuse += DLSS_RR_MIN_ALBEDO;
	}
	imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse, 1.0));
	imageStore(dlss_rr_specular_albedo, pixel, vec4(specular, 1.0));
}
#endif

// Binding 14 is reserved for GlobalShaderUniformData (declared above).
// Samplers occupy 16-27 (see raytracing_samplers_inc.glsl); velocity sits at
// the first free slot past them so we do not collide with either.
layout(set = 0, binding = 28, rg16f) uniform image2D rt_velocity_image;
layout(set = 0, binding = 15, r32f) uniform image2D rt_depth_image;

#ifdef DLSS_RR_ENABLED
// Reflection through a plane with unit normal p_n; the sign of p_n does not matter.
mat3 dlss_rr_householder(vec3 p_n) {
	return mat3(1.0) - 2.0 * outerProduct(p_n, p_n);
}

// Primary surface replacement: writes motion and albedo guides of a surface seen through the primary
// mirror (and any mirrors after it, p_chain_throughput). Depth and normal/roughness stay the mirror's,
// texture-modulated roughness flips replacement per texel and replaced depth would tear there.
// Reads the primary mirror's albedos back from this pixel's guides, so it must run before anything
// else overwrites them.
void dlss_rr_write_replaced_guides(vec3 p_virtual_pos, vec3 p_virtual_motion, vec3 p_chain_throughput,
		vec3 p_hit_diffuse, vec3 p_hit_specular) {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	vec3 own_diffuse = imageLoad(dlss_rr_diffuse_albedo, pixel).rgb;
	vec3 own_specular = imageLoad(dlss_rr_specular_albedo, pixel).rgb;
	vec2 mv = rt_motion_vector(p_virtual_pos, p_virtual_pos + p_virtual_motion);
	dlss_rr_write_specular_motion_vector(mv);
	imageStore(rt_velocity_image, pixel, vec4(mv, 0.0, 0.0));
	// Seen through the chain, the reflected surface is scaled by its throughput; the mirror's own
	// diffuse stays on top and its specular hands over by its specular share.
	vec3 throughput = own_specular * p_chain_throughput;
	dlss_rr_write_albedo_guides(throughput * p_hit_diffuse + own_diffuse,
			mix(own_specular, throughput * p_hit_specular, dlss_rr_specular_share(own_diffuse, own_specular)));
}
#endif

#endif // !RT_STAGE_ANY_HIT

// Shared hitAttributeEXT layout for all hit-group stages.
// Vulkan requires every shader in a hit group to agree on this layout.
#if defined(RT_STAGE_CLOSEST_HIT) || defined(RT_STAGE_ANY_HIT) || defined(RT_STAGE_INTERSECTION)
struct HitAttribs {
	vec2 bary_or_uv;
#ifdef ENABLE_INTERSECTION_SHADERS
	uint packed_normal; // xyz: snorm8 normal,  w: delta.x FP16 low byte.
	uint packed_tangent; // xyz: snorm8 tangent, w: delta.x FP16 high byte.
	uint prev_pos_delta_yz; // packHalf2x16(delta.y, delta.z).
#endif
};
hitAttributeEXT HitAttribs hit_attribs;
#endif
