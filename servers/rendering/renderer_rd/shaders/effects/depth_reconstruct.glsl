#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D lowres_depth;
layout(set = 0, binding = 1) uniform sampler2D upscaled_color;
layout(set = 0, binding = 2) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 3) uniform sampler2D history_depth;
layout(r32f, set = 0, binding = 4) uniform restrict writeonly image2D dest_depth;

layout(push_constant, std430) uniform Params {
	ivec2 target_size;
	ivec2 lowres_size;
	float z_near;
	float z_far;
	float history_weight;
	float _pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.target_size))) {
		return;
	}

	vec2 texel_size = 1.0 / vec2(params.target_size);
	vec2 uv = (vec2(pos) + 0.5) * texel_size;

	vec3 center_color = textureLod(upscaled_color, uv, 0.0).rgb;

	// Joint bilateral upscale: 4x4 low-res neighborhood weighted by
	// color similarity and spatial distance.
	vec2 lowres_texel_size = 1.0 / vec2(params.lowres_size);
	vec2 lowres_pos = uv * vec2(params.lowres_size) - 0.5;
	ivec2 base = ivec2(floor(lowres_pos));
	vec2 frac_part = lowres_pos - vec2(base);

	float color_sigma_inv2 = -1.0 / (2.0 * 0.08 * 0.08);
	float spatial_sigma_inv2 = -1.0 / (2.0 * 1.5 * 1.5);

	float weight_sum = 0.0;
	float depth_sum = 0.0;
	float closest_depth = 0.0;
	float farthest_depth = 1.0;
	vec2 closest_uv = uv;

	for (int dy = -1; dy <= 2; dy++) {
		for (int dx = -1; dx <= 2; dx++) {
			ivec2 tap = base + ivec2(dx, dy);
			vec2 sample_uv = (vec2(tap) + 0.5) * lowres_texel_size;

			float d = textureLod(lowres_depth, sample_uv, 0.0).r;
			vec3 c = textureLod(upscaled_color, sample_uv, 0.0).rgb;

			vec2 dist = vec2(dx, dy) - frac_part;
			float sw = exp(dot(dist, dist) * spatial_sigma_inv2);

			vec3 diff = c - center_color;
			float cw = exp(dot(diff, diff) * color_sigma_inv2);

			float w = sw * cw;
			depth_sum += d * w;
			weight_sum += w;

			farthest_depth = min(farthest_depth, d);
			if (d > closest_depth) {
				closest_depth = d;
				closest_uv = sample_uv;
			}
		}
	}

	float upscaled_depth = depth_sum / max(weight_sum, 1e-6);

	// Bias toward closer objects (higher reverse-Z) to dilate foreground.
	float dilation_bias = 0.3;
	float current_depth = mix(upscaled_depth, closest_depth, dilation_bias);

	// Use velocity from the closest surface to avoid edge artifacts.
	vec2 velocity = textureLod(velocity_buffer, closest_uv, 0.0).rg;
	vec2 prev_uv = uv + velocity;

	float history_raw = textureLod(history_depth, prev_uv, 0.0).r;

	// Clamp history to current frame's depth neighborhood range.
	float history = clamp(history_raw, farthest_depth, closest_depth);

	bool offscreen = any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)));
	float hw = offscreen ? 0.0 : params.history_weight;
	float result = mix(current_depth, history, hw);

	imageStore(dest_depth, pos, vec4(result));
}
