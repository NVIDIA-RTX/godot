/**************************************************************************/
/*  dlss.h                                                                */
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

#include "servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl.gen.h"

namespace RendererRD {

class DLSSEffect;
class DLSSContext {
public:
	struct Parameters {
		DLSSContext *context;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID exposure;
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		bool reverse_depth = true;
		Vector2 jitter;
		float delta_time = 0.0f;
		float sharpness = 0.0f;
		char preset = '?';
		bool reset_accumulation = false;
		Projection reprojection;
		Projection cam_projection;
		Transform3D cam_transform;
		bool dlss_g = false;

		// DLSS Ray Reconstruction buffers
		bool dlss_rr = false; // Enable DLSS-RR mode instead of regular DLSS
		bool dlss_rr_alpha_upscaling = false; // Upscale alpha channel (needed for depth reconstruction)
		RID dlss_rr_diffuse_albedo; // Diffuse albedo buffer (RGB)
		RID dlss_rr_specular_albedo; // Specular albedo buffer (RGB)
		RID dlss_rr_normal_roughness; // World-space normal (XYZ) + roughness (W)
		RID dlss_rr_specular_hit_dist; // Specular hit distance (R16F, -1 = sky)
	} last_parameters;
	DLSSEffect *last_effect = nullptr;
	bool is_d3d12 = false;
	int delay = 4; // Warmup frames before DLSS evaluates (Vulkan stability workaround).

	virtual ~DLSSContext() {}
};

class DLSSEffect {
public:
	struct Shaders {
		MotionVectorDecodeShaderRD mvec_decode_shader;
		RID mvec_decode_version;
		RID mvec_decode_pipeline;
	} shaders;

	DLSSEffect();
	~DLSSEffect();
	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	void upscale(const DLSSContext::Parameters &p_params);
	bool is_ready(DLSSContext *context);

private:
	void _upscale_internal(RDD::CommandBufferID cmdid, const DLSSContext::Parameters &p_params);
	static void _upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
