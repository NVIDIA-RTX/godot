/**************************************************************************/
/*  render_raytracing_fog_shadows.h                                       */
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

#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"

class RenderDataRD;

namespace RendererSceneRenderImplementation {

class RenderRaytracing;
struct RTViewportState;

// Owns a separate fog process shader so a failing ray query variant can't break the raster variants.
class RTFogShadows {
public:
	static constexpr uint32_t MAX_POSITIONAL_SHADOW_RAYS = 4;

	void initialize();
	bool is_supported() const { return shader_version.is_valid(); }

	bool prepare(RenderRaytracing *p_raytracing, RTViewportState *p_state, const RenderDataRD *p_render_data, RendererRD::Fog::VolumetricFogRayQueryShadows &r_shadows);

	~RTFogShadows();

private:
	struct ParamsUBO {
		float view_to_world[16];
		uint32_t frame_index;
		uint32_t trace_shadows;
		uint32_t shadow_mask; // TLAS shadow-caster groups, without the SELF_SHADOWING group.
		uint32_t pad;
	};

	VolumetricFogProcessShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MAX_POSITIONAL_SHADOW_RAYS];

	RID bindless_uniform_set;
	uint64_t bindless_version = UINT64_MAX;

	RID _get_shader_rd();
	RID _update_bindless_uniform_set(RenderRaytracing *p_raytracing);
};

} // namespace RendererSceneRenderImplementation
