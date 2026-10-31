// Requires raytracing_inc.glsl, raytracing_hit_inc.glsl and raytracing_entry_material_inc.glsl. The includer
// declares tlas, geometries[], materials[], bindless_textures[] and SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT.

vec4 ray_query_surface(uint geometry_idx, uint primitive_id, vec2 candidate_bary, float lod) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	MaterialData mat = rt_entry_material(geometry_idx);
	uv = uv * mat.uv1_scale.xy + mat.uv1_offset.xy;
	vec4 albedo = textureLod(sampler2D(bindless_textures[nonuniformEXT(rt_texture_index(mat.albedo_texture))], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, lod);
	albedo *= mat.albedo_color;
	albedo.a = rt_entry_alpha(mat, albedo.a, 0.5);
	if ((mat.flags & RT_MAT_BLEND_CLASS_MASK) == RT_BLEND_CLASS_PREMULT && albedo.a > 1e-6) {
		albedo.rgb /= albedo.a;
	}

	return clamp(albedo, vec4(0.0), vec4(1.0));
}

float ray_query_alpha(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_surface(geometry_idx, primitive_id, candidate_bary, 0.0).a;
}

bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_alpha(geometry_idx, primitive_id, candidate_bary) >= 0.5;
}

// Colored transmittance along the ray, 0 when an opaque caster is hit. Transparent casters tint the light
// stochastically by coverage, so all candidates must be visited (no opaque traversal flags).
vec3 ray_query_shadow_transmittance(vec3 origin, vec3 direction, float t_min, float t_max, uint mask, float lod, inout uint rng_state) {
	vec3 transmittance = vec3(1.0);
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT, mask, origin, t_min, direction, t_max);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (rayQueryGetIntersectionTypeEXT(shadow_rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) {
			continue;
		}
		uint geometry_idx = RT_RQ_GEOMETRY_IDX(shadow_rq, false);
		uint mat_flags = rt_entry_material_flags(geometry_idx);
		if ((mat_flags & (RT_MAT_FLAG_TRANSPARENT | RT_MAT_FLAG_ALPHA_SCISSOR)) == 0u) {
			rayQueryConfirmIntersectionEXT(shadow_rq);
			continue;
		}

		bool transparent_shadow = (mat_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
		// The albedo sample below would be meaningless, so like raster, cast no shadow. Faded opaque
		// materials only need the instance fade.
		if (transparent_shadow && (mat_flags & (RT_MAT_FLAG_NO_RAY_QUERY_ALPHA | RT_MAT_FLAG_INSTANCE_FADE)) == RT_MAT_FLAG_NO_RAY_QUERY_ALPHA) {
			continue;
		}

		vec4 surface = ray_query_surface(geometry_idx, rayQueryGetIntersectionPrimitiveIndexEXT(shadow_rq, false), rayQueryGetIntersectionBarycentricsEXT(shadow_rq, false), lod);
		if (transparent_shadow && rand(rng_state) >= surface.a) {
			transmittance *= mix(vec3(1.0), surface.rgb, surface.a);
		} else if (transparent_shadow || surface.a >= 0.5) {
			rayQueryConfirmIntersectionEXT(shadow_rq);
		}
	}

	return rayQueryGetIntersectionTypeEXT(shadow_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? transmittance : vec3(0.0);
}
