#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : require

#include "motion_vector_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rg16f, set = 0, binding = 0) uniform restrict image2D velocity_buffer;
layout(set = 1, binding = 0) uniform texture2D depth_buffer;

layout(push_constant, std430) uniform Params {
	vec4 resolution;
	mat4 reprojection;
}
params;

void main() {
	const ivec2 screen_pos = ivec2(gl_GlobalInvocationID.xy);
	vec2 velocity = imageLoad(velocity_buffer, screen_pos).rg;

	bool is_invalid_motion_vector = all(lessThanEqual(velocity, vec2(-1.0f, -1.0f)));
	if (is_invalid_motion_vector) {
		float depth_value = texelFetch(depth_buffer, screen_pos, 0).r;
		vec2 uv = (vec2(screen_pos) + float(0.5)) / params.resolution.xy;
		velocity = derive_motion_vector(uv, depth_value, params.reprojection);
	}

	imageStore(velocity_buffer, screen_pos, vec4(velocity, 0.0f, 0.0f));
}
