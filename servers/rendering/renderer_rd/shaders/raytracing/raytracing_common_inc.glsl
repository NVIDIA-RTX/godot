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
				? max(1u, uint(get_rt_param(RT_PARAM_SAMPLE_COUNT)))          \
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

#ifdef DLSS_RR_ENABLED
layout(set = 0, binding = 9, rgba16f) uniform image2D dlss_rr_diffuse_albedo;
layout(set = 0, binding = 10, rgba16f) uniform image2D dlss_rr_specular_albedo;
layout(set = 0, binding = 11, rgba16f) uniform image2D dlss_rr_normal_roughness;
layout(set = 0, binding = 12, r16f) uniform image2D dlss_rr_specular_hit_dist;
#endif

// Binding 14 is reserved for GlobalShaderUniformData (declared above).
// Samplers occupy 16-27 (see raytracing_samplers_inc.glsl); velocity sits at
// the first free slot past them so we do not collide with either.
layout(set = 0, binding = 28, rg16f) uniform image2D rt_velocity_image;
layout(set = 0, binding = 15, r32f) uniform image2D rt_depth_image;

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
