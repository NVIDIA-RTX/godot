/**************************************************************************/
/*  render_forward_clustered_pt.h                                         */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_fog_shadows.h"

namespace RendererSceneRenderImplementation {

// Path-tracing / DLSS variant of the clustered renderer. Inherits all of the
// raster setup and resource management from RenderForwardClustered and overrides
// only the scene render entry points to add the raytraced opaque path, DLSS Ray
// Reconstruction, and the associated debug visualizations.
class RenderForwardClusteredPT : public RenderForwardClustered {
	/* Raytracing */

	RenderRaytracing *raytracing = nullptr;
	RTFogShadows *fog_shadows = nullptr;

	RID rt_capture_framebuffer;
	LocalVector<RTCaptureRequest> rt_capture_requests;
	uint32_t rt_capture_preload_version = 0;

	bool _setup_rt();
	void _record_rt_captures(const RenderDataRD *p_render_data, uint32_t p_rt_flags, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index);
	bool _record_rt_capture(RD::DrawListID p_draw_list, uint32_t p_element);
	bool _rt_capture_variant_ready(const SceneShaderForwardClustered::ShaderData *p_shader);
	uint64_t _rt_capture_input_mask(SceneShaderForwardClustered::ShaderData *p_shader);
	SceneShaderForwardClustered::ShaderData::PipelineKey _rt_capture_pipeline_key(const GeometryInstanceSurfaceDataCache *p_surface_cache, RD::VertexFormatID p_vertex_format) const;
	bool _preload_rt_capture_pipeline(const GeometryInstanceSurfaceDataCache *p_surface_cache, RTVertexCapture::Mode p_mode);
	void _update_rt_capture_preloads();
	void _age_out_motion_vectors(const RenderDataRD *p_render_data);
	void _update_pt_volumetric_fog(RenderDataRD *p_render_data, RTViewportState *p_rt_state, uint32_t p_directional_light_count, uint32_t p_positional_light_count);

protected:
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void _mesh_generate_extra_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache) override;

public:
	virtual bool pathtracing_is_used(RID p_env, RID p_viewport) const override;
	virtual bool pathtracing_uses_raster_list() const override;
	virtual BitField<RSE::RenderingNotReadyReason> get_not_ready_reasons() override;

	RenderForwardClusteredPT();
	~RenderForwardClusteredPT();
};

} // namespace RendererSceneRenderImplementation
