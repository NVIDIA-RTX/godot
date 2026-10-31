// Froxel fog lookups for the path tracer. The froxel grid follows the unjittered
// camera frustum, so only camera segments (bounce 0) can sample it.

layout(set = 0, binding = 35) uniform texture3D rt_volumetric_fog_texture;

bool rt_volumetric_fog_enabled() {
	return (RT_FLAGS & RT_FLAG_VOLUMETRIC_FOG_ENABLED) != 0u && get_rt_param(RT_PARAM_VOLUMETRIC_FOG_INV_LENGTH) > 0.0;
}

bool rt_fog_use_legacy_blending() {
	return get_rt_param(RT_PARAM_FOG_USE_LEGACY_BLENDING) > 0.5;
}

vec4 rt_volumetric_fog_sample(vec2 screen_uv, float z) {
	vec3 fog_pos = vec3(screen_uv, z * get_rt_param(RT_PARAM_VOLUMETRIC_FOG_INV_LENGTH));
	if (fog_pos.z < 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	} else if (fog_pos.z < 1.0) {
		fog_pos.z = pow(fog_pos.z, get_rt_param(RT_PARAM_VOLUMETRIC_FOG_DETAIL_SPREAD));
	}
	return textureLod(sampler3D(rt_volumetric_fog_texture, SAMPLER_LINEAR_CLAMP), fog_pos, 0.0);
}

vec4 rt_volumetric_fog_at(vec3 world_pos, vec3 view_vertex) {
	return rt_volumetric_fog_sample(project_uv(world_pos, curr_vp_unjittered), -view_vertex.z);
}

// Same as the raster pass, p_fog is premultiplied inscatter and transmittance.
vec4 rt_combine_fog(vec4 p_fog, bool p_use_fog, vec4 p_volumetric_fog) {
	vec4 res;
	if (p_use_fog) {
		if (rt_fog_use_legacy_blending()) {
			res.a = mix(0.0, p_fog.a, p_volumetric_fog.a);
			res.rgb = mix(p_volumetric_fog.rgb, p_fog.rgb, p_volumetric_fog.a);
		} else {
			res.a = p_fog.a * p_volumetric_fog.a;
			res.rgb = p_fog.rgb * p_volumetric_fog.a + p_volumetric_fog.rgb;
		}
	} else {
		res = p_volumetric_fog;
		if (rt_fog_use_legacy_blending()) {
			res.rgb *= 1.0 - res.a;
		}
	}
	return res;
}

vec4 rt_volumetric_fog_sky_sample(vec2 p_screen_uv) {
	return textureLod(sampler3D(rt_volumetric_fog_texture, SAMPLER_LINEAR_CLAMP), vec3(p_screen_uv, 1.0), 0.0);
}

vec3 rt_volumetric_fog_apply_sky(vec3 p_sky, vec2 p_screen_uv) {
	vec4 fog = rt_volumetric_fog_sky_sample(p_screen_uv);
	float sky_affect = get_rt_param(RT_PARAM_VOLUMETRIC_FOG_SKY_AFFECT);
	if (rt_fog_use_legacy_blending()) {
		return mix(p_sky, fog.rgb, (1.0 - fog.a) * sky_affect);
	}
	return mix(p_sky, p_sky * fog.a + fog.rgb, sky_affect);
}

#define RT_VIS_VOLUMETRIC_FOG 25
#define RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE 26

// Magenta when the froxel grid is not bound for this viewport.
vec3 rt_volumetric_fog_debug(int p_vis_mode, vec4 p_fog) {
	if (!rt_volumetric_fog_enabled()) {
		return vec3(1.0, 0.0, 1.0);
	}
	return p_vis_mode == RT_VIS_VOLUMETRIC_FOG ? p_fog.rgb : vec3(p_fog.a);
}
