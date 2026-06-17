/**************************************************************************/
/*  scene_shader_raytracing.cpp                                           */
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

#include "scene_shader_raytracing.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/math/math_defs.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

static void _dump_failed_shader(const String &p_source, const String &p_label) {
	String tmp_dir = OS::get_singleton()->get_temp_path();
	String path = tmp_dir.path_join("rt_shader_" + p_label + ".glsl");
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(p_source);
		WARN_PRINT("Dumped failed shader to: " + path);
	}
}

namespace {

struct RaygenShaderOption {
	uint32_t rt_flag_mask;
	const char *define_snippet;
};

// Variant index is a uint32 bitmask: bit i selects RAYGEN_SHADER_OPTIONS[i].
// Add entries here to grow the raygen permutation set (2^N variants).
static constexpr RaygenShaderOption RAYGEN_SHADER_OPTIONS[] = {
	{ SceneShaderRaytracing::RT_FLAG_DLSS_RR_ENABLED, "#define DLSS_RR_ENABLED\n" },
	{ SceneShaderRaytracing::RT_FLAG_SER_ENABLED, "#define USE_SER\n" },
	{ SceneShaderRaytracing::RT_FLAG_RAY_QUERY_SHADOWS_ENABLED, "#define USE_RAY_QUERY_SHADOWS\n" },

};

static constexpr uint32_t RAYGEN_SHADER_OPTION_COUNT = sizeof(RAYGEN_SHADER_OPTIONS) / sizeof(RAYGEN_SHADER_OPTIONS[0]);

static uint32_t _raygen_variant_from_rt_flags(uint32_t p_rt_flags) {
	uint32_t variant = 0;
	for (uint32_t i = 0; i < RAYGEN_SHADER_OPTION_COUNT; i++) {
		if (p_rt_flags & RAYGEN_SHADER_OPTIONS[i].rt_flag_mask) {
			variant |= (1u << i);
		}
	}
	return variant;
}

// Returns the GLSL preamble for the variant identified by p_variant_mask.
// Each set bit i activates RAYGEN_SHADER_OPTIONS[i].define_snippet.
static String _raygen_variant_preamble(uint32_t p_variant_mask) {
	String preamble = "\n";
	for (uint32_t opt = 0; opt < RAYGEN_SHADER_OPTION_COUNT; opt++) {
		if (p_variant_mask & (1u << opt)) {
			preamble += RAYGEN_SHADER_OPTIONS[opt].define_snippet;
		}
	}
	return preamble;
}

} // namespace

void SceneShaderRaytracing::ShaderData::set_code(const String &p_code) {
	code = p_code;

	if (raygen_version.is_null()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		raygen_version = SceneShaderRaytracing::singleton->raygen_shader.version_create();
	}

	blend_mode = BLEND_MODE_MIX;
	depth_draw = DEPTH_DRAW_OPAQUE;
	depth_test = DEPTH_TEST_ENABLED;
	cull_mode = CULL_BACK;
	alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;

	uses_point_size = false;
	uses_alpha = false;
	uses_alpha_clip = false;
	uses_alpha_antialiasing = false;
	uses_blend_alpha = false;
	uses_depth_prepass_alpha = false;
	uses_discard = false;
	uses_roughness = false;
	uses_normal = false;
	uses_tangent = false;
	uses_normal_map = false;
	uses_vertex = false;
	uses_position = false;
	uses_sss = false;
	uses_transmittance = false;
	uses_time = false;
	uses_screen_texture = false;
	uses_screen_texture_mipmaps = false;
	uses_depth_texture = false;
	uses_normal_texture = false;
	uses_vertex_time = false;
	uses_fragment_time = false;
	writes_modelview_or_projection = false;
	uses_world_coordinates = false;
	uses_particle_trails = false;
	wireframe = false;
	unshaded = false;

	ubo_size = 0;
	uniforms.clear();
	ubo_offsets.clear();
	texture_uniforms.clear();
	_clear_vertex_input_mask_cache();
	pipeline_hash_map.clear_pipelines();
}

bool SceneShaderRaytracing::ShaderData::is_animated() const {
	return (uses_fragment_time && uses_discard) || (uses_vertex_time && uses_vertex);
}

bool SceneShaderRaytracing::ShaderData::casts_shadows() const {
	bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
	bool has_alpha = has_base_alpha || uses_blend_alpha;

	return !has_alpha || (uses_depth_prepass_alpha && !(depth_draw == DEPTH_DRAW_DISABLED || depth_test == DEPTH_TEST_DISABLED));
}

RenderingServerTypes::ShaderNativeSourceCode SceneShaderRaytracing::ShaderData::get_native_source_code() const {
	// For raytracing: return source code from raygen shader, not rasterization shader
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		return SceneShaderRaytracing::singleton->raygen_shader.version_get_native_source_code(raygen_version);
	} else {
		return RenderingServerTypes::ShaderNativeSourceCode();
	}
}

SceneShaderRaytracing::ShaderVersion SceneShaderRaytracing::ShaderData::_get_shader_version(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	// Simplified: we only have one shader version now (index 0)
	return ShaderVersion(0);
}

void SceneShaderRaytracing::ShaderData::_create_pipeline(PipelineKey p_pipeline_key) {
#if PRINT_PIPELINE_COMPILATION_KEYS
	print_line(
			"HASH:", p_pipeline_key.hash(),
			"VERSION:", version,
			"VERTEX:", p_pipeline_key.vertex_format_id,
			"FRAMEBUFFER:", p_pipeline_key.framebuffer_format_id,
			"CULL:", p_pipeline_key.cull_mode,
			"PRIMITIVE:", p_pipeline_key.primitive_type,
			"VERSION:", p_pipeline_key.version,
			"PASS FLAGS:", p_pipeline_key.color_pass_flags,
			"SPEC PACKED #0:", p_pipeline_key.shader_specialization.packed_0,
			"WIREFRAME:", p_pipeline_key.wireframe);
#endif

	// Simplified: just create a simple opaque color blend state
	RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(1);

	RD::PipelineDepthStencilState depth_stencil_state;
	if (depth_test != DEPTH_TEST_DISABLED) {
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		depth_stencil_state.enable_depth_write = depth_draw != DEPTH_DRAW_DISABLED ? true : false;
	}

	RD::RenderPrimitive primitive_rd_table[RSE::PRIMITIVE_MAX] = {
		RD::RENDER_PRIMITIVE_POINTS,
		RD::RENDER_PRIMITIVE_LINES,
		RD::RENDER_PRIMITIVE_LINESTRIPS,
		RD::RENDER_PRIMITIVE_TRIANGLES,
		RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
	};

	RD::RenderPrimitive primitive_rd = uses_point_size ? RD::RENDER_PRIMITIVE_POINTS : primitive_rd_table[p_pipeline_key.primitive_type];

	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = p_pipeline_key.cull_mode;
	raster_state.wireframe = wireframe || p_pipeline_key.wireframe;

	RD::PipelineMultisampleState multisample_state;
	multisample_state.sample_count = RD::get_singleton()->framebuffer_format_get_texture_samples(p_pipeline_key.framebuffer_format_id, 0);

	// Simplified: no specialization constants
	Vector<RD::PipelineSpecializationConstant> specialization_constants;

	RID shader_rid = get_shader_variant(p_pipeline_key.version, p_pipeline_key.color_pass_flags, p_pipeline_key.ubershader);
	ERR_FAIL_COND(shader_rid.is_null());

	RID pipeline = RD::get_singleton()->render_pipeline_create(shader_rid, p_pipeline_key.framebuffer_format_id, p_pipeline_key.vertex_format_id, primitive_rd, raster_state, multisample_state, depth_stencil_state, blend_state, 0, 0, specialization_constants);
	ERR_FAIL_COND(pipeline.is_null());

	pipeline_hash_map.add_compiled_pipeline(p_pipeline_key.hash(), pipeline);
}

RD::PolygonCullMode SceneShaderRaytracing::ShaderData::get_cull_mode_from_cull_variant(CullVariant p_cull_variant) {
	const RD::PolygonCullMode cull_mode_rd_table[CULL_VARIANT_MAX][3] = {
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_FRONT, RD::POLYGON_CULL_BACK },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_BACK, RD::POLYGON_CULL_FRONT },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED }
	};

	return cull_mode_rd_table[p_cull_variant][cull_mode];
}

RID SceneShaderRaytracing::ShaderData::_get_shader_variant(ShaderVersion p_shader_version) const {
	return RID();
}

void SceneShaderRaytracing::ShaderData::_clear_vertex_input_mask_cache() {
	for (uint32_t i = 0; i < VERTEX_INPUT_MASKS_SIZE; i++) {
		vertex_input_masks[i].store(0);
	}
}

RID SceneShaderRaytracing::ShaderData::get_shader_variant(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	return _get_shader_variant(_get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader));
}

RID SceneShaderRaytracing::ShaderData::get_raygen_shader_variant() const {
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL_V(SceneShaderRaytracing::singleton, RID());
		return SceneShaderRaytracing::singleton->raygen_shader.version_get_shader(raygen_version, 0);
	} else {
		return RID();
	}
}

uint64_t SceneShaderRaytracing::ShaderData::get_vertex_input_mask(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) {
	// Vertex input masks require knowledge of the shader. Since querying the shader can be expensive due to high contention and the necessary mutex, we cache the result instead.
	ShaderVersion shader_version = _get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader);
	uint64_t input_mask = vertex_input_masks[shader_version].load(std::memory_order_relaxed);
	if (input_mask == 0) {
		RID shader_rid = _get_shader_variant(shader_version);
		ERR_FAIL_COND_V(shader_rid.is_null(), 0);

		input_mask = RD::get_singleton()->shader_get_vertex_input_attribute_mask(shader_rid);
		vertex_input_masks[shader_version].store(input_mask, std::memory_order_relaxed);
	}

	return input_mask;
}

bool SceneShaderRaytracing::ShaderData::is_valid() const {
	// Validity follows raygen_version.
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL_V(SceneShaderRaytracing::singleton, false);
		return SceneShaderRaytracing::singleton->raygen_shader.version_is_valid(raygen_version);
	} else {
		return false;
	}
}

SceneShaderRaytracing::ShaderData::ShaderData() :
		shader_list_element(this) {
	pipeline_hash_map.set_creation_object_and_function(this, &ShaderData::_create_pipeline);
	pipeline_hash_map.set_compilations(SceneShaderRaytracing::singleton->pipeline_compilations, &SceneShaderRaytracing::singleton_mutex);
}

SceneShaderRaytracing::ShaderData::~ShaderData() {
	pipeline_hash_map.clear_pipelines();

	// Tear down raygen only (no raster shader RID).
	if (raygen_version.is_valid()) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		ERR_FAIL_NULL(SceneShaderRaytracing::singleton);
		SceneShaderRaytracing::singleton->raygen_shader.version_free(raygen_version);
	}
}

Pair<ShaderRD *, RID> SceneShaderRaytracing::ShaderData::get_native_shader_and_version() const {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	if (SceneShaderRaytracing::singleton == nullptr) {
		return Pair<ShaderRD *, RID>(nullptr, RID());
	}
	return Pair<ShaderRD *, RID>(&SceneShaderRaytracing::singleton->raygen_shader, raygen_version); // Raygen pair for tooling.
}

RendererRD::MaterialStorage::ShaderData *SceneShaderRaytracing::_create_shader_func() {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	ShaderData *shader_data = memnew(ShaderData);
	singleton->shader_list.add(&shader_data->shader_list_element);
	return shader_data;
}

void SceneShaderRaytracing::MaterialData::set_render_priority(int p_priority) {
	priority = p_priority - RSE::MATERIAL_RENDER_PRIORITY_MIN; //8 bits
}

void SceneShaderRaytracing::MaterialData::set_next_pass(RID p_pass) {
	next_pass = p_pass;
}

bool SceneShaderRaytracing::MaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	// RT path does not drive MaterialStorage uniform sets here.
	return true;
}

SceneShaderRaytracing::MaterialData::~MaterialData() {
	free_parameters_uniform_set(uniform_set);
}

RendererRD::MaterialStorage::MaterialData *SceneShaderRaytracing::_create_material_func(ShaderData *p_shader) {
	MaterialData *material_data = memnew(MaterialData);
	material_data->shader_data = p_shader;
	return material_data;
}

SceneShaderRaytracing *SceneShaderRaytracing::singleton = nullptr;
Mutex SceneShaderRaytracing::singleton_mutex;

SceneShaderRaytracing::SceneShaderRaytracing() {
	// There should be only one of these, contained within our RenderForwardClustered singleton.
	singleton = this;
}

SceneShaderRaytracing::~SceneShaderRaytracing() {
	_join_lane_for_shutdown();
	invalidate_pipeline_bundles();

	if (raygen_shader_version.is_valid()) {
		raygen_shader.version_free(raygen_shader_version);
	}

	singleton = nullptr;
}

void SceneShaderRaytracing::invalidate_pipeline_bundles() {
	// Bundles own HG + base_shader RIDs; free_rid is deferred-safe for in-flight frames.
	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		PipelineBundle &b = kv.value;
		if (b.hit_sbt.is_valid()) {
			RD::get_singleton()->free_rid(b.hit_sbt);
		}
		if (b.pipeline.is_valid()) {
			RD::get_singleton()->free_rid(b.pipeline);
		}
		for (const RID &rid : b.per_hg_shaders) {
			if (rid.is_valid()) {
				RD::get_singleton()->free_rid(rid);
			}
		}
		if (b.base_shader.is_valid()) {
			RD::get_singleton()->free_rid(b.base_shader);
		}
	}
	pipeline_bundles.clear();
	variant_compile_contexts.clear();
}

// Async bundle rebuild task (worker: SPIR-V + pipeline; main thread: SBT + swap). Single-lane globally.

struct SceneShaderRaytracing::PipelineBuildTask {
	uint32_t rt_flags = 0;

	RID base_shader;
	Vector<RD::PipelineSpecializationConstant> spec_constants;
	Vector<RD::ShaderStageSPIRVData> base_non_hit_stages;
	RD::ShaderStageSPIRVData base_any_hit_stage;
	bool base_any_hit_valid = false;

	struct SlotInput {
		RID existing_per_hg_shader;
		bool needs_compile = false;
		bool uses_alpha_clip = false;
		bool is_procedural = false;
		String ch_src;
		String ah_src;
		String is_src;
	};
	LocalVector<SlotInput> slots;

	LocalVector<RID> new_per_hg_shaders;
	LocalVector<bool> new_ready_mask;
	RID new_pipeline;
	bool failed = false;
	String error;

	SafeFlag abort_requested;
	SafeFlag done;
	WorkerThreadPool::TaskID worker_id = WorkerThreadPool::INVALID_TASK_ID;
};

void SceneShaderRaytracing::_strip_texture_globals(String &r_globals, const String &p_tex_name) {
	const String decl_marker = " m_" + p_tex_name + ";";
	int pos = r_globals.find(decl_marker);
	if (pos < 0) {
		return;
	}
	int line_start = r_globals.rfind("\n", pos);
	line_start = (line_start < 0) ? 0 : line_start + 1;
	int line_end = r_globals.find("\n", pos);
	if (line_end < 0) {
		line_end = r_globals.length();
	} else {
		line_end += 1;
	}
	r_globals = r_globals.substr(0, line_start) + r_globals.substr(line_end);
}

void SceneShaderRaytracing::_finalize_uniforms_with_textures(
		CustomShaderEntry &r_entry,
		const ShaderCompiler::GeneratedCode &p_gen_code,
		const HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> &p_uniforms,
		bool p_strip_intersection_globals) {
	// ShaderCompiler rounds uniform_total_size to 16 bytes for UBO requirements,
	// but our buffer_reference struct uses std140 member layout without UBO-level padding.
	// Recompute the raw (unrounded) end of the last uniform member.
	uint32_t raw_uniform_end = 0;
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : p_uniforms) {
		const ShaderLanguage::ShaderNode::Uniform &uu = kv.value;
		if (ShaderLanguage::is_sampler_type(uu.type) || uu.order < 0 || uu.order >= (int)p_gen_code.uniform_offsets.size()) {
			continue;
		}
		uint32_t end = p_gen_code.uniform_offsets[uu.order] + ShaderLanguage::get_datatype_size(uu.type);
		if (end > raw_uniform_end) {
			raw_uniform_end = end;
		}
	}

	for (int ti = 0; ti < p_gen_code.texture_uniforms.size(); ti++) {
		const ShaderCompiler::GeneratedCode::Texture &tex = p_gen_code.texture_uniforms[ti];
		if (tex.name.is_empty()) {
			continue;
		}

		_strip_texture_globals(r_entry.fragment_globals, tex.name);
		if (p_strip_intersection_globals) {
			_strip_texture_globals(r_entry.intersection_globals, tex.name);
		}

		// Append a bindless-index uint member to the uniform buffer.
		uint32_t offset = raw_uniform_end;
		if (offset % 4 != 0) {
			offset += 4 - (offset % 4);
		}

		TextureUniformInfo tui;
		tui.name = tex.name;
		tui.hint = tex.hint;
		tui.use_color = tex.use_color;
		tui.is_global = tex.global;
		tui.buffer_offset = offset;
		r_entry.texture_uniforms.push_back(tui);

		r_entry.uniform_members += "uint m_" + tex.name + ";\n";
		raw_uniform_end = offset + 4;
	}

	r_entry.uniform_total_size = raw_uniform_end;
	if (r_entry.uniform_total_size % 16 != 0) {
		r_entry.uniform_total_size += 16 - (r_entry.uniform_total_size % 16);
	}
}

// Slot table: append-only by 128-bit source hash (stable across reloads).

uint32_t SceneShaderRaytracing::register_custom_shader(uint32_t /*p_shader_id*/, RID p_material) {
	return _register_slot(/*p_shader_id*/ 0, p_material, /*p_is_procedural=*/false);
}

uint32_t SceneShaderRaytracing::register_procedural_shader(uint32_t /*p_shader_id*/, RID p_material) {
	return _register_slot(/*p_shader_id*/ 0, p_material, /*p_is_procedural=*/true);
}

uint32_t SceneShaderRaytracing::_register_slot(uint32_t /*p_shader_id*/, RID p_material, bool p_is_procedural) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	SourceHash128 hash;
	hash.a = material_storage->material_get_shader_code_rt_hash(p_material);
	hash.b = material_storage->material_get_shader_code_rt_hash_b(p_material);
	if (hash.is_zero()) {
		return 0;
	}

	HashMap<SourceHash128, uint32_t, SourceHash128Hasher>::Iterator it = source_hash_to_slot.find(hash);
	if (it != source_hash_to_slot.end()) {
		return it->value;
	}

	// New slot: preprocess here; heavy compile is on worker.
	CustomShaderEntry entry;
	entry.is_procedural = p_is_procedural;
	if (!_preprocess_shader(p_material, p_is_procedural, entry)) {
		// Failed slot still occupies an index so hash lookups stay stable.
		HitGroupSlot failed_slot;
		failed_slot.source_hash = hash;
		failed_slot.state = HGState::Failed;
		uint32_t failed_index = (uint32_t)hit_group_slots.size();
		hit_group_slots.push_back(failed_slot);
		source_hash_to_slot.insert(hash, failed_index);
		for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
			_bundle_resize_for_slots(kv.value);
			kv.value.per_hg_states[failed_index] = HGState::Failed;
		}
		return 0;
	}
	// Empty custom HG source uses slot 0 (default).
	if (!p_is_procedural && entry.fragment_code.is_empty() && entry.vertex_code.is_empty()) {
		return 0;
	}
	if (p_is_procedural && entry.intersection_code.is_empty()) {
		return 0;
	}

	HitGroupSlot slot;
	slot.source_hash = hash;
	slot.state = HGState::Ready;
	slot.entry = entry;
	uint32_t slot_index = (uint32_t)hit_group_slots.size();
	hit_group_slots.push_back(slot);
	source_hash_to_slot.insert(hash, slot_index);

	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		PipelineBundle &bundle = kv.value;
		_bundle_resize_for_slots(bundle);
		bundle.dirty = true;
	}

	return slot_index;
}

bool SceneShaderRaytracing::_preprocess_shader(RID p_material, bool p_is_procedural, CustomShaderEntry &r_entry) {
	String code = RendererRD::MaterialStorage::get_singleton()->material_get_shader_code_rt(p_material);
	if (code.is_empty()) {
		return false;
	}

	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	if (p_is_procedural) {
		actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
		actions.entry_point_stages["intersection"] = ShaderCompiler::STAGE_INTERSECTION;
	}

	bool detected_alpha_clip = false;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &detected_alpha_clip;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &detected_alpha_clip;

	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniform_sink;
	actions.uniforms = &uniform_sink;

	ShaderCompiler::GeneratedCode gen_code;
	Error err = compiler.compile(RSE::SHADER_SPATIAL, code, &actions, String(), gen_code);
	if (err != OK) {
		WARN_PRINT(vformat("RT: Failed to preprocess %s shader; falling back to default material.",
				p_is_procedural ? String("procedural") : String("custom")));
		return false;
	}

	r_entry.is_procedural = p_is_procedural;
	r_entry.uses_alpha_clip = detected_alpha_clip;
	r_entry.vertex_code = gen_code.code.has("vertex") ? gen_code.code["vertex"] : String();
	r_entry.fragment_code = gen_code.code.has("fragment") ? gen_code.code["fragment"] : String();
	r_entry.fragment_globals = gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	if (p_is_procedural) {
		r_entry.intersection_code = gen_code.code.has("intersection") ? gen_code.code["intersection"] : String();
		r_entry.intersection_globals = gen_code.stage_globals[ShaderCompiler::STAGE_INTERSECTION];
	}
	r_entry.uniform_members = gen_code.uniforms;
	r_entry.uniform_total_size = gen_code.uniform_total_size;
	r_entry.uniform_offsets = gen_code.uniform_offsets;
	r_entry.uniforms = uniform_sink;
	_finalize_uniforms_with_textures(r_entry, gen_code, uniform_sink, /*strip_intersection_globals=*/p_is_procedural);
	return true;
}

void SceneShaderRaytracing::finalize_custom_shaders() {
	async_compilation_enabled = GLOBAL_GET_CACHED(bool, "rendering/pathtracer/async_shader_compilation");

	_kick_rebuild_if_idle(); // Async dispatch only; sync drains below.

	if (!async_compilation_enabled) {
		_drain_lane_inline_main_thread();
	}
}

const SceneShaderRaytracing::CustomShaderEntry *SceneShaderRaytracing::get_custom_shader_entry(uint32_t p_slot_index) const {
	if (p_slot_index == 0 || p_slot_index >= hit_group_slots.size()) {
		return nullptr;
	}
	const HitGroupSlot &slot = hit_group_slots[p_slot_index];
	if (slot.state == HGState::Failed) {
		return nullptr;
	}
	return &slot.entry;
}

bool SceneShaderRaytracing::is_hg_ready_in_bundle(uint32_t p_slot_index, uint32_t p_rt_flags) const {
	if (p_slot_index == 0) {
		return true;
	}
	if (p_slot_index >= hit_group_slots.size()) {
		return false;
	}
	HashMap<uint32_t, PipelineBundle>::ConstIterator it = pipeline_bundles.find(p_rt_flags);
	if (it == pipeline_bundles.end()) {
		return false;
	}
	const PipelineBundle &b = it->value;
	if (p_slot_index >= b.live_hg_count) {
		return false;
	}
	if (p_slot_index >= b.live_ready_mask.size()) {
		return false;
	}
	return b.live_ready_mask[p_slot_index];
}

uint32_t SceneShaderRaytracing::compute_rt_flags(RID p_environment, bool p_fog_enabled) {
	uint32_t flags = RT_FLAG_NONE;
	uint32_t sample_count = 1;
	uint32_t max_bounces = 3;

	if (p_environment.is_valid()) {
		RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();

		if (env_storage->environment_get_pathtracing_debug_mode(p_environment) != 0) {
			flags |= RT_FLAG_DEBUG_VIS_ENABLED;
		}

		if (env_storage->environment_get_pathtracing_use_simple_shadows(p_environment)) {
			flags |= RT_FLAG_RAY_QUERY_SHADOWS_ENABLED;
		}

		sample_count = MAX(1, env_storage->environment_get_pathtracing_samples_per_pixel(p_environment));
		max_bounces = CLAMP(env_storage->environment_get_pathtracing_max_bounces(p_environment), 1, 8);
		if (env_storage->environment_get_pathtracing_denoiser(p_environment) == RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION) {
			flags |= RT_FLAG_DLSS_RR_ENABLED;
		}
	}

	if (p_fog_enabled) {
		flags |= RT_FLAG_FOG_ENABLED;
	}

	if (GLOBAL_GET("rendering/pathtracer/use_shader_execution_reordering")) {
		flags |= RT_FLAG_SER_ENABLED;
	}

	return rt_flags_pack(flags, sample_count, max_bounces);
}

SceneShaderRaytracing *SceneShaderRaytracing::get_singleton() {
	if (singleton == nullptr) {
		MutexLock lock(SceneShaderRaytracing::singleton_mutex);
		if (singleton == nullptr) {
			memnew(SceneShaderRaytracing);
		}
	}
	return singleton;
}

// Per-rt_flags base SPIR-V + HG templates (immutable cache for worker tasks).

bool SceneShaderRaytracing::_ensure_variant_compile_context(uint32_t p_rt_flags) {
	if (variant_compile_contexts.has(p_rt_flags)) {
		return true;
	}
	ERR_FAIL_COND_V(!raygen_shader_version.is_valid(), false);

	int variant = (int)_raygen_variant_from_rt_flags(p_rt_flags);

	Vector<String> sources = raygen_shader.version_build_variant_stage_sources(raygen_shader_version, variant);
	ERR_FAIL_COND_V(sources.is_empty(), false);

	auto inject_define = [](Vector<String> &p_sources, const String &p_define) {
		for (int i = 0; i < p_sources.size(); i++) {
			if (p_sources[i].is_empty()) {
				continue;
			}
			int ver_pos = p_sources[i].find("#version");
			if (ver_pos >= 0) {
				int nl = p_sources[i].find("\n", ver_pos);
				if (nl >= 0) {
					p_sources.write[i] = p_sources[i].insert(nl + 1, p_define);
				}
			}
		}
	};

	// Fixed intersection hit-attribute layout for all variants (keeps base_shader layout stable).
	inject_define(sources, "#define ENABLE_INTERSECTION_SHADERS\n");

	if (p_rt_flags & RT_FLAG_DEBUG_VIS_ENABLED) {
		inject_define(sources, "#define RT_DEBUG_ENABLED\n");
	}

	// Procedural templates come from intersection stage; strip from base HG0 path.
	String intersection_source_template;
	if (sources.size() > RD::SHADER_STAGE_INTERSECTION) {
		intersection_source_template = sources[RD::SHADER_STAGE_INTERSECTION];
		sources.write[RD::SHADER_STAGE_INTERSECTION] = String();
	}

	String base_ch_src = sources[RD::SHADER_STAGE_CLOSEST_HIT];
	String base_ah_src = sources[RD::SHADER_STAGE_ANY_HIT];
	ERR_FAIL_COND_V(base_ch_src.is_empty(), false);
	ERR_FAIL_COND_V(base_ah_src.is_empty(), false);

	static const String custom_hg_define = "#define RT_CUSTOM_HIT_GROUP\n";
	int base_ch_ver = base_ch_src.find("#version");
	int base_ah_ver = base_ah_src.find("#version");
	ERR_FAIL_COND_V(base_ch_ver < 0 || base_ah_ver < 0, false);

	VariantCompileContext ctx;
	ctx.ch_template = base_ch_src.insert(base_ch_src.find("\n", base_ch_ver) + 1, custom_hg_define);
	ctx.ah_template = base_ah_src.insert(base_ah_src.find("\n", base_ah_ver) + 1, custom_hg_define);
	if (!intersection_source_template.is_empty()) {
		int base_is_ver = intersection_source_template.find("#version");
		if (base_is_ver >= 0) {
			ctx.is_template = intersection_source_template.insert(intersection_source_template.find("\n", base_is_ver) + 1, custom_hg_define);
		}
	}

	// Full base stages; CH/AH SPIR-V is HG0 default; RG+miss duplicated into each HG bytecode.
	Vector<RD::ShaderStageSPIRVData> base_stages;
	{
		MutexLock lock(spirv_compile_mutex);
		base_stages = ShaderRD::compile_stages(sources, {});
	}
	ERR_FAIL_COND_V(base_stages.is_empty(), false);

	for (const RD::ShaderStageSPIRVData &sd : base_stages) {
		if (sd.shader_stage == RD::SHADER_STAGE_RAYGEN || sd.shader_stage == RD::SHADER_STAGE_MISS) {
			ctx.base_non_hit_stages.push_back(sd);
		} else if (sd.shader_stage == RD::SHADER_STAGE_ANY_HIT) {
			ctx.base_any_hit_stage = sd;
			ctx.base_any_hit_valid = true;
		}
	}

	variant_compile_contexts.insert(p_rt_flags, ctx);

	// Creates layout-defining base_shader RID for uniform sets (immutable per variant).
	HashMap<uint32_t, PipelineBundle>::Iterator bit = pipeline_bundles.find(p_rt_flags);
	if (bit != pipeline_bundles.end() && bit->value.base_shader.is_valid()) {
		return true;
	}

	Vector<uint8_t> base_binary;
	{
		MutexLock lock(spirv_compile_mutex);
		base_binary = RD::get_singleton()->shader_compile_binary_from_spirv(base_stages, "RT_base");
	}
	ERR_FAIL_COND_V(base_binary.is_empty(), false);

	RID base_shader_rd = RD::get_singleton()->shader_create_from_bytecode(base_binary);
	ERR_FAIL_COND_V(!base_shader_rd.is_valid(), false);

	PipelineBundle &bundle = pipeline_bundles[p_rt_flags];
	bundle.base_shader = base_shader_rd;
	_bundle_resize_for_slots(bundle);
	if (!bundle.per_hg_states.is_empty()) {
		bundle.per_hg_states[0] = HGState::Ready;
	}

	return true;
}

void SceneShaderRaytracing::_bundle_resize_for_slots(PipelineBundle &r_bundle) {
	uint32_t n = (uint32_t)hit_group_slots.size();
	uint32_t old = (uint32_t)r_bundle.per_hg_states.size();
	if (old == n) {
		return;
	}
	r_bundle.per_hg_states.resize(n);
	r_bundle.per_hg_shaders.resize(n);
	for (uint32_t i = old; i < n; i++) {
		r_bundle.per_hg_states[i] = HGState::Empty;
		r_bundle.per_hg_shaders[i] = RID();
	}
	if (n > 0 && r_bundle.base_shader.is_valid()) {
		r_bundle.per_hg_states[0] = HGState::Ready;
	}
}

const SceneShaderRaytracing::PipelineBundle &SceneShaderRaytracing::ensure_pipeline_bundle(uint32_t p_rt_flags) {
	static PipelineBundle EMPTY_BUNDLE;

	HashMap<uint32_t, PipelineBundle>::Iterator it = pipeline_bundles.find(p_rt_flags);
	if (it != pipeline_bundles.end() && it->value.initial_pipeline_built) {
		return it->value;
	}

	if (!_ensure_variant_compile_context(p_rt_flags)) {
		WARN_PRINT(vformat("RT: Failed to build base compile context for variant 0x%x.", p_rt_flags));
		return EMPTY_BUNDLE;
	}

	PipelineBundle &bundle = pipeline_bundles[p_rt_flags];
	_bundle_resize_for_slots(bundle);

	// Sync bootstrap: HG0-only pipeline + SBT on render thread; dirty if extra slots exist.
	if (!_build_initial_bundle(p_rt_flags, bundle)) {
		return EMPTY_BUNDLE;
	}
	if (hit_group_slots.size() > 1) {
		bundle.dirty = true;
	}
	bundle.initial_pipeline_built = true;
	return bundle;
}

// Minimal HG0 + empty sentinel pipeline/SBT for first use of a variant.

bool SceneShaderRaytracing::_build_initial_bundle(uint32_t p_rt_flags, PipelineBundle &r_bundle) {
	if (!r_bundle.base_shader.is_valid()) {
		return false;
	}

	Vector<RD::PipelineSpecializationConstant> spec_constants;
	{
		RD::PipelineSpecializationConstant sc;
		sc.constant_id = 0;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.int_value = (int)p_rt_flags;
		spec_constants.push_back(sc);
	}

	RD::PipelineShader base_ps = { r_bundle.base_shader, spec_constants };
	RD::HitGroup hg0_default;
	hg0_default.closest_hit_shader = base_ps;
	hg0_default.any_hit_shader = base_ps;
	const RD::HitGroup empty_hg;

	// HG[0]=default, HG[1]=empty until rebuild maps custom slots.
	LocalVector<RD::HitGroup> hit_groups;
	hit_groups.push_back(hg0_default);
	hit_groups.push_back(empty_hg);

	RID new_pipeline = RD::get_singleton()->raytracing_pipeline_create(
			{ &base_ps, 1 }, { &base_ps, 1 },
			{ hit_groups.ptr(), (uint64_t)hit_groups.size() },
			RT_MAX_RECURSION_DEPTH);
	if (new_pipeline.is_null()) {
		WARN_PRINT(vformat("RT: initial raytracing_pipeline_create failed for variant 0x%x.", p_rt_flags));
		return false;
	}
	RD::get_singleton()->set_resource_name(new_pipeline, String("RT Pipeline initial [flags=") + itos(p_rt_flags) + "]");

	static constexpr uint32_t HIT_SBT_CAPACITY = 4096;
	uint32_t sbt_size = HIT_SBT_CAPACITY;

	RID new_sbt = RD::get_singleton()->hit_sbt_create(new_pipeline, sbt_size);
	if (new_sbt.is_null()) {
		RD::get_singleton()->free_rid(new_pipeline);
		return false;
	}
	RD::get_singleton()->set_resource_name(new_sbt, String("RT Hit SBT initial [flags=") + itos(p_rt_flags) + "]");

	RD::HitShaderBindingTableRange sbt_range = RD::get_singleton()->hit_sbt_range_alloc(new_sbt, sbt_size);
	if (!sbt_range) {
		RD::get_singleton()->free_rid(new_sbt);
		RD::get_singleton()->free_rid(new_pipeline);
		return false;
	}

	// Instance slot i maps to HG min(i,1) until full rebuild (custom TLAS-skipped meanwhile).
	LocalVector<uint32_t> indices;
	indices.resize(sbt_size);
	for (uint32_t i = 0; i < sbt_size; i++) {
		indices[i] = (i == 0) ? 0 : 1;
	}
	RD::get_singleton()->hit_sbt_range_update(new_sbt, sbt_range, 0, indices);

	r_bundle.pipeline = new_pipeline;
	r_bundle.hit_sbt = new_sbt;
	r_bundle.live_hg_count = 1;
	r_bundle.live_ready_mask.clear();
	r_bundle.live_ready_mask.push_back(true);
	return true;
}

SceneShaderRaytracing::PipelineBuildTask *SceneShaderRaytracing::_make_pipeline_build_task(uint32_t p_rt_flags, PipelineBundle &p_bundle) {
	if (!_ensure_variant_compile_context(p_rt_flags)) {
		return nullptr;
	}
	HashMap<uint32_t, VariantCompileContext>::ConstIterator cit = variant_compile_contexts.find(p_rt_flags);
	if (cit == variant_compile_contexts.end()) {
		return nullptr;
	}
	const VariantCompileContext &ctx = cit->value;

	PipelineBuildTask *task = memnew(PipelineBuildTask);
	task->rt_flags = p_rt_flags;
	task->base_shader = p_bundle.base_shader;
	task->base_non_hit_stages = ctx.base_non_hit_stages;
	task->base_any_hit_stage = ctx.base_any_hit_stage;
	task->base_any_hit_valid = ctx.base_any_hit_valid;
	{
		RD::PipelineSpecializationConstant sc;
		sc.constant_id = 0;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.int_value = (int)p_rt_flags;
		task->spec_constants.push_back(sc);
	}

	uint32_t slot_count = (uint32_t)hit_group_slots.size();
	task->slots.resize(slot_count);

	task->slots[0].existing_per_hg_shader = RID();
	task->slots[0].needs_compile = false;

	for (uint32_t i = 1; i < slot_count; i++) {
		const HitGroupSlot &slot = hit_group_slots[i];
		PipelineBuildTask::SlotInput &si = task->slots[i];

		si.uses_alpha_clip = slot.entry.uses_alpha_clip;
		si.is_procedural = slot.entry.is_procedural;

		if (slot.state != HGState::Ready) {
			si.needs_compile = false;
			si.existing_per_hg_shader = RID();
			continue;
		}

		if (i < p_bundle.per_hg_shaders.size() && p_bundle.per_hg_shaders[i].is_valid() &&
				i < p_bundle.per_hg_states.size() && p_bundle.per_hg_states[i] == HGState::Ready) {
			si.existing_per_hg_shader = p_bundle.per_hg_shaders[i];
			si.needs_compile = false;
			continue;
		}
		if (i < p_bundle.per_hg_states.size() && p_bundle.per_hg_states[i] == HGState::Failed) {
			si.needs_compile = false;
			continue;
		}

		const CustomShaderEntry &entry = slot.entry;
		si.needs_compile = true;

		String tex_defines;
		for (int ti = 0; ti < entry.texture_uniforms.size(); ti++) {
			const TextureUniformInfo &tui = entry.texture_uniforms[ti];
			tex_defines += "#define m_" + tui.name + " bindless_textures[nonuniformEXT(material.m_" + tui.name + ")]\n";
		}
		String uniform_members = entry.uniform_members.is_empty() ? String("float _rt_pad;") : entry.uniform_members;

		String vertex_function;
		String vertex_call;
		if (!entry.vertex_code.is_empty()) {
			vertex_function = "void rt_run_vertex_shader() {\n" + entry.vertex_code + "\n}\n";
			vertex_call = "rt_run_vertex_shader();";
		}

		// Procedural with no fragment stage: default material shading.
		String fragment_code = entry.fragment_code;
		if (fragment_code.is_empty() && entry.is_procedural) {
			fragment_code =
					"vec2 mat_uv = uv_interp * rt_mat.uv1_scale + rt_mat.uv1_offset;\n"
					"vec4 albedo_tex = sample_material_texture(rt_mat.albedo_texture_idx, mat_uv, rt_mat.flags);\n"
					"albedo = albedo_tex.rgb * rt_mat.albedo_color.rgb;\n"
					"alpha = albedo_tex.a * rt_mat.albedo_color.a;\n"
					"vec3 orm = sample_material_texture(rt_mat.orm_texture_idx, mat_uv, rt_mat.flags).rgb;\n"
					"roughness = orm.g * rt_mat.roughness;\n"
					"metallic = orm.b * rt_mat.metallic;\n"
					"if ((rt_mat.flags & 1u) != 0u) {\n"
					"    normal_map = sample_material_texture(rt_mat.normal_texture_idx, mat_uv, rt_mat.flags).rgb;\n"
					"    normal_map_depth = rt_mat.normal_map_depth;\n"
					"}\n"
					"if ((rt_mat.flags & 2u) != 0u) {\n"
					"    emission = sample_material_texture(rt_mat.emission_texture_idx, mat_uv, rt_mat.flags).rgb\n"
					"             * rt_mat.emission_color * rt_mat.emission_strength;\n"
					"}\n";
		}

		{
			String s = ctx.ch_template;
			s = s.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", entry.fragment_globals);
			s = s.replace("/* RT_CUSTOM_FRAGMENT_CODE */", fragment_code);
			s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
			s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
			s = s.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", vertex_function);
			s = s.replace("/* RT_CUSTOM_VERTEX_CALL */", vertex_call);
			si.ch_src = s;
		}
		if (entry.uses_alpha_clip) {
			String s = ctx.ah_template;
			s = s.replace("/* RT_CUSTOM_FRAGMENT_GLOBALS */", entry.fragment_globals);
			s = s.replace("/* RT_CUSTOM_FRAGMENT_CODE */", fragment_code);
			s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
			s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
			s = s.replace("/* RT_CUSTOM_VERTEX_FUNCTION */", vertex_function);
			s = s.replace("/* RT_CUSTOM_VERTEX_CALL */", vertex_call);
			si.ah_src = s;
		}
		if (entry.is_procedural && !ctx.is_template.is_empty()) {
			String s = ctx.is_template;
			s = s.replace("/* RT_CUSTOM_INTERSECTION_GLOBALS */", entry.intersection_globals);
			s = s.replace("/* RT_CUSTOM_INTERSECTION_CODE */", entry.intersection_code);
			s = s.replace("/* RT_CUSTOM_UNIFORM_MEMBERS */", uniform_members);
			s = s.replace("/* RT_CUSTOM_TEXTURE_DEFINES */", tex_defines);
			si.is_src = s;
		}
	}

	return task;
}

void SceneShaderRaytracing::_build_pipeline_worker_static(void *p_userdata) {
	PipelineBuildTask *t = static_cast<PipelineBuildTask *>(p_userdata);
	if (singleton) {
		singleton->_build_pipeline_worker(t);
	} else {
		t->done.set();
	}
}

void SceneShaderRaytracing::_build_pipeline_worker(PipelineBuildTask *p_task) {
	auto check_abort = [p_task]() {
		return p_task->abort_requested.is_set();
	};
	auto finish = [p_task]() {
		p_task->done.set();
	};

	uint32_t n = (uint32_t)p_task->slots.size();
	p_task->new_per_hg_shaders.resize(n);
	p_task->new_ready_mask.resize(n);

	LocalVector<RID> created_here;
	auto cleanup_created = [&]() {
		for (const RID &r : created_here) {
			if (r.is_valid()) {
				RD::get_singleton()->free_rid(r);
			}
		}
		created_here.clear();
		for (uint32_t i = 0; i < n; i++) {
			if (p_task->slots[i].existing_per_hg_shader.is_null()) {
				p_task->new_per_hg_shaders[i] = RID();
			}
		}
	};

	if (n > 0) {
		p_task->new_per_hg_shaders[0] = RID();
		p_task->new_ready_mask[0] = true;
	}

	String error;
	for (uint32_t i = 1; i < n; i++) {
		if (check_abort()) {
			cleanup_created();
			p_task->failed = true;
			finish();
			return;
		}

		const PipelineBuildTask::SlotInput &si = p_task->slots[i];

		if (si.existing_per_hg_shader.is_valid()) {
			p_task->new_per_hg_shaders[i] = si.existing_per_hg_shader;
			p_task->new_ready_mask[i] = true;
			continue;
		}
		if (!si.needs_compile) {
			p_task->new_per_hg_shaders[i] = RID();
			p_task->new_ready_mask[i] = false;
			continue;
		}

		Vector<RD::ShaderStageSPIRVData> custom_stages = p_task->base_non_hit_stages;

		Vector<uint8_t> ch_spirv;
		{
			MutexLock lock(spirv_compile_mutex);
			ch_spirv = RD::get_singleton()->shader_compile_spirv_from_source(
					RD::SHADER_STAGE_CLOSEST_HIT, si.ch_src, RD::SHADER_LANGUAGE_GLSL, &error);
		}
		if (ch_spirv.is_empty()) {
			_dump_failed_shader(si.ch_src, vformat("hg%d_v%x_closest_hit", i, p_task->rt_flags));
			p_task->new_per_hg_shaders[i] = RID();
			p_task->new_ready_mask[i] = false;
			continue;
		}
		{
			RD::ShaderStageSPIRVData sd;
			sd.shader_stage = RD::SHADER_STAGE_CLOSEST_HIT;
			sd.spirv = ch_spirv;
			custom_stages.push_back(sd);
		}

		if (si.uses_alpha_clip) {
			Vector<uint8_t> ah_spirv;
			{
				MutexLock lock(spirv_compile_mutex);
				ah_spirv = RD::get_singleton()->shader_compile_spirv_from_source(
						RD::SHADER_STAGE_ANY_HIT, si.ah_src, RD::SHADER_LANGUAGE_GLSL, &error);
			}
			if (ah_spirv.is_empty()) {
				_dump_failed_shader(si.ah_src, vformat("hg%d_v%x_any_hit", i, p_task->rt_flags));
				p_task->new_per_hg_shaders[i] = RID();
				p_task->new_ready_mask[i] = false;
				continue;
			}
			RD::ShaderStageSPIRVData sd;
			sd.shader_stage = RD::SHADER_STAGE_ANY_HIT;
			sd.spirv = ah_spirv;
			custom_stages.push_back(sd);
		} else if (p_task->base_any_hit_valid) {
			custom_stages.push_back(p_task->base_any_hit_stage);
		}

		if (si.is_procedural && !si.is_src.is_empty()) {
			Vector<uint8_t> is_spirv;
			{
				MutexLock lock(spirv_compile_mutex);
				is_spirv = RD::get_singleton()->shader_compile_spirv_from_source(
						RD::SHADER_STAGE_INTERSECTION, si.is_src, RD::SHADER_LANGUAGE_GLSL, &error);
			}
			if (is_spirv.is_empty()) {
				_dump_failed_shader(si.is_src, vformat("hg%d_v%x_intersection", i, p_task->rt_flags));
				p_task->new_per_hg_shaders[i] = RID();
				p_task->new_ready_mask[i] = false;
				continue;
			}
			RD::ShaderStageSPIRVData sd;
			sd.shader_stage = RD::SHADER_STAGE_INTERSECTION;
			sd.spirv = is_spirv;
			custom_stages.push_back(sd);
		}

		Vector<uint8_t> binary;
		{
			MutexLock lock(spirv_compile_mutex);
			binary = RD::get_singleton()->shader_compile_binary_from_spirv(
					custom_stages, vformat("RT_hg%d_v%x", i, p_task->rt_flags));
		}
		if (binary.is_empty()) {
			p_task->new_per_hg_shaders[i] = RID();
			p_task->new_ready_mask[i] = false;
			continue;
		}

		RID rid = RD::get_singleton()->shader_create_from_bytecode(binary);
		if (!rid.is_valid()) {
			p_task->new_per_hg_shaders[i] = RID();
			p_task->new_ready_mask[i] = false;
			continue;
		}
		p_task->new_per_hg_shaders[i] = rid;
		p_task->new_ready_mask[i] = true;
		created_here.push_back(rid);
	}

	if (check_abort()) {
		cleanup_created();
		p_task->failed = true;
		finish();
		return;
	}

	// raytracing_pipeline_create may run off render thread (RD uses internal sync).
	RD::PipelineShader base_ps = { p_task->base_shader, p_task->spec_constants };
	RD::HitGroup hg0_default;
	hg0_default.closest_hit_shader = base_ps;
	hg0_default.any_hit_shader = base_ps;

	// No-op HG: failed / pending slots + trailing SBT sentinel (never default material).
	const RD::HitGroup empty_hg;

	// Layout: [0] default [1..n-1] custom or empty [n] sentinel.
	LocalVector<RD::HitGroup> hit_groups;
	hit_groups.resize(n + 1);
	hit_groups[0] = hg0_default;
	for (uint32_t i = 1; i < n; i++) {
		if (!p_task->new_ready_mask[i] || p_task->new_per_hg_shaders[i].is_null()) {
			hit_groups[i] = empty_hg;
			continue;
		}
		RD::PipelineShader custom_ps = { p_task->new_per_hg_shaders[i], p_task->spec_constants };
		RD::HitGroup hg;
		hg.closest_hit_shader = custom_ps;
		hg.any_hit_shader = p_task->slots[i].uses_alpha_clip ? custom_ps : base_ps;
		if (p_task->slots[i].is_procedural) {
			hg.intersection_shader = custom_ps;
		}
		hit_groups[i] = hg;
	}
	hit_groups[n] = empty_hg;

	RID new_pipeline = RD::get_singleton()->raytracing_pipeline_create(
			{ &base_ps, 1 }, { &base_ps, 1 },
			{ hit_groups.ptr(), (uint64_t)hit_groups.size() },
			RT_MAX_RECURSION_DEPTH);
	if (new_pipeline.is_null()) {
		cleanup_created();
		p_task->failed = true;
		p_task->error = "raytracing_pipeline_create returned null";
		finish();
		return;
	}
	p_task->new_pipeline = new_pipeline;
	finish();
}

void SceneShaderRaytracing::_finalize_pipeline_build(PipelineBuildTask *p_task) {
	HashMap<uint32_t, PipelineBundle>::Iterator bit = pipeline_bundles.find(p_task->rt_flags);
	if (bit == pipeline_bundles.end()) {
		for (uint32_t i = 0; i < p_task->slots.size(); i++) {
			if (p_task->slots[i].existing_per_hg_shader.is_null() &&
					i < p_task->new_per_hg_shaders.size() &&
					p_task->new_per_hg_shaders[i].is_valid()) {
				RD::get_singleton()->free_rid(p_task->new_per_hg_shaders[i]);
			}
		}
		if (p_task->new_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(p_task->new_pipeline);
		}
		return;
	}
	PipelineBundle &bundle = bit->value;
	_bundle_resize_for_slots(bundle);

	if (p_task->failed) {
		WARN_PRINT(vformat("RT: pipeline rebuild failed for variant 0x%x: %s",
				p_task->rt_flags, p_task->error));
		return;
	}

	// Install worker outputs into bundle arrays.
	uint32_t n = (uint32_t)p_task->slots.size();
	for (uint32_t i = 1; i < n; i++) {
		const PipelineBuildTask::SlotInput &si = p_task->slots[i];
		if (i >= bundle.per_hg_shaders.size()) {
			break;
		}
		if (si.existing_per_hg_shader.is_valid()) {
			continue;
		}
		if (si.needs_compile && i < p_task->new_per_hg_shaders.size() && p_task->new_per_hg_shaders[i].is_valid()) {
			bundle.per_hg_shaders[i] = p_task->new_per_hg_shaders[i];
			bundle.per_hg_states[i] = HGState::Ready;
		} else if (si.needs_compile) {
			bundle.per_hg_states[i] = HGState::Failed;
		}
	}

	static constexpr uint32_t HIT_SBT_CAPACITY = 4096;
	uint32_t sbt_size = MAX(n, HIT_SBT_CAPACITY);

	RID new_sbt = RD::get_singleton()->hit_sbt_create(p_task->new_pipeline, sbt_size);
	if (new_sbt.is_null()) {
		WARN_PRINT(vformat("RT: hit_sbt_create failed for variant 0x%x.", p_task->rt_flags));
		RD::get_singleton()->free_rid(p_task->new_pipeline);
		return;
	}
	RD::get_singleton()->set_resource_name(new_sbt, String("RT Hit SBT [flags=") + itos(p_task->rt_flags) + "]");

	RD::HitShaderBindingTableRange sbt_range = RD::get_singleton()->hit_sbt_range_alloc(new_sbt, sbt_size);
	if (!sbt_range) {
		WARN_PRINT(vformat("RT: hit_sbt_range_alloc failed for variant 0x%x.", p_task->rt_flags));
		RD::get_singleton()->free_rid(new_sbt);
		RD::get_singleton()->free_rid(p_task->new_pipeline);
		return;
	}

	// Slots i >= n map to sentinel HG n.
	LocalVector<uint32_t> indices;
	indices.resize(sbt_size);
	for (uint32_t i = 0; i < sbt_size; i++) {
		indices[i] = (i < n) ? i : n;
	}
	RD::get_singleton()->hit_sbt_range_update(new_sbt, sbt_range, 0, indices);

	if (bundle.hit_sbt.is_valid()) {
		RD::get_singleton()->free_rid(bundle.hit_sbt);
	}
	if (bundle.pipeline.is_valid()) {
		RD::get_singleton()->free_rid(bundle.pipeline);
	}
	RD::get_singleton()->set_resource_name(p_task->new_pipeline, String("RT Pipeline [flags=") + itos(p_task->rt_flags) + "]");
	bundle.pipeline = p_task->new_pipeline;
	bundle.hit_sbt = new_sbt;
	bundle.live_hg_count = n;
	bundle.live_ready_mask = p_task->new_ready_mask;
}

// Single-lane rebuild dispatcher.

void SceneShaderRaytracing::_enqueue_build(PipelineBuildTask *p_task) {
	MutexLock lock(compile_lane.mutex);
	compile_lane.queue.push_back(p_task);
	_dispatch_next_locked();
}

void SceneShaderRaytracing::_dispatch_next_locked() {
	if (!async_compilation_enabled) {
		return;
	}
	if (compile_lane.current != nullptr || compile_lane.queue.is_empty()) {
		return;
	}
	PipelineBuildTask *t = compile_lane.queue[0];
	compile_lane.queue.remove_at(0);
	compile_lane.current = t;
	t->worker_id = WorkerThreadPool::get_singleton()->add_native_task(
			&SceneShaderRaytracing::_build_pipeline_worker_static, t, /*high_priority=*/false,
			"RT Pipeline Build");
}

void SceneShaderRaytracing::_kick_rebuild_if_idle() {
	bool lane_idle;
	{
		MutexLock lock(compile_lane.mutex);
		lane_idle = (compile_lane.current == nullptr) && compile_lane.queue.is_empty();
	}
	if (!lane_idle) {
		return;
	}
	for (KeyValue<uint32_t, PipelineBundle> &kv : pipeline_bundles) {
		if (!kv.value.dirty || !kv.value.initial_pipeline_built) {
			continue;
		}
		PipelineBuildTask *task = _make_pipeline_build_task(kv.key, kv.value);
		if (task) {
			kv.value.dirty = false;
			_enqueue_build(task);
		}
		break;
	}
}

void SceneShaderRaytracing::drain_completed_compiles() {
	while (true) {
		PipelineBuildTask *finished = nullptr;
		{
			MutexLock lock(compile_lane.mutex);
			if (compile_lane.current && compile_lane.current->done.is_set()) {
				finished = compile_lane.current;
				compile_lane.current = nullptr;
			}
		}
		if (!finished) {
			break;
		}
		if (finished->worker_id != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(finished->worker_id);
		}
		_finalize_pipeline_build(finished);
		memdelete(finished);

		{
			MutexLock lock(compile_lane.mutex);
			_dispatch_next_locked();
		}
	}
}

void SceneShaderRaytracing::_drain_lane_inline_main_thread() {
	while (true) {
		PipelineBuildTask *t = nullptr;
		{
			MutexLock lock(compile_lane.mutex);
			if (compile_lane.current) {
				break;
			}
			if (compile_lane.queue.is_empty()) {
				break;
			}
			t = compile_lane.queue[0];
			compile_lane.queue.remove_at(0);
		}
		_build_pipeline_worker(t);
		_finalize_pipeline_build(t);
		memdelete(t);
	}
}

void SceneShaderRaytracing::_join_lane_for_shutdown() {
	PipelineBuildTask *current = nullptr;
	LocalVector<PipelineBuildTask *> queued;
	{
		MutexLock lock(compile_lane.mutex);
		current = compile_lane.current;
		compile_lane.current = nullptr;
		queued = compile_lane.queue;
		compile_lane.queue.clear();
	}

	if (current) {
		current->abort_requested.set();
		if (current->worker_id != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton()) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(current->worker_id);
		}
		if (current->new_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(current->new_pipeline);
		}
		memdelete(current);
	}
	for (PipelineBuildTask *t : queued) {
		memdelete(t);
	}
}

void SceneShaderRaytracing::init(const String p_defines) {
	async_compilation_enabled = (bool)GLOBAL_GET("rendering/pathtracer/async_shader_compilation");

	// Raygen: one mode per bitmask of RAYGEN_SHADER_OPTIONS.
	const uint32_t variant_count = 1u << RAYGEN_SHADER_OPTION_COUNT;
	Vector<String> modes;
	modes.resize((int)variant_count);
	String *modes_ptr = modes.ptrw();
	for (uint32_t mask = 0; mask < variant_count; mask++) {
		modes_ptr[mask] = _raygen_variant_preamble(mask);
	}
	raygen_shader.initialize(modes, p_defines);

	// Slot 0: default HG (source_hash stays zero).
	{
		HitGroupSlot default_slot;
		default_slot.state = HGState::Ready;
		hit_group_slots.push_back(default_slot);
	}

	// Now create a version to access the embedded raytracing shader
	raygen_shader_version = raygen_shader.version_create();
	if (raygen_shader_version.is_valid()) {
		const PipelineBundle &b = ensure_pipeline_bundle(RT_FLAG_NONE);
		if (!b.pipeline.is_valid()) {
			WARN_PRINT("Failed to create default raytracing pipeline bundle");
		}
	} else {
		WARN_PRINT("Failed to create raytracing shader version");
	}

	{
		// Shader compiler (not used for RT, but needed for compatibility).
		ShaderCompiler::DefaultIdentifierActions actions;

		actions.renames["MODEL_MATRIX"] = "read_model_matrix";
		actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
		actions.renames["VIEW_MATRIX"] = "read_view_matrix";
		actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
		actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
		actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
		actions.renames["MODELVIEW_MATRIX"] = "(read_view_matrix * read_model_matrix)";
		actions.renames["MODELVIEW_NORMAL_MATRIX"] = "mat3(read_view_matrix * read_model_matrix)";
		actions.renames["MAIN_CAM_INV_VIEW_MATRIX"] = "inv_view_matrix";

		actions.renames["VERTEX"] = "vertex";
		actions.renames["NORMAL"] = "normal";
		actions.renames["TANGENT"] = "tangent";
		actions.renames["BINORMAL"] = "binormal";
		actions.renames["POSITION"] = "position";
		actions.renames["UV"] = "uv_interp";
		actions.renames["UV2"] = "uv2_interp";
		actions.renames["COLOR"] = "color_interp";
		actions.renames["POINT_SIZE"] = "rt_point_size";
		actions.renames["INSTANCE_ID"] = "rt_instance_id";
		actions.renames["VERTEX_ID"] = "rt_vertex_id";

		actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
		actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
		actions.renames["ALPHA_ANTIALIASING_EDGE"] = "alpha_antialiasing_edge";
		actions.renames["ALPHA_TEXTURE_COORDINATE"] = "alpha_texture_coordinate";

		// Builtins.

		actions.renames["TIME"] = "global_time";
		actions.renames["PREV_TIME"] = "global_prev_time";
		actions.renames["EXPOSURE"] = "(1.0 / scene_data_block.data.emissive_exposure_normalization)";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["OUTPUT_IS_SRGB"] = "SHADER_IS_SRGB";
		actions.renames["CLIP_SPACE_FAR"] = "SHADER_SPACE_FAR";
		actions.renames["VIEWPORT_SIZE"] = "read_viewport_size";

		actions.renames["FRAGCOORD"] = "rt_frag_coord";
		actions.renames["FRONT_FACING"] = "rt_front_facing";
		actions.renames["NORMAL_MAP"] = "normal_map";
		actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
		actions.renames["ALBEDO"] = "albedo";
		actions.renames["ALPHA"] = "alpha";
		actions.renames["PREMUL_ALPHA_FACTOR"] = "premul_alpha";
		actions.renames["METALLIC"] = "metallic";
		actions.renames["SPECULAR"] = "specular";
		actions.renames["ROUGHNESS"] = "roughness";
		actions.renames["RIM"] = "rim";
		actions.renames["RIM_TINT"] = "rim_tint";
		actions.renames["CLEARCOAT"] = "clearcoat";
		actions.renames["CLEARCOAT_ROUGHNESS"] = "clearcoat_roughness";
		actions.renames["ANISOTROPY"] = "anisotropy";
		actions.renames["ANISOTROPY_FLOW"] = "anisotropy_flow";
		actions.renames["SSS_STRENGTH"] = "sss_strength";
		actions.renames["SSS_TRANSMITTANCE_COLOR"] = "transmittance_color";
		actions.renames["SSS_TRANSMITTANCE_DEPTH"] = "transmittance_depth";
		actions.renames["SSS_TRANSMITTANCE_BOOST"] = "transmittance_boost";
		actions.renames["BACKLIGHT"] = "backlight";
		actions.renames["AO"] = "ao";
		actions.renames["AO_LIGHT_AFFECT"] = "ao_light_affect";
		actions.renames["EMISSION"] = "emission";
		actions.renames["POINT_COORD"] = "rt_point_coord";
		actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
		actions.renames["SCREEN_UV"] = "rt_screen_uv";
		actions.renames["DEPTH"] = "rt_depth";
		actions.renames["FOG"] = "fog";
		actions.renames["RADIANCE"] = "custom_radiance";
		actions.renames["IRRADIANCE"] = "custom_irradiance";
		actions.renames["BONE_INDICES"] = "bone_attrib";
		actions.renames["BONE_WEIGHTS"] = "weight_attrib";
		actions.renames["CUSTOM0"] = "custom0_attrib";
		actions.renames["CUSTOM1"] = "custom1_attrib";
		actions.renames["CUSTOM2"] = "custom2_attrib";
		actions.renames["CUSTOM3"] = "custom3_attrib";
		actions.renames["LIGHT_VERTEX"] = "light_vertex";

		actions.renames["NODE_POSITION_WORLD"] = "read_model_matrix[3].xyz";
		actions.renames["CAMERA_POSITION_WORLD"] = "inv_view_matrix[3].xyz";
		actions.renames["CAMERA_DIRECTION_WORLD"] = "inv_view_matrix[2].xyz";
		actions.renames["CAMERA_VISIBLE_LAYERS"] = "scene_data_block.data.camera_visible_layers";
		actions.renames["NODE_POSITION_VIEW"] = "(read_view_matrix * read_model_matrix)[3].xyz";

		actions.renames["VIEW_INDEX"] = "ViewIndex";
		actions.renames["VIEW_MONO_LEFT"] = "0";
		actions.renames["VIEW_RIGHT"] = "1";
		actions.renames["EYE_OFFSET"] = "eye_offset";

		// For light.
		actions.renames["VIEW"] = "view";
		actions.renames["SPECULAR_AMOUNT"] = "specular_amount";
		actions.renames["LIGHT_COLOR"] = "light_color";
		actions.renames["LIGHT_IS_DIRECTIONAL"] = "is_directional";
		actions.renames["LIGHT"] = "light";
		actions.renames["ATTENUATION"] = "attenuation";
		actions.renames["DIFFUSE_LIGHT"] = "diffuse_light";
		actions.renames["SPECULAR_LIGHT"] = "specular_light";

		actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
		actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
		actions.usage_defines["BINORMAL"] = "@TANGENT";
		actions.usage_defines["RIM"] = "#define LIGHT_RIM_USED\n";
		actions.usage_defines["RIM_TINT"] = "@RIM";
		actions.usage_defines["CLEARCOAT"] = "#define LIGHT_CLEARCOAT_USED\n";
		actions.usage_defines["CLEARCOAT_ROUGHNESS"] = "@CLEARCOAT";
		actions.usage_defines["ANISOTROPY"] = "#define LIGHT_ANISOTROPY_USED\n";
		actions.usage_defines["ANISOTROPY_FLOW"] = "@ANISOTROPY";
		actions.usage_defines["AO"] = "#define AO_USED\n";
		actions.usage_defines["AO_LIGHT_AFFECT"] = "#define AO_USED\n";
		actions.usage_defines["UV"] = "#define UV_USED\n";
		actions.usage_defines["UV2"] = "#define UV2_USED\n";
		actions.usage_defines["BONE_INDICES"] = "#define BONES_USED\n";
		actions.usage_defines["BONE_WEIGHTS"] = "#define WEIGHTS_USED\n";
		actions.usage_defines["CUSTOM0"] = "#define CUSTOM0_USED\n";
		actions.usage_defines["CUSTOM1"] = "#define CUSTOM1_USED\n";
		actions.usage_defines["CUSTOM2"] = "#define CUSTOM2_USED\n";
		actions.usage_defines["CUSTOM3"] = "#define CUSTOM3_USED\n";
		actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
		actions.usage_defines["NORMAL_MAP_DEPTH"] = "@NORMAL_MAP";
		actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
		actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
		actions.usage_defines["POSITION"] = "#define OVERRIDE_POSITION\n";
		actions.usage_defines["LIGHT_VERTEX"] = "#define LIGHT_VERTEX_USED\n";
		actions.usage_defines["PREMUL_ALPHA_FACTOR"] = "#define PREMUL_ALPHA_USED\n";

		actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
		actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
		actions.usage_defines["ALPHA_ANTIALIASING_EDGE"] = "#define ALPHA_ANTIALIASING_EDGE_USED\n";
		actions.usage_defines["ALPHA_TEXTURE_COORDINATE"] = "@ALPHA_ANTIALIASING_EDGE";

		actions.usage_defines["SSS_STRENGTH"] = "#define ENABLE_SSS\n";
		actions.usage_defines["SSS_TRANSMITTANCE_DEPTH"] = "#define ENABLE_TRANSMITTANCE\n";
		actions.usage_defines["BACKLIGHT"] = "#define LIGHT_BACKLIGHT_USED\n";
		actions.usage_defines["SCREEN_UV"] = "#define SCREEN_UV_USED\n";

		actions.usage_defines["FOG"] = "#define CUSTOM_FOG_USED\n";
		actions.usage_defines["RADIANCE"] = "#define CUSTOM_RADIANCE_USED\n";
		actions.usage_defines["IRRADIANCE"] = "#define CUSTOM_IRRADIANCE_USED\n";

		actions.usage_defines["MODEL_MATRIX"] = "#define MODEL_MATRIX_USED\n";

		actions.render_mode_defines["skip_vertex_transform"] = "#define SKIP_TRANSFORM_USED\n";
		actions.render_mode_defines["world_vertex_coords"] = "#define VERTEX_WORLD_COORDS_USED\n";
		actions.render_mode_defines["ensure_correct_normals"] = "#define ENSURE_CORRECT_NORMALS\n";
		actions.render_mode_defines["cull_front"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["cull_disabled"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["particle_trails"] = "#define USE_PARTICLE_TRAILS\n";
		actions.render_mode_defines["depth_prepass_alpha"] = "#define USE_OPAQUE_PREPASS\n";

		bool force_lambert = GLOBAL_GET("rendering/shading/overrides/force_lambert_over_burley");

		if (!force_lambert) {
			actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
		}

		actions.render_mode_defines["diffuse_lambert_wrap"] = "#define DIFFUSE_LAMBERT_WRAP\n";
		actions.render_mode_defines["diffuse_toon"] = "#define DIFFUSE_TOON\n";

		actions.render_mode_defines["sss_mode_skin"] = "#define SSS_MODE_SKIN\n";

		actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

		actions.render_mode_defines["specular_toon"] = "#define SPECULAR_TOON\n";
		actions.render_mode_defines["specular_disabled"] = "#define SPECULAR_DISABLED\n";
		actions.render_mode_defines["shadows_disabled"] = "#define SHADOWS_DISABLED\n";
		actions.render_mode_defines["ambient_light_disabled"] = "#define AMBIENT_LIGHT_DISABLED\n";
		actions.render_mode_defines["shadow_to_opacity"] = "#define USE_SHADOW_TO_OPACITY\n";
		actions.render_mode_defines["unshaded"] = "#define MODE_UNSHADED\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (!force_vertex_shading) {
			// If forcing vertex shading, this will be defined already.
			actions.render_mode_defines["vertex_lighting"] = "#define USE_VERTEX_LIGHTING\n";
		}

		actions.render_mode_defines["debug_shadow_splits"] = "#define DEBUG_DRAW_PSSM_SPLITS\n";
		actions.render_mode_defines["fog_disabled"] = "#define FOG_DISABLED\n";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = RenderForwardClustered::MATERIAL_UNIFORM_SET;
		actions.base_uniform_string = "material.";
		actions.base_varying_index = 14;
		actions.suppress_varying_io = true;

		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms.data";
		actions.instance_uniform_index_variable = "instances.data[instance_index_interp].instance_uniforms_ofs";

		actions.check_multiview_samplers = RendererCompositorRD::get_singleton()->is_xr_enabled(); // Make sure we check sampling multiview textures.

		// Intersection stage built-in renames (must match locals in the GLSL template).
		actions.renames["ORIGIN"] = "m_ORIGIN";
		actions.renames["DIRECTION"] = "m_DIRECTION";
		actions.renames["WORLD_ORIGIN"] = "m_WORLD_ORIGIN";
		actions.renames["WORLD_DIRECTION"] = "m_WORLD_DIRECTION";
		actions.renames["T_MIN"] = "m_T_MIN";
		actions.renames["T_MAX"] = "m_T_MAX";
		actions.renames["HIT_UV"] = "m_HIT_UV";
		actions.renames["HIT_NORMAL"] = "m_HIT_NORMAL";
		actions.renames["HIT_TANGENT"] = "m_HIT_TANGENT";
		actions.renames["PREV_POSITION"] = "m_PREV_POSITION";
		actions.renames["AABB_MIN"] = "m_AABB_MIN";
		actions.renames["AABB_MAX"] = "m_AABB_MAX";

		// Stage function rename: bypass _mkid prefix to match the GLSL macro.
		actions.renames["report_intersection"] = "report_intersection";

		compiler.initialize(actions);
	}

	// Raster shader/material/sampler defaults unused on RT path.
	default_shader = RID();
	default_material = RID();
	default_shader_rd = RID();
	default_shader_sdfgi_rd = RID();
	default_material_shader_ptr = nullptr;
	default_material_uniform_set = RID();

	overdraw_material_shader = RID();
	overdraw_material = RID();
	overdraw_material_shader_ptr = nullptr;
	overdraw_material_uniform_set = RID();

	debug_shadow_splits_material_shader = RID();
	debug_shadow_splits_material = RID();
	debug_shadow_splits_material_shader_ptr = nullptr;
	debug_shadow_splits_material_uniform_set = RID();

	default_vec4_xform_buffer = RID();
	default_vec4_xform_uniform_set = RID();
	shadow_sampler = RID();
}

void SceneShaderRaytracing::set_default_specialization(const ShaderSpecialization &p_specialization) {
	default_specialization = p_specialization;

	for (SelfList<ShaderData> *E = shader_list.first(); E; E = E->next()) {
		E->self()->pipeline_hash_map.clear_pipelines();
	}
}

void SceneShaderRaytracing::enable_advanced_shader_group(bool p_needs_multiview) {
}

bool SceneShaderRaytracing::is_multiview_shader_group_enabled() const {
	return false;
}

bool SceneShaderRaytracing::is_advanced_shader_group_enabled(bool p_multiview) const {
	return false;
}

uint32_t SceneShaderRaytracing::get_pipeline_compilations(RSE::PipelineSource p_source) {
	MutexLock lock(SceneShaderRaytracing::singleton_mutex);
	return pipeline_compilations[p_source];
}
