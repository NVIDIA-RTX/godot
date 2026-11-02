// Shared data structures for raytracing shaders.
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, raytracing_inc.glsl (for OFFSET_NONE/FLAG_*)

// ============================================================================
// BUFFER REFERENCES
// ============================================================================
layout(buffer_reference, std430) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430) readonly buffer Uint32Buffer {
	uint v[];
};

// ============================================================================
// GEOMETRY DATA (matches C++ RT_GeometryData, 160 bytes, 8-byte alignment)
// ============================================================================
struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed; // fp16 x, fp16 y
	uint index_format;
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;

	// Byte offset of the vertex color attribute inside `attribute_stride`,
	// or OFFSET_NONE if the mesh has no vertex colors.
	uint color_byte_offset;

	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;

	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;

	uint layers;
	uint instance_uniforms_ofs;
	uint uv2_byte_offset;
	uint uv2_scale_packed; // fp16 x, fp16 y
	uint instance_data_primitive_divisor;
	uint64_t instance_data_address;
	uint instance_data_layout; // See rt_pack_instance_data_layout().
	uint debug_state; // See RTDynamicGeometryDebug::pack().
	uint material_index; // See rt_pack_material_index().
	uint material_flags;
	uint material_feedback_index; // Texture streaming feedback slot, 0xFFFFFFFF = none.
	uint pad;
};

// ============================================================================
// PER-INSTANCE MOTION DATA (matches C++ RT_InstanceMotionData, 48 bytes)
// ============================================================================
struct InstanceMotionData {
	float prev_xform[12]; // Previous object-to-world (mat3x4, transposed 3x4)
};

// ============================================================================
// MATERIAL DATA (matches C++ layout, 128 bytes)
// ============================================================================

// MaterialData::flags bits (must match RT_MAT_FLAG_* in render_raytracing.h).
#define RT_MAT_FLAG_HAS_NORMAL_MAP 1u
#define RT_MAT_FLAG_HAS_EMISSION_TEX 2u
#define RT_MAT_FLAG_POINT_FILTER 4u
#define RT_MAT_FLAG_TRIPLANAR 8u
#define RT_MAT_FLAG_TRIPLANAR_WORLD 16u
#define RT_MAT_FLAG_DEPTH_DRAW_ALWAYS 32u
#define RT_MAT_FLAG_UNSHADED 64u
#define RT_MAT_FLAG_TRANSPARENT 128u
#define RT_MAT_FLAG_ALPHA_SCISSOR 256u
#define RT_MAT_FLAG_PROXIMITY_FADE 512u
#define RT_MAT_FLAG_DISTANCE_FADE 1024u
#define RT_MAT_FLAG_FOG_DISABLED 2048u
#define RT_MAT_FLAG_NO_RAY_QUERY_ALPHA 4096u
#define RT_MAT_FLAG_EMISSION_ADD 8192u
#define RT_MAT_FLAG_EMISSION_UV2 16384u
#define RT_MAT_FLAG_UV2_TRIPLANAR 32768u
#define RT_MAT_FLAG_UV2_TRIPLANAR_WORLD 65536u
#define RT_MAT_FLAG_INSTANCE_FADE 131072u

// Multi-bit fields fill the top bits so new single-bit flags can grow upward.
// Blend class of transparent surfaces (bits 25-27). Opaque surfaces leave 0.
#define RT_MAT_BLEND_CLASS_SHIFT 25u
#define RT_MAT_BLEND_CLASS_MASK (7u << RT_MAT_BLEND_CLASS_SHIFT)
#define RT_BLEND_CLASS_MIX 0u
#define RT_BLEND_CLASS_ADD 1u
#define RT_BLEND_CLASS_SUB 2u
#define RT_BLEND_CLASS_OIT 3u
#define RT_BLEND_CLASS_MUL 4u
#define RT_BLEND_CLASS_PREMULT 5u
// Godot diffuse mode (bits 28-29) and specular mode (bits 30-31), values GODOT_DIFFUSE_* / GODOT_SPECULAR_*.
#define RT_MAT_DIFFUSE_MODE_SHIFT 28u
#define RT_MAT_SPECULAR_MODE_SHIFT 30u
#define RT_MAT_SHADING_MODE_MASK 3u

// Texture channel of the roughness, metallic and AO textures (must match RTTextureChannel in render_raytracing.h).
#define RT_TEXTURE_CHANNEL_GRAYSCALE 4u

// Bitfields of C++ RT_MaterialData, unpacked by the accessors below. *_texture: bindless index in bits 0-23
// (RT_MaterialData::TEXTURE_INDEX_BITS), channel or albedo instance transparency in bits 24-31.
#define RT_TEXTURE_INDEX_BITS 24u
#define RT_TEXTURE_INDEX_MASK 0xFFFFFFu
// GeometryData::material_index keeps the instance transparency where albedo_texture does (C++ RT_MATERIAL_INDEX_MASK).
#define RT_MATERIAL_INDEX_MASK RT_TEXTURE_INDEX_MASK

struct MaterialData {
	uint albedo_texture; // Bits 24-31: transparency of the TLAS entry's instance (set by rt_entry_material()), see rt_instance_alpha().
	uint normal_texture;
	uint roughness_texture; // texture_roughness, or texture_orm for ORMMaterial3D.
	uint emission_texture;

	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;

	vec3 uv1_scale; // UV1 scale (default 1,1,1)
	float normal_map_depth; // Normal map strength (default 1.0)
	vec3 uv1_offset; // UV1 offset (default 0,0,0)
	uint surface_params; // unorm8x4: roughness, metallic, specular, unused. See rt_material_surface_params().

	uint uv2_scale_xy; // fp16x2. UV2 transform: identity unless triplanar or applied by the vertex stage.
	uint flags; // See RT_MAT_FLAG_*.
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none)

	uint blend_sharpness; // fp16x2: UV1, UV2 triplanar per-axis blend exponents (default 1.0)
	uint distance_fade; // fp16x2: distance_fade_min, distance_fade_max.
	float proximity_fade_distance;
	uint uv2_offset_xy; // fp16x2, modulo 1.
	uint uv2_scale_offset_z; // fp16x2: scale.z, offset.z (modulo 1).
	uint metallic_texture; // texture_metallic, or texture_orm for ORMMaterial3D.
	uint ao_texture; // AO texture when AO is enabled; 0 (white) otherwise.
	float emission_indirect; // BaseMaterial3D emission_indirect_energy, see MaterialResult::emission_indirect.
};

// Bindless index of a MaterialData *_texture.
uint rt_texture_index(uint p_texture) {
	return p_texture & RT_TEXTURE_INDEX_MASK;
}

// RT_TEXTURE_CHANNEL_* of a MaterialData roughness, metallic or AO texture.
uint rt_texture_channel(uint p_texture) {
	return p_texture >> RT_TEXTURE_INDEX_BITS;
}

// GeometryInstance3D transparency and visibility-parent fade of the TLAS entry, scaling surface alpha.
float rt_instance_alpha(MaterialData p_mat) {
	return 1.0 - float(p_mat.albedo_texture >> RT_TEXTURE_INDEX_BITS) / 255.0;
}

// Coverage of material alpha p_alpha on the TLAS entry. Opaque materials traced as transparent only for the
// instance fade (RT_MAT_FLAG_INSTANCE_FADE) ignore material alpha like raster, except for their alpha scissor.
float rt_entry_alpha(MaterialData p_mat, float p_alpha, float p_scissor_threshold) {
	if ((p_mat.flags & RT_MAT_FLAG_INSTANCE_FADE) != 0u) {
		p_alpha = (p_mat.flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u && p_alpha < p_scissor_threshold ? 0.0 : 1.0;
	}
	return p_alpha * rt_instance_alpha(p_mat);
}

// BaseMaterial3D roughness, metallic, specular (x, y, z).
vec3 rt_material_surface_params(MaterialData p_mat) {
	return unpackUnorm4x8(p_mat.surface_params).xyz;
}
