// Body snippet of fragment_shader() in scene_forward_clustered.glsl, included once after lighting and fog.
// Replaces the lit color with material_debug_output from material_debug_capture_inc.glsl.

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)
if (sc_material_debug_mode() != 0u) {
	emission = material_debug_output;
	ambient_light = vec3(0.0);
	diffuse_light = vec3(0.0);
	direct_specular_light = vec3(0.0);
	indirect_specular_light = vec3(0.0);
	sss_strength = 0.0;
	fog = vec4(0.0, 0.0, 0.0, 1.0);
}
#endif // !MODE_RENDER_DEPTH && !MODE_UNSHADED
