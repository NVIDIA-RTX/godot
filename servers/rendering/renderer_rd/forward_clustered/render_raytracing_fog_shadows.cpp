/**************************************************************************/
/*  render_raytracing_fog_shadows.cpp                                     */
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

#include "render_raytracing_fog_shadows.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"

using namespace RendererSceneRenderImplementation;

void RTFogShadows::initialize() {
	if (shader_version.is_valid() || !RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return;
	}

	RendererRD::Fog *fog = RendererRD::Fog::get_singleton();
	ERR_FAIL_NULL(fog);

	Vector<ShaderRD::VariantDefine> variants;
	for (int group = 0; group < RendererRD::Fog::get_process_shader_group_count(); group++) {
		variants.push_back(ShaderRD::VariantDefine(group, RendererRD::Fog::get_process_shader_group_define(group) + "\n#define MODE_DENSITY\n#define USE_RAY_QUERY_SHADOWS\n", false));
	}
	shader.initialize(variants, fog->get_process_shader_defines());
	shader.enable_group(RendererRD::Fog::get_process_shader_group());
	shader_version = shader.version_create();

	RID shader_rd = _get_shader_rd();
	if (shader_rd.is_null()) {
		shader.version_free(shader_version);
		shader_version = RID();
		return;
	}

	for (uint32_t i = 0; i < MAX_POSITIONAL_SHADOW_RAYS; i++) {
		RD::PipelineSpecializationConstant sc;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.constant_id = 0;
		sc.int_value = i + 1;
		pipelines[i].create_compute_pipeline(shader_rd, { sc });
	}
}

RID RTFogShadows::_get_shader_rd() {
	return shader.version_get_shader(shader_version, RendererRD::Fog::get_process_shader_group());
}

RID RTFogShadows::_update_bindless_uniform_set(RenderRaytracing *p_raytracing) {
	BindlessBlock *bindless_block = p_raytracing->get_bindless_block();
	if (!bindless_block || !bindless_block->is_initialized()) {
		return RID();
	}

	bool valid = bindless_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(bindless_uniform_set);
	if (valid && bindless_version == bindless_block->get_version()) {
		return bindless_uniform_set;
	}
	if (valid) {
		RD::get_singleton()->free_rid(bindless_uniform_set);
	}
	bindless_uniform_set = bindless_block->create_uniform_set(_get_shader_rd(), 2);
	bindless_version = bindless_block->get_version();
	return bindless_uniform_set;
}

bool RTFogShadows::prepare(RenderRaytracing *p_raytracing, RTViewportState *p_state, const RenderDataRD *p_render_data, RendererRD::Fog::VolumetricFogRayQueryShadows &r_shadows) {
	ERR_FAIL_NULL_V(p_raytracing, false);
	if (!is_supported() || !p_state || p_state->tlas.is_null()) {
		return false;
	}

	RID bindless_set = _update_bindless_uniform_set(p_raytracing);
	if (bindless_set.is_null()) {
		return false;
	}

	if (p_state->fog_shadows_ubo.is_null()) {
		p_state->fog_shadows_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(ParamsUBO));
		RD::get_singleton()->set_resource_name(p_state->fog_shadows_ubo, "RT Fog Shadows UBO");
	}

	ParamsUBO params = {};
	RendererRD::MaterialStorage::store_transform(p_render_data->scene_data->cam_transform, params.view_to_world);
	params.frame_index = p_state->frame_counter;
	params.trace_shadows = GLOBAL_GET_CACHED(bool, "rendering/pathtracing/volumetric_fog_shadows") ? 1 : 0;
	params.shadow_mask = RT_SHADOW_GROUPS_MASK & ~uint32_t(p_state->shadow_groups.self_shadow_bit);
	RD::get_singleton()->buffer_update(p_state->fog_shadows_ubo, 0, sizeof(ParamsUBO), &params);

	if (p_state->fog_shadows_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->fog_shadows_uniform_set)) {
		RD::get_singleton()->free_rid(p_state->fog_shadows_uniform_set);
	}

	RID default_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer();
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_state->tlas));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_state->geometry_buffer.is_valid() ? p_state->geometry_buffer : default_buffer));
	const RID material_buffer = p_raytracing->get_material_buffer();
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, material_buffer.is_valid() ? material_buffer : default_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, p_state->fog_shadows_ubo));
	p_state->fog_shadows_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, _get_shader_rd(), 1, true);
	if (p_state->fog_shadows_uniform_set.is_null()) {
		return false;
	}
	RD::get_singleton()->set_resource_name(p_state->fog_shadows_uniform_set, "RT Fog Shadows Uniform Set");

	int rays = GLOBAL_GET_CACHED(int, "rendering/pathtracing/volumetric_fog_positional_shadow_rays");
	rays = CLAMP(rays, 1, int(MAX_POSITIONAL_SHADOW_RAYS));

	r_shadows.pipeline = pipelines[rays - 1].get_rid();
	r_shadows.uniform_set = p_state->fog_shadows_uniform_set;
	r_shadows.bindless_uniform_set = bindless_set;
	return true;
}

RTFogShadows::~RTFogShadows() {
	for (uint32_t i = 0; i < MAX_POSITIONAL_SHADOW_RAYS; i++) {
		pipelines[i].free();
	}
	if (bindless_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(bindless_uniform_set)) {
		RD::get_singleton()->free_rid(bindless_uniform_set);
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}
