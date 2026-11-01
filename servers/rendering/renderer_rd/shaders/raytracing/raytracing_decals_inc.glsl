// Decals for path tracing. Mirrors the decal block of scene_forward_clustered.glsl, but finds the
// decals covering a hit through a volume TLAS (one AABB instance per decal) instead of the cluster grid,
// and samples the decal's own textures through bindless_textures[] instead of the raster decal atlas.
//
// Required (before this file): raytracing_closest_hit_common_inc.glsl (HitData, MaterialResult, payload,
// geometries[], bindless_textures[], scene_data_block, rt_params) and GL_EXT_ray_query.

#ifdef RT_DECALS_ENABLED

// Must match RT_VOLUME_MASK_* in render_raytracing.h.
#define RT_VOLUME_MASK_DECAL 0x01u

// Must match RT_DECAL_NO_TEXTURE in render_raytracing.h.
#define RT_DECAL_NO_TEXTURE 0xFFFFFFFFu

// Indices into RTDecalData.textures, must match RSE::DecalTexture.
#define RT_DECAL_TEXTURE_ALBEDO 0
#define RT_DECAL_TEXTURE_NORMAL 1
#define RT_DECAL_TEXTURE_ORM 2
#define RT_DECAL_TEXTURE_EMISSION 3

// Must match SceneShaderRaytracing::RTDecalMode.
#define RT_DECAL_MODE_ALL_HITS 0u
#define RT_DECAL_MODE_SKIP_AFTER_DIFFUSE 1u
#define RT_DECAL_MODE_SINGULAR_ONLY 2u

// Overlapping decals applied per point. Past this, the bottom-most ones are dropped.
#define RT_DECAL_MAX_OVERLAP 4

#define RT_VIS_DECAL_OVERLAP 28

// Must match RT_DecalData in render_raytracing.h (std430, 176 bytes). World space.
struct RTDecalData {
	mat3x4 world_to_box; // Rows of the world to unit box (x/z in [0, 1], y in [-1, 1]) affine transform.
	mat3x4 normal_basis; // Columns (xyz) of the orthonormal decal basis.
	vec3 normal; // Projection direction (decal +Y).
	float normal_fade;
	vec4 modulate; // Linear; alpha includes distance fade.
	float emission_energy; // Includes distance fade.
	float albedo_mix;
	float upper_fade;
	float lower_fade;
	uvec4 textures; // Bindless indices; RT_DECAL_NO_TEXTURE when unset.
	uint mask;
	uint pad0;
	uint pad1;
	uint pad2;
};

layout(set = 0, binding = 36) uniform accelerationStructureEXT rt_decal_tlas;

// clang-format off
layout(set = 0, binding = 37, std430) readonly buffer RTDecalBuffer {
	RTDecalData rt_decals[];
};
// clang-format on

layout(set = 0, binding = 38) uniform sampler rt_decal_sampler;

// Top-most decals covering a point, in ascending index order (index 0 is drawn on top).
struct RTDecalStack {
	uint index[RT_DECAL_MAX_OVERLAP];
	float fade[RT_DECAL_MAX_OVERLAP];
	uint count; // Entries used in index/fade.
	uint total; // All covering decals, including dropped ones.
};

vec3 rt_decal_uv_local(uint p_decal, vec3 p_pos) {
	return vec4(p_pos, 1.0) * rt_decals[p_decal].world_to_box;
}

// Mip 0 sample of a decal texture at uv_local.xz. Callers check for RT_DECAL_NO_TEXTURE.
vec4 rt_decal_sample(uint p_texture, vec2 p_uv) {
	return textureLod(sampler2D(bindless_textures[nonuniformEXT(p_texture)], rt_decal_sampler), p_uv, 0.0);
}

// Opacity of a decal at a point, or a negative value when the point is outside its box or masked out.
float rt_decal_coverage(uint p_decal, vec3 p_pos, vec3 p_geometry_normal, uint p_layers) {
	if ((rt_decals[p_decal].mask & p_layers) == 0u) {
		return -1.0;
	}
	vec3 uv_local = rt_decal_uv_local(p_decal, p_pos);
	if (any(lessThan(uv_local, vec3(0.0, -1.0, 0.0))) || any(greaterThan(uv_local, vec3(1.0)))) {
		return -1.0;
	}
	float fade = pow(1.0 - abs(uv_local.y), uv_local.y > 0.0 ? rt_decals[p_decal].upper_fade : rt_decals[p_decal].lower_fade);
	if (rt_decals[p_decal].normal_fade > 0.0) {
		fade *= smoothstep(rt_decals[p_decal].normal_fade, 1.0, dot(p_geometry_normal, rt_decals[p_decal].normal) * 0.5 + 0.5);
	}
	return fade;
}

// Keeps the RT_DECAL_MAX_OVERLAP lowest indices, sorted ascending.
void rt_decal_stack_insert(inout RTDecalStack r_stack, uint p_decal, float p_fade) {
	const uint last = uint(RT_DECAL_MAX_OVERLAP - 1);
	r_stack.total++;
	uint slot = r_stack.count;
	if (slot > last) {
		if (p_decal > r_stack.index[last]) {
			return;
		}
		slot = last;
	} else {
		r_stack.count++;
	}
	for (; slot > 0u && r_stack.index[slot - 1u] > p_decal; slot--) {
		r_stack.index[slot] = r_stack.index[slot - 1u];
		r_stack.fade[slot] = r_stack.fade[slot - 1u];
	}
	r_stack.index[slot] = p_decal;
	r_stack.fade[slot] = p_fade;
}

RTDecalStack rt_decal_gather(vec3 p_pos, vec3 p_geometry_normal, uint p_layers) {
	RTDecalStack stack;
	stack.count = 0u;
	stack.total = 0u;

	// A near-zero-length ray reports every volume containing the point. Candidates are
	// conservative, so each is re-tested against its box.
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, rt_decal_tlas, gl_RayFlagsNoneEXT, RT_VOLUME_MASK_DECAL, p_pos, 0.0, vec3(0.0, 1.0, 0.0), 1e-4);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionAABBEXT) {
			continue;
		}
		uint decal = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
		float fade = rt_decal_coverage(decal, p_pos, p_geometry_normal, p_layers);
		if (fade > 0.0) {
			rt_decal_stack_insert(stack, decal, fade);
		}
	}
	return stack;
}

void rt_decal_apply(uint p_decal, float p_fade, vec3 p_pos, inout MaterialResult r_m) {
	vec2 uv = rt_decal_uv_local(p_decal, p_pos).xz;
	uvec4 textures = rt_decals[p_decal].textures;

	if (textures[RT_DECAL_TEXTURE_ALBEDO] != RT_DECAL_NO_TEXTURE) {
		vec4 decal_albedo = rt_decal_sample(textures[RT_DECAL_TEXTURE_ALBEDO], uv);
		decal_albedo *= rt_decals[p_decal].modulate;
		decal_albedo.a *= p_fade;
		r_m.albedo = mix(r_m.albedo, decal_albedo.rgb, decal_albedo.a * rt_decals[p_decal].albedo_mix);

		if (textures[RT_DECAL_TEXTURE_NORMAL] != RT_DECAL_NO_TEXTURE) {
			vec3 decal_normal = rt_decal_sample(textures[RT_DECAL_TEXTURE_NORMAL], uv).xyz;
			decal_normal.xy = decal_normal.xy * vec2(2.0, -2.0) - vec2(1.0, -1.0); // Flipped y, like raster.
			decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));
			decal_normal = (rt_decals[p_decal].normal_basis * decal_normal.xzy).xyz; // Y is up in decal space.
			r_m.normal = normalize(mix(r_m.normal, decal_normal, decal_albedo.a));
		}

		if (textures[RT_DECAL_TEXTURE_ORM] != RT_DECAL_NO_TEXTURE) {
			vec3 decal_orm = rt_decal_sample(textures[RT_DECAL_TEXTURE_ORM], uv).xyz;
			r_m.roughness = mix(r_m.roughness, decal_orm.g, decal_albedo.a);
			r_m.metalness = mix(r_m.metalness, decal_orm.b, decal_albedo.a);
		}
	}

	if (textures[RT_DECAL_TEXTURE_EMISSION] != RT_DECAL_NO_TEXTURE) {
		// Additive, independent of albedo.
		vec3 decal_emission = rt_decal_sample(textures[RT_DECAL_TEXTURE_EMISSION], uv).rgb;
		r_m.emissive += decal_emission * rt_decals[p_decal].modulate.rgb * rt_decals[p_decal].emission_energy * p_fade * scene_data_block.data.emissive_exposure_normalization;
	}
}

bool rt_decals_enabled_for_path(uint p_packed_bounces_flags) {
	if (get_rt_param(RT_PARAM_DECAL_COUNT) <= 0.0) {
		return false;
	}
	uint mode = uint(get_rt_param(RT_PARAM_DECAL_MODE));
	if (mode == RT_DECAL_MODE_SKIP_AFTER_DIFFUSE) {
		return get_diffuse_bounces(p_packed_bounces_flags) == 0u;
	}
	if (mode == RT_DECAL_MODE_SINGULAR_ONLY) {
		return is_singular_path(p_packed_bounces_flags);
	}
	return true;
}

#ifdef RT_DEBUG_ENABLED
// Black: none. Green, yellow, orange, red: 1-4 decals. Magenta: over RT_DECAL_MAX_OVERLAP (some dropped).
void rt_decal_debug_overlap(uint p_total) {
	const vec3 colors[RT_DECAL_MAX_OVERLAP + 1] = vec3[](vec3(0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 0.0, 0.0));
	PathState ps = path_unpack(payload);
	ps.hit_t = gl_HitTEXT;
	ps.radiance = p_total > uint(RT_DECAL_MAX_OVERLAP) ? vec3(1.0, 0.0, 1.0) : colors[p_total];
	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	path_pack(payload, ps);
}
#endif

// Projects the decals covering the hit onto r_m, bottom to top.
// Returns true when the Decal Overlap debug view consumed the hit (payload already written).
bool rt_decals_process(HitData p_h, inout MaterialResult r_m) {
#ifdef RT_DEBUG_ENABLED
	if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_DECAL_OVERLAP) {
		RTDecalStack stack = rt_decal_gather(p_h.hit_pos, p_h.geometry_normal, geometries[p_h.geometry_idx].layers);
		rt_decal_debug_overlap(stack.total);
		return true;
	}
#endif
	if (!rt_decals_enabled_for_path(payload.packed_bounces_flags)) {
		return false;
	}
	RTDecalStack stack = rt_decal_gather(p_h.hit_pos, p_h.geometry_normal, geometries[p_h.geometry_idx].layers);
	for (uint i = stack.count; i > 0u; i--) {
		rt_decal_apply(stack.index[i - 1u], stack.fade[i - 1u], p_h.hit_pos, r_m);
	}
	return false;
}

#endif // RT_DECALS_ENABLED
