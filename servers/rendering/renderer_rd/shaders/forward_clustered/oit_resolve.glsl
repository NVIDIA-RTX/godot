/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

/* clang-format on */

layout(set = 0, binding = 0) uniform sampler2D oit_accum;
layout(set = 0, binding = 1) uniform sampler2D oit_reveal;

layout(location = 0) out vec4 frag_color;

// Weighted-blended OIT resolve (McGuire/Bavoil), composited with
// premultiplied-alpha blending: dst = src.rgb + (1 - src.a) * dst.
void main() {
	ivec2 pos = ivec2(gl_FragCoord.xy);
	vec4 accum = texelFetch(oit_accum, pos, 0);
	float reveal = texelFetch(oit_reveal, pos, 0).r;
	vec3 average_color = accum.rgb / max(accum.a, 1e-4);
	frag_color = vec4(average_color * (1.0 - reveal), 1.0 - reveal);
}
