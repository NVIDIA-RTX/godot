/**************************************************************************/
/*  depth_reconstruct.cpp                                                 */
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

#include "depth_reconstruct.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

DepthReconstruct::DepthReconstruct() {
	Vector<String> shader_modes;
	shader_modes.push_back("");

	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

DepthReconstruct::~DepthReconstruct() {
	shader.version_free(shader_version);
	if (history_texture.is_valid()) {
		RD::get_singleton()->free_rid(history_texture);
	}
}

void DepthReconstruct::_ensure_history_texture(const Size2i &p_size) {
	if (history_texture.is_valid() && history_size == p_size) {
		return;
	}

	if (history_texture.is_valid()) {
		RD::get_singleton()->free_rid(history_texture);
	}

	RD::TextureFormat fmt;
	fmt.format = RD::DATA_FORMAT_R32_SFLOAT;
	fmt.width = p_size.x;
	fmt.height = p_size.y;
	fmt.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	fmt.texture_type = RD::TEXTURE_TYPE_2D;

	history_texture = RD::get_singleton()->texture_create(fmt, RD::TextureView());
	RD::get_singleton()->texture_clear(history_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	history_size = p_size;
	frame_count = 0;
}

void DepthReconstruct::process(RID p_lowres_depth, RID p_upscaled_color, RID p_velocity,
		RID p_dest_depth,
		const Size2i &p_target_size, const Size2i &p_lowres_size,
		float p_z_near, float p_z_far) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	_ensure_history_texture(p_target_size);

	frame_count++;
	float history_weight = (frame_count <= 1) ? 0.0f : 0.875f;

	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.target_size[0] = p_target_size.x;
	push_constant.target_size[1] = p_target_size.y;
	push_constant.lowres_size[0] = p_lowres_size.x;
	push_constant.lowres_size[1] = p_lowres_size.y;
	push_constant.z_near = p_z_near;
	push_constant.z_far = p_z_far;
	push_constant.history_weight = history_weight;

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID dr_shader = shader.version_get_shader(shader_version, 0);

	RD::Uniform u_lowres_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_lowres_depth }));
	RD::Uniform u_upscaled_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, p_upscaled_color }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_velocity }));
	RD::Uniform u_history_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear_sampler, history_texture }));
	RD::Uniform u_dest_depth(RD::UNIFORM_TYPE_IMAGE, 4, p_dest_depth);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(dr_shader, 0, u_lowres_depth, u_upscaled_color, u_velocity, u_history_depth, u_dest_depth), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_target_size.x, p_target_size.y, 1);
	RD::get_singleton()->compute_list_end();

	// Copy result to history for next frame.
	RD::get_singleton()->texture_copy(p_dest_depth, history_texture,
			Vector3(0, 0, 0), Vector3(0, 0, 0),
			Vector3(p_target_size.x, p_target_size.y, 1),
			0, 0, 0, 0);
}
