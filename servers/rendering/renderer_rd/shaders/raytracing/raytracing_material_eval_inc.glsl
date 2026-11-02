// Material evaluation result - common output format for all hit groups.
// Standard materials and custom ShaderMaterials both produce this struct.
//
// Requires (before this file): raytracing_data_inc.glsl, bindless_textures[],
// raytracing_samplers_inc.glsl.

struct MaterialResult {
	vec3 albedo;
	float alpha;
	float roughness;
	float metalness;
	float specular; // Dielectric specular reflectance control [0..1], default 0.5 -> F0 = 0.04.
	vec3 emissive;
	float emission_indirect; // EMISSION_INDIRECT: weight of emissive (and unshaded) radiance on indirect paths.
	vec3 normal; // Final shading normal (world space, after normal mapping).
	vec3 backlight; // Light "leaking through" thin/translucent surfaces (BACKLIGHT).
};

// Sensible default for a mid-grey diffuse surface.
MaterialResult default_material_result(vec3 geometry_normal) {
	MaterialResult r;
	r.albedo = vec3(0.8);
	r.alpha = 1.0;
	r.roughness = 0.5;
	r.metalness = 0.0;
	r.specular = 0.5;
	r.emissive = vec3(0.0);
	r.emission_indirect = 1.0;
	r.normal = geometry_normal;
	r.backlight = vec3(0.0);
	return r;
}

// ============================================================================
// TEXTURE SAMPLING
// ============================================================================

// Sample a MaterialData *_texture with the appropriate filter based on material flags. p_ddx/p_ddy: ray cone
// footprint (MaterialUV::uv_ddx); zero samples the most detailed mip, as ray tracing stages have no derivatives.
vec4 sample_material_texture(uint p_texture, vec2 uv, vec2 p_ddx, vec2 p_ddy, uint mat_flags) {
	uint tex_idx = rt_texture_index(p_texture);
	if ((mat_flags & RT_MAT_FLAG_POINT_FILTER) != 0u) {
		return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv, p_ddx, p_ddy);
	}
	return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, p_ddx, p_ddy);
}

vec4 sample_material_texture(uint p_texture, vec2 uv, uint mat_flags) {
	return sample_material_texture(p_texture, uv, vec2(0.0), vec2(0.0), mat_flags);
}

// ============================================================================
// UV ADDRESSING (plain UV1/UV2 or triplanar)
// ============================================================================

// Resolved texture addressing for one hit. Mirrors the UV setup that
// BaseMaterial3D generates for the rasterizer, so both mapping modes can be
// sampled through a single entry point.
struct MaterialUV {
	vec2 uv; // Mesh UV with scale/offset applied (plain mapping).
	vec3 triplanar_pos; // Scaled/offset position, object or world space.
	vec3 triplanar_weight; // Per-axis blend weights, normalized to sum 1.
	vec3 blend_normal; // Normal the weights came from, in the mapping space.
	bool triplanar;
	bool object_space; // Triplanar mapping runs in model space, not world space.
	// Ray cone UV footprint, zero when disabled. Only the lengths are valid.
	vec2 uv_ddx;
	vec2 uv_ddy;
};

// Addressing parameters of one BaseMaterial3D UV channel.
struct MaterialUVChannel {
	vec3 scale;
	vec3 offset;
	float blend_sharpness;
	bool triplanar;
	bool object_space;
};

MaterialUVChannel material_uv1_channel(MaterialData mat) {
	MaterialUVChannel ch;
	ch.scale = mat.uv1_scale;
	ch.offset = mat.uv1_offset;
	ch.blend_sharpness = unpackHalf2x16(mat.blend_sharpness).x;
	ch.triplanar = (mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u;
	ch.object_space = (mat.flags & RT_MAT_FLAG_TRIPLANAR_WORLD) == 0u;
	return ch;
}

MaterialUVChannel material_uv2_channel(MaterialData mat) {
	MaterialUVChannel ch;
	vec2 z = unpackHalf2x16(mat.uv2_scale_offset_z);
	ch.scale = vec3(unpackHalf2x16(mat.uv2_scale_xy), z.x);
	ch.offset = vec3(unpackHalf2x16(mat.uv2_offset_xy), z.y);
	ch.blend_sharpness = unpackHalf2x16(mat.blend_sharpness).y;
	ch.triplanar = (mat.flags & RT_MAT_FLAG_UV2_TRIPLANAR) != 0u;
	ch.object_space = (mat.flags & RT_MAT_FLAG_UV2_TRIPLANAR_WORLD) == 0u;
	return ch;
}

// Build the addressing for a hit. `object_pos`/`object_normal` must be in
// decompressed model space (what the rasterizer sees as VERTEX/NORMAL);
// `world_pos`/`world_normal` are only used by world triplanar mode.
MaterialUV material_uv_compute(MaterialUVChannel ch, vec2 mesh_uv,
		vec3 object_pos, vec3 object_normal, vec3 world_pos, vec3 world_normal) {
	MaterialUV muv;
	muv.uv = mesh_uv * ch.scale.xy + ch.offset.xy;
	muv.triplanar_pos = vec3(0.0);
	muv.triplanar_weight = vec3(0.0);
	muv.blend_normal = world_normal;
	muv.triplanar = ch.triplanar;
	muv.object_space = ch.object_space;
	muv.uv_ddx = vec2(0.0);
	muv.uv_ddy = vec2(0.0);

	if (muv.triplanar) {
		vec3 pos = muv.object_space ? object_pos : world_pos;
		muv.blend_normal = muv.object_space ? object_normal : world_normal;

		vec3 weight = pow(abs(muv.blend_normal), vec3(ch.blend_sharpness));
		muv.triplanar_weight = weight / max(dot(weight, vec3(1.0)), 0.0001);

		muv.triplanar_pos = (pos * ch.scale + ch.offset) * vec3(1.0, -1.0, 1.0);
	}

	return muv;
}

// Ray-tracing stages only carry world-space hit data, so recover the model
// space that local triplanar mapping needs.
MaterialUV material_uv_channel_from_world(MaterialUVChannel ch, vec2 mesh_uv,
		vec3 world_pos, vec3 world_normal) {
	vec3 object_pos = vec3(0.0);
	vec3 object_normal = vec3(0.0);

	if (ch.triplanar && ch.object_space) {
		mat4 world_to_model = mat4(gl_WorldToObjectEXT);
		object_pos = (world_to_model * vec4(world_pos, 1.0)).xyz;
		object_normal = normalize(mat3(world_to_model) * world_normal);
	}

	return material_uv_compute(ch, mesh_uv, object_pos, object_normal, world_pos, world_normal);
}

MaterialUV material_uv_from_world(MaterialData mat, GeometryData geom, vec2 mesh_uv,
		vec3 world_pos, vec3 world_normal) {
	return material_uv_channel_from_world(material_uv1_channel(mat), mesh_uv, world_pos, world_normal);
}

// Sample a material texture through the resolved addressing.
vec4 material_uv_sample(uint tex_idx, MaterialUV muv, uint mat_flags) {
	if (!muv.triplanar) {
		return sample_material_texture(tex_idx, muv.uv, muv.uv_ddx, muv.uv_ddy, mat_flags);
	}

	vec3 p = muv.triplanar_pos;
	vec3 w = muv.triplanar_weight;
	vec4 samp = sample_material_texture(tex_idx, p.xy, muv.uv_ddx, muv.uv_ddy, mat_flags) * w.z;
	samp += sample_material_texture(tex_idx, p.xz, muv.uv_ddx, muv.uv_ddy, mat_flags) * w.y;
	samp += sample_material_texture(tex_idx, p.zy * vec2(-1.0, 1.0), muv.uv_ddx, muv.uv_ddy, mat_flags) * w.x;
	return samp;
}

// World-space tangent frame implied by triplanar mapping. Normal maps on a
// triplanar material are authored against these axes rather than the mesh
// tangents, so they replace the fetched frame.
void material_uv_triplanar_tangents(MaterialUV muv, out vec3 tangent, out vec3 binormal) {
	vec3 n = abs(muv.blend_normal);

	tangent = vec3(0.0, 0.0, -1.0) * n.x;
	tangent += vec3(1.0, 0.0, 0.0) * n.y;
	tangent += vec3(1.0, 0.0, 0.0) * n.z;
	tangent = normalize(tangent);

	binormal = vec3(0.0, 1.0, 0.0) * n.x;
	binormal += vec3(0.0, 0.0, -1.0) * n.y;
	binormal += vec3(0.0, 1.0, 0.0) * n.z;
	binormal = normalize(binormal);

	if (muv.object_space) {
		// Match compute_hit_data: normalized columns keep uniformly scaled
		// instances from skewing the frame.
		mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
		mat3 object_to_world = mat3(
				normalize(model_rotation[0]),
				normalize(model_rotation[1]),
				normalize(model_rotation[2]));
		tangent = normalize(object_to_world * tangent);
		binormal = normalize(object_to_world * binormal);
	}
}

// ============================================================================
// ROUGHNESS / METALLIC / AO
// ============================================================================

// dot(texel, mask) with the BaseMaterial3D texture channel mask of p_texture.
float material_texture_channel(vec4 p_texel, uint p_texture) {
	uint channel = rt_texture_channel(p_texture);
	return channel == RT_TEXTURE_CHANNEL_GRAYSCALE ? dot(p_texel.rgb, vec3(0.333333)) : p_texel[channel];
}

// ROUGHNESS and METALLIC (x, y) of BaseMaterial3D from already sampled textures.
vec2 material_roughness_metallic_from_texels(MaterialData mat, vec4 p_roughness_tex, vec4 p_metallic_tex) {
	vec3 params = rt_material_surface_params(mat);
	return clamp(vec2(
						 material_texture_channel(p_roughness_tex, mat.roughness_texture) * params.x,
						 material_texture_channel(p_metallic_tex, mat.metallic_texture) * params.y),
			0.0, 1.0);
}

// ROUGHNESS and METALLIC (x, y) of BaseMaterial3D. ORM materials share one texture fetch.
vec2 material_roughness_metallic(MaterialData mat, MaterialUV muv) {
	vec4 roughness_tex = material_uv_sample(mat.roughness_texture, muv, mat.flags);
	vec4 metallic_tex = rt_texture_index(mat.metallic_texture) == rt_texture_index(mat.roughness_texture)
			? roughness_tex
			: material_uv_sample(mat.metallic_texture, muv, mat.flags);
	return material_roughness_metallic_from_texels(mat, roughness_tex, metallic_tex);
}

// AO of BaseMaterial3D (1.0 when disabled). UV1 only: AO on UV2 is not mirrored, and the
// path tracer only shows AO in debug views since it traces occlusion itself.
float material_ao(MaterialData mat, MaterialUV muv) {
	return material_texture_channel(material_uv_sample(mat.ao_texture, muv, mat.flags), mat.ao_texture);
}

// ============================================================================
// EMISSION
// ============================================================================

// Raw emission output of BaseMaterial3D for one hit (before exposure
// normalization). Mirrors the generated fragment code: the emission texture
// defaults to black, and the operator combines it with the emission color.
// p_emission_uv: UV1 addressing, or UV2 with RT_MAT_FLAG_EMISSION_UV2.
vec3 material_emission(MaterialData mat, MaterialUV p_emission_uv) {
	vec3 emission_tex = vec3(0.0);
	if ((mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
		emission_tex = material_uv_sample(mat.emission_texture, p_emission_uv, mat.flags).rgb;
	}
	vec3 emission = (mat.flags & RT_MAT_FLAG_EMISSION_ADD) != 0u
			? mat.emission_color + emission_tex
			: mat.emission_color * emission_tex;
	return emission * mat.emission_strength;
}
