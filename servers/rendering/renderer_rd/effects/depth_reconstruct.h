/**************************************************************************/
/*  depth_reconstruct.h                                                   */
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

#include "servers/rendering/renderer_rd/shaders/effects/depth_reconstruct.glsl.gen.h"

namespace RendererRD {

/// Reconstructs a full-resolution depth buffer from low-res depth using
/// joint bilateral upsampling guided by the upscaled color buffer, with
/// temporal accumulation via motion-vector reprojection (8-frame history).
class DepthReconstruct {
private:
	struct PushConstant {
		int32_t target_size[2];
		int32_t lowres_size[2];
		float z_near;
		float z_far;
		float history_weight;
		float _pad;
	};

	PushConstant push_constant;
	DepthReconstructShaderRD shader;
	RID shader_version;
	RID pipeline;

	RID history_texture;
	Size2i history_size;
	int frame_count = 0;

	void _ensure_history_texture(const Size2i &p_size);

public:
	DepthReconstruct();
	~DepthReconstruct();

	/// Reconstruct full-resolution depth with temporal accumulation.
	/// @param p_lowres_depth    Low-resolution depth texture (reverse-Z).
	/// @param p_upscaled_color  Upscaled color texture for edge guidance.
	/// @param p_velocity        Motion vector texture (RG16F, prev_uv - current_uv).
	/// @param p_dest_depth      Output R32F image at target resolution.
	/// @param p_target_size     Target (output) resolution.
	/// @param p_lowres_size     Low-resolution input size.
	/// @param p_z_near          Camera near plane.
	/// @param p_z_far           Camera far plane.
	void process(RID p_lowres_depth, RID p_upscaled_color, RID p_velocity,
			RID p_dest_depth,
			const Size2i &p_target_size, const Size2i &p_lowres_size,
			float p_z_near, float p_z_far);
};

} // namespace RendererRD
