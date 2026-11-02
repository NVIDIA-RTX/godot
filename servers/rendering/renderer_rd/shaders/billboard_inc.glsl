// Billboard and fixed_size render modes; keep in sync with rendering_billboard.h.

#if defined(BILLBOARD_ENABLED) || defined(BILLBOARD_FIXED_Y) || defined(BILLBOARD_PARTICLES) || defined(FIXED_SIZE_USED)
#define BILLBOARD_RENDER_MODE_USED

void billboard_apply_render_modes(inout mat4 modelview, inout mat3 modelview_normal, mat4 view_matrix, mat4 main_cam_inv_view_matrix, mat4 inv_view_matrix, mat4 projection_matrix, vec4 custom, bool is_multiview) {
#if defined(BILLBOARD_ENABLED) || defined(BILLBOARD_FIXED_Y) || defined(BILLBOARD_PARTICLES)
#ifdef BILLBOARD_KEEP_SCALE
	vec3 model_scale = vec3(length(modelview[0].xyz), length(modelview[1].xyz), length(modelview[2].xyz));
#endif

	mat3 basis;
#if defined(BILLBOARD_ENABLED)
	// The main camera faces the billboard even in shadow passes.
	basis = mat3(main_cam_inv_view_matrix);
#elif defined(BILLBOARD_FIXED_Y)
	const vec3 up = vec3(0.0, 1.0, 0.0);
	vec3 right = cross(up, main_cam_inv_view_matrix[2].xyz);
	vec3 back = cross(main_cam_inv_view_matrix[0].xyz, up);
	if (dot(right, right) < 1e-10) {
		right = cross(up, back);
	}
	if (dot(back, back) < 1e-10) {
		back = cross(right, up);
	}
	basis = mat3(normalize(right), up, normalize(back));
#else
	float c = cos(custom.x);
	float s = sin(custom.x);
	basis = mat3(normalize(inv_view_matrix[0].xyz), normalize(inv_view_matrix[1].xyz), normalize(inv_view_matrix[2].xyz)) * mat3(vec3(c, -s, 0.0), vec3(s, c, 0.0), vec3(0.0, 0.0, 1.0));
#endif

#ifdef BILLBOARD_KEEP_SCALE
	basis[0] *= model_scale.x;
	basis[1] *= model_scale.y;
	basis[2] *= model_scale.z;
#endif

	mat3 view_basis = mat3(view_matrix) * basis;
	modelview[0] = vec4(view_basis[0], 0.0);
	modelview[1] = vec4(view_basis[1], 0.0);
	modelview[2] = vec4(view_basis[2], 0.0);
	modelview_normal = view_basis;
#endif

#ifdef FIXED_SIZE_USED
	float sc;
	if (projection_matrix[3][3] != 0.0) {
		// Orthogonal: consistent with vertical FOV (Keep Height).
		sc = abs(1.0 / (2.0 * projection_matrix[1][1])) * 2.0;
	} else if (is_multiview) {
		// Stereo: moving in the z-plane changes the apparent size, so use the full distance.
		sc = length(modelview[3].xyz);
	} else {
		sc = -modelview[3].z;
	}
	modelview[0] *= sc;
	modelview[1] *= sc;
	modelview[2] *= sc;
#endif
}

#endif
