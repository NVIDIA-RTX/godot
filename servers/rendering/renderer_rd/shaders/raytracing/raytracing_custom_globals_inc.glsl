// Custom shader globals for RT hit groups (closest-hit and any-hit).
// Include under #ifdef RT_CUSTOM_HIT_GROUP, outside main().
// Requires: bindless_textures[] and rt_entry_material() already declared.

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

// Shared vertex/fragment built-ins at global scope so that the vertex function,
// texture #defines, and fragment_globals functions can all access them.
// Assigned to real hit values in main() before use.
CustomMaterialUniforms material = CustomMaterialUniforms(uint64_t(0));
MaterialData rt_mat; // Assigned in main() before rt_run_fragment_shader() is called.
vec3 vertex = vec3(0.0);
vec3 normal = vec3(0.0, 0.0, 1.0);
vec3 tangent = vec3(1.0, 0.0, 0.0);
vec3 binormal = vec3(0.0, 1.0, 0.0);
vec2 uv_interp = vec2(0.0);
vec2 uv2_interp = vec2(0.0);
vec4 color_interp = vec4(1.0);
vec3 view = vec3(0.0, 0.0, -1.0);
mat4 read_model_matrix = mat4(1.0);
mat3 model_normal_matrix = mat3(1.0);
// Writable so vertex code can override them (billboard shaders).
mat4 rt_modelview_matrix = mat4(1.0);
mat3 rt_modelview_normal_matrix = mat3(1.0);
mat4 read_view_matrix = mat4(1.0);
mat4 inv_view_matrix = mat4(1.0);
mat4 projection_matrix = mat4(1.0);
mat4 inv_projection_matrix = mat4(1.0);
float global_time = 0.0;
vec2 read_viewport_size = vec2(1.0);
bool rt_front_facing = true;
vec2 rt_screen_uv = vec2(0.0);
vec4 rt_frag_coord = vec4(0.0);
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);
// Vertex-stage built-ins with no RT equivalent; writes are discarded.
vec4 position = vec4(0.0);
vec4 instance_custom = vec4(0.0); // Filled by hit setup only when the material uses INSTANCE_CUSTOM.
float rt_point_size = 1.0;
int rt_instance_id = 0;
int rt_vertex_id = 0;
float roughness = 0.5;
uvec4 bone_attrib = uvec4(0);
vec4 weight_attrib = vec4(0.0);
vec4 custom0_attrib = vec4(0.0);
vec4 custom1_attrib = vec4(0.0);
vec4 custom2_attrib = vec4(0.0);
vec4 custom3_attrib = vec4(0.0);
vec3 eye_offset = vec3(0.0);
float global_prev_time = 0.0;

// Global so rt_run_fragment_shader() can `return` on discard and keep its outputs.
vec3 albedo = vec3(1.0);
float alpha = 1.0;
float metallic = 0.0;
float specular = 0.5;
vec3 emission = vec3(0.0);
float emission_indirect = 1.0;
vec3 normal_map = vec3(0.5, 0.5, 1.0);
float normal_map_depth = 1.0;
float ao = 1.0;
float ao_light_affect = 0.0;
vec3 backlight = vec3(0.0);
float sss_strength = 0.0;
float rim = 0.0;
float rim_tint = 0.0;
float clearcoat = 0.0;
float clearcoat_roughness = 0.0;
float anisotropy = 0.0;
vec2 anisotropy_flow = vec2(1.0, 0.0);
float alpha_scissor_threshold = -1.0; // Negative until the shader writes ALPHA_SCISSOR_THRESHOLD; 0 keeps every texel, as in raster.
float alpha_hash_scale = 1.0;
vec3 light_vertex = vec3(0.0);
vec2 rt_point_coord = vec2(0.0);
float rt_depth = 0.0;
float premul_alpha = 1.0;
vec4 custom_radiance = vec4(0.0);
vec4 custom_irradiance = vec4(0.0);
vec4 transmittance_color = vec4(0.0);
float transmittance_depth = 0.0;
float transmittance_boost = 0.0;
// Raster-only builtin. FOG writes are discarded.
vec4 fog = vec4(0.0);

#ifndef ViewIndex
#define ViewIndex 0
#endif
#ifndef SHADER_SPACE_FAR
#define SHADER_SPACE_FAR 0.0
#endif

// Screen/depth textures are unavailable in RT -- alias to bindless slot 0
// so shaders that reference them still compile (reads return dummy values).
// Use SCENE_DEPTH instead of hint_depth_texture for correct RT behavior.
#define depth_buffer bindless_textures[0]
#define color_buffer bindless_textures[0]
#define normal_roughness_buffer bindless_textures[0]

// SCENE_DEPTH; far (0.0) except for primary transparent hits.
float rt_scene_depth = 0.0;

// No derivatives in hit shaders; non-zero keeps 1.0 / fwidth() finite.
const float RT_FWIDTH_EPSILON = 1e-6;

float rt_fwidth(float value) {
	return RT_FWIDTH_EPSILON;
}

vec2 rt_fwidth(vec2 value) {
	return vec2(RT_FWIDTH_EPSILON);
}

vec3 rt_fwidth(vec3 value) {
	return vec3(RT_FWIDTH_EPSILON);
}

vec4 rt_fwidth(vec4 value) {
	return vec4(RT_FWIDTH_EPSILON);
}

#include "raytracing_custom_texture_inc.glsl"

/* RT_CUSTOM_TEXTURE_DEFINES */
/* RT_CUSTOM_FRAGMENT_GLOBALS */

/* RT_CUSTOM_VERTEX_FUNCTION */
/* RT_CUSTOM_FRAGMENT_FUNCTION */
