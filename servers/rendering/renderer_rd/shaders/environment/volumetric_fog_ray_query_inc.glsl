#extension GL_EXT_control_flow_attributes : enable
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#include "../raytracing/raytracing_inc.glsl"

#include "../raytracing/raytracing_hit_inc.glsl"

layout(set = 1, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 1, binding = 1, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 1, binding = 2, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};

#include "../raytracing/raytracing_entry_material_inc.glsl"

layout(set = 1, binding = 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;

layout(set = 1, binding = 4, std140) uniform FogRayQueryParams {
	mat4 view_to_world;
	uint frame_index;
	uint trace_shadows;
	uint shadow_mask; // Shadow-caster groups of the unified TLAS mask, without the SELF_SHADOWING group.
	uint pad1;
}
fog_ray_query;

layout(set = 2, binding = 0) uniform texture2D bindless_textures[];

#include "../raytracing/raytracing_shadow_query_inc.glsl"

layout(constant_id = 0) const uint FOG_RQ_POSITIONAL_SHADOW_RAYS = 2u;

// Must match RTFogShadows::MAX_POSITIONAL_SHADOW_RAYS.
#define FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS 4
#define FOG_RQ_DIRECTIONAL_DISTANCE 10000.0
#define FOG_RQ_ALPHA_LOD 2.0

struct FogShadowSlot {
	vec3 light;
	float weight;
	vec3 to_light;
	float weight_sum;
	float distance;
};

struct FogShadowReservoir {
	vec3 view_pos;
	uint rng;
	uint positional_count;
	FogShadowSlot directional;
	FogShadowSlot positional[FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS];
};

FogShadowReservoir fog_shadows_begin(vec3 p_view_pos, ivec3 p_froxel) {
	const FogShadowSlot empty_slot = FogShadowSlot(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0);

	FogShadowReservoir reservoir;
	reservoir.view_pos = p_view_pos;
	uint froxel_index = (uint(p_froxel.z) * uint(params.fog_volume_size.y) + uint(p_froxel.y)) * uint(params.fog_volume_size.x) + uint(p_froxel.x);
	reservoir.rng = pcg_hash(froxel_index ^ pcg_hash(fog_ray_query.frame_index));
	reservoir.positional_count = 0u;
	reservoir.directional = empty_slot;
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		reservoir.positional[i] = empty_slot;
	}
	return reservoir;
}

// Weighted reservoir sampling: each slot keeps one light with probability proportional to its weight.
void fog_shadows_slot_add(inout FogShadowSlot r_slot, vec3 p_light, vec3 p_to_light, float p_distance, float p_weight, inout uint r_rng) {
	r_slot.weight_sum += p_weight;
	if (rand(r_rng) * r_slot.weight_sum < p_weight) {
		r_slot.light = p_light;
		r_slot.weight = p_weight;
		r_slot.to_light = p_to_light;
		r_slot.distance = p_distance;
	}
}

vec3 fog_shadows_scatter(vec3 p_view_pos, vec3 p_light, vec3 p_to_light) {
	return p_light * henyey_greenstein(dot(normalize(p_to_light), normalize(p_view_pos)), params.phase_g);
}

float fog_shadows_weight(vec3 p_light) {
	return abs(dot(p_light, vec3(0.2126, 0.7152, 0.0722)));
}

void fog_shadows_add_directional(inout FogShadowReservoir r_reservoir, vec3 p_light, vec3 p_direction) {
	vec3 light = fog_shadows_scatter(r_reservoir.view_pos, p_light, p_direction);
	float weight = fog_shadows_weight(light);
	if (weight > 0.0) {
		fog_shadows_slot_add(r_reservoir.directional, light, p_direction, FOG_RQ_DIRECTIONAL_DISTANCE, weight, r_reservoir.rng);
	}
}

void fog_shadows_add_positional(inout FogShadowReservoir r_reservoir, vec3 p_light, vec3 p_to_light, float p_distance) {
	vec3 light = fog_shadows_scatter(r_reservoir.view_pos, p_light, p_to_light);
	float weight = fog_shadows_weight(light);
	if (weight <= 0.0) {
		return;
	}

	uint slot = r_reservoir.positional_count % FOG_RQ_POSITIONAL_SHADOW_RAYS;
	r_reservoir.positional_count++;
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		if (i == slot) {
			fog_shadows_slot_add(r_reservoir.positional[i], light, p_to_light, p_distance, weight, r_reservoir.rng);
		}
	}
}

vec3 fog_shadows_trace(vec3 p_view_pos, FogShadowSlot p_slot, inout uint r_rng) {
	vec3 origin = (fog_ray_query.view_to_world * vec4(p_view_pos, 1.0)).xyz;
	vec3 direction = normalize(mat3(fog_ray_query.view_to_world) * p_slot.to_light);
	vec3 visibility = vec3(1.0);
	if (fog_ray_query.trace_shadows != 0u) {
		visibility = ray_query_shadow_transmittance(origin, direction, 0.001, p_slot.distance - 0.001, fog_ray_query.shadow_mask, FOG_RQ_ALPHA_LOD, r_rng);
	}
	return p_slot.light * visibility * (p_slot.weight_sum / p_slot.weight);
}

vec3 fog_shadows_resolve(inout FogShadowReservoir r_reservoir) {
	vec3 light = vec3(0.0);
	if (r_reservoir.directional.weight_sum > 0.0) {
		light += fog_shadows_trace(r_reservoir.view_pos, r_reservoir.directional, r_reservoir.rng);
	}
	[[unroll]] for (uint i = 0u; i < FOG_RQ_MAX_POSITIONAL_SHADOW_RAYS; i++) {
		if (i < FOG_RQ_POSITIONAL_SHADOW_RAYS && r_reservoir.positional[i].weight_sum > 0.0) {
			light += fog_shadows_trace(r_reservoir.view_pos, r_reservoir.positional[i], r_reservoir.rng);
		}
	}
	return light;
}
