// Body snippet of fragment_shader() in scene_forward_clustered.glsl, included once after the material
// inputs are final and before lighting modifies ao. Declares material_debug_output, which
// material_debug_apply_inc.glsl draws instead of the lit color.

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)
vec3 material_debug_output = vec3(0.0);
if (sc_material_debug_mode() != 0u) {
	mat3 view_to_world = mat3(inv_view_matrix);
	MaterialDebugInputs dbg = material_debug_inputs_default();
#ifdef NORMAL_USED
	vec3 dbg_geometry_normal = normalize(normal_interp);
#if defined(DO_SIDE_CHECK)
	if (!gl_FrontFacing) {
		dbg_geometry_normal = -dbg_geometry_normal;
	}
#endif // DO_SIDE_CHECK
	dbg.geometry_normal = normalize(view_to_world * dbg_geometry_normal);
	dbg.normal = normalize(view_to_world * normal);
	dbg.view_normal = normal;
	dbg.NdotV = max(dot(normal, view), 0.0001);
#endif // NORMAL_USED
#ifdef NORMAL_MAP_USED
	dbg.tangent_space_normal = normal_map;
#endif
#ifdef TANGENT_USED
	dbg.tangent = normalize(view_to_world * tangent);
	dbg.bitangent = normalize(view_to_world * binormal);
#endif
#ifdef UV_USED
	dbg.uv = uv;
#endif
	dbg.albedo = albedo;
	dbg.ao = ao;
	dbg.roughness = roughness;
	dbg.metallic = metallic;
	dbg.specular = specular;
	dbg.emission = emission;
	dbg.front_facing = gl_FrontFacing;
	dbg.depth = length(vertex - eye_offset) / (scene_data.z_far - scene_data.z_near);
	material_debug_output = material_debug_color(int(sc_material_debug_mode()), dbg);
}
#endif // !MODE_RENDER_DEPTH && !MODE_UNSHADED
