/**************************************************************************/
/*  bindless_block.cpp                                                    */
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

#include "bindless_block.h"

#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

void BindlessBlock::initialize(RenderingDevice *p_rd) {
	ERR_FAIL_NULL(p_rd);

	rd = p_rd;

	default_texture = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(
			RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	textures.push_back(default_texture);
	texture_to_index[default_texture] = 0;
}

void BindlessBlock::begin_frame() {
	if (!is_initialized()) {
		return;
	}

	bool set_valid = uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set);
	if (set_valid) {
		return;
	}

	// Uniform set was externally invalidated (a texture was destroyed/recreated).
	// Scrub dead RIDs: replace with default and add slots to the freelist.
	uniform_set = RID();

	texture_to_index.clear();
	texture_to_index[default_texture] = 0;

	for (uint32_t i = 1; i < textures.size(); i++) {
		if (!rd->texture_is_valid(textures[i])) {
			textures[i] = default_texture;
			if (free_indices_count < free_indices.size()) {
				free_indices[free_indices_count] = i;
			} else {
				free_indices.push_back(i);
			}
			++free_indices_count;
		} else {
			texture_to_index[textures[i]] = i;
		}
	}

	needs_refinalize = true;
}

uint32_t BindlessBlock::add_texture(RID p_texture) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), 0, "BindlessBlock not initialized. Call initialize() first.");

	// Substitute null/stale RIDs with the default white texture.
	if (!p_texture.is_valid() || !rd->texture_is_valid(p_texture)) {
		p_texture = default_texture;
	}

	HashMap<RID, uint32_t>::Iterator it = texture_to_index.find(p_texture);
	if (it != texture_to_index.end()) {
		return it->value;
	}

	if (textures.size() >= MAX_BINDLESS_TEXTURES && free_indices_count == 0) {
		ERR_PRINT_ONCE("BindlessBlock: Maximum texture count exceeded. Using default texture.");
		return 0;
	}

	if (is_finalized()) {
		needs_refinalize = true;
	}

	uint32_t index;
	if (free_indices_count > 0) {
		--free_indices_count;
		index = free_indices[free_indices_count];
		textures[index] = p_texture;
	} else {
		index = textures.size();
		textures.push_back(p_texture);
	}

	texture_to_index[p_texture] = index;
	return index;
}

void BindlessBlock::finalize(RID p_shader, uint32_t p_set_index) {
	ERR_FAIL_COND_MSG(!is_initialized(), "BindlessBlock not initialized.");
	ERR_FAIL_COND_MSG(textures.is_empty(), "BindlessBlock has no textures.");

	if (is_finalized() && !needs_refinalize && rd->uniform_set_is_valid(uniform_set)) {
		return;
	}

	if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();

	Vector<RD::Uniform> uniforms;

	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	u.binding = 0;
	u.variable_count = true;
	for (uint32_t i = 0; i < textures.size(); i++) {
		u.append_id(textures[i]);
	}
	uniforms.push_back(u);

	uniform_set = rd->uniform_set_create(uniforms, p_shader, p_set_index);
	ERR_FAIL_COND_MSG(!uniform_set.is_valid(), "Failed to create bindless uniform set.");
	rd->set_resource_name(uniform_set, "Bindless Texture Set");

	needs_refinalize = false;
}

void BindlessBlock::clear() {
	if (uniform_set.is_valid() && rd && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
	textures.clear();
	texture_to_index.clear();
	free_indices.clear();
	free_indices_count = 0;
	rd = nullptr;
}

BindlessBlock::~BindlessBlock() {
	clear();
}
