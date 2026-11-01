/**************************************************************************/
/*  rendering_rt_material_state.h                                         */
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

#include "core/typedefs.h"

#include <cstring>

// BaseMaterial3D state for the path tracer, packed into the PARAM_NAME material parameter.
struct RTBaseMaterialState {
	static constexpr const char *PARAM_NAME = "rt_base_material_state";

	uint32_t uv1_triplanar : 1;
	uint32_t uv1_world_triplanar : 1;
	uint32_t uv2_triplanar : 1;
	uint32_t uv2_world_triplanar : 1;
	uint32_t uv2_vertex_transform : 1;
	uint32_t proximity_fade : 1;
	uint32_t distance_fade_mode : 2; // BaseMaterial3D::DistanceFadeMode.
	uint32_t emission : 1;
	uint32_t emission_add : 1;
	uint32_t emission_on_uv2 : 1;
	uint32_t orm : 1;
	uint32_t ambient_occlusion : 1;
	uint32_t roughness_texture_channel : 3; // BaseMaterial3D::TextureChannel.
	uint32_t unused : 16;

	RTBaseMaterialState() { std::memset(this, 0, sizeof(*this)); }

	int64_t encode() const {
		uint32_t packed;
		std::memcpy(&packed, this, sizeof(packed));
		return packed;
	}

	static RTBaseMaterialState decode(int64_t p_packed) {
		RTBaseMaterialState state;
		const uint32_t packed = uint32_t(p_packed);
		std::memcpy(static_cast<void *>(&state), &packed, sizeof(packed));
		return state;
	}
};

static_assert(sizeof(RTBaseMaterialState) == sizeof(uint32_t), "RTBaseMaterialState must pack into 32 bits.");
