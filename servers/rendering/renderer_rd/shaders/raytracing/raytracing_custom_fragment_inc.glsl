// Custom shader fragment setup for RT hit groups (closest-hit and any-hit).
// Include inside main() under #ifdef RT_CUSTOM_HIT_GROUP.
//
// Required variables before inclusion:
//   uint  rt_geometry_idx   -- geometry/material index
//   vec3  rt_hit_pos        -- world-space hit position
//   vec2  rt_uv             -- interpolated UV
//   vec2  rt_uv2            -- interpolated UV2
//   vec3  rt_normal         -- world-space geometry normal (flipped like raster DO_SIDE_CHECK)
//   vec3  rt_tangent        -- world-space tangent
//   vec3  rt_bitangent      -- world-space bitangent
//   bool  rt_front_face     -- FRONT_FACING equivalent
//
// Required bindings/types:
//   rt_entry_material(), CustomMaterialUniforms, scene_data_block

rt_mat = rt_entry_material(rt_geometry_idx);
material = CustomMaterialUniforms(rt_mat.uniform_address);

// Matrices.
mat4 rt_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
		scene_data_block.data.view_matrix[1],
		scene_data_block.data.view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));

GeometryData rt_geom = geometries[rt_geometry_idx];
#ifdef RT_USES_INSTANCE_CUSTOM
instance_custom = fetch_instance_custom(rt_geom, uint(gl_PrimitiveID));
#endif
read_model_matrix = mat4(gl_ObjectToWorldEXT);
// Matches the rasterizer's uniform-scale case; instance scale flags that would
// select the inverse-transpose form are not available to hit shaders.
model_normal_matrix = mat3(read_model_matrix);
read_view_matrix = rt_view_matrix;
inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
		scene_data_block.data.inv_view_matrix[1],
		scene_data_block.data.inv_view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));
projection_matrix = scene_data_block.data.projection_matrix;
inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
read_viewport_size = scene_data_block.data.viewport_size;
global_time = scene_data_block.data.time;

mat4 rt_world_to_object = mat4(gl_WorldToObjectEXT);

vertex = (rt_world_to_object * vec4(rt_hit_pos, 1.0)).xyz;
normal = mat3(rt_world_to_object) * rt_normal;
tangent = mat3(rt_world_to_object) * rt_tangent;
binormal = mat3(rt_world_to_object) * rt_bitangent;
uv_interp = rt_uv;
uv2_interp = rt_uv2;
color_interp = rt_color * fetch_instance_color(rt_geom, uint(gl_PrimitiveID));
// VIEW is view-space in the rasterizer; rotate the incoming ray to match.
view = -normalize(mat3(rt_view_matrix) * gl_WorldRayDirectionEXT);
rt_front_facing = rt_front_face;
rt_screen_uv = vec2(gl_LaunchIDEXT.xy) / vec2(gl_LaunchSizeEXT.xy);
rt_frag_coord = vec4(gl_LaunchIDEXT.xy, 0.0, 1.0);

mat4 rt_default_modelview_matrix = rt_view_matrix * read_model_matrix;
rt_modelview_matrix = rt_default_modelview_matrix;
rt_modelview_normal_matrix = mat3(rt_modelview_matrix);

// Captured geometry already went through vertex(); re-running it must only produce the varyings.
bool rt_captured = (rt_geom.flags & FLAG_CAPTURED) != 0u;
vec3 rt_captured_vertex = vertex;
vec3 rt_captured_normal = normal;
vec3 rt_captured_tangent = tangent;
vec3 rt_captured_binormal = binormal;

// Run vertex shader (computes varyings, may modify built-ins).
/* RT_CUSTOM_VERTEX_CALL */

if (rt_captured) {
	vertex = rt_captured_vertex;
	normal = rt_captured_normal;
	tangent = rt_captured_tangent;
	binormal = rt_captured_binormal;
	rt_modelview_matrix = rt_default_modelview_matrix;
	rt_modelview_normal_matrix = mat3(rt_modelview_matrix);
}

// Post-vertex transform: object-space -> view-space (mirrors rasterizer post-vertex,
// honoring MODELVIEW_MATRIX overrides from the vertex code).
vertex = (rt_modelview_matrix * vec4(vertex, 1.0)).xyz;
normal = normalize(rt_modelview_normal_matrix * normal);
tangent = normalize(rt_modelview_normal_matrix * tangent);
binormal = normalize(rt_modelview_normal_matrix * binormal);

#ifdef RT_USES_SCENE_DEPTH
rt_scene_depth = payload_peel_scene_depth(payload);
#endif

// Fragment outputs are globals, reset per invocation.
light_vertex = vertex;

// A call so that `discard` (rewritten to `return`) only exits the fragment code.
rt_run_fragment_shader();
