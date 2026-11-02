// Shared fog evaluation used by both rasterized (forward_clustered/mobile) and
// ray-traced paths.  Requires scene_data_inc.glsl to be included first so that
// SceneData and the SCENE_DATA_FLAGS_* defines are available.
//
// Caller must define before including this file:
//   vec3 fog_get_directional_color(uint index)
//   vec3 fog_get_directional_direction(uint index)
//
// Optionally define FOG_HAS_RADIANCE and provide:
//   vec3 fog_sample_radiance(vec3 view_dir, float mip_level)

vec4 fog_process(SceneData scene_data, vec3 vertex) {
	vec3 fog_color = scene_data.fog_light_color;
	bool use_depth_fog = (scene_data.flags & SCENE_DATA_FLAGS_USE_DEPTH_FOG) != 0u;

#ifdef FOG_HAS_RADIANCE
	if (scene_data.fog_aerial_perspective > 0.0) {
		float mip_level = mix(
				1.0 / MAX_ROUGHNESS_LOD, 1.0,
				1.0 - (abs(vertex.z) - scene_data.z_near) / (scene_data.z_far - scene_data.z_near));
		vec3 sky_fog_color = fog_sample_radiance(vertex, mip_level);
		fog_color = mix(fog_color, sky_fog_color, scene_data.fog_aerial_perspective);
	}
#endif

	if (scene_data.fog_sun_scatter > 0.001) {
		vec3 view = normalize(vertex);
		for (uint i = 0u; i < scene_data.directional_light_count; i++) {
			vec3 light_color = fog_get_directional_color(i);
			vec3 light_dir = fog_get_directional_direction(i);
			float light_amount = pow(max(dot(view, light_dir), 0.0), 8.0);
			fog_color += light_color * light_amount * scene_data.fog_sun_scatter;
		}
	}

	float fog_amount = 0.0;

	if (use_depth_fog) {
		float fog_z = smoothstep(scene_data.fog_depth_begin, scene_data.fog_depth_end, length(vertex));
		fog_amount = pow(fog_z, scene_data.fog_depth_curve) * scene_data.fog_density;
	} else {
		fog_amount = 1.0 - exp(min(0.0, -length(vertex) * scene_data.fog_density));
	}

	if (abs(scene_data.fog_height_density) >= 0.0001) {
		mat4 inv_view_matrix = transpose(mat4(
				scene_data.inv_view_matrix[0],
				scene_data.inv_view_matrix[1],
				scene_data.inv_view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));

		float y = (inv_view_matrix * vec4(vertex, 1.0)).y;
		float y_dist = y - scene_data.fog_height;
		float vfog_amount = 1.0 - exp(min(0.0, y_dist * scene_data.fog_height_density));
		fog_amount = max(vfog_amount, fog_amount);
	}

	return vec4(fog_color, fog_amount);
}
