/**************************************************************************/
/*  test_render_raytracing.h                                              */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_rt_material_state.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

#include "tests/test_macros.h"

namespace TestRenderRaytracing {

using namespace RendererSceneRenderImplementation;

static constexpr uint32_t transparent(uint32_t p_blend_class) {
	return RT_MAT_FLAG_TRANSPARENT | (p_blend_class << RT_MAT_BLEND_CLASS_SHIFT);
}

TEST_CASE("[RenderRaytracing] Velocity ownership follows the dominant peel layer rule") {
	constexpr float dominant = 0.5f;
	CHECK(rt_material_can_own_velocity(0, dominant));
	CHECK(rt_material_can_own_velocity(RT_MAT_FLAG_ALPHA_SCISSOR, dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_PREMULT), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MUL), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_OIT), dominant));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_ADD), dominant));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_SUB), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_ADD) | RT_MAT_FLAG_DEPTH_DRAW_ALWAYS, dominant));
}

TEST_CASE("[RenderRaytracing] A zero dominant layer alpha leaves velocity to opaque and depth_draw_always surfaces") {
	CHECK(rt_material_can_own_velocity(0, 0.0f));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX), 0.0f));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX) | RT_MAT_FLAG_DEPTH_DRAW_ALWAYS, 0.0f));
}

TEST_CASE("[RenderRaytracing] Emission flags carry the operator and UV2 addressing only when emission samples UV2") {
	CHECK(rt_material_emission_flags(false, false, false, false) == 0u);
	CHECK(rt_material_emission_flags(true, false, false, false) == uint32_t(RT_MAT_FLAG_EMISSION_ADD));
	CHECK(rt_material_emission_flags(false, false, true, true) == 0u);
	CHECK(rt_material_emission_flags(false, true, false, true) == uint32_t(RT_MAT_FLAG_EMISSION_UV2));
	CHECK(rt_material_emission_flags(false, true, true, false) == uint32_t(RT_MAT_FLAG_EMISSION_UV2 | RT_MAT_FLAG_UV2_TRIPLANAR));
	CHECK(rt_material_emission_flags(true, true, true, true) ==
			uint32_t(RT_MAT_FLAG_EMISSION_ADD | RT_MAT_FLAG_EMISSION_UV2 | RT_MAT_FLAG_UV2_TRIPLANAR | RT_MAT_FLAG_UV2_TRIPLANAR_WORLD));
}

TEST_CASE("[RenderRaytracing] Material index keeps the instance transparency where albedo_texture does") {
	RT_MaterialData albedo = {};
	albedo.albedo_texture.instance_transparency = 0xAB;
	uint32_t albedo_bits = 0;
	memcpy(&albedo_bits, &albedo.albedo_texture, sizeof(uint32_t));

	const uint32_t packed = rt_pack_material_index(12345, 0xAB);
	CHECK((packed & RT_MATERIAL_INDEX_MASK) == 12345u);
	CHECK((packed & ~RT_MATERIAL_INDEX_MASK) == albedo_bits);
	CHECK(rt_pack_material_index(7, rt_instance_transparency(1.0f)) == 7u);
}

TEST_CASE("[RenderRaytracing] Material table merges dirty entries into upload ranges") {
	LocalVector<uint32_t> dirty;
	LocalVector<RTMaterialTable::Range> ranges;
	RTMaterialTable::merge_ranges(dirty, 2, ranges);
	CHECK(ranges.is_empty());

	for (uint32_t index : { 9u, 1u, 3u, 3u, 20u, 2u }) {
		dirty.push_back(index);
	}
	RTMaterialTable::merge_ranges(dirty, 2, ranges);
	REQUIRE(ranges.size() == 3);
	CHECK(ranges[0].begin == 1);
	CHECK(ranges[0].end == 4);
	CHECK(ranges[1].begin == 9);
	CHECK(ranges[1].end == 10);
	CHECK(ranges[2].begin == 20);

	dirty.push_back(6);
	RTMaterialTable::merge_ranges(dirty, 2, ranges);
	REQUIRE(ranges.size() == 2);
	CHECK(ranges[0].end == 10);
	CHECK(ranges[1].begin == 20);
}

TEST_CASE("[RenderRaytracing] Material table grows with default materials and keeps entry 0 the default") {
	RTMaterialTable table;
	RT_MaterialData red = rt_material_data_default();
	red.albedo_color[1] = 0.0f;
	table.set(3, red);

	const Span<RT_MaterialData> entries = table.get_entries();
	REQUIRE(entries.size() == 4);
	CHECK(memcmp(&entries[0], &entries[1], sizeof(RT_MaterialData)) == 0);
	CHECK(entries[0].albedo_color[1] == 1.0f);
	CHECK(entries[3].albedo_color[1] == 0.0f);
	CHECK_FALSE(table.get_buffer().is_valid());
}

TEST_CASE("[RenderRaytracing] BaseMaterial3D state survives the material parameter round trip") {
	CHECK(RTBaseMaterialState().encode() == 0);

	RTBaseMaterialState state;
	state.uv1_triplanar = true;
	state.uv2_world_triplanar = true;
	state.uv2_vertex_transform = true;
	state.distance_fade_mode = 3;
	state.emission = true;
	state.emission_on_uv2 = true;
	state.ambient_occlusion = true;
	state.roughness_texture_channel = 4;

	const int64_t packed = state.encode();
	CHECK(packed > 0);
	CHECK(packed <= INT32_MAX);

	const RTBaseMaterialState decoded = RTBaseMaterialState::decode(packed);
	CHECK(decoded.uv1_triplanar);
	CHECK_FALSE(decoded.uv1_world_triplanar);
	CHECK_FALSE(decoded.uv2_triplanar);
	CHECK(decoded.uv2_world_triplanar);
	CHECK(decoded.uv2_vertex_transform);
	CHECK_FALSE(decoded.proximity_fade);
	CHECK(decoded.distance_fade_mode == 3);
	CHECK(decoded.emission);
	CHECK_FALSE(decoded.emission_add);
	CHECK(decoded.emission_on_uv2);
	CHECK_FALSE(decoded.orm);
	CHECK(decoded.ambient_occlusion);
	CHECK(decoded.roughness_texture_channel == 4);
	CHECK(decoded.encode() == packed);
}

TEST_CASE("[RenderRaytracing] The default material is a non-emissive dielectric with identity UV transforms") {
	const RT_MaterialData d = rt_material_data_default();
	CHECK(d.flags == 0u);
	CHECK(d.emission_strength == 0.0f);
	CHECK(d.emission_indirect == 1.0f);
	CHECK(d.emission_texture.index == 0u);
	CHECK(d.albedo_color[3] == 1.0f);
	CHECK(d.albedo_texture.index == 0u);
	CHECK(d.albedo_texture.instance_transparency == 0u);
	CHECK(d.surface_params.roughness == 255u);
	CHECK(d.surface_params.metallic == 0u);
	CHECK(d.surface_params.specular == 128u);
	for (int i = 0; i < 3; i++) {
		CHECK(d.emission_color[i] == 0.0f);
		CHECK(d.uv1_scale[i] == 1.0f);
		CHECK(d.uv1_offset[i] == 0.0f);
	}
	CHECK(Math::half_to_float(d.uv2_scale_xy.x) == 1.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.y) == 1.0f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.scale) == 1.0f);
	CHECK(d.uv2_scale_offset_z.offset == 0u);
	CHECK(d.uv2_offset_xy.x == 0u);
	CHECK(d.uv2_offset_xy.y == 0u);
	CHECK(Math::half_to_float(d.blend_sharpness.uv1) == 1.0f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv2) == 1.0f);
}

TEST_CASE("[RenderRaytracing] Packed UV fields round-trip, with the UV2 offset reduced modulo 1") {
	RT_MaterialData d = rt_material_data_default();
	rt_material_set_packed_uv(d, 8.0f, Vector3(2.0f, 0.5f, 4.0f), Vector3(3.25f, -0.75f, 1.5f), 40.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.x) == 2.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.y) == 0.5f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.scale) == 4.0f);
	CHECK(Math::half_to_float(d.uv2_offset_xy.x) == 0.25f);
	CHECK(Math::half_to_float(d.uv2_offset_xy.y) == 0.25f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.offset) == 0.5f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv1) == 8.0f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv2) == 40.0f);
}

template <typename T>
static uint32_t as_uint(const T &p_bitfields) {
	static_assert(sizeof(T) == sizeof(uint32_t));
	uint32_t word = 0;
	memcpy(&word, &p_bitfields, sizeof(word));
	return word;
}

TEST_CASE("[RenderRaytracing] Material bitfields fill from the low bit, as the GLSL accessors unpack them") {
	RT_MaterialData d = {};
	d.roughness_texture.index = BindlessBlock::MAX_BINDLESS_TEXTURES - 1;
	d.roughness_texture.channel = RT_TEXTURE_CHANNEL_GRAYSCALE;
	CHECK(as_uint(d.roughness_texture) == ((BindlessBlock::MAX_BINDLESS_TEXTURES - 1) | (uint32_t(RT_TEXTURE_CHANNEL_GRAYSCALE) << RT_MaterialData::TEXTURE_INDEX_BITS)));

	d.albedo_texture.index = 7;
	d.albedo_texture.instance_transparency = 0xAB;
	CHECK(as_uint(d.albedo_texture) == (7u | (0xABu << RT_MaterialData::TEXTURE_INDEX_BITS)));

	rt_material_set_surface_params(d, 1.0f, 0.0f, 0.5f);
	CHECK(as_uint(d.surface_params) == (255u | (128u << 16)));

	// unpackHalf2x16() reads x from the low half.
	d.uv2_scale_xy.x = 0x1234;
	d.uv2_scale_xy.y = 0xABCD;
	CHECK(as_uint(d.uv2_scale_xy) == 0xABCD1234u);
}

TEST_CASE("[RenderRaytracing] Light bitfields fill from the low bit, as lights_get() unpacks them") {
	RT_LightData d = {};
	d.masks.cull_mask = 0xFFFFF;
	d.masks.type = RT_LIGHT_TYPE_SPOT;
	d.masks.shadow_ray_mask = 0xA5;
	CHECK(as_uint(d.masks) == (0xFFFFFu | (uint32_t(RT_LIGHT_TYPE_SPOT) << 20) | (0xA5u << 24)));

	d.projector_texture.index = BindlessBlock::MAX_BINDLESS_TEXTURES - 1;
	CHECK(as_uint(d.projector_texture) == BindlessBlock::MAX_BINDLESS_TEXTURES - 1);

	d.emission_rg.r = 0x1234;
	d.emission_rg.g = 0xABCD;
	CHECK(as_uint(d.emission_rg) == 0xABCD1234u);
}

TEST_CASE("[RenderRaytracing] Light fp16 saturates instead of overflowing, snorm16 matches unpackSnorm2x16()") {
	CHECK(Math::half_to_float(rt_half(1.0e6f)) == 65504.0f);
	CHECK(Math::half_to_float(rt_half(-1.0e6f)) == -65504.0f);
	CHECK(Math::half_to_float(rt_half(0.5f)) == 0.5f);
	CHECK(rt_snorm16(1.0f) == 0x7FFFu);
	CHECK(rt_snorm16(-1.0f) == 0x8001u);
	CHECK(rt_snorm16(2.0f) == 0x7FFFu);
	CHECK(rt_snorm16(0.0f) == 0u);
}

TEST_CASE("[RenderRaytracing] Instance transparency is zero when opaque and saturates outside [0, 1]") {
	CHECK(rt_instance_transparency(1.0f) == 0u);
	CHECK(rt_instance_transparency(0.0f) == 255u);
	CHECK(rt_instance_transparency(0.5f) == 127u);
	CHECK(rt_instance_transparency(2.0f) == 0u);
	CHECK(rt_instance_transparency(-1.0f) == 255u);
}

TEST_CASE("[RenderRaytracing] Light distance fade is full before the begin distance and smoothstepped to zero over the length") {
	using RendererRD::LightStorage;
	CHECK(LightStorage::distance_fade_factor(10.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(40.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(45.0f, 40.0f, 10.0f) == doctest::Approx(0.5f));
	CHECK(LightStorage::distance_fade_factor(42.0f, 40.0f, 10.0f) == doctest::Approx(Math::smoothstep(0.0f, 1.0f, 0.8f)));
	CHECK(LightStorage::distance_fade_factor(50.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(60.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(41.0f, 40.0f, 0.0f) == 0.0f);
}

#ifdef RT_VALIDATION_ENABLED
static RT_GeometryData rtv_geometry(uint64_t p_vertex_address, uint32_t p_vertex_count) {
	RT_GeometryData geometry = {};
	geometry.vertex_buffer_address = p_vertex_address;
	geometry.vertex_count = p_vertex_count;
	geometry.position_stride = 12;
	geometry.normal_byte_offset = RT_OFFSET_NONE;
	geometry.tangent_byte_offset = RT_OFFSET_NONE;
	geometry.index_format = RT_INDEX_FORMAT_NONE;
	return geometry;
}

// One geometry using the default material, checked against p_buffers.
static LocalVector<String> rtv_check_geometry(const RT_GeometryData &p_geometry, RID p_vertex_source, Span<RD::BufferAddressRange> p_buffers) {
	const RT_GeometryData geometries[] = { p_geometry };
	RTGeometrySources sources;
	sources.vertex = p_vertex_source;
	const RTGeometrySources geometry_sources[] = { sources };
	const RT_MaterialData materials[] = { rt_material_data_default() };
	const RID material_sources[] = { RID() };

	RTValidationView view;
	view.geometries = geometries;
	view.geometry_sources = geometry_sources;
	view.materials = materials;
	view.material_sources = material_sources;
	view.buffers = p_buffers;

	LocalVector<String> issues;
	RTValidation().check_view(view, issues);
	return issues;
}

TEST_CASE("[RenderRaytracing] Strict validation requires a recorded source buffer large enough for the read") {
	const RID buffer = RID::from_uint64(1);
	const RD::BufferAddressRange buffers[] = { { buffer, 0x10000, 1200 } };

	CHECK(rtv_check_geometry(rtv_geometry(0x10000, 100), RID(), buffers).size() == 1);
	CHECK(rtv_check_geometry(rtv_geometry(0x10000, 100), buffer, buffers).is_empty());
	CHECK(rtv_check_geometry(rtv_geometry(0x10000, 101), buffer, buffers).size() == 1);
}

TEST_CASE("[RenderRaytracing] Strict validation rejects addresses into freed or reused buffers") {
	const RID source = RID::from_uint64(1);
	const RID reused = RID::from_uint64(2);
	const RD::BufferAddressRange other[] = { { source, 0x30000, 1200 } };
	const RD::BufferAddressRange reallocated[] = { { reused, 0x20000, 1200 } };

	CHECK(rtv_check_geometry(rtv_geometry(0x20000, 10), source, Span<RD::BufferAddressRange>()).size() == 1);
	CHECK(rtv_check_geometry(rtv_geometry(0x20000, 10), source, other).size() == 1);
	CHECK(rtv_check_geometry(rtv_geometry(0x20000, 10), source, reallocated).size() == 1);
}

TEST_CASE("[RenderRaytracing] Strict validation checks TLAS instances and stale BLASes") {
	const RTValidation validation;
	LocalVector<String> issues;

	const uint32_t custom_indices[] = { 0, 5 };
	validation.check_tlas(custom_indices, 1, "TLAS", issues);
	CHECK(issues.size() == 1);

	issues.clear();
	const RID stale[] = { RID::from_uint64(3), RID::from_uint64(4) };
	validation.check_tlas_blases(stale, "TLAS", issues);
	CHECK(issues.size() == 2);
}

TEST_CASE("[RenderRaytracing] Strict validation checks material indices and bindless texture indices") {
	RT_GeometryData geometry = rtv_geometry(0, 0);
	RT_MaterialData material = rt_material_data_default();
	RT_MaterialData faded = rt_material_data_default();
	const RID material_sources[] = { RID(), RID() };
	const RTGeometrySources geometry_sources[] = { RTGeometrySources() };

	RTValidationView view;
	view.geometry_sources = geometry_sources;
	view.material_sources = material_sources;
	view.bindless_texture_count = 4;

	const auto check = [&](uint32_t p_material_index) {
		geometry.material_index = p_material_index;
		const RT_GeometryData geometries[] = { geometry };
		const RT_MaterialData materials[] = { material, faded };
		view.geometries = geometries;
		view.materials = materials;
		LocalVector<String> issues;
		RTValidation().check_view(view, issues);
		return issues.size();
	};

	material.albedo_texture.index = 7;
	CHECK(check(0) == 1);
	CHECK(check(2) == 1);

	// The instance transparency is not part of the bindless index.
	faded.albedo_texture.index = 3;
	CHECK(check(rt_pack_material_index(1, rt_instance_transparency(0.0f))) == 0);
}

TEST_CASE("[RenderRaytracing] Strict validation reports custom texture slots outside the block or bindless range") {
	RTValidation validation;
	validation.begin_view();
	validation.check_texture_slot("tex", 60, 64, 3, 4);

	RTValidationView view;
	LocalVector<String> issues;
	validation.check_view(view, issues);
	CHECK(issues.is_empty());

	validation.check_texture_slot("tex", 62, 64, 9, 4);
	validation.check_view(view, issues);
	CHECK(issues.size() == 2);

	validation.begin_view();
	issues.clear();
	validation.check_view(view, issues);
	CHECK(issues.is_empty());
}
#endif

TEST_CASE("[RenderRaytracing] Material buffer uniform sizes match the shader compiler layout") {
	using Uniform = ShaderLanguage::ShaderNode::Uniform;
	Uniform uniform;
	uniform.type = ShaderLanguage::TYPE_FLOAT;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);

	uniform.array_size = 3;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 48);

	uniform.type = ShaderLanguage::TYPE_MAT4;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 192);

	uniform.type = ShaderLanguage::TYPE_VEC4;
	uniform.array_size = 0;
	uniform.scope = Uniform::SCOPE_GLOBAL;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);
}

TEST_CASE("[RenderRaytracing] Shadow renderers only apply while shadows are enabled and visible") {
	constexpr RS::ShadowRenderer RASTER = RS::SHADOW_RENDERER_RASTERIZER;
	constexpr RS::ShadowRenderer PT = RS::SHADOW_RENDERER_PATH_TRACER;
	constexpr uint32_t ALL = RS::SHADOW_RENDERER_ALL;

	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, 0, RASTER));

	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 0.01f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, PT));
}

TEST_CASE("[RenderRaytracing] Geometry shadow renderers only apply while cast shadows is not off") {
	constexpr RS::ShadowRenderer RASTER = RS::SHADOW_RENDERER_RASTERIZER;
	constexpr RS::ShadowRenderer PT = RS::SHADOW_RENDERER_PATH_TRACER;
	constexpr uint32_t ALL = RS::SHADOW_RENDERER_ALL;

	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_OFF, ALL, RASTER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_OFF, ALL, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, ALL, RASTER));
	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, ALL, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, PT, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, PT, RASTER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, RASTER, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_ON, 0, RASTER));
	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_DOUBLE_SIDED, PT, PT));
	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_SHADOWS_ONLY, PT, PT));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_SHADOWS_ONLY, PT, RASTER));
}

TEST_CASE("[RenderRaytracing] Self shadowing geometry only casts path traced shadows") {
	constexpr uint32_t ALL = RS::SHADOW_RENDERER_ALL;

	CHECK(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_SELF_SHADOWING, ALL, RS::SHADOW_RENDERER_PATH_TRACER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_SELF_SHADOWING, ALL, RS::SHADOW_RENDERER_RASTERIZER));
	CHECK_FALSE(RendererSceneCull::is_shadow_cast_by(RS::SHADOW_CASTING_SETTING_SELF_SHADOWING, RS::SHADOW_RENDERER_RASTERIZER, RS::SHADOW_RENDERER_PATH_TRACER));
}

TEST_CASE("[RenderRaytracing] Shadow group table leaves all six groups to world casters without self shadowing") {
	RTShadowGroupTable table;
	table.register_caster_mask(0xFFFFFFFF);
	CHECK(table.self_shadow_bit == 0);
	CHECK(table.group_bits_for_layers(1) == RTShadowGroupTable::group_bit(0));
}

TEST_CASE("[RenderRaytracing] Shadow group table keeps the self shadowing group apart from world groups") {
	constexpr uint32_t WORLD_LAYER = 1u << 0;
	constexpr uint32_t VIEWMODEL_LAYER = 1u << 1;
	RTShadowGroupTable table;
	table.register_caster_mask(0xFFFFFFFF); // Sun.
	table.register_caster_mask(WORLD_LAYER); // Lamp that ignores the viewmodel.
	table.reserve_self_shadow_group();

	const uint8_t self_bit = table.self_shadow_bits_for_layers(VIEWMODEL_LAYER);
	const uint8_t world_bit = table.group_bits_for_layers(WORLD_LAYER);
	CHECK(self_bit == table.self_shadow_bit);
	CHECK(self_bit != 0);
	CHECK((world_bit & self_bit) == 0);
	CHECK(table.ray_mask_for_caster_mask(0xFFFFFFFF) == (self_bit | world_bit));
	CHECK(table.ray_mask_for_caster_mask(WORLD_LAYER) == world_bit);
}

TEST_CASE("[RenderRaytracing] Shadow group overflow never merges world casters into the self shadowing group") {
	RTShadowGroupTable table;
	for (uint32_t layer = 0; layer < 10; layer++) {
		table.register_caster_mask(1u << layer);
	}
	table.reserve_self_shadow_group();
	ERR_PRINT_OFF;
	for (uint32_t layer = 0; layer < 10; layer++) {
		CHECK((table.group_bits_for_layers(1u << layer) & table.self_shadow_bit) == 0);
	}
	ERR_PRINT_ON;
}

TEST_CASE("[RenderRaytracing] Instance data layout packs stride and field offsets") {
	CHECK(rt_pack_instance_data_layout(20, 12, 16) == (20u | (12u << 8) | (16u << 16)));
	const uint32_t no_fields = rt_pack_instance_data_layout(12, UINT32_MAX, UINT32_MAX);
	CHECK((no_fields & 0xFFu) == 12u);
	CHECK(((no_fields >> 8) & 0xFFu) == RT_INSTANCE_DATA_FIELD_NONE);
	CHECK(((no_fields >> 16) & 0xFFu) == RT_INSTANCE_DATA_FIELD_NONE);
}

// What one camera ray sees of a triangle: whether it is drawn, its FRONT_FACING and whether its normal is flipped.
struct FacingResult {
	bool visible = false;
	bool front_facing = false;
	bool normal_flipped = false;

	bool operator==(const FacingResult &p_other) const {
		return visible == p_other.visible && (!visible || (front_facing == p_other.front_facing && normal_flipped == p_other.normal_flipped));
	}
};

// The rasterizer: culls by the mesh winding (screen winding, reversed for mirrored instances); FRONT_FACING is the
// screen winding, or the mesh winding when p_preserve_front_facing.
static FacingResult raster_facing(RS::CullMode p_cull_mode, bool p_raster_mirror, bool p_world_front, bool p_preserve_front_facing) {
	const bool mesh_front = p_world_front != p_raster_mirror;
	FacingResult result;
	result.visible = p_cull_mode == RS::CULL_MODE_DISABLED || mesh_front == (p_cull_mode == RS::CULL_MODE_BACK);
	result.front_facing = p_preserve_front_facing ? mesh_front : p_world_front;
	result.normal_flipped = p_cull_mode != RS::CULL_MODE_BACK && !result.front_facing;
	return result;
}

// The ray tracer: decides facing in BLAS space, where the default front face is the rasterizer's back face, culls back
// faces unless the instance disables it, and the hit shader applies the geometry flags.
static FacingResult traced_facing(const RTTriangleFacing &p_facing, bool p_blas_front) {
	const bool flip = p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
	const bool hit_front = p_blas_front == flip;
	FacingResult result;
	result.visible = hit_front || (p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
	result.front_facing = hit_front != bool(p_facing.geometry_flags & RT_GEOM_FLAG_FACING_INVERTED);
	result.normal_flipped = !result.front_facing && (p_facing.geometry_flags & RT_GEOM_FLAG_SIDE_CHECK);
	return result;
}

TEST_CASE("[RenderRaytracing] Path traced triangle facing matches raster for every cull mode and mirroring") {
	const RS::CullMode cull_modes[] = { RS::CULL_MODE_BACK, RS::CULL_MODE_FRONT, RS::CULL_MODE_DISABLED };
	for (RS::CullMode cull_mode : cull_modes) {
		for (int bits = 0; bits < 16; bits++) {
			const bool raster_mirror = bits & 1;
			// Billboards, captures and merged instances can have a TLAS transform mirrored unlike the node.
			const bool tlas_mirror = bits & 2;
			const bool blas_front = bits & 4;
			const bool preserve = bits & 8;
			const bool world_front = blas_front != tlas_mirror;
			const FacingResult traced = traced_facing(rt_triangle_facing(cull_mode, raster_mirror, tlas_mirror, preserve), blas_front);
			CHECK_MESSAGE(traced == raster_facing(cull_mode, raster_mirror, world_front, preserve),
					vformat("cull mode %d, raster mirror %d, TLAS mirror %d, BLAS front %d, preserve %d", int(cull_mode), int(raster_mirror), int(tlas_mirror), int(blas_front), int(preserve)));
		}
	}
}

TEST_CASE("[RenderRaytracing] Preserved front facing keeps double-sided normals of mirrored instances unflipped") {
	// A mesh front face of a mirrored node (BLAS front, TLAS mirrored).
	const FacingResult legacy = traced_facing(rt_triangle_facing(RS::CULL_MODE_DISABLED, true, true, false), true);
	const FacingResult preserved = traced_facing(rt_triangle_facing(RS::CULL_MODE_DISABLED, true, true, true), true);
	CHECK_FALSE(legacy.front_facing);
	CHECK(legacy.normal_flipped);
	CHECK(preserved.front_facing);
	CHECK_FALSE(preserved.normal_flipped);
}

TEST_CASE("[RenderRaytracing] Unmirrored back face culling keeps the TLAS facing bits of a plain mesh") {
	const RTTriangleFacing facing = rt_triangle_facing(RS::CULL_MODE_BACK, false, false, false);
	CHECK(facing.instance_flags == uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT));
	CHECK(facing.geometry_flags == 0);
}

// ShaderLanguage reads RSG::utilities, which only a live RenderingServer keeps valid.
TEST_CASE("[SceneTree][RenderRaytracing] RAY_* built-ins are read-only fragment inputs with a raster value each") {
	const ShaderTypes *types = ShaderTypes::get_singleton();
	REQUIRE(types != nullptr);
	const ShaderLanguage::FunctionInfo &fragment = types->get_functions(RS::SHADER_SPATIAL).get("fragment");
	HashMap<StringName, String> raster_renames;
	ShaderTypes::add_raster_ray_renames(raster_renames);

	int ray_built_ins = 0;
	for (const KeyValue<StringName, ShaderLanguage::BuiltInInfo> &built_in : fragment.built_ins) {
		const String name = built_in.key;
		if (!name.begins_with("RAY_")) {
			continue;
		}
		ray_built_ins++;
		CHECK_MESSAGE(built_in.value.constant, name);
		CHECK_MESSAGE(raster_renames.has(built_in.key), name);
	}
	CHECK(ray_built_ins == raster_renames.size());

	ShaderLanguage::ShaderCompileInfo info;
	info.functions = types->get_functions(RS::SHADER_SPATIAL);
	info.render_modes = types->get_modes(RS::SHADER_SPATIAL);
	info.stencil_modes = types->get_stencil_modes(RS::SHADER_SPATIAL);
	info.shader_types = types->get_types();
	const String read = "shader_type spatial;\nvoid fragment() {\n"
						"	if (!RAY_IS_PRIMARY && !RAY_IS_SHADOW && (RAY_IS_DIFFUSE || RAY_IS_SPECULAR) && !RAY_IS_SINGULAR) {\n"
						"		ALBEDO *= float(RAY_BOUNCES + RAY_DIFFUSE_BOUNCES);\n"
						"	}\n}\n";
	ShaderLanguage language;
	CHECK(language.compile(read, info) == OK);

	ERR_PRINT_OFF;
	CHECK(language.compile("shader_type spatial;\nvoid fragment() { RAY_IS_PRIMARY = false; }\n", info) != OK);
	CHECK(language.compile("shader_type spatial;\nvoid vertex() { bool p = RAY_IS_PRIMARY; }\n", info) != OK);
	ERR_PRINT_ON;
}

using DynamicDebug = RTDynamicGeometryDebug;

static DynamicDebug::State dynamic_state(DynamicDebug::Category p_category, uint32_t p_age = 0, DynamicDebug::Category p_rest = DynamicDebug::CATEGORY_STATIC) {
	DynamicDebug::State state;
	state.category = p_category;
	state.rest = p_rest;
	state.age = p_age;
	return state;
}

static DynamicDebug::Stamps dynamic_stamps(uint64_t p_build, uint64_t p_refit, bool p_dynamic = false) {
	DynamicDebug::Stamps stamps;
	stamps.last_build = p_build;
	stamps.last_refit = p_refit;
	stamps.dynamic = p_dynamic;
	return stamps;
}

TEST_CASE("[RTDynamicGeometryDebug] Geometry without BLAS work is static, or moved when its transform changed") {
	const DynamicDebug::Stamps none;
	CHECK(DynamicDebug::classify(none, false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] The most recent BLAS work picks the category, a build on ties") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(DynamicDebug::NEVER, 100), false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(DynamicDebug::classify(dynamic_stamps(96, 99), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(99, 96), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(100, 100), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
}

TEST_CASE("[RTDynamicGeometryDebug] Captured geometry gets its own build and refit categories") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(90, 98), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 2));
}

TEST_CASE("[RTDynamicGeometryDebug] BLAS work fades out after FADE_FRAMES") {
	const uint64_t frame = 1000;
	const uint64_t last_visible = frame - (DynamicDebug::FADE_FRAMES - 1);
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_BUILD, DynamicDebug::FADE_FRAMES - 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, true, frame) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] A dynamic BLAS whose last work was a build fades to a build idle category") {
	const uint64_t built = 100;
	const uint64_t idle = built + DynamicDebug::FADE_FRAMES;
	const DynamicDebug::Stamps dynamic_build = dynamic_stamps(built, DynamicDebug::NEVER, true);
	CHECK(DynamicDebug::classify(dynamic_build, false, false, built) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, true, false, idle) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE, 0, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, true, idle) == dynamic_state(DynamicDebug::CATEGORY_MOVED, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	// A build after a refit settles; a refit after the build fades to static, as does a BLAS built once at load.
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built - 1, true), false, false, idle).rest == DynamicDebug::CATEGORY_BUILT_IDLE);
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built + 1, true), false, false, idle + 1) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(built, DynamicDebug::NEVER), false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
}

TEST_CASE("[RTDynamicGeometryDebug] Packing keeps the category in the low bits, the rest category and the age above") {
	const uint32_t packed = DynamicDebug::pack(dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD, 17, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(((packed >> DynamicDebug::REST_SHIFT) & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed >> DynamicDebug::AGE_SHIFT) == 17u);
	CHECK(DynamicDebug::pack(DynamicDebug::State()) == 0u);
	static_assert(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE <= DynamicDebug::CATEGORY_MASK, "Categories must fit their bits.");
	static_assert(DynamicDebug::REST_SHIFT * 2 <= DynamicDebug::AGE_SHIFT, "The rest category must fit below the age.");
	static_assert(DynamicDebug::FADE_FRAMES <= (UINT32_MAX >> DynamicDebug::AGE_SHIFT), "Ages must fit above the categories.");
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker stamps BLASes per frame and forgets faded ones") {
	DynamicDebug tracker;
	const RID skinned = RID::from_uint64(0x100000001ULL);
	const RID captured = RID::from_uint64(0x100000002ULL);
	const RID static_blas = RID::from_uint64(0x100000003ULL);

	tracker.begin_frame(10);
	tracker.record_build(skinned);
	tracker.record_refit(captured);
	CHECK(tracker.get_tracked_count() == 2);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT));
	CHECK(tracker.observe(static_blas, false, false, false) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(tracker.get_tracked_count() == 2);

	tracker.begin_frame(11);
	tracker.record_refit(skinned);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 1));

	// Repeated calls within a frame (one per viewport) keep the frame's stamps.
	tracker.begin_frame(11);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));

	tracker.begin_frame(10 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT, DynamicDebug::FADE_FRAMES - 1));
	tracker.begin_frame(11 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);

	tracker.record_build(static_blas);
	tracker.clear();
	CHECK(tracker.get_tracked_count() == 0);
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker keeps idle built dynamic BLASes while TLAS instances use them") {
	DynamicDebug tracker;
	const RID idle = RID::from_uint64(0x100000001ULL);
	const RID loaded = RID::from_uint64(0x100000002ULL);

	tracker.begin_frame(10);
	tracker.record_build(idle);
	tracker.record_build(loaded);
	tracker.observe(idle, false, false, true);
	tracker.observe(loaded, false, false, false);

	const uint64_t settled = 10 + 2 * DynamicDebug::FADE_FRAMES;
	for (uint64_t frame = 11; frame <= settled; frame++) {
		tracker.begin_frame(frame);
		tracker.observe(idle, false, false, true);
		tracker.observe(loaded, false, false, false);
	}
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(idle, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));

	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES - 1);
	CHECK(tracker.get_tracked_count() == 1);
	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);
}

} // namespace TestRenderRaytracing
