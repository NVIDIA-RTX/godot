/**************************************************************************/
/*  render_raytracing.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_capture.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_debug.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_plan.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_scheduler.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_validation.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_billboard.h"
#include "servers/rendering/rendering_device.h"

#define RB_TEX_RAYTRACING SNAME("raytracing")
#define RB_TEX_RT_DEPTH SNAME("rt_depth")
#define RB_TEX_RT_SKY SNAME("rt_sky")

#define RB_SCOPE_RT_TIMING SNAME("rt_timing")
#define RB_TEX_RT_TIMING SNAME("ticks")

#define RB_SCOPE_DLSS_RR SNAME("dlss_rr")
#define RB_TEX_DLSS_RR_DIFFUSE_ALBEDO SNAME("diffuse_albedo")
#define RB_TEX_DLSS_RR_SPECULAR_ALBEDO SNAME("specular_albedo")
#define RB_TEX_DLSS_RR_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_DLSS_RR_SPECULAR_HIT_DIST SNAME("specular_hit_dist")

class RenderDataRD;
class RenderSceneBuffersRD;
struct RTViewFrustum;

namespace RendererRD {
class MaterialStorage;
class MeshStorage;
} // namespace RendererRD

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;
class SceneShaderRaytracing;

// Must match GLSL GeometryData (std430, 144 bytes, 8-byte alignment).
struct alignas(8) RT_GeometryData {
	uint64_t vertex_buffer_address;
	uint64_t attribute_buffer_address;
	uint64_t index_buffer_address;
	uint32_t vertex_count;
	uint32_t position_stride;
	uint32_t normal_byte_offset;
	uint32_t normal_stride;
	uint32_t tangent_byte_offset;
	uint32_t tangent_stride;
	uint32_t attribute_stride;
	uint32_t uv_byte_offset;
	uint32_t uv_scale_packed;
	uint32_t index_format;
	uint32_t primitive_count;
	uint32_t flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;
	uint32_t color_byte_offset;
	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;
	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint32_t prev_vertex_buffer_address_lo;
	uint32_t prev_vertex_buffer_address_hi;
	uint32_t layers;
	uint32_t instance_uniforms_ofs;
	uint32_t uv2_byte_offset;
	uint32_t uv2_scale_packed;
	uint32_t instance_data_primitive_divisor;
	uint64_t instance_data_address;
	uint32_t instance_data_layout; // See rt_pack_instance_data_layout().
	uint32_t debug_state; // See RTDynamicGeometryDebug::pack().
	uint32_t material_index; // See rt_pack_material_index().
	uint32_t material_flags;
};
static_assert(sizeof(RT_GeometryData) == 152, "RT_GeometryData must be 152 bytes for std430");

// Marks an absent field in RT_GeometryData::instance_data_layout.
static constexpr uint32_t RT_INSTANCE_DATA_FIELD_NONE = 0xFF;

// Packs an instance record layout: bits 0-7 stride, 8-15 color offset, 16-23 custom offset, all in floats.
// Offsets of UINT32_MAX mean the field is absent. Must match fetch_instance_field() in raytracing_hit_inc.glsl.
constexpr uint32_t rt_pack_instance_data_layout(uint32_t p_stride, uint32_t p_color_offset, uint32_t p_custom_offset) {
	return (p_stride & 0xFF) |
			((p_color_offset == UINT32_MAX ? RT_INSTANCE_DATA_FIELD_NONE : (p_color_offset & 0xFF)) << 8) |
			((p_custom_offset == UINT32_MAX ? RT_INSTANCE_DATA_FIELD_NONE : (p_custom_offset & 0xFF)) << 16);
}

// First touch this frame of a cache entry shared by viewports. Query it up front; commit only once the touch got past
// every early return, so an aborted touch can't keep a later viewport from doing the frame's work.
class RTFrameTouch {
	uint32_t &stamp;
	const uint32_t frame;

public:
	RTFrameTouch(uint32_t &r_stamp, uint32_t p_frame) :
			stamp(r_stamp), frame(p_frame) {}
	// True until a touch of this frame was committed.
	bool is_first() const { return stamp != frame; }
	// Marks the frame's work as done.
	void commit() { stamp = frame; }
};

// Per-instance motion data for velocity computation (matches GLSL InstanceMotionData, 48 bytes).
struct RT_InstanceMotionData {
	float prev_object_to_world[12]; // Previous object-to-world (mat3x4, transposed 3x4).
};
static_assert(sizeof(RT_InstanceMotionData) == 48, "RT_InstanceMotionData must be 48 bytes");

// Must match GLSL MaterialData (std430, 128 bytes). Bitfields fill from the low bit on every supported compiler,
// which test_render_raytracing.h checks; GLSL unpacks them with the accessors in raytracing_data_inc.glsl.
struct alignas(16) RT_MaterialData {
	// Bits of the bindless texture index (GLSL rt_texture_index()) in every *_texture; 0 = white.
	static constexpr uint32_t TEXTURE_INDEX_BITS = 24;

	struct {
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t instance_transparency : 8; // 0 in RTMaterialTable; GLSL rt_entry_material() fills it from RT_GeometryData::material_index.
	} albedo_texture;
	struct {
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t unused : 8;
	} normal_texture;
	struct { // texture_roughness, or texture_orm for ORMMaterial3D.
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t channel : 8; // RTTextureChannel (GLSL rt_texture_channel()).
	} roughness_texture;
	struct {
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t unused : 8;
	} emission_texture;
	float albedo_color[4];
	float emission_color[3];
	float emission_strength;
	float uv1_scale[3];
	float normal_map_depth; // Strength [0..N], default 1.0 (not Z-depth).
	float uv1_offset[3];
	struct { // BaseMaterial3D scalars in [0, 1] as unorm8 (GLSL rt_material_surface_params()), see rt_unorm8().
		uint32_t roughness : 8;
		uint32_t metallic : 8;
		uint32_t specular : 8; // Dielectric specular, default 0.5 -> F0 = 0.04.
		uint32_t unused : 8;
	} surface_params;
	// UV2 transform, fp16 (see rt_material_set_packed_uv()): identity unless triplanar or applied by the vertex stage.
	struct {
		uint32_t x : 16;
		uint32_t y : 16;
	} uv2_scale_xy;
	uint32_t flags;
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none).
	struct { // fp16 triplanar per-axis blend exponents, default 1.0.
		uint32_t uv1 : 16;
		uint32_t uv2 : 16;
	} blend_sharpness;
	struct { // fp16 BaseMaterial3D distance_fade_min_distance, distance_fade_max_distance.
		uint32_t min_distance : 16;
		uint32_t max_distance : 16;
	} distance_fade;
	float proximity_fade_distance;
	struct { // fp16, modulo 1.
		uint32_t x : 16;
		uint32_t y : 16;
	} uv2_offset_xy;
	struct { // fp16, offset modulo 1.
		uint32_t scale : 16;
		uint32_t offset : 16;
	} uv2_scale_offset_z;
	struct { // texture_metallic, or texture_orm for ORMMaterial3D.
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t channel : 8; // RTTextureChannel (GLSL rt_texture_channel()).
	} metallic_texture;
	struct { // texture_ambient_occlusion (or texture_orm) when AO is enabled; 0 (white) otherwise.
		uint32_t index : TEXTURE_INDEX_BITS;
		uint32_t channel : 8; // RTTextureChannel (GLSL rt_texture_channel()).
	} ao_texture;
	float emission_indirect; // BaseMaterial3D emission_indirect_energy: emission weight on indirect paths.
};
static_assert(sizeof(RT_MaterialData) == 128, "RT_MaterialData must be 128 bytes for std430, with every bitfield struct in one uint");
static_assert(BindlessBlock::MAX_BINDLESS_TEXTURES <= (1u << RT_MaterialData::TEXTURE_INDEX_BITS), "Bindless texture indices must fit RT_MaterialData textures");

// p_value clamped to [0, 1] as GLSL unpackUnorm4x8() reads one byte.
constexpr uint32_t rt_unorm8(float p_value) {
	return uint32_t((p_value < 0.0f ? 0.0f : (p_value > 1.0f ? 1.0f : p_value)) * 255.0f + 0.5f);
}

// RT_MaterialData::albedo_texture.instance_transparency of an instance drawn with p_alpha (force_alpha * parent_fade_alpha), 0 = opaque.
constexpr uint32_t rt_instance_transparency(float p_alpha) {
	return 255u - rt_unorm8(p_alpha);
}

// Stores BaseMaterial3D's roughness, metallic and specular in r_mat.surface_params.
constexpr void rt_material_set_surface_params(RT_MaterialData &r_mat, float p_roughness, float p_metallic, float p_specular) {
	r_mat.surface_params.roughness = rt_unorm8(p_roughness);
	r_mat.surface_params.metallic = rt_unorm8(p_metallic);
	r_mat.surface_params.specular = rt_unorm8(p_specular);
}

// BaseMaterial3D::TextureChannel values (matches GLSL RT_TEXTURE_CHANNEL_* in raytracing_data_inc.glsl).
enum RTTextureChannel : uint32_t {
	RT_TEXTURE_CHANNEL_RED,
	RT_TEXTURE_CHANNEL_GREEN,
	RT_TEXTURE_CHANNEL_BLUE,
	RT_TEXTURE_CHANNEL_ALPHA,
	RT_TEXTURE_CHANNEL_GRAYSCALE,
};

// Inverse of BaseMaterial3D's texture channel masks: one-hot, or thirds for grayscale.
constexpr RTTextureChannel rt_texture_channel_from_mask(float p_r, float p_g, float p_b, float p_a) {
	if (p_r > 0.0f && p_g > 0.0f) {
		return RT_TEXTURE_CHANNEL_GRAYSCALE;
	}
	if (p_g > 0.0f) {
		return RT_TEXTURE_CHANNEL_GREEN;
	}
	if (p_b > 0.0f) {
		return RT_TEXTURE_CHANNEL_BLUE;
	}
	return p_a > 0.0f ? RT_TEXTURE_CHANNEL_ALPHA : RT_TEXTURE_CHANNEL_RED;
}

// Stores the fp16 addressing fields. Material textures always repeat in RT, so the UV2 offset is
// reduced modulo 1, which keeps its fp16 error below 2^-11.
inline void rt_material_set_packed_uv(RT_MaterialData &r_mat, float p_uv1_blend_sharpness,
		const Vector3 &p_uv2_scale, const Vector3 &p_uv2_offset, float p_uv2_blend_sharpness) {
	const Vector3 offset = p_uv2_offset - p_uv2_offset.floor();
	r_mat.uv2_scale_xy.x = Math::make_half_float(p_uv2_scale.x);
	r_mat.uv2_scale_xy.y = Math::make_half_float(p_uv2_scale.y);
	r_mat.uv2_offset_xy.x = Math::make_half_float(offset.x);
	r_mat.uv2_offset_xy.y = Math::make_half_float(offset.y);
	r_mat.uv2_scale_offset_z.scale = Math::make_half_float(p_uv2_scale.z);
	r_mat.uv2_scale_offset_z.offset = Math::make_half_float(offset.z);
	r_mat.blend_sharpness.uv1 = Math::make_half_float(p_uv1_blend_sharpness);
	r_mat.blend_sharpness.uv2 = Math::make_half_float(p_uv2_blend_sharpness);
}

// White, rough, non-emissive dielectric with identity UV transforms and no textures.
inline RT_MaterialData rt_material_data_default() {
	RT_MaterialData d = {};
	for (int i = 0; i < 4; i++) {
		d.albedo_color[i] = 1.0f;
	}
	for (int i = 0; i < 3; i++) {
		d.uv1_scale[i] = 1.0f;
	}
	d.normal_map_depth = 1.0f;
	d.emission_indirect = 1.0f;
	rt_material_set_surface_params(d, 1.0f, 0.0f, 0.5f);
	rt_material_set_packed_uv(d, 1.0f, Vector3(1, 1, 1), Vector3(), 1.0f);
	return d;
}

// Low bits of RT_GeometryData::material_index holding the RTMaterialTable index.
static constexpr uint32_t RT_MATERIAL_INDEX_MASK = (1u << RT_MaterialData::TEXTURE_INDEX_BITS) - 1;

// RT_GeometryData::material_index: the RTMaterialTable index, with the instance transparency (see
// rt_instance_transparency()) in the bits it occupies in RT_MaterialData::albedo_texture. Must match rt_entry_material().
constexpr uint32_t rt_pack_material_index(uint32_t p_table_index, uint32_t p_instance_transparency) {
	return (p_table_index & RT_MATERIAL_INDEX_MASK) | (p_instance_transparency << RT_MaterialData::TEXTURE_INDEX_BITS);
}

// Persistent GPU copy of the material cache, indexed by RT_GeometryData::material_index and shared by every view.
// Entry 0 is the default material. Only entries set since the last upload are uploaded.
class RTMaterialTable {
public:
	// Entries [begin, end) uploaded with one buffer update.
	struct Range {
		uint32_t begin = 0;
		uint32_t end = 0;
	};
	// Dirty runs separated by at most this many clean entries are uploaded as one range.
	static constexpr uint32_t MERGE_GAP = 8;

	// Sorts r_dirty (duplicates allowed) into r_ranges, merging runs at most p_max_gap entries apart.
	static void merge_ranges(LocalVector<uint32_t> &r_dirty, uint32_t p_max_gap, LocalVector<Range> &r_ranges);

	// Stores p_data in entry p_index for the next upload(), growing the table with default materials.
	void set(uint32_t p_index, const RT_MaterialData &p_data);
	// Uploads the entries set since the last upload; recreates the buffer when the table outgrew it.
	void upload();
	// Frees the buffer and drops every entry.
	void reset();
	// Storage buffer of the table; null before the first upload().
	RID get_buffer() const { return buffer; }
	// Entries by table index, as last set.
	Span<RT_MaterialData> get_entries() const { return Span<RT_MaterialData>(entries.ptr(), entries.size()); }

	~RTMaterialTable() { reset(); }

private:
	LocalVector<RT_MaterialData> entries;
	LocalVector<uint32_t> dirty;
	LocalVector<Range> ranges;
	RID buffer;
	uint32_t capacity = 0; // In entries.

	void _grow(uint32_t p_size);
};

// Light types for raytracing (matches GLSL RT_LIGHT_TYPE_* defines).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_SPOT = 3,
};

// Bindless index meaning "no projector" (the default white texture, matches GLSL RT_LIGHT_NO_PROJECTOR).
static constexpr uint32_t RT_LIGHT_NO_PROJECTOR = 0u;

// Must match GLSL RTLightPacked (std430, 64 bytes), unpacked by lights_get() in raytracing_lights_inc.glsl.
// Bitfields fill from the low bit (checked in test_render_raytracing.h). Positional-only fields stay zero for directional lights.
struct alignas(16) RT_LightData {
	float position[3]; // World position (omni/spot) or normalized direction (directional).
	float range; // Omni/spot range, 0 for infinite.
	struct { // fp16 HDR emission (color * energy).
		uint32_t r : 16;
		uint32_t g : 16;
	} emission_rg;
	struct { // fp16.
		uint32_t b : 16;
		uint32_t radius : 16; // Size (omni/spot) or angular radius in radians (directional).
	} emission_b_radius;
	struct { // fp16 multipliers.
		uint32_t specular : 16;
		uint32_t indirect : 16;
	} energy;
	struct {
		uint32_t cull_mask : 20; // light_cull_mask, tested against instance layers.
		uint32_t type : 2; // RTLightType.
		uint32_t unused : 2;
		uint32_t shadow_ray_mask : 8; // TLAS mask for shadow rays.
	} masks;
	struct { // fp16.
		uint32_t attenuation : 16; // Distance attenuation exponent (omni/spot).
		uint32_t shadow_opacity : 16; // 0 skips the shadow ray; otherwise blends traced transmittance towards 1.
	} attenuation_shadow;
	struct { // fp16, spot only.
		uint32_t one_minus_cos : 16; // 1 - cos(spot angle), precise for narrow cones.
		uint32_t inv_attenuation : 16; // 1 / spot attenuation.
	} spot;
	// snorm16 rotation quaternion (omni/spot): the spot cone points along local -Z, and it orients the projector.
	struct {
		uint32_t x : 16;
		uint32_t y : 16;
	} orientation_xy;
	struct {
		uint32_t z : 16;
		uint32_t w : 16;
	} orientation_zw;
	struct {
		uint32_t index : RT_MaterialData::TEXTURE_INDEX_BITS; // Bindless sRGB projector (omni/spot), RT_LIGHT_NO_PROJECTOR when unset.
		uint32_t unused : 8;
	} projector_texture;
	uint32_t reserved[3]; // Free for future fields, keeps the 64-byte stride.
};
static_assert(sizeof(RT_LightData) == 64, "RT_LightData must be 64 bytes for std430, with every bitfield struct in one uint");

// fp16 as GLSL unpackHalf2x16() reads it, clamped to the finite range so bright lights don't become inf.
inline uint16_t rt_half(float p_value) {
	return Math::make_half_float(CLAMP(p_value, -65504.0f, 65504.0f));
}

// snorm16 as GLSL unpackSnorm2x16() reads it.
inline uint16_t rt_snorm16(float p_value) {
	return uint16_t(int16_t(Math::round(CLAMP(p_value, -1.0f, 1.0f) * 32767.0f)));
}

enum {
	RT_LIGHTS_MAX = 64,
	RT_LIGHTS_FRUSTUM_BUDGET = 48,
	RT_LIGHTS_INDIRECT_BUDGET = RT_LIGHTS_MAX - RT_LIGHTS_FRUSTUM_BUDGET,
};

constexpr uint32_t RT_OFFSET_NONE = 0xFFFFFFFFu;

enum {
	RT_CACHE_CHUNK_SIZE = 256,
	RT_CACHE_CHUNK_SHIFT = 8,
	RT_CACHE_CHUNK_MASK = 255,
};

// Material flags for RT (matches GLSL RT_MAT_FLAG_* in raytracing_data_inc.glsl).
enum {
	RT_MAT_FLAG_HAS_NORMAL_MAP = 1u,
	RT_MAT_FLAG_HAS_EMISSION_TEX = 2u,
	RT_MAT_FLAG_POINT_FILTER = 4u,
	RT_MAT_FLAG_TRIPLANAR = 8u,
	RT_MAT_FLAG_TRIPLANAR_WORLD = 16u,
	RT_MAT_FLAG_DEPTH_DRAW_ALWAYS = 32u,
	RT_MAT_FLAG_UNSHADED = 64u,
	RT_MAT_FLAG_TRANSPARENT = 128u,
	RT_MAT_FLAG_ALPHA_SCISSOR = 256u,
	RT_MAT_FLAG_PROXIMITY_FADE = 512u,
	RT_MAT_FLAG_DISTANCE_FADE = 1024u,
	RT_MAT_FLAG_FOG_DISABLED = 2048u,
	RT_MAT_FLAG_NO_RAY_QUERY_ALPHA = 4096u,
	RT_MAT_FLAG_EMISSION_ADD = 8192u,
	RT_MAT_FLAG_EMISSION_UV2 = 16384u,
	RT_MAT_FLAG_UV2_TRIPLANAR = 32768u,
	RT_MAT_FLAG_UV2_TRIPLANAR_WORLD = 65536u,
	// Opaque material traced as transparent only because of the instance fade; it ignores material alpha like raster.
	RT_MAT_FLAG_INSTANCE_FADE = 131072u,
	// Multi-bit fields fill the top bits so new single-bit flags can grow upward.
	// Blend class of transparent surfaces (bits 25-27); opaque surfaces leave 0.
	RT_MAT_BLEND_CLASS_SHIFT = 25u,
	RT_MAT_BLEND_CLASS_MIX = 0u,
	RT_MAT_BLEND_CLASS_ADD = 1u,
	RT_MAT_BLEND_CLASS_SUB = 2u,
	RT_MAT_BLEND_CLASS_OIT = 3u,
	RT_MAT_BLEND_CLASS_MUL = 4u,
	RT_MAT_BLEND_CLASS_PREMULT = 5u,
	// Diffuse mode (bits 28-29) and specular mode (bits 30-31), matching GODOT_DIFFUSE_* / GODOT_SPECULAR_* in brdf_inc.glsl.
	RT_MAT_DIFFUSE_MODE_SHIFT = 28u,
	RT_MAT_SPECULAR_MODE_SHIFT = 30u,
	RT_MAT_DIFFUSE_LAMBERT = 0u,
	RT_MAT_DIFFUSE_BURLEY = 1u,
	RT_MAT_DIFFUSE_LAMBERT_WRAP = 2u,
	RT_MAT_DIFFUSE_TOON = 3u,
	RT_MAT_SPECULAR_SCHLICK_GGX = 0u,
	RT_MAT_SPECULAR_TOON = 1u,
	RT_MAT_SPECULAR_DISABLED = 2u,
};

// Packs an RT_MAT_DIFFUSE_* and an RT_MAT_SPECULAR_* value into material flag bits.
constexpr uint32_t rt_material_shading_mode_flags(uint32_t p_diffuse_mode, uint32_t p_specular_mode) {
	return (p_diffuse_mode << RT_MAT_DIFFUSE_MODE_SHIFT) | (p_specular_mode << RT_MAT_SPECULAR_MODE_SHIFT);
}

// RT_MAT_FLAG_* emission bits for a BaseMaterial3D. UV2 addressing bits are only set when
// emission samples UV2, the one HG0 texture that can.
constexpr uint32_t rt_material_emission_flags(bool p_add, bool p_on_uv2, bool p_uv2_triplanar, bool p_uv2_world_triplanar) {
	uint32_t flags = p_add ? uint32_t(RT_MAT_FLAG_EMISSION_ADD) : 0u;
	if (p_on_uv2) {
		flags |= RT_MAT_FLAG_EMISSION_UV2;
		if (p_uv2_triplanar) {
			flags |= RT_MAT_FLAG_UV2_TRIPLANAR;
			if (p_uv2_world_triplanar) {
				flags |= RT_MAT_FLAG_UV2_TRIPLANAR_WORLD;
			}
		}
	}
	return flags;
}

// True when a surface with these RT_MAT_FLAG_* bits can write the pixel's velocity: opaque surfaces,
// and transparent ones that can become the dominant peel layer in shade_and_bounce given
// rendering/pathtracing/transparency_dominant_layer_alpha (0 = never). Add/Sub layers report zero
// peel alpha, so only depth_draw_always lets them win.
constexpr bool rt_material_can_own_velocity(uint32_t p_flags, float p_dominant_layer_alpha) {
	if (!(p_flags & RT_MAT_FLAG_TRANSPARENT) || (p_flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS)) {
		return true;
	}
	const uint32_t blend_class = (p_flags >> RT_MAT_BLEND_CLASS_SHIFT) & 7u;
	return p_dominant_layer_alpha > 0.0f && blend_class != RT_MAT_BLEND_CLASS_ADD && blend_class != RT_MAT_BLEND_CLASS_SUB;
}

// Index format for RT geometry (matches GLSL fetch_indices).
enum {
	RT_INDEX_FORMAT_UINT16 = 0,
	RT_INDEX_FORMAT_UINT32 = 1,
	RT_INDEX_FORMAT_NONE = 2,
};

enum {
	RT_GEOM_FLAG_COMPRESSED = 1u,
	RT_GEOM_FLAG_PROCEDURAL = 2u,
	// Set when the BLAS uses a vertex buffer the renderer writes: skinning, blend shapes, vertex capture, merged MultiMesh / particle bakes.
	RT_GEOM_FLAG_DEFORMED = 4u,
	// Positions and TBN come from the vertex capture pass; hit shaders must not re-apply vertex() displacement.
	RT_GEOM_FLAG_CAPTURED = 8u,
	// UVs in the attribute buffer are unorm16; independent of RT_GEOM_FLAG_COMPRESSED (deformed copies are uncompressed).
	RT_GEOM_FLAG_COMPRESSED_UV = 16u,
	// A triangle hit the ray tracer reports as front facing is a raster back face (FRONT_FACING false).
	RT_GEOM_FLAG_FACING_INVERTED = 32u,
	// The material flips the normal of back faces, like raster DO_SIDE_CHECK (cull_front and cull_disabled).
	RT_GEOM_FLAG_SIDE_CHECK = 64u,
	// SHADOW_CASTING_SETTING_SELF_SHADOWING receiver: its shadow rays keep the self-shadow group bit.
	RT_GEOM_FLAG_SELF_SHADOWING = 128u,
};

// Facing state of the path traced triangles of one TLAS instance.
struct RTTriangleFacing {
	uint32_t instance_flags = 0; // TLAS TRIANGLE_FACING_CULL_DISABLE and TRIANGLE_FLIP_FACING bits.
	uint32_t geometry_flags = 0; // RT_GEOM_FLAG_FACING_INVERTED and RT_GEOM_FLAG_SIDE_CHECK.
};

// Facing of triangles drawn with p_cull_mode, matching the rasterizer: culling follows the winding in mesh space, while
// FRONT_FACING follows the winding on screen, or in mesh space when p_preserve_front_facing (preserve_front_facing_when_mirrored).
// The ray tracer decides facing in BLAS space. p_raster_mirror is the mirror flag the rasterizer culls the instance
// with; p_tlas_mirror is true when the TLAS instance transform has a negative determinant.
constexpr RTTriangleFacing rt_triangle_facing(RS::CullMode p_cull_mode, bool p_raster_mirror, bool p_tlas_mirror, bool p_preserve_front_facing) {
	// Without TRIANGLE_FLIP_FACING, the ray tracer's front face is the rasterizer's back face.
	const bool cull_front = p_cull_mode == RS::CULL_MODE_FRONT;
	const bool space_mirrored = p_raster_mirror != p_tlas_mirror;
	const bool screen_mirrored = p_raster_mirror && !p_preserve_front_facing;
	RTTriangleFacing facing;
	facing.instance_flags = (cull_front == space_mirrored ? uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT) : 0u) |
			(p_cull_mode == RS::CULL_MODE_DISABLED ? uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT) : 0u);
	facing.geometry_flags = (cull_front != screen_mirrored ? uint32_t(RT_GEOM_FLAG_FACING_INVERTED) : 0u) |
			(p_cull_mode != RS::CULL_MODE_BACK ? uint32_t(RT_GEOM_FLAG_SIDE_CHECK) : 0u);
	return facing;
}

// Unified TLAS instance mask layout: bit 0 = opaque camera lane, bit 1 =
// transparent camera lane, bits 2-7 = shadow-caster groups.
enum : uint8_t {
	RT_VIS_BIT = 0x01,
	RT_MASK_TRANSPARENT = 0x02,
	RT_SHADOW_GROUPS_MASK = 0xFC,
};

// Volume TLAS instance masks (matches GLSL RT_VOLUME_MASK_*). Free bits are reserved for light volumes.
enum : uint8_t {
	RT_VOLUME_MASK_DECAL = 0x01,
};

// Bindless index of an unset decal texture (matches GLSL RT_DECAL_NO_TEXTURE).
static constexpr uint32_t RT_DECAL_NO_TEXTURE = 0xFFFFFFFFu;

// Must match GLSL RTDecalData (std430, 176 bytes). World space.
struct alignas(16) RT_DecalData {
	float world_to_box[12]; // Rows of the world to unit box (x/z in [0, 1], y in [-1, 1]) affine transform.
	float normal_basis[12]; // Columns (xyz, w unused) of the orthonormal decal basis.
	float normal[3]; // Projection direction (decal +Y).
	float normal_fade;
	float modulate[4]; // Linear; alpha includes distance fade.
	float emission_energy; // Includes distance fade.
	float albedo_mix;
	float upper_fade;
	float lower_fade;
	uint32_t textures[4]; // Bindless indices, indexed by RS::DecalTexture; RT_DECAL_NO_TEXTURE when unset.
	uint32_t mask;
	uint32_t pad[3];
};
static_assert(sizeof(RT_DecalData) == 176, "RT_DecalData must be 176 bytes for std430");
static_assert(RS::DECAL_TEXTURE_MAX == 4, "RT_DecalData::textures must cover every RS::DecalTexture");

// Per-instance state for procedural RT geometry. Heap-allocated, only exists for procedural instances.
struct RTProceduralState {
	AABB culling_aabb;
	PackedFloat32Array aabb_data; // N * 6 floats (min/max per AABB). Empty = single AABB.
	bool expose_bounds = false;
	bool dirty = true;
	RID blas;
	RID gpu_buffer;
	uint32_t gpu_buffer_capacity = 0; // Bytes, grow-only.
	uint64_t gpu_buffer_address = 0; // BDA (0 = not exposed).
	uint32_t aabb_count = 0;
};

// Fixed-size pages of row-major 3x4 BLAS build transforms, shared by all BLASes.
// Pages are never resized: RD frees a BLAS together with the buffers it was built from.
class RTBlasTransformPool {
public:
	static constexpr uint32_t MATRIX_SIZE = 12 * sizeof(float);
	static constexpr uint32_t SLOTS_PER_PAGE = 1024;

	// One matrix inside a page.
	struct Slot {
		RID buffer;
		uint32_t offset = 0; // Byte offset of the matrix in `buffer`; 16-byte aligned.

		bool is_valid() const { return buffer.is_valid(); }
	};

	// Stores p_matrix (12 floats) in a free slot, adding a page when none is left.
	Slot allocate(const float *p_matrix);
	// Returns r_slot to the pool and invalidates it. Free the BLAS built from it first.
	void release(Slot &r_slot);
	// Frees every page. RD also frees any BLAS still built from them.
	void clear();
	// Pages currently allocated.
	uint32_t get_page_count() const { return pages.size(); }

private:
	LocalVector<RID> pages;
	LocalVector<Slot> free_slots;
};

// The buffers the device addresses of one RT_GeometryData were taken from, so validation can tell a stale address
// from one into a live buffer. Set through _rt_buffer_address().
struct RTGeometrySources {
	RID vertex;
	RID attribute;
	RID index;
	RID prev_vertex;
	RID instance_data;
};

struct RTSurfaceData {
	RID blas;
	// Maps compressed positions to mesh space during the BLAS build; invalid when uncompressed.
	RTBlasTransformPool::Slot blas_transform;
	RT_GeometryData geometry = {};
	RTGeometrySources sources;
	bool is_compressed = false;
	uint64_t blas_size = 0;
};

// Identifies a multi-geometry BLAS: a mesh and the subset of its surfaces merged into one TLAS instance.
struct RTMeshGroupKey {
	uint64_t mesh_id = 0; // Full RID id, so a recycled mesh slot never matches.
	uint64_t surface_mask = 0; // Bit i set when surface i is a member.
	uint64_t opaque_mask = 0; // Bit i set when member i is built with the opaque geometry flag.

	bool operator==(const RTMeshGroupKey &p_other) const {
		return mesh_id == p_other.mesh_id && surface_mask == p_other.surface_mask && opaque_mask == p_other.opaque_mask;
	}

	uint32_t hash() const {
		return hash_fmix32(hash_murmur3_one_64(opaque_mask, hash_murmur3_one_64(surface_mask, hash_murmur3_one_64(mesh_id))));
	}
};

// Multi-geometry BLAS shared by every instance that merges the same surfaces of a mesh.
struct RTMeshGroupEntry {
	RID blas;
	LocalVector<RTBlasTransformPool::Slot> transforms; // Per member; invalid for uncompressed members.
	LocalVector<RT_GeometryData> geometries; // In BLAS geometry order (ascending surface index).
	LocalVector<RTGeometrySources> sources; // Parallel to `geometries`.
	LocalVector<uint32_t> surface_counters; // Per member; any change rebuilds the BLAS.
	uint64_t last_used = 0; // Use stamp; picks the variant to drop when a mesh has too many.
};

// For each surface, writes the index of the first surface with an equal key, or its own index when
// it has no equal predecessor or is not groupable. Quadratic, meant for the surfaces of one instance.
void rt_assign_surface_groups(Span<RTSurfaceGroupKey> p_keys, LocalVector<uint32_t> &r_leaders);

struct RTDecalCandidate {
	uint32_t source = 0;
	float score = 0.0f;
	float sort_key = 0.0f; // Lower is drawn on top.
};

// Inputs for a surface backed by a per-frame-deformed vertex buffer.
struct RTDeformedGeometrySource {
	RID *cache_handle = nullptr; // RTDeformedCacheEntry handle of the surface; a new entry is made when it isn't valid.
	// Vertex buffer (object-space positions) the BLAS is built / refit against. Invalid for a capture not recorded
	// this view: its entry already holds the capture, so there is nothing to copy.
	RID current_vb;
	uint32_t current_vb_offset = 0; // Byte offset of the surface's positions in `current_vb`.
	bool captured = false; // Vertex capture output (RT_GEOM_FLAG_CAPTURED). Only `change_stamp` tells captures apart.
	bool force_rebuild = false; // Rebuild the BLAS even when a refit would do.
	RID prev_vb; // Previous-frame positions for motion vectors. Optional.
	uint64_t change_stamp = 0; // Changes when `current_vb` contents change.
	uint64_t cache_key = 0; // Caller-defined cache identity.
	uint32_t cache_version = 0; // Changes when the resource behind `cache_key` is recycled.
	uint32_t surface_counter = 0; // Changes when the underlying mesh surface changes.

	// False for static surfaces, which are traced from their mesh buffers.
	bool is_deformed() const { return captured || current_vb.is_valid(); }
};

struct RTMaterialData {
	alignas(16) RT_MaterialData data = {};
	uint32_t table_index = 0; // Entry in RTMaterialTable; 0 is the default material.
	uint32_t global_buffer_index = UINT32_MAX;
	uint32_t rt_sbt_offset = 0;
	bool is_custom_shader = false;
	RID uniform_buffer; // Buffer pointer for mats > 512 bytes.
	uint32_t uniform_pool_slot = UINT32_MAX; // Index into the material UBO pool, or UINT32_MAX (unused)
	RID albedo_texture_rd;
	RID normal_texture_rd;
	RID orm_texture_rd;
	RID emission_texture_rd;
};

// A traced surface of one mesh instance while it is planned, before grouping.
struct RTSurfaceRecord {
	RTPlanSurface surface; // Written to the plan once grouped.
	RTSurfaceGroupKey key; // Decides the surface's group.
};

// Per-instance scratch of the TLAS plan, kept to avoid per-frame allocations. One per concurrent planner.
struct RTPlanScratch {
	LocalVector<RTSurfaceRecord> records; // Traced surfaces of the instance being planned.
	LocalVector<RTSurfaceGroupKey> group_keys; // Key of each record, in record order.
	LocalVector<uint32_t> group_leaders; // Per record: the first record of its group.
	LocalVector<uint32_t> group_cursors; // Per leader: its group's next member position.
	LocalVector<uint32_t> group_order; // Records in plan order.
	LocalVector<Pair<const void *, uint64_t>> global_uniform_stamps; // Per shader: its global_uniforms_stamp of global_uniform_stamps_frame.
	uint64_t global_uniform_stamps_frame = UINT64_MAX; // Frame global_uniform_stamps belongs to.
};

// Scratch of planning a view's instances on the worker pool, then serially for the deferred ones.
struct RTPlanWorkspace {
	RTPlanScratch serial; // Used by the serial pass.
	LocalVector<RTPlanScratch> chunks; // One per worker chunk.
	LocalVector<uint8_t> deferred; // Per instance: whether a worker deferred its plan to the serial pass.
};

struct RTCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint32_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint8_t failed_attempts = 0;
	uint64_t size_bytes = 0;
};

// Vertex capture held by a cache entry; later viewports of `frame` reuse it instead of recapturing.
struct RTCaptureState {
	uint32_t frame = UINT32_MAX; // Frame the capture was recorded in.
	uint64_t stamp = 0; // Unique per recorded capture; 0 before the first one.
	RTVertexCapture::Stamps inputs; // Inputs of vertex() when the capture was recorded.
};

// BLAS cache entry for a surface driven by a per-frame-deformed vertex buffer.
//
// The skinned vertex buffer is owned by the engine's MeshInstance system and may
// be freed/reallocated outside our control. To decouple BLAS lifetime from that
// (and keep last-frame positions alive for motion vectors) we copy the skinned VB
// into our own buffers each frame:
//   * owned_vb_full mirrors the skinned VB layout (positions + N + T) and is what
//     the BLAS and the hit shader read from this frame.
//   * prev_pos_vb stores the previous-frame positions only (float3 packed) for the
//     motion-vector path. Updated from owned_vb_full's position section each frame
//     before owned_vb_full is overwritten with the new skinned data.
struct RTDeformedCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint64_t cached_change_stamp = 0;
	uint32_t cached_key_version = 0;
	uint32_t cached_surface_counter = 0;
	uint64_t cached_buffer_id = 0; // RID id of the deformed vertex buffer at the time of build.
	bool blas_built_once = false; // True once the BLAS has been fully built; subsequent ticks can refit.

	RID owned_vb_full;
	uint32_t owned_vb_full_capacity = 0; // Bytes; grow-only.
	RID prev_pos_vb;
	uint64_t prev_pos_vb_address = 0; // Device address of prev_pos_vb, taken when it is allocated.
	uint32_t prev_pos_vb_capacity = 0; // Bytes; grow-only.
	// Copy of the mesh index buffer: RD frees a BLAS with its input buffers, so the BLAS must not read mesh-owned memory.
	RID owned_ib;
	uint32_t owned_ib_index_count = 0;
	RD::IndexBufferFormat owned_ib_format = RD::INDEX_BUFFER_FORMAT_UINT16;
	uint32_t cached_vertex_count = 0;
	uint32_t cached_full_size = 0;
	bool prev_pos_seeded = false; // False until the first owned->prev copy has run.
	bool prev_pos_current = false; // prev_pos_vb already equals the positions in owned_vb_full.

	RTCaptureState capture; // Vertex capture held by this entry, for captured mesh surfaces.
};

// A surface whose raster vertex() output is captured for the path tracer this frame.
// Made by `RenderRaytracing::prepare_vertex_captures`, drawn by the renderer, consumed by `build_tlas`.
struct RTCaptureRequest {
	const void *surf = nullptr; // RenderForwardClustered::GeometryInstanceSurfaceDataCache.
	RTVertexCapture::Layout layout; // Points and TBN block written by the capture draw.
	uint32_t arena_offset = 0; // Byte offset of the capture in the capture arena.
	bool recorded = false; // Set by the renderer once the capture draw is recorded; unrecorded requests count as pending.
	bool committed = false; // Set by build_tlas once the recorded capture replaced the surface's kept capture.
};

// Inputs of the plan passes of one view, read once per frame. Plans read engine state only through these and
// RenderRaytracing's read-only lookups, and write only their own arena records, so they can run on worker threads.
struct RTPlanContext {
	RendererRD::MeshStorage *mesh_storage = nullptr; // Read only.
	RendererRD::MaterialStorage *material_storage = nullptr; // Read only.
	const SceneShaderRaytracing *rt_shader = nullptr;
	const LocalVector<RTCaptureRequest> *captures = nullptr; // The view's capture requests; null while they are prepared.
	RTPlanVersions versions; // Global versions of the frame.
	RTVertexCapture::Stamps view_stamps; // Per-view capture inputs: frame, camera, mode and instance uniform versions.
	uint32_t rt_flags = 0; // SceneShaderRaytracing::RT_FLAG_* of the pipeline the view is traced with.
	uint32_t camera_visible_layers = 0xFFFFFFFFu; // Camera lane input of the instance masks.
	RTVertexCapture::Mode capture_mode = RTVertexCapture::MODE_DISABLED;
	const RTViewFrustum *frustum = nullptr; // Camera frustum of the view while its captures are prepared, else null.
	// Plans may update caches at their isolated points: material misses and first shader dependency analysis.
	bool allow_mutation = false;
};

// Per-instance records the merge bake reads: a MultiMesh or a 3D particle system.
struct RTInstanceSource {
	RID base; // MultiMesh or Particles RID.
	RID gpu_buffer; // Records: row-major 3x4 transform, then optional color and custom data.
	uint32_t count = 0; // Instances to bake.
	uint32_t stride = 0; // Floats per record.
	uint32_t current_offset = 0; // First record of this frame, in records (motion vector double buffering).
	uint32_t prev_offset = 0; // First record of the previous frame; equals current_offset when records aren't double buffered.
	uint32_t color_offset = UINT32_MAX; // Float offset of the instance color in a record; UINT32_MAX without colors.
	uint32_t custom_offset = UINT32_MAX; // Float offset of INSTANCE_CUSTOM in a record; UINT32_MAX without custom data.
	uint32_t trail_steps = 1; // Records per particle of a particle system with trails; particle_trails shaders read them all.
	uint64_t last_change = 0; // Changes whenever the records do.
	Vector3 dead_position; // Node-space point dead particles collapse to; keeps BLAS bounds near the emitter.
	bool particles = false; // Particle systems have no triangle cap and no CPU fallback.
	bool world_space = false; // Records are already in world space; the TLAS instance uses identity.
};

// One billboard entry of multimesh_merge.glsl (std430 layout). Bases are columns in MultiMesh node space.
struct RTMergeBillboardParams {
	float facing[3][4] = {}; // Facing basis before particle rotation and scaling.
	float node_basis[3][4] = {}; // Node basis; keep_scale reads world scale from node_basis * instance basis.
	float fixed_size[4] = {}; // fixed_size scale at instance origin o: dot(xyz, o) + w.
	uint32_t mode = 0; // RenderingBillboard::Mode.
	uint32_t keep_scale = 0; // Non-zero to keep the instance world scale.
	uint32_t fixed_size_enabled = 0; // Non-zero to apply fixed_size.
	uint32_t custom_offset = UINT32_MAX; // Float offset of INSTANCE_CUSTOM in an instance record; UINT32_MAX without custom data.

	bool operator==(const RTMergeBillboardParams &p_other) const { return memcmp(this, &p_other, sizeof(*this)) == 0; }
	bool operator!=(const RTMergeBillboardParams &p_other) const { return !(*this == p_other); }
};
static_assert(sizeof(RTMergeBillboardParams) == 128, "Must match BillboardParams in multimesh_merge.glsl.");

// Per-frame inputs of a merged bake, besides the instance records.
struct RTMergeBakeInputs {
	uint32_t billboard_index = UINT32_MAX; // Into RTViewportState::merge_billboards; UINT32_MAX when not camera-dependent.
	uint32_t prev_billboard_index = UINT32_MAX; // Same, for the previous camera and node transform.
	bool bake_motion = false; // Also bake previous-frame positions for motion vectors.
	// The entry holds vertex() output captured in TLAS instance space instead of transformed mesh vertices.
	bool captured = false;
	// Byte offset of this view's capture in the capture arena; RT_OFFSET_NONE keeps the entry's last bake.
	uint32_t capture_offset = RT_OFFSET_NONE;
};

// Cache entry for a per-(MultiMesh, surface) merged BLAS.
// Vertex data (positions, normals, tangents, mesh attributes) is baked per instance; instance
// color and custom data are read from the instance records at hit time.
struct RTMergedMMEntry {
	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if mesh has normals).
	// The BLAS reads only the position section; the hit shader reads TBN via normal_byte_offset.
	RID merged_vtx_buffer;
	uint32_t vtx_capacity_bytes = 0;

	// Merged attribute buffer: [UV + color × N*V] replicated per instance.
	RID merged_attr_buffer;
	uint32_t attr_capacity_bytes = 0;

	// Replicated index buffer: uint32 N*I entries (invalid if non-indexed).
	RID replicated_idx_buffer;
	uint32_t idx_capacity = 0;

	RID prev_pos_buffer; // Previous-frame float3 positions (N*V) for motion vectors.
	uint64_t prev_pos_address = 0; // Device address of prev_pos_buffer, taken when it is allocated.
	uint32_t prev_pos_capacity_bytes = 0;
	uint32_t motion_frame = UINT32_MAX; // Frame prev_pos_buffer was baked for; stale on any other frame.

	RID blas;
	uint32_t last_mm_count = 0;
	uint32_t last_surface_counter = 0;
	uint32_t last_used_frame = 0;
	uint64_t cached_mm_last_change = 0;
	bool blas_built_once = false;
	bool indexed = false; // selects MODE_INDEXED vs MODE_NON_INDEXED variant
	RTSurfaceData surface; // Traced geometry over the merged buffers, without motion vectors or BLAS.
	bool surface_valid = false; // Cleared by structure changes; `surface` is rebuilt by the next traced use.
	RTMergeBillboardParams baked_billboard; // Billboard inputs of the last bake (camera-dependent entries only).
	RTCaptureState capture; // Vertex capture held by this entry, for captured MultiMesh and particle surfaces.
};

// Surface of one geometry instance of a base resource: a camera-dependent merged MultiMesh surface (the bake
// depends on the node transform, so instances sharing a MultiMesh get separate entries), or a captured surface.
struct RTMMSurfaceKey {
	uint64_t multimesh_id = 0; // Full RID id of the MultiMesh or mesh, so a recycled slot never matches.
	uint64_t instance_id = 0; // Geometry instance identity.
	uint32_t surface_index = 0; // Mesh surface index.

	bool operator==(const RTMMSurfaceKey &p_other) const {
		return multimesh_id == p_other.multimesh_id && instance_id == p_other.instance_id && surface_index == p_other.surface_index;
	}

	uint32_t hash() const {
		return hash_fmix32(hash_murmur3_one_32(surface_index, hash_murmur3_one_64(instance_id, hash_murmur3_one_64(multimesh_id))));
	}
};

// Where an incremental prune of a HashMap<RTMMSurfaceKey, RID> resumes next frame.
struct RTHandleMapCursor {
	RTMMSurfaceKey key; // Next entry to check; the sweep restarts from the first entry when it is gone.
	bool valid = false; // False when the next check starts from the first entry.
};

// Incremental sweep over a RID_Owner: a snapshot of its RIDs, walked a bounded slice per frame.
struct RTPoolSweep {
	LocalVector<RID> pending; // RIDs owned when the snapshot was taken; freed ones fail get_or_null.
	uint32_t cursor = 0; // Next index into pending.
};

struct RTMaterialCacheEntry {
	RTMaterialData *ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint32_t slot_checked_frame = 0; // Frame the hit group slot of a custom shader was last found current.
	uint16_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
};

// Lookup key for per-viewport raytracing state.
//
// Shared viewports can trace different worlds into the same render buffers.
// The viewport RID keeps their TLAS / SSBO state separate.
struct RTViewportStateKey {
	RenderSceneBuffersRD *render_buffers = nullptr;
	RID viewport;
	RID environment;

	bool operator==(const RTViewportStateKey &p_other) const {
		return render_buffers == p_other.render_buffers &&
				viewport == p_other.viewport &&
				environment == p_other.environment;
	}

	bool operator!=(const RTViewportStateKey &p_other) const {
		return !(*this == p_other);
	}

	uint32_t hash() const {
		uint32_t h = hash_murmur3_one_64((uint64_t)(uintptr_t)render_buffers);
		h = hash_murmur3_one_64(viewport.get_id(), h);
		h = hash_murmur3_one_64(environment.get_id(), h);
		return hash_fmix32(h);
	}

	RTViewportStateKey() {}
	RTViewportStateKey(RenderSceneBuffersRD *p_render_buffers, RID p_viewport, RID p_environment) :
			render_buffers(p_render_buffers), viewport(p_viewport), environment(p_environment) {}
};

// Per-frame table compressing 20-bit render layers into the six
// shadow-caster group bits of the 8-bit TLAS instance mask. Two layer masks share a group iff they intersect the same
// subset of this frame's light shadow_caster_masks.
// SELF_SHADOWING casters get one extra group that only rays from SELF_SHADOWING receivers keep (RT_PARAM_SELF_SHADOW_MASK).
struct RTShadowGroupTable {
	LocalVector<uint32_t> caster_masks; // Distinct light shadow_caster_masks this frame.
	LocalVector<uint32_t> group_signatures; // Per group: which caster_masks it intersects.
	HashMap<uint32_t, uint32_t> signature_to_group;
	uint8_t self_shadow_bit = 0; // Group bit of SELF_SHADOWING casters; 0 when none this frame.

	static constexpr uint8_t group_bit(uint32_t p_group) { return uint8_t(1u << (2u + p_group)); }

	void reset() {
		caster_masks.clear();
		group_signatures.clear();
		signature_to_group.clear();
		self_shadow_bit = 0;
	}

	// Must run before group_bits_for_layers, so world groups never fill the slot or merge into it.
	void reserve_self_shadow_group() {
		ERR_FAIL_COND(!group_signatures.is_empty());
		self_shadow_bit = group_bit(0);
		group_signatures.push_back(0);
	}

	uint8_t self_shadow_bits_for_layers(uint32_t p_layers) {
		ERR_FAIL_COND_V(self_shadow_bit == 0, 0);
		group_signatures[0] |= signature_for_layers(p_layers);
		return self_shadow_bit;
	}

	void register_caster_mask(uint32_t p_caster_mask) {
		if (caster_masks.find(p_caster_mask) != -1) {
			return;
		}
		if (caster_masks.size() >= 31) {
			WARN_PRINT_ONCE("Raytracing: more than 31 distinct light shadow caster masks in a frame; extra lights will hit every shadow-caster group.");
			return;
		}
		caster_masks.push_back(p_caster_mask);
	}

	uint32_t signature_for_layers(uint32_t p_layers) const {
		uint32_t sig = 0;
		for (uint32_t i = 0; i < caster_masks.size(); i++) {
			if (p_layers & caster_masks[i]) {
				sig |= 1u << i;
			}
		}
		return sig;
	}

	// Lazily allocates a group per signature; past 6 groups, merges into the
	// last one (conservative: shadows may over-cast, never go missing).
	uint8_t group_bits_for_layers(uint32_t p_layers) {
		uint32_t sig = signature_for_layers(p_layers);
		const uint32_t *found = signature_to_group.getptr(sig);
		uint32_t group;
		if (found) {
			group = *found;
		} else if (group_signatures.size() < 6) {
			group = group_signatures.size();
			group_signatures.push_back(sig);
			signature_to_group.insert(sig, group);
		} else {
			WARN_PRINT_ONCE("Raytracing: more than 6 shadow-caster layer groups in a frame; merging (shadows may over-cast).");
			group = 5;
			group_signatures[5] |= sig;
			signature_to_group.insert(sig, group);
		}
		return group_bit(group);
	}

	uint8_t ray_mask_for_caster_mask(uint32_t p_caster_mask) const {
		int64_t idx = caster_masks.find(p_caster_mask);
		if (idx < 0) {
			return RT_SHADOW_GROUPS_MASK; // Unregistered (overflow): hit every group.
		}
		uint8_t mask = 0;
		for (uint32_t g = 0; g < group_signatures.size(); g++) {
			if (group_signatures[g] & (1u << (uint32_t)idx)) {
				mask |= group_bit(g);
			}
		}
		return mask;
	}
};

// Per-viewport raytracing state.
//
// Each viewport has its own visibility set (frustum/LOD/visibility ranges), so
// the TLAS instance composition and per-instance SSBO contents differ across
// viewports. Sharing them caused the wrong `gl_InstanceCustomIndexEXT` to
// resolve to the wrong `geometries[]` / `materials[]` entries, dereferencing
// stale BDAs and faulting the GPU.
//
// Lifetime is tied to a `RenderSceneBuffersRD`: created lazily on first
// `build_tlas` for that viewport, freed via `RenderRaytracing::free_viewport_state`
// from `RenderBufferDataForwardClustered::free_data()`.
struct RTViewportState {
	RenderSceneBuffersRD *render_buffers = nullptr;
	RID tlas;
	uint32_t tlas_max_instances = 0;
	RID tlas_transparent;
	uint32_t tlas_transparent_max_instances = 0;

	// Owned per state: the raster path's recycled scene UBO slots can race a
	// later pass's buffer_update against this state's in-flight RT dispatch.
	RID scene_ubo;
	uint32_t transparent_instance_count = 0;
	RTShadowGroupTable shadow_groups;

	RID geometry_buffer;
	uint32_t geometry_buffer_capacity = 0;
	RID motion_index_buffer;
	uint32_t motion_index_buffer_capacity = 0;
	RID motion_transform_buffer;
	uint32_t motion_transform_buffer_capacity = 0;

	// Camera-dependent merged MultiMesh surfaces, baked against this viewport's camera.
	HashMap<RTMMSurfaceKey, RID> billboard_mm_entries; // Handles into merged_mm_pool.
	RTHandleMapCursor billboard_mm_prune;
	LocalVector<RTMergeBillboardParams> merge_billboards; // This frame's entries, indexed by the merge push constant.
	RID merge_billboard_buffer;
	uint32_t merge_billboard_buffer_capacity = 0;

	// Camera-dependent vertex captures, captured with this viewport's camera. Handles into deformed_pool for
	// meshes, into merged_mm_pool for MultiMesh and particle surfaces.
	HashMap<RTMMSurfaceKey, RID> capture_entries;
	HashMap<RTMMSurfaceKey, RID> captured_mm_entries;
	RTHandleMapCursor capture_prune;
	RTHandleMapCursor captured_mm_prune;

	// Instance plans: plan_arenas[plan_current] is this frame's, the other last frame's.
	RTPlanArena plan_arenas[2];
	uint32_t plan_current = 0;
	uint64_t plan_frame = UINT64_MAX; // Frame plan_arenas[plan_current] was opened in.
	uint32_t plan_rt_flags = 0; // RTPlanContext::rt_flags the plans were made with.
	LocalVector<uint32_t> plan_lookup; // By instance rt_index: its record in last frame's arena, see rt_plan_lookup().

	RID light_buffer;
	RID params_buffer;
	RID scene_uniform_set;

	RID fog_shadows_ubo;
	RID fog_shadows_uniform_set;

	// Decal volumes: one instance per selected decal, queried inline from closest-hit.
	RID decal_tlas;
	uint32_t decal_tlas_max_instances = 0;
	RID decal_buffer;
	uint32_t decal_buffer_capacity = 0; // Bytes; grow-only.
	uint32_t decal_count = 0;
	HashSet<RID> selected_decals; // Last frame's selection while over budget (hysteresis).

	uint32_t frame_counter = 0;

	// Set per frame: the screen-space sky texture was drawn and primary-ray
	// misses should read it instead of the prefiltered radiance octmap.
	bool sky_screen_enabled = false;
};

class RenderRaytracing {
	friend class RenderForwardClustered;
	friend class RenderForwardClusteredPT;
	friend class RTTlasBuilder;

	RenderForwardClustered *owner = nullptr;
	SceneShaderRaytracing *shader = nullptr;
	BindlessBlock *bindless_block = nullptr;

	RID bindless_uniform_set;

	// Caching (chunked sparse caches indexed by RID low bits / 256).
	Vector<RTCacheEntry *> surface_chunks;
	Vector<RTMaterialCacheEntry *> material_chunks;

	// Merged MultiMesh BLAS cache and compute shader.
	struct MergeShader {
		enum Mode {
			MODE_NON_INDEXED = 0,
			MODE_INDEXED = 1,
			MODE_MAX,
		};
		// Matches BAKE_* in multimesh_merge.glsl.
		enum BakeFlags : uint32_t {
			BAKE_TBN = 1u, // Bake normals and tangents.
			BAKE_CAPTURED = 2u, // Copy positions and TBN from a vertex capture instead of transforming mesh vertices.
		};
		MultimeshMergeShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} mm_merge_shader;

	struct MMSurfaceHandles {
		LocalVector<RID> per_surface; // Grown on demand; entry per touched surface index.
		uint32_t mm_validator = 0; // High 32 bits of MM RID id at last access.

		// Resolve / grow storage for `p_surface_index`.
		RID &surface_handle(uint32_t p_surface_index) {
			if (p_surface_index >= per_surface.size()) {
				per_surface.resize(p_surface_index + 1);
			}
			return per_surface[p_surface_index];
		}
	};
	LocalVector<MMSurfaceHandles> mm_handles;

	RID_Owner<RTDeformedCacheEntry> deformed_pool;
	RID_Owner<RTMergedMMEntry> merged_mm_pool;
	RTPoolSweep deformed_sweep; // TTL eviction of deformed_pool.
	RTPoolSweep merged_mm_sweep; // TTL eviction of merged_mm_pool.

	RTDeformedCacheEntry *_access_deformed_slot(RID &r_handle);
	RTDeformedCacheEntry *_access_capture_entry(RID &r_handle);
	RTMergedMMEntry *_access_merged_mm_slot(RID &r_handle);
	RTMergedMMEntry *_access_captured_mm_entry(RID &r_handle);
	void _free_deformed_entry(RID p_handle);

	// Vertex capture. The arena holds one RTVertexCapture::Layout per request of the current view, the params
	// buffer one RTVertexCapture::DrawParams per request. Both are freed in cleanup_caches; the params buffer is
	// grow-only, the arena is sized by _reserve_capture_arena.
	// Captures are kept in pool entries: camera-independent ones here, camera-dependent ones per viewport.
	// Mesh surfaces use deformed_pool entries, MultiMesh and particle surfaces merged_mm_pool entries.
	RID capture_arena;
	uint32_t capture_arena_capacity = 0;
	uint64_t capture_arena_used = 0; // Bytes the current view's capture requests take.
	uint64_t capture_arena_window_peak = 0; // Largest view's arena size since capture_arena_window_start.
	uint64_t capture_arena_window_start = 0; // OS ticks in msec.
	void _reserve_capture_arena(uint64_t p_size);
	RID capture_params_buffer;
	uint32_t capture_params_capacity = 0;
	uint64_t capture_stamp_counter = 0;
	int32_t capture_mode = -1; // RTVertexCapture::Mode of the last prepared view; -1 before the first.
	uint32_t capture_mode_version = 0; // Bumped whenever capture_mode changes.
	uint32_t capture_code_version_counter = 0; // Source of RTVertexCapture::ShaderTraits::code_version.
	HashMap<RTMMSurfaceKey, RID> shared_capture_entries; // Handles into deformed_pool.
	HashMap<RTMMSurfaceKey, RID> shared_captured_mm_entries; // Handles into merged_mm_pool.
	RTHandleMapCursor shared_capture_prune;
	RTHandleMapCursor shared_captured_mm_prune;

	// Capture state of one surface for the current view.
	struct CaptureBinding {
		RID entry; // Owned by shared_capture_entries / shared_captured_mm_entries or the viewport's maps.
		bool merged = false; // `entry` is in merged_mm_pool (MultiMesh or particle surface), else in deformed_pool.
		uint32_t request = UINT32_MAX; // Capture recorded this view; UINT32_MAX keeps the entry's last capture.
		bool rebuild = false; // Rebuild the BLAS from the new capture instead of refitting it.
		RTVertexCapture::Stamps inputs; // Committed to the entry once the request is recorded.
	};
	struct CaptureCandidate {
		const void *surf = nullptr; // RenderForwardClustered::GeometryInstanceSurfaceDataCache.
		CaptureBinding binding;
		RTVertexCapture::Layout layout;
		Transform3D transform; // Capture space to world space.
		Vector3 dead_position; // Capture-space point culled instances collapse to.
	};
	LocalVector<CaptureCandidate> capture_candidates; // Per-view scratch: captured surfaces that are out of date.
	LocalVector<RTVertexCapture::DrawParams> capture_params; // Per-view scratch.
	LocalVector<CaptureBinding> capture_bindings; // Per-view scratch, indexed by the surface's rt_capture_binding.
	uint64_t capture_view = 1; // Number of the view capture_bindings belongs to; bumped when they are cleared. Surfaces start at 0.
	void _clear_capture_bindings();
	void _bind_capture(const void *p_surf, const CaptureBinding &p_binding);
	CaptureBinding *_capture_binding(const void *p_surf);
	void _update_capture_mode(RTVertexCapture::Mode p_mode);
	// Reads only, so plans call it on workers; r_scratch memoizes the per-shader global uniform stamp.
	static RTVertexCapture::Stamps _capture_inputs(RTPlanScratch &r_scratch, const void *p_surface_cache, uint32_t p_dependencies, const RTVertexCapture::Stamps &p_view, const RTVertexCapture::Stamps &p_kept, const RTVertexCapture::Layout &p_layout, uint64_t p_instances);
	RTCaptureState *_capture_state(const CaptureBinding &p_binding);
	const RTCaptureState *_peek_capture_state(const CaptureBinding &p_binding) const;
	void _reset_capture_entry(const CaptureBinding &p_binding);
	void _free_capture_entry_map(HashMap<RTMMSurfaceKey, RID> &r_map, bool p_merged);
	RID _surface_material_rid(const void *p_surf) const;
	RTMaterialData *_surface_material(const void *p_surf);
	// True when _commit_capture would return a binding for p_surf, without committing.
	bool _peek_captured(const void *p_surf, const LocalVector<RTCaptureRequest> &p_captures) const;
	CaptureBinding *_commit_capture(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RTCaptureRequest *&r_recorded);
	bool _resolve_captured_source(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RTDeformedGeometrySource &r_source);
	bool _resolve_captured_mm(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RID &r_entry, RTSchedulerAction &r_action, RTMergeBakeInputs &r_bake);
	void _snapshot_captured_positions(RTMergedMMEntry &r_entry, uint32_t p_point_count);
	void _free_merged_mm_entry(RID p_handle);
	RID &_shared_merged_mm_handle(const RTInstanceSource &p_source, uint32_t p_surface_key);

	// Dynamic BLAS scheduling (particles, camera-dependent MultiMesh, vertex captures). Budgets are shared by all viewports of a frame.
	RTDynamicScheduler dynamic_scheduler;
	RTCpuVisibilityEstimator cpu_visibility_estimator;
	uint64_t dynamic_scheduler_frame = UINT64_MAX;
	LocalVector<RTSchedulerItem> dynamic_items; // Per-viewport scratch.
	LocalVector<RTSchedulerAction> dynamic_actions; // Per-viewport scratch.
	void _begin_dynamic_scheduler_frame();
	void _schedule_dynamic_items(const RenderDataRD *p_render_data, const RTViewportState *p_state);

	// Dynamic Geometry debug view. Fed only while a viewport uses it; shared by viewports, since a BLAS updated by one is traced by all.
	RTDynamicGeometryDebug dynamic_geometry_debug;
	void _annotate_dynamic_geometry(const LocalVector<RID> &p_built, const LocalVector<RID> &p_refit);
	// Writes the scratch and capture arena use of the view just built to its render info.
	void _record_memory_info(const RenderDataRD *p_render_data) const;

	// Unconverged state traced during frame not_ready_frame, accumulated over its viewports.
	uint64_t not_ready_frame = UINT64_MAX;
	BitField<RSE::RenderingNotReadyReason> frame_not_ready_reasons;
	void _flag_not_ready(RSE::RenderingNotReadyReason p_reason);
	// Registers the light's projector texture as bindless; RT_LIGHT_NO_PROJECTOR when it has none.
	uint32_t _light_projector_texture(RID p_light);

	// Camera-independent merged particle surfaces, keyed by (particles, 0, surface ordinal).
	// Particles RIDs come from another allocator than MultiMesh ones, so they can't share mm_handles.
	HashMap<RTMMSurfaceKey, RID> particle_mm_entries; // Handles into merged_mm_pool.
	RTHandleMapCursor particle_mm_prune;

	// Multi-geometry BLASes of merged mesh surfaces. Not evicted by age: the RT instance set follows the
	// camera, so aging out would rebuild large BLASes when turning around.
	HashMap<RTMeshGroupKey, RTMeshGroupEntry> mesh_groups;
	uint64_t mesh_group_use_counter = 0;
	uint64_t mesh_group_build_first_use = 0; // mesh_group_use_counter when the current build_tlas started; newer variants are already in its TLAS.

	RTPlanWorkspace plan_workspace; // TLAS planning of the view being built.

	// Instance plans of the views, see render_raytracing_plan.h. Plans run before their apply pass, which keeps every
	// side effect; the context and arena of a view are opened once per frame.
	LocalVector<uint32_t> plan_capacities; // Scratch of _open_plan_arena.
	RTPlanContext _make_plan_context(const RenderDataRD *p_render_data, uint32_t p_rt_flags, const LocalVector<RTCaptureRequest> *p_captures) const;
	RTPlanArena &_open_plan_arena(RTViewportState *p_state, const RenderDataRD *p_render_data, const RTPlanContext &p_ctx);
	// Capture part of record p_index for p_inst (a RenderForwardClustered::GeometryInstanceForwardClustered): reused from
	// last frame's record when it had no capture surface, else planned. False when deferred.
	bool _plan_captures(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const RTViewportState &p_state, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index);
	bool _plan_capture_instance(const RTPlanContext &p_ctx, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index);
	bool _plan_capture_dependencies(const RTPlanContext &p_ctx, const void *p_surf);
	// Frustum test of the captures of record p_index, plus apply's inputs and decision for those whose memo entry is live.
	void _evaluate_captures(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index) const;
	// Kept capture of a live, unexpired entry; null when apply would allocate or reset it.
	const RTCaptureState *_peek_live_capture_state(RID p_entry, bool p_merged, uint32_t p_frame) const;
	// Binds or schedules the captures planned for p_inst in record p_index; memoizes their entries in the record.
	void _apply_capture_instance(const RTPlanContext &p_ctx, RTViewportState *p_state, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index);
	HashMap<RTMMSurfaceKey, RID> &_capture_entry_map(RTViewportState *p_state, uint8_t p_capture_flags);
	RID _resolve_capture_entry(RTViewportState *p_state, const RTMMSurfaceKey &p_key, RTPlanCapture &r_capture);
	const RTMaterialData *_plan_surface_material(const RTPlanContext &p_ctx, const void *p_surf, uint32_t &r_slot, uint16_t &r_counter);

	// Compression transforms of every compressed BLAS. Cleared last in cleanup_caches.
	RTBlasTransformPool blas_transform_pool;

	RTMeshGroupEntry *_process_mesh_group(const RTMeshGroupKey &p_key, Span<const RTPlanSurface> p_members, LocalVector<RID> &r_dirty_blas_list);
	void _trim_mesh_group_variants(uint64_t p_mesh_id);
	void _evict_dead_mesh_groups();

	LocalVector<uint32_t> material_free_slots;
	uint32_t next_material_slot = 0;
	uint64_t vram_used = 0;
	uint32_t cache_hits = 0;
	uint32_t cache_misses = 0;

	// Per-frame scratch arrays. Each unified-TLAS instance records its index
	// into geometry_data via gl_InstanceCustomIndexEXT.
	LocalVector<RT_GeometryData> geometry_data;
	RTMaterialTable material_table;
	LocalVector<int32_t> motion_indices; // Per-instance: index into motion_transforms[], or -1.
	LocalVector<RT_InstanceMotionData> motion_transforms; // Compact: only moving instances.

	struct RTInstanceArrays {
		LocalVector<RID> blass;
		LocalVector<Transform3D> transforms;
		LocalVector<uint32_t> flags;
		LocalVector<uint8_t> masks;
		LocalVector<uint32_t> sbt_offsets; // 0 = default material hit group
		LocalVector<uint32_t> custom_indices;

		void clear() {
			blass.clear();
			transforms.clear();
			flags.clear();
			masks.clear();
			sbt_offsets.clear();
			custom_indices.clear();
		}

		// Like clear(), but also frees the allocations.
		void reset() {
			blass.reset();
			transforms.reset();
			flags.reset();
			masks.reset();
			sbt_offsets.reset();
			custom_indices.reset();
		}

		void push_copy(const RTInstanceArrays &p_src, uint32_t p_index, uint8_t p_mask) {
			blass.push_back(p_src.blass[p_index]);
			transforms.push_back(p_src.transforms[p_index]);
			flags.push_back(p_src.flags[p_index]);
			masks.push_back(p_mask);
			sbt_offsets.push_back(p_src.sbt_offsets[p_index]);
			custom_indices.push_back(p_src.custom_indices[p_index]);
		}
	};
	RTInstanceArrays tlas_instances;
	RTInstanceArrays tlas_transparent_instances;

	// Decal volumes. Every instance shares one AABB BLAS spanning the decal's uv_local box.
	RID decal_box_aabb_buffer;
	RID decal_box_blas;
	RTInstanceArrays decal_instances;
	LocalVector<RendererRD::TextureStorage::DecalSortInfo> decal_infos;
	LocalVector<RTDecalCandidate> decal_candidates;
	LocalVector<RTDecalCandidate> decal_sort_scratch;
	LocalVector<RT_DecalData> decal_data;

	// Packed decals without distance fade, indexed by decal instance RID local index.
	struct DecalCacheEntry {
		RID instance; // Detects local index reuse.
		uint64_t version = 0; // DecalSortInfo::version at pack time.
		uint64_t bindless_version = 0; // BindlessBlock::get_version() at pack time.
		RT_DecalData data = {};
		Transform3D box_transform;
	};
	LocalVector<DecalCacheEntry> decal_cache;

	void _ensure_decal_box_blas();
	void _free_decal_box_blas();
	const DecalCacheEntry &_get_decal_cache_entry(const RendererRD::TextureStorage::DecalSortInfo &p_info);

	// Shared render buffers still get one state per viewport/world.
	HashMap<RTViewportStateKey, RTViewportState *> viewport_states;

	RTViewportState *_get_or_create_viewport_state(const RenderDataRD *p_render_data);
	RTViewportState *_get_viewport_state(const RenderDataRD *p_render_data) const;
	void _free_viewport_state_internal(RTViewportState *p_state);

	// Material UBO sub-allocation pool. One large device-address buffer divided
	// into fixed-size slots for performance reasons, and easier to debug.
	RID mat_ubo_pool_buffer;
	uint64_t mat_ubo_pool_bda = 0;
	LocalVector<uint32_t> mat_ubo_pool_free_slots;
	uint32_t mat_ubo_pool_free_count = 0;
	uint32_t mat_ubo_pool_next_slot = 0;

	void mat_ubo_pool_ensure_initialized();
	uint32_t mat_ubo_pool_allocate(); // Returns slot index or UINT32_MAX if pool is exhausted.
	void mat_ubo_pool_release(uint32_t p_slot);
	void mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size);
	uint64_t mat_ubo_pool_get_address(uint32_t p_slot) const;

	// Frees the uniform buffer of a material that didn't fit a pool slot.
	void _mat_free_dedicated_buffer(RTMaterialData *p_mat_data);

	// Cache helpers.
	static uint32_t get_rid_index(RID p_rid);
	static uint32_t get_rid_version(RID p_rid);
	RTCacheEntry *get_surface_cache_entry(uint32_t p_index);
	RTMaterialCacheEntry *get_material_cache_entry(uint32_t p_index);
	const RTMaterialCacheEntry *_find_material_cache_entry(uint32_t p_slot) const;
	uint32_t allocate_material_slot();

	// Material cache access split for planning. Custom-shader slots used last frame are checked by
	// _refresh_custom_shader_slots before planning; others are checked by process_material on first use.
	LocalVector<uint32_t> custom_material_slots; // Custom-shader material slots touched in material_frame.
	LocalVector<uint32_t> custom_material_slots_prev; // Same, for the frame before.
	uint32_t material_frame = UINT32_MAX; // Frame _refresh_custom_shader_slots last ran in.
	uint64_t material_slot_version = 0; // Bumped when a material's hit group slot changes; part of RTPlanKey::material_version.
	void _touch_material_entry(RTMaterialCacheEntry &r_entry, uint32_t p_slot, uint32_t p_frame);
	bool _custom_shader_slot_current(RTMaterialCacheEntry &r_entry, RID p_material_rid, uint32_t p_frame);
	void _refresh_custom_shader_slots();
	// Cached material, or null when missing, stale, or its custom-shader slot wasn't checked this frame. r_slot is the
	// material cache slot, UINT32_MAX for the default material.
	const RTMaterialData *lookup_material(RID p_material_rid, uint16_t p_material_invalidation_counter, uint32_t &r_slot) const;
	// Marks the material in p_slot used this frame like process_material does; the default material for UINT32_MAX.
	RTMaterialData *touch_material(uint32_t p_slot);

	// Internal methods.
	// Surface BLAS cache key of p_surf: the mesh RID local index (MultiMeshes use their mesh) and the surface index.
	static void _surface_cache_key(const void *p_surf, uint32_t &r_cache_key, uint32_t &r_mesh_rid_version);
	RTSurfaceData *process_surface(
			const void *p_surf,
			uint32_t p_surface_invalidation_counter,
			const Transform3D &p_transform,
			LocalVector<RID> &r_dirty_blas_list);
	// process_surface with the cache key resolved by _surface_cache_key.
	RTSurfaceData *process_surface_cached(
			const void *p_surf,
			uint32_t p_cache_key,
			uint32_t p_mesh_rid_version,
			uint32_t p_surface_invalidation_counter,
			const Transform3D &p_transform,
			LocalVector<RID> &r_dirty_blas_list);
	RTSurfaceData *process_deformed_surface(
			void *p_mesh_surface,
			const struct RTDeformedGeometrySource &p_source,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list);
	void _populate_surface_blas(
			void *p_mesh_surface,
			RID p_vertex_buffer_override,
			RID p_index_buffer_override,
			bool p_force_uncompressed,
			bool p_prefer_fast_build,
			bool p_allow_update,
			uint32_t p_cache_key,
			RTSurfaceData *r_surf_data,
			LocalVector<RID> &r_dirty_blas_list);
	RTMaterialData *process_material(RID p_material_rid, uint16_t p_material_invalidation_counter);
	bool _build_merged_mm_blas(
			const RTInstanceSource &p_source,
			void *p_mesh_surface,
			uint32_t p_surface_counter,
			RTMergedMMEntry &r_entry,
			uint32_t p_cache_key,
			RTViewportState *p_state,
			const RTMergeBakeInputs &p_inputs,
			RTSchedulerAction p_action,
			RD::ComputeListID p_compute_list,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list,
			RTSurfaceData *r_surf_data);
	void update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list);
	// Instances of the opaque or decal TLAS, and of the transparent TLAS, as handed to RD.
	LocalVector<RD::AccelerationStructureInstance> tlas_instance_scratch;
	LocalVector<RD::AccelerationStructureInstance> tlas_transparent_instance_scratch;
	// Creates r_tlas, or recreates it when p_instances outgrew it, and converts p_instances into r_instances.
	void _prepare_tlas(RID &r_tlas, uint32_t &r_max_instances, const RTInstanceArrays &p_instances, const String &p_name, LocalVector<RD::AccelerationStructureInstance> &r_instances);
	// Prepares and builds r_tlas on its own, tracked by RD.
	Error _build_one_tlas(RID &r_tlas, uint32_t &r_max_instances, const RTInstanceArrays &p_instances, const String &p_name);
	// Builds the dirty BLASes and both TLASes in one batch, behind a full barrier. Nothing may read them before the
	// next untracked_barrier(). On error the TLASes are invalidated and must not be traced this frame.
	Error build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list);
	void finalize_buffers(RTViewportState *p_state);
	void prepare_frame();

#ifdef RT_VALIDATION_ENABLED
	// Starts validation for the view being built and picks up the strict_validation setting.
	void _rtv_begin_view();
	// The buffer the uniform address of p_mat_data was taken from, or a null RID when it has none.
	RID _rtv_material_source(const RTMaterialData *p_mat_data) const;
	// Runs the strict checks on the tables of the view just built; false when the view must not be traced.
	bool _rtv_validate_view(const RTViewportState *p_state);

	// Strict validation only: parallel to geometry_data.
	LocalVector<RTGeometrySources> geometry_sources;

	// Strict validation only: every reused plan record is planned again into rtv_plan_arena and compared.
	RTPlanArena rtv_plan_arena;
	// Sizes rtv_plan_arena to one empty record like record p_index of p_arena, with its key.
	RTPlanArena &_rtv_replan_arena(const RTPlanArena &p_arena, uint32_t p_index);
	// Reports when record p_index differs in its TLAS or capture part from rtv_plan_arena's; p_planned false when deferred.
	void _rtv_check_reused_plan(const RTPlanArena &p_arena, uint32_t p_index, bool p_tlas_part, bool p_planned) const;
	void _rtv_check_reused_captures(const RTPlanContext &p_ctx, const void *p_inst, const RTPlanArena &p_arena, uint32_t p_index);
#endif

public:
	void initialize(RenderForwardClustered *p_owner);

	void cleanup_caches();

	// Value of the rendering/pathtracing/vertex_capture_mode project setting.
	static RTVertexCapture::Mode read_capture_mode();
	// True when p_surf (a RenderForwardClustered::GeometryInstanceSurfaceDataCache) of a mesh, 3D MultiMesh or 3D
	// particle system is vertex-captured under p_mode.
	static bool is_capture_surface(const void *p_surf, RTVertexCapture::Mode p_mode);
	// Changes whenever the vertex capture mode does; surfaces eligible for capture change with it.
	uint32_t get_capture_mode_version() const { return capture_mode_version; }

	// Collects the captured surfaces of this view whose capture is out of date and whose hit group is ready in
	// the p_rt_flags pipeline, lets the dynamic scheduler pick the ones recaptured this frame, assigns their arena
	// ranges and uploads their draw parameters.
	// Leaves `r_requests` empty when nothing needs capturing, or capture is disabled or unsupported.
	void prepare_vertex_captures(const RenderDataRD *p_render_data, uint32_t p_rt_flags, LocalVector<RTCaptureRequest> &r_requests);
	// Storage buffer of RTVertexCapture::DrawParams, indexed by the capture draw's instance data index.
	RID get_capture_params_buffer() const { return capture_params_buffer; }
	// Storage buffer the capture draws write into.
	RID get_capture_arena() const { return capture_arena; }

	// Builds the view's TLAS. `r_captures` are the requests of `prepare_vertex_captures`, after the renderer recorded them;
	// the ones traced this view are marked committed.
	RTViewportState *build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags, LocalVector<RTCaptureRequest> &r_captures);
	void build_decal_volumes(RTViewportState *p_state, const RenderDataRD *p_render_data);
	uint32_t gather_lights(const RenderDataRD *p_render_data, const RTViewportState *p_state, RT_LightData *r_light_data, uint32_t p_max_lights);
	void update_scene_ubo(RTViewportState *p_state, const RenderDataRD *p_render_data, const Size2i &p_screen_size, const Color &p_default_bg_color);
	RID update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags);

	void copy_output_texture(const RenderDataRD *p_render_data);
	void free_viewport_state(RenderSceneBuffersRD *p_render_buffers);

	// Raytracing output textures (stored on the render buffers via named scopes).
	void rt_ensure_textures(RenderSceneBuffersRD *p_render_buffers);
	bool rt_has_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_texture(RenderSceneBuffersRD *p_render_buffers) const;
	bool rt_has_depth_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_depth_texture(RenderSceneBuffersRD *p_render_buffers) const;

	// Screen-space sky, drawn before the trace so primary-ray misses can read
	// the real sky shader instead of the prefiltered radiance octmap.
	void rt_ensure_sky_texture(RenderSceneBuffersRD *p_render_buffers);
	bool rt_has_sky_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_sky_texture(RenderSceneBuffersRD *p_render_buffers) const;
	RID rt_get_sky_framebuffer(RenderSceneBuffersRD *p_render_buffers) const;

	// DLSS Ray Reconstruction guide buffers (stored on the render buffers).
	void dlss_rr_ensure_buffers(RenderSceneBuffersRD *p_render_buffers);
	void dlss_rr_free_buffers(RenderSceneBuffersRD *p_render_buffers);
	bool dlss_rr_has_buffers(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_diffuse_albedo(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_specular_albedo(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_normal_roughness(RenderSceneBuffersRD *p_render_buffers) const;
	RID dlss_rr_get_specular_hit_dist(RenderSceneBuffersRD *p_render_buffers) const;

	BitField<RSE::RenderingNotReadyReason> get_not_ready_reasons() const;

#ifdef RT_VALIDATION_ENABLED
	// Under strict validation, false when a BLAS that a TLAS of p_state references was freed, rebuilt or refit after
	// the TLAS was built, so the trace must be skipped. Call right before tracing.
	bool rtv_validate_trace(const RTViewportState *p_state);
#endif

	SceneShaderRaytracing *get_shader() const { return shader; }

	RID get_bindless_uniform_set() const { return bindless_uniform_set; }
	BindlessBlock *get_bindless_block() const { return bindless_block; }
	RID get_mat_ubo_pool_buffer() const { return mat_ubo_pool_buffer; }
	// Storage buffer of RTMaterialTable; null before the first view was finalized.
	RID get_material_buffer() const { return material_table.get_buffer(); }

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
