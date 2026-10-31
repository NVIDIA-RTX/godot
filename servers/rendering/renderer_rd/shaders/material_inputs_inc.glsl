// Material input helpers and debug views shared by the path tracer and the
// forward clustered rasterizer, so Environment.pathtracing_debug_mode draws
// identical channels in both renderers.

#ifndef MATERIAL_INPUTS_INC_GLSL
#define MATERIAL_INPUTS_INC_GLSL

// Environment::PathtracingDebugMode values that only depend on material inputs.
// Must match SceneShaderForwardClustered::material_debug_mode_from_environment().
#define MATERIAL_DEBUG_GEOMETRY_NORMALS 2
#define MATERIAL_DEBUG_FINAL_NORMALS 3
#define MATERIAL_DEBUG_NORMAL_MAP 4
#define MATERIAL_DEBUG_TANGENT 5
#define MATERIAL_DEBUG_BITANGENT 6
#define MATERIAL_DEBUG_UV 7
#define MATERIAL_DEBUG_ALBEDO 8
#define MATERIAL_DEBUG_ORM 9
#define MATERIAL_DEBUG_DIFFUSE_ALBEDO 10
#define MATERIAL_DEBUG_SPECULAR_ALBEDO 11
#define MATERIAL_DEBUG_NORMAL_ROUGHNESS 12
#define MATERIAL_DEBUG_METALNESS 14
#define MATERIAL_DEBUG_ROUGHNESS 15
#define MATERIAL_DEBUG_VIEW_NORMALS 16
#define MATERIAL_DEBUG_DIFFUSE_SPECULAR_SPLIT 17
#define MATERIAL_DEBUG_FRESNEL_F0 18
#define MATERIAL_DEBUG_FRONT_BACK_FACE 19
#define MATERIAL_DEBUG_DEPTH 20
#define MATERIAL_DEBUG_EMISSIVE 21

// Dielectric F0 from BaseMaterial3D's specular parameter (0.5 -> 4%).
float material_specular_to_f0(float p_specular) {
	return 0.16 * p_specular * p_specular;
}

// Specular F0 for the metalness workflow.
vec3 material_specular_f0(vec3 p_albedo, float p_metallic, float p_specular) {
	return mix(vec3(material_specular_to_f0(p_specular)), p_albedo, p_metallic);
}

// Diffuse reflectance for the metalness workflow.
vec3 material_diffuse_albedo(vec3 p_albedo, float p_metallic) {
	return p_albedo * (1.0 - p_metallic);
}

// Split-sum specular albedo (F0 * scale + bias), Karis' mobile approximation.
// Source: https://www.unrealengine.com/blog/physically-based-shading-on-mobile
vec3 material_env_brdf_approx(vec3 p_f0, float p_roughness, float p_NdotV) {
	const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = p_roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * p_NdotV)) * r.x + r.y;
	vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
	return p_f0 * ab.x + ab.y;
}

// Specular albedo as fed to the DLSS RR guide buffers.
vec3 material_specular_albedo(vec3 p_f0, float p_roughness, float p_NdotV) {
	return material_env_brdf_approx(p_f0, p_roughness, p_NdotV) * 2.0;
}

// Shading inputs of one surface point. Directions are world space unless noted.
struct MaterialDebugInputs {
	vec3 geometry_normal; // Interpolated vertex normal, side-checked.
	vec3 normal; // Final shading normal, after normal mapping.
	vec3 view_normal; // Final shading normal in view space.
	vec3 tangent_space_normal; // Unpacked normal map sample in [-1, 1], (0, 0, 1) without one.
	vec3 tangent;
	vec3 bitangent;
	vec2 uv;
	vec3 albedo;
	float ao;
	float roughness;
	float metallic;
	float specular;
	vec3 emission; // Exposure-normalized.
	float NdotV;
	bool front_facing;
	float depth; // Camera distance divided by (z_far - z_near).
};

// Neutral inputs for channels a material does not provide.
MaterialDebugInputs material_debug_inputs_default() {
	MaterialDebugInputs inputs;
	inputs.geometry_normal = vec3(0.0, 0.0, 1.0);
	inputs.normal = vec3(0.0, 0.0, 1.0);
	inputs.view_normal = vec3(0.0, 0.0, 1.0);
	inputs.tangent_space_normal = vec3(0.0, 0.0, 1.0);
	inputs.tangent = vec3(1.0, 0.0, 0.0);
	inputs.bitangent = vec3(0.0, 1.0, 0.0);
	inputs.uv = vec2(0.0);
	inputs.albedo = vec3(1.0);
	inputs.ao = 1.0;
	inputs.roughness = 1.0;
	inputs.metallic = 0.0;
	inputs.specular = 0.5;
	inputs.emission = vec3(0.0);
	inputs.NdotV = 1.0;
	inputs.front_facing = true;
	inputs.depth = 0.0;
	return inputs;
}

// True for the debug modes material_debug_color() draws.
bool material_debug_is_material_mode(int p_mode) {
	return (p_mode >= MATERIAL_DEBUG_GEOMETRY_NORMALS && p_mode <= MATERIAL_DEBUG_NORMAL_ROUGHNESS) ||
			(p_mode >= MATERIAL_DEBUG_METALNESS && p_mode <= MATERIAL_DEBUG_EMISSIVE);
}

// Color of material debug mode p_mode for p_in.
vec3 material_debug_color(int p_mode, MaterialDebugInputs p_in) {
	vec3 f0 = material_specular_f0(p_in.albedo, p_in.metallic, p_in.specular);
	switch (p_mode) {
		case MATERIAL_DEBUG_GEOMETRY_NORMALS:
			return p_in.geometry_normal * 0.5 + 0.5;
		case MATERIAL_DEBUG_FINAL_NORMALS:
			return p_in.normal * 0.5 + 0.5;
		case MATERIAL_DEBUG_NORMAL_MAP:
			return p_in.tangent_space_normal * 0.5 + 0.5;
		case MATERIAL_DEBUG_TANGENT:
			return p_in.tangent * 0.5 + 0.5;
		case MATERIAL_DEBUG_BITANGENT:
			return p_in.bitangent * 0.5 + 0.5;
		case MATERIAL_DEBUG_UV:
			return vec3(fract(p_in.uv), 0.0);
		case MATERIAL_DEBUG_ALBEDO:
			return p_in.albedo;
		case MATERIAL_DEBUG_ORM:
			return vec3(p_in.ao, p_in.roughness, p_in.metallic);
		case MATERIAL_DEBUG_DIFFUSE_ALBEDO:
			return material_diffuse_albedo(p_in.albedo, p_in.metallic);
		case MATERIAL_DEBUG_SPECULAR_ALBEDO:
			return material_specular_albedo(f0, p_in.roughness, p_in.NdotV);
		case MATERIAL_DEBUG_NORMAL_ROUGHNESS:
			return (p_in.normal * 0.5 + 0.5) * (1.0 - p_in.roughness * 0.5);
		case MATERIAL_DEBUG_METALNESS:
			return vec3(p_in.metallic);
		case MATERIAL_DEBUG_ROUGHNESS:
			return vec3(p_in.roughness);
		case MATERIAL_DEBUG_VIEW_NORMALS:
			return p_in.view_normal * 0.5 + 0.5;
		case MATERIAL_DEBUG_DIFFUSE_SPECULAR_SPLIT:
			return mix(material_diffuse_albedo(p_in.albedo, p_in.metallic), material_specular_albedo(f0, p_in.roughness, p_in.NdotV), p_in.metallic);
		case MATERIAL_DEBUG_FRESNEL_F0:
			return f0;
		case MATERIAL_DEBUG_FRONT_BACK_FACE:
			return p_in.front_facing ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		case MATERIAL_DEBUG_DEPTH:
			return vec3(clamp(p_in.depth, 0.0, 1.0));
		case MATERIAL_DEBUG_EMISSIVE:
			return p_in.emission;
	}
	return vec3(0.0);
}

#endif // MATERIAL_INPUTS_INC_GLSL
