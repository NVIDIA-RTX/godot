/**************************************************************************/
/*  render_forward_clustered_pt.cpp                                       */
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

// ---------------------------------------------------------------------------
// Path tracer synchronization
// ---------------------------------------------------------------------------
//
// Most of the path traced scene is read through raw device addresses: the hit shaders and the RT fog shadows fetch
// vertices, indices, attributes, instance data and material uniforms from the addresses in RT_GeometryData and
// RT_MaterialData, and BLAS builds read their vertex and index buffers the same way. The draw graph only sees
// resources bound through uniform sets, vertex and index bindings and RD calls, so it can't order these accesses.
// Instead of tracking them one by one, every view runs in fixed phases separated by full barriers. A full barrier
// orders every command recorded before it, including those of earlier views and frames, ahead of every command
// recorded after it. No dependency has to be declared anywhere.
//
// The phases of one view, as recorded by _render_scene():
//
//   1. Captures and merges. _record_rt_captures() draws deformed surfaces into the capture arena. build_tlas() then
//      copies them into their cache entries and bakes MultiMesh and particle surfaces into merged buffers. Everything
//      this phase reads and writes is bound or copied through RD, so the graph orders it. Only this phase writes data
//      that is later read through device addresses.
//   -- barrier --  (recorded by RD::acceleration_structure_build_batch())
//   2. Builds. One batch: all dirty BLAS builds, then all BLAS refits, then both TLASes of the view. The batch adds
//      barriers of its own: one before the TLAS builds, which read the BLASes, and one each time the shared scratch
//      buffer is full at its maximum size. The work recorded after the batch overlaps it: the geometry and material
//      tables, decal volumes, scene UBO, sky setup, pre-opaque compositor effects, lights and clusters. None of it may
//      read what the batch builds or write what the batch reads.
//   -- barrier --  (untracked_barrier() before the volumetric fog)
//   3. Readers. The volumetric fog, the trace and the copies of its output. Nothing here may write a buffer that is
//      read through a device address.
//   -- barrier --  (untracked_barrier() after the trace, when the view has an rt_state)
//
// Each barrier protects:
//   - Before the builds: they read, through device addresses, the vertices phase 1 wrote and the mesh data other
//     systems wrote earlier. It also keeps them behind earlier batches, which used the same scratch memory and BLASes.
//   - Before the readers: the fog and the trace read the structures just built. The batch is untracked, so binding
//     the TLAS in a uniform set doesn't order the trace after its build. It also keeps the reads of the batch ahead of
//     the writes of later views and frames.
//   - After the trace: the captures and merges of later views, and other systems in later frames (particles,
//     MultiMesh buffers, mesh surface updates), rewrite buffers the fog and the trace read through device addresses.
//     Their writes are tracked, but the graph doesn't know those buffers were read.
//
// Phase 1 needs no barrier in front of it: earlier batches are behind the barrier before their readers, and earlier
// fog and traces behind the barrier after their trace.
//
// What doesn't need a barrier:
//   - Initial buffer contents. Buffers created with data are uploaded by transfer workers, and
//     RD::buffer_get_device_address() makes the frame wait for that upload. Taking the address is enough.
//   - TLAS instances. The CPU writes them into mapped buffers that aren't reused until their frame is done.
//   - Anything bound through a uniform set (tables, lights, decals, params, textures): the graph orders it as usual.
//
// Acceleration structures don't reference each other: unlike RD::tlas_build(), the batch adds no BLAS to TLAS links.
// Rebuilding or refitting a BLAS leaves every TLAS built from it stale until that TLAS is rebuilt. That's safe because
// every view rebuilds both of its TLASes in its own batch, after its BLAS updates and before its trace, so a BLAS
// updated by a later view only affects TLASes that were already traced. A BLAS may appear only once per batch, since
// the builds of a batch run concurrently. The decal TLAS is the exception: build_decal_volumes() uses the tracked
// RD::blas_build() and RD::tlas_build(), which the graph orders by itself.
//
// Cost: three full barriers per view plus the ones in the batch. Each drains the queue; only the work recorded between
// the batch and the barrier before the readers overlaps the builds.
//
// Checks: in DEV builds, rendering/pathtracing/debug/strict_validation verifies the addresses in the tables and that
// no TLAS is traced after a BLAS it references was freed, rebuilt or refit (render_raytracing_validation.h). Missing
// barriers are not detected. A new pass belongs in the phase matching what it reads and writes; otherwise it needs a
// barrier of its own.

#include "render_forward_clustered_pt.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h" // IWYU pragma: keep. RENDER_TIMESTAMP macro uses RSG.

using namespace RendererSceneRenderImplementation;

void RenderForwardClusteredPT::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	ERR_FAIL_NULL(p_render_data);

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	bool is_reflection_probe = p_render_data->reflection_probe.is_valid();

	const bool use_rt = !is_reflection_probe && pathtracing_is_used(p_render_data->environment, p_render_data->viewport) && _setup_rt();
	const bool timing_enabled = use_rt &&
			RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_debug_mode(p_render_data->environment) == SceneShaderRaytracing::RT_DEBUG_TIMING_HEATMAP &&
			RD::get_singleton()->has_feature(RD::SUPPORTS_SHADER_DEVICE_CLOCK);
	if (!timing_enabled && rb->has_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING)) {
		rb->clear_context(RB_SCOPE_RT_TIMING);
	}
	if (!use_rt) {
		_age_out_motion_vectors(p_render_data);

		// Path tracing is off for this view: drop any DLSS Ray Reconstruction state
		if (rb_data.is_valid()) {
			if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
				raytracing->dlss_rr_free_buffers(rb.ptr());
			}
			rb->set_depth_reconstruct_requested(false);
		}
		RenderForwardClustered::_render_scene(p_render_data, p_default_bg_color);
		return;
	}

	scene_state.used_uniform_buffer_count = 0;

	static const int texture_multisamples[RSE::VIEWPORT_MSAA_MAX] = { 1, 2, 4, 8 };

	RENDER_TIMESTAMP("Prepare 3D Scene");

	// Compositor effect requirements.
	bool ce_needs_motion_vectors = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS);
	bool ce_needs_normal_roughness = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS);

	current_cluster_builder = rb_data->cluster_builder;
	p_render_data->voxel_gi_count = 0;

	ERR_FAIL_NULL(current_cluster_builder);

	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();

	_update_vrs(rb);

	RENDER_TIMESTAMP("Setup 3D Scene");

	bool using_debug_mvs = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS;
	bool using_taa = rb->get_use_taa();

	Scale3DMode scale_type = _resolve_scale_3d_mode(rb);
	bool using_upscaling = scale_type != SCALE_3D_NONE;

	// Motion vectors are produced by the path tracer (rt_velocity_image)
	bool motion_vectors_required = using_debug_mvs || ce_needs_motion_vectors || using_taa || using_upscaling;

	p_render_data->scene_data->calculate_motion_vectors = motion_vectors_required;
	p_render_data->scene_data->directional_light_count = 0;
	p_render_data->scene_data->opaque_prepass_threshold = 0.99f;

	uint32_t color_pass_flags = 0;
	bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0;

	Size2i screen_size = rb->get_internal_size();

	if (p_render_data->scene_data->calculate_motion_vectors) {
		color_pass_flags |= COLOR_PASS_FLAG_MOTION_VECTORS;
		scene_shader.enable_advanced_shader_group();
		global_pipeline_data_required.use_motion_vectors = true;
	}

	// Free GPU resources for the screen-space effects the path tracer replaces.
	rb->clear_context(RB_SCOPE_SSIL);
	rb->clear_context(RB_SCOPE_SSAO);
	rb->clear_context(RB_SCOPE_SSR);

	if (p_render_data->scene_data->view_count > 1) {
		color_pass_flags |= COLOR_PASS_FLAG_MULTIVIEW;
		scene_shader.shader.enable_group(SceneShaderForwardClustered::SHADER_GROUP_MULTIVIEW);
		global_pipeline_data_required.use_multiview = true;
	}

	RID color_only_framebuffer = rb_data->get_color_only_fb();
	RendererRD::MaterialStorage::Samplers samplers = rb->get_samplers();

	p_render_data->scene_data->emissive_exposure_normalization = -1.0;

	RD::get_singleton()->draw_command_begin_label("Render Setup");

	_setup_lightmaps(p_render_data, *p_render_data->lightmaps, p_render_data->scene_data->cam_transform);

	p_render_data->scene_data->directional_light_count = _count_directional_lights(p_render_data);

	const uint32_t scene_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

	// May have changed due to the above (light buffer enlarged, as an example).
	_update_render_base_uniform_set();

	_age_out_motion_vectors(p_render_data);

	RENDER_TIMESTAMP("Fill Render Lists");

	// With a nonzero transparency layer budget, blend surfaces are path-traced
	// through the unified TLAS and the raster alpha overlay is skipped.
	// A zero budget restores the legacy raster overlay.
	const bool pt_transparency = rb_data.is_valid() && raytracing && raytracing->get_shader() &&
			(int)GLOBAL_GET("rendering/pathtracing/max_transparency_layers") > 0;

	if (pt_transparency) {
		// _fill_render_list is skipped, but the TLAS build still needs fresh
		// surface caches.
		_update_dirty_geometry_instances();
		render_list[RENDER_LIST_ALPHA].clear();
	} else {
		// TLAS is built from rt_instances and the path tracer writes velocity itself,
		// so this collapses to populating the ALPHA (transparent) list only.
		_fill_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_COLOR, false, false, false, false, true);

		int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : (int *)nullptr;

		render_list[RENDER_LIST_ALPHA].sort_by_reverse_depth_and_priority();
		_fill_instance_data(RENDER_LIST_ALPHA, render_info);
	}

	// RT pipeline flags (packed with sample count / max bounces). Computed once
	// here and reused at trace-dispatch time below so the uniform set and the
	// pipeline agree on spec-constant values.
	uint32_t rt_flags = SceneShaderRaytracing::RT_FLAG_NONE;
	// Captured from build_tlas/update_uniform_set so the trace-dispatch block
	// below can bind without RenderRaytracing keeping hidden "current" state.
	RID rt_uniform_set;
	RTViewportState *rt_state = nullptr;
	bool using_depth_reconstruct = false;
	// Set below and consumed by the sky block further down, which draws the real
	// sky shader into the RT sky texture before the trace dispatch.
	bool pt_sky_screen = false;

	const bool rt_available = rb_data.is_valid() && raytracing && raytracing->get_shader();
	if (rt_available) {
		RENDER_TIMESTAMP("Build Acceleration Structures");
		RID rt_environment = p_render_data->environment.is_valid() ? p_render_data->environment : RID();
		const bool fog_enabled = rt_environment.is_valid() && environment_get_fog_enabled(rt_environment);
		const bool volumetric_fog_enabled = rt_environment.is_valid() && environment_get_volumetric_fog_enabled(rt_environment);
		rt_flags = SceneShaderRaytracing::compute_rt_flags(rt_environment, fog_enabled, volumetric_fog_enabled);

		const bool dlss_rr_enabled = (rt_flags & SceneShaderRaytracing::RT_FLAG_DLSS_RR_ENABLED) != 0;
		if (dlss_rr_enabled) {
			raytracing->dlss_rr_ensure_buffers(rb.ptr());
			using_depth_reconstruct = true;
		} else if (raytracing->dlss_rr_has_buffers(rb.ptr())) {
			raytracing->dlss_rr_free_buffers(rb.ptr());
		}

		// The uniform set is built here, ahead of the sky block, so the sky texture
		// has to exist by now even though it is drawn into later this frame. Only
		// worth it when the sky is actually the background: everything else already
		// resolves correctly through the octmap.
		pt_sky_screen = rt_environment.is_valid() &&
				environment_get_pathtracing_sky_shader_enabled(rt_environment) &&
				!p_render_data->transparent_bg &&
				environment_get_background(rt_environment) == RSE::ENV_BG_SKY &&
				environment_get_sky(rt_environment).is_valid();
		if (pt_sky_screen) {
			raytracing->rt_ensure_sky_texture(rb.ptr());
			pt_sky_screen = raytracing->rt_has_sky_texture(rb.ptr());
		}

		_record_rt_captures(p_render_data, rt_flags, samplers, scene_uniform_buffer_index);
		rt_state = raytracing->build_tlas(p_render_data, rt_flags, rt_capture_requests);
		if (rt_state) {
			if (rt_flags & SceneShaderRaytracing::RT_FLAG_DECALS_ENABLED) {
				raytracing->build_decal_volumes(rt_state, p_render_data);
			}
			rt_state->sky_screen_enabled = pt_sky_screen;
			raytracing->update_scene_ubo(rt_state, p_render_data, screen_size, p_default_bg_color);
		}
	} else if (rb_data.is_valid() && raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
		// No RT shader available: free DLSS RR buffers so DLSS falls back to SR.
		raytracing->dlss_rr_free_buffers(rb.ptr());
	}

	RD::get_singleton()->draw_command_end_label();

	// Transparent geometry can still use lightmaps; make sure the matching shader
	// group and pipeline requirements are live before we (re)compile pipelines.
	if (scene_state.used_lightmap || scene_state.lightmaps_used > 0) {
		scene_shader.enable_advanced_shader_group(p_render_data->scene_data->view_count > 1);
		global_pipeline_data_required.use_lightmaps = true;
	}

	// Recompile pipelines if any of the requirements changed this frame.
	_update_dirty_geometry_pipelines();

	RID radiance_texture;
	bool draw_sky = false;
	bool draw_sky_fog_only = false;
	// We invert luminance_multiplier for sky so that we can combine it with exposure value.
	float sky_luminance_multiplier = 1.0 / rb->get_luminance_multiplier();
	float sky_brightness_multiplier = 1.0;

	Color clear_color;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1); //in overdraw mode, BG should always be black
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);
		RSE::EnvironmentReflectionSource reflection_source = environment_get_reflection_source(p_render_data->environment);

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		switch (bg_mode) {
			case RSE::ENV_BG_CLEAR_COLOR:
			case RSE::ENV_BG_COLOR: {
				clear_color = bg_mode == RSE::ENV_BG_CLEAR_COLOR ? p_default_bg_color : environment_get_bg_color(p_render_data->environment);

				if (!p_render_data->transparent_bg && (RendererRD::Fog::get_active(rb.ptr(), p_render_data->environment).is_valid() || environment_get_fog_enabled(p_render_data->environment))) {
					draw_sky_fog_only = true;
					RendererRD::MaterialStorage::get_singleton()->material_set_param(sky.sky_scene_state.fog_material, "clear_color", Variant(clear_color));
				}

				clear_color = clear_color.srgb_to_linear();
				clear_color.r *= bg_energy_multiplier;
				clear_color.g *= bg_energy_multiplier;
				clear_color.b *= bg_energy_multiplier;
			} break;
			case RSE::ENV_BG_SKY: {
				draw_sky = !p_render_data->transparent_bg;
			} break;
			case RSE::ENV_BG_CANVAS: {
				RID texture = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_texture(rb->get_render_target());
				bool convert_to_linear = !RendererRD::TextureStorage::get_singleton()->render_target_is_using_hdr(rb->get_render_target());
				copy_effects->copy_to_fb_rect(texture, color_only_framebuffer, Rect2i(), false, false, false, false, RID(), false, false, convert_to_linear);
			} break;
			case RSE::ENV_BG_KEEP: {
			} break;
			case RSE::ENV_BG_CAMERA_FEED: {
			} break;
			default: {
			}
		}

		// setup sky if used for ambient, reflections, or background
		if (draw_sky || draw_sky_fog_only || (reflection_source == RSE::ENV_REFLECTION_SOURCE_BG && bg_mode == RSE::ENV_BG_SKY) || reflection_source == RSE::ENV_REFLECTION_SOURCE_SKY || environment_get_ambient_source(p_render_data->environment) == RSE::ENV_AMBIENT_SOURCE_SKY) {
			RENDER_TIMESTAMP("Setup Sky");
			RD::get_singleton()->draw_command_begin_label("Setup Sky");

			// Setup our sky render information for this frame/viewport
			sky.setup_sky(p_render_data, screen_size);

			sky_brightness_multiplier *= bg_energy_multiplier;

			RID sky_rid = environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				sky.update_radiance_buffers(rb, p_render_data->environment, p_render_data->scene_data->cam_transform.origin, time, sky_luminance_multiplier, sky_brightness_multiplier);
				radiance_texture = sky.sky_get_radiance_texture_rd(sky_rid);
			} else {
				// do not try to draw sky if invalid
				draw_sky = false;
			}

			if (draw_sky || draw_sky_fog_only) {
				// update sky half/quarter res buffers (if required)
				sky.update_res_buffers(rb, p_render_data->environment, time, sky_luminance_multiplier, sky_brightness_multiplier);
			}

			RD::get_singleton()->draw_command_end_label();
		}

		if (bg_mode != RSE::ENV_BG_CLEAR_COLOR && bg_mode != RSE::ENV_BG_COLOR) {
			clear_color = clear_color.srgb_to_linear();
		}
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	// After this point clear_color has linear encoding.
	RSE::ViewportMSAA msaa = rb->get_msaa_3d();
	bool use_msaa = msaa != RSE::VIEWPORT_MSAA_DISABLED;

	bool ce_pre_opaque_wants_resolved = use_msaa &&
			(_compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE) ||
					_compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE));
	bool ce_post_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	SceneShaderForwardClustered::ShaderSpecialization base_specialization = scene_shader.default_specialization;
	base_specialization.use_depth_fog = p_render_data->environment.is_valid() && environment_get_fog_mode(p_render_data->environment) == RSE::EnvironmentFogMode::ENV_FOG_MODE_DEPTH;

	{
		if (ce_pre_opaque_wants_resolved) {
			// The path tracer hasn't produced color/depth yet at this point.
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved color/depth buffers when path tracing.");
		}

		RENDER_TIMESTAMP("Process Pre Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, p_render_data);
	}

	// Sky (sun disk), volumetric fog and the remaining raster passes still read
	// the light UBOs and cluster grid, so this runs even with pathtraced transparency.
	{
		p_render_data->shadows_from_ray_queries = rt_state != nullptr;
		// Ray-queried fog shadows stand in for path-traced media; the shadow map fallback needs the raster toggle.
		p_render_data->shadow_renderer = rt_state != nullptr ? RSE::SHADOW_RENDERER_PATH_TRACER : RSE::SHADOW_RENDERER_RASTERIZER;
		uint32_t pt_directional_light_count = 0;
		uint32_t pt_positional_light_count = 0;
		// Pathtraced transparency leaves no raster surface to receive decals.
		_setup_lights_cluster_decals(p_render_data, pt_directional_light_count, pt_positional_light_count, !pt_transparency);

		if (rt_available) {
			// The fog and the trace read what the batch built. Recorded this late so the work above can overlap the builds.
			RD::get_singleton()->untracked_barrier();
		}
		RENDER_TIMESTAMP("Update Volumetric Fog");
		_update_pt_volumetric_fog(p_render_data, rt_state, pt_directional_light_count, pt_positional_light_count);
	}

	// Built after the fog update, which the trace samples.
	if (rt_state) {
		rt_uniform_set = raytracing->update_uniform_set(rt_state, p_render_data, rt_flags);
	}

	// Run the sky shader per pixel into its own target. The trace below reads
	// it for primary-ray misses, so the background matches the raster sky
	// exactly (and stays inside the denoiser's input, unlike a post-trace
	// composite). Indirect rays keep using the octmap.
	if (pt_sky_screen && draw_sky) {
		RID sky_fb = raytracing->rt_get_sky_framebuffer(rb.ptr());
		if (sky_fb.is_valid()) {
			RENDER_TIMESTAMP("Render Sky (Pathtraced Primary Rays)");
			RD::get_singleton()->draw_command_begin_label("Draw Sky (Pathtraced Primary Rays)");
			RD::DrawListID sky_draw_list = RD::get_singleton()->draw_list_begin(sky_fb, RD::DRAW_IGNORE_COLOR_ALL);
			sky.draw_sky(sky_draw_list, rb, p_render_data->environment, sky_fb, time, sky_luminance_multiplier, sky_brightness_multiplier);
			RD::get_singleton()->draw_list_end();
			RD::get_singleton()->draw_command_end_label();
		}
	}

	// Execute raytracing (replaces the opaque + motion vector pass). Without a built TLAS and its uniform set there is nothing valid to trace.
	bool trace = rb_data.is_valid() && raytracing && raytracing->get_shader() && rt_state && rt_uniform_set.is_valid();
#ifdef RT_VALIDATION_ENABLED
	trace = trace && raytracing->rtv_validate_trace(rt_state);
#endif
	if (trace) {
		RD::get_singleton()->draw_command_begin_label("Raytracing");
		RENDER_TIMESTAMP("TLAS Build");

		// Ensure raytracing output textures exist.
		raytracing->rt_ensure_textures(rb.ptr());

		RENDER_TIMESTAMP("Pathtracer");

		// rt_flags was computed at TLAS-build time above and matches what
		// update_uniform_set used, so pipeline + uniform set stay in sync.
		RID rt_pipeline = raytracing->get_shader()->get_raytracing_pipeline(rt_flags);
		if (!rt_pipeline.is_valid()) {
			ERR_PRINT_ONCE_ED("Failed to get raytracing pipeline. Aborting render.");
			RD::get_singleton()->draw_command_end_label();
			return;
		}
		RD::RaytracingListID raytracing_list = RD::get_singleton()->raytracing_list_begin();
		RD::get_singleton()->raytracing_list_bind_raytracing_pipeline(raytracing_list, rt_pipeline);
		RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, rt_uniform_set, 0);

		// Bindless set 1 can be unset when no bindless textures are live
		RID bindless_set = raytracing->get_bindless_uniform_set();
		if (bindless_set.is_valid()) {
			RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, bindless_set, 1);
		}

		// Raytracing dispatches at internal (pre-upscale) size
		Size2i rt_size = rb->get_internal_size();
		RD::get_singleton()->raytracing_list_trace_rays(raytracing_list, 0, raytracing->get_shader()->get_hit_sbt(rt_flags), rt_size.width, rt_size.height, 1);
		RD::get_singleton()->raytracing_list_end();

		RD::get_singleton()->draw_command_end_label();

		// Copy RT depth (R32F storage image) to D32F depth buffer after tracing.
		if (rb_data.is_valid() && raytracing->rt_has_depth_texture(rb.ptr())) {
			RENDER_TIMESTAMP("Copy RT Depth (R32F -> D32F)");
			RD::get_singleton()->draw_command_begin_label("Copy RT Depth");
			copy_effects->copy_r32f_to_depth_fb(raytracing->rt_get_depth_texture(rb.ptr()), rb_data->get_depth_fb(), Rect2i(0, 0, rb->get_internal_size().x, rb->get_internal_size().y));
			RD::get_singleton()->draw_command_end_label();
		}

		raytracing->copy_output_texture(p_render_data);
	}

	if (rt_state) {
		// The captures and merges of later views, and other systems in later frames (particles, MultiMeshes, mesh
		// updates), rewrite buffers the fog and the trace read through device addresses.
		RD::get_singleton()->untracked_barrier();
	}

	{
		// RT output is already in internal_texture, so the color MSAA resolve is
		// skipped; only depth is resolved for compositor effects that need it.
		if (ce_post_opaque_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}

		RENDER_TIMESTAMP("Process Post Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, p_render_data);
	}

	if (use_msaa) {
		RENDER_TIMESTAMP("Resolve MSAA");

		// RT writes internal_texture, so only depth is resolved here.
		if (scene_state.used_depth_texture || scene_state.used_normal_texture || ce_needs_normal_roughness || ce_pre_transparent_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}
	}

	{
		RENDER_TIMESTAMP("Process Post Sky Compositor Effects");
		// Don't need to check for depth or color resolve here, we've already triggered it.
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY, p_render_data);
	}

	if (using_upscaling) {
		// Make sure the upscaled texture is initialized, but not necessarily filled, before running screen copies
		// so it properly detect if a dedicated copy texture should be used.
		rb->ensure_upscaled();
	}

	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("Copy Screen Texture");

		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			// Copy screen texture to backbuffer so we can read from it
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			// Copy depth texture to backbuffer so we can read from it
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	{
		RENDER_TIMESTAMP("Process Pre Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT, p_render_data);
	}

	if (!pt_transparency) {
		RENDER_TIMESTAMP("Render 3D Transparent Pass");

		RD::get_singleton()->draw_command_begin_label("Render 3D Transparent Pass");

		uint32_t transparent_pass_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

		RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, radiance_texture, samplers, transparent_pass_uniform_buffer_index, true);

		{
			uint32_t transparent_color_pass_flags = (color_pass_flags | uint32_t(COLOR_PASS_FLAG_TRANSPARENT)) & ~uint32_t(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
			// Motion vectors should not be overwritten by transparent objects.
			transparent_color_pass_flags &= ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS);

			RID alpha_framebuffer = rb_data->get_color_pass_fb(transparent_color_pass_flags);
			RenderListParameters render_list_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), reverse_cull, PASS_MODE_COLOR, transparent_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization);
			_render_list_with_draw_list(&render_list_params, alpha_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
		}

		RD::get_singleton()->draw_command_end_label();
	}

	RENDER_TIMESTAMP("Resolve");

	RD::get_singleton()->draw_command_begin_label("Resolve");

	if (use_msaa) {
		// RT writes internal_texture and the path tracer writes velocity itself, so
		// only depth needs resolving.
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
		}
	}

	RD::get_singleton()->draw_command_end_label();

	{
		RENDER_TIMESTAMP("Process Post Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, p_render_data);
	}

	rb->set_depth_reconstruct_requested(using_depth_reconstruct);

	if (using_upscaling || using_taa) {
		DLSSRRGuideBuffers dlss_rr_guides;
		if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
			dlss_rr_guides.active = true;
			dlss_rr_guides.diffuse_albedo = raytracing->dlss_rr_get_diffuse_albedo(rb.ptr());
			dlss_rr_guides.specular_albedo = raytracing->dlss_rr_get_specular_albedo(rb.ptr());
			dlss_rr_guides.normal_roughness = raytracing->dlss_rr_get_normal_roughness(rb.ptr());
			dlss_rr_guides.specular_motion_vectors = raytracing->dlss_rr_get_specular_motion_vectors(rb.ptr());
		}
		_render_3d_upscaling(p_render_data, scale_type, using_taa, time_step, dlss_rr_guides);
	}

	_debug_draw_cluster(rb);

	RENDER_TIMESTAMP("Tonemap");

	_render_buffers_post_process_and_tonemap(p_render_data);

	if ((rt_flags & SceneShaderRaytracing::RT_FLAG_TIMING_ENABLED) && rb->has_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING)) {
		// Overlay after denoising, exposure and tone mapping, keeping their inputs and history unchanged.
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		RID render_target = rb->get_render_target();
		copy_effects->copy_timing_heatmap_to_fb(rb->get_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING),
				texture_storage->render_target_get_rd_framebuffer(render_target),
				Rect2i(Point2i(), texture_storage->render_target_get_size(render_target)),
				GLOBAL_GET("rendering/pathtracing/debug_heatmap_max_ticks"),
				GLOBAL_GET("rendering/pathtracing/debug_heatmap_opacity"),
				texture_storage->render_target_is_using_hdr(render_target));
	}

	_render_buffers_debug_draw(p_render_data);
}

// Raytracing methods

BitField<RSE::RenderingNotReadyReason> RenderForwardClusteredPT::get_not_ready_reasons() {
	BitField<RSE::RenderingNotReadyReason> reasons = RenderForwardClustered::get_not_ready_reasons();
	if (raytracing) {
		reasons.set_flag(raytracing->get_not_ready_reasons());
	}
	return reasons;
}

bool RenderForwardClusteredPT::pathtracing_is_used(RID p_env, RID p_viewport) const {
	return p_env.is_valid() && environment_get_pathtracing_enabled(p_env) &&
			(p_viewport.is_null() || RSG::viewport->viewport_get_use_pathtracing(p_viewport)) &&
			RD::get_singleton()->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE);
}

bool RenderForwardClusteredPT::_setup_rt() {
	if (!RD::get_singleton()->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE)) {
		WARN_PRINT_ONCE("Raytracing not supported on this device.");
		return false;
	}

	if (!raytracing) {
		raytracing = memnew(RenderRaytracing);
		raytracing->initialize(this);
		raytracing->shader = SceneShaderRaytracing::get_singleton();
		String rt_defines;
		rt_defines += "\n#define RT 1\n";
		rt_defines += "\n#define MAX_ROUGHNESS_LOD " + itos(get_roughness_layers() - 1) + ".0\n";
		rt_defines += "\n#define RT_PARAM_COUNT " + itos(SceneShaderRaytracing::RT_PARAM_COUNT) + "\n";
		if (is_using_radiance_octmap_array()) {
			rt_defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY \n";
		}
		raytracing->shader->init(rt_defines);
	}

	if (!fog_shadows) {
		fog_shadows = memnew(RTFogShadows);
		fog_shadows->initialize();
	}

	return true;
}

void RenderForwardClusteredPT::_record_rt_captures(const RenderDataRD *p_render_data, uint32_t p_rt_flags, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	raytracing->prepare_vertex_captures(p_render_data, p_rt_flags, rt_capture_requests);
	_update_rt_capture_preloads();
	if (rt_capture_requests.is_empty()) {
		return;
	}

	RENDER_TIMESTAMP("RT Vertex Capture");
	RD::get_singleton()->draw_command_begin_label("RT Vertex Capture");

	// The instance data index of request i is i: it selects the request's draw parameters in the capture shader.
	RenderList &capture_list = render_list[RENDER_LIST_SECONDARY];
	capture_list.clear();
	for (const RTCaptureRequest &request : rt_capture_requests) {
		capture_list.add_element(const_cast<GeometryInstanceSurfaceDataCache *>(static_cast<const GeometryInstanceSurfaceDataCache *>(request.surf)));
	}
	_fill_instance_data(RENDER_LIST_SECONDARY);
	RID render_pass_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, p_render_data, RID(), p_samplers, p_uniform_buffer_index);

	if (rt_capture_framebuffer.is_null()) {
		rt_capture_framebuffer = RD::get_singleton()->framebuffer_create_empty(Size2i(1, 1), RD::TEXTURE_SAMPLES_1, RD::get_singleton()->framebuffer_format_create_empty());
		RD::get_singleton()->set_resource_name(rt_capture_framebuffer, "RT Vertex Capture Framebuffer");
	}

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(rt_capture_framebuffer);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, render_base_uniform_set, SCENE_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
	for (uint32_t i = 0; i < rt_capture_requests.size(); i++) {
		rt_capture_requests[i].recorded = _record_rt_capture(draw_list, i);
	}
	RD::get_singleton()->draw_list_end();
	RD::get_singleton()->draw_command_end_label();
}

bool RenderForwardClusteredPT::_rt_capture_variant_ready(const SceneShaderForwardClustered::ShaderData *p_shader) {
	return p_shader->version.is_valid() && scene_shader.shader.version_is_group_compiled(p_shader->version, SceneShaderForwardClustered::SHADER_GROUP_RT_CAPTURE);
}

// Blocks until the capture variant is compiled; call only once _rt_capture_variant_ready() returned true.
uint64_t RenderForwardClusteredPT::_rt_capture_input_mask(SceneShaderForwardClustered::ShaderData *p_shader) {
	return p_shader->get_vertex_input_mask(SceneShaderForwardClustered::PIPELINE_VERSION_RT_CAPTURE, 0, false);
}

// Key of the capture pipeline of a surface for p_vertex_format; preloads and draws must agree on it.
// Instanced surfaces need the raster draw's multimesh specialization to read their instance records.
SceneShaderForwardClustered::ShaderData::PipelineKey RenderForwardClusteredPT::_rt_capture_pipeline_key(const GeometryInstanceSurfaceDataCache *p_surface_cache, RD::VertexFormatID p_vertex_format) const {
	const uint32_t base_flags = p_surface_cache->owner->base_flags;
	SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
	pipeline_key.vertex_format_id = p_vertex_format;
	pipeline_key.framebuffer_format_id = RD::get_singleton()->framebuffer_format_create_empty();
	pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
	pipeline_key.primitive_type = RSE::PRIMITIVE_POINTS;
	pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_RT_CAPTURE;
	pipeline_key.shader_specialization = scene_shader.default_specialization;
	pipeline_key.shader_specialization.multimesh = bool(base_flags & INSTANCE_DATA_FLAG_MULTIMESH);
	pipeline_key.shader_specialization.multimesh_format_2d = bool(base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D);
	pipeline_key.shader_specialization.multimesh_has_color = bool(base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR);
	pipeline_key.shader_specialization.multimesh_has_custom_data = bool(base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA);
	return pipeline_key;
}

// Queues the capture pipeline of a surface captured under p_mode. False when the capture variant isn't compiled yet.
bool RenderForwardClusteredPT::_preload_rt_capture_pipeline(const GeometryInstanceSurfaceDataCache *p_surface_cache, RTVertexCapture::Mode p_mode) {
	if (!RenderRaytracing::is_capture_surface(p_surface_cache, p_mode)) {
		return true;
	}
	SceneShaderForwardClustered::ShaderData *shader = p_surface_cache->shader;
	if (!_rt_capture_variant_ready(shader)) {
		return false;
	}
	const uint64_t input_mask = _rt_capture_input_mask(shader);
	const RD::VertexFormatID vertex_format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_vertex_format(
			p_surface_cache->surface, input_mask, p_surface_cache->owner->mesh_instance.is_valid(), false, false);
	const SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key = _rt_capture_pipeline_key(p_surface_cache, vertex_format);
	shader->pipeline_hash_map.compile_pipeline(pipeline_key, pipeline_key.hash(), RSE::PIPELINE_SOURCE_SURFACE, false);
	return true;
}

// Queues capture pipelines of every surface once per capture mode, retrying while capture variants still compile.
void RenderForwardClusteredPT::_update_rt_capture_preloads() {
	const uint32_t mode_version = raytracing->get_capture_mode_version();
	if (mode_version == rt_capture_preload_version) {
		return;
	}
	const RTVertexCapture::Mode mode = RenderRaytracing::read_capture_mode();
	if (mode == RTVertexCapture::MODE_DISABLED || !RD::get_singleton()->has_feature(RD::SUPPORTS_VERTEX_PIPELINE_STORES)) {
		rt_capture_preload_version = mode_version;
		return;
	}
	scene_shader.shader.enable_group(SceneShaderForwardClustered::SHADER_GROUP_RT_CAPTURE);
	bool complete = true;
	for (SelfList<GeometryInstanceSurfaceDataCache> *element = geometry_surface_compilation_all_list.first(); element; element = element->next()) {
		complete = _preload_rt_capture_pipeline(element->self(), mode) && complete;
	}
	if (complete) {
		rt_capture_preload_version = mode_version;
	}
}

void RenderForwardClusteredPT::_mesh_generate_extra_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache) {
	if (rt_capture_preload_version != 0) {
		_preload_rt_capture_pipeline(p_surface_cache, RenderRaytracing::read_capture_mode());
	}
}

// Records the capture draw of request p_element: a non-indexed point draw of every vertex of every instance.
// False while its shader variant or pipeline is still compiling; the surface is then skipped for this frame.
bool RenderForwardClusteredPT::_record_rt_capture(RD::DrawListID p_draw_list, uint32_t p_element) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const RTCaptureRequest &request = rt_capture_requests[p_element];
	const GeometryInstanceSurfaceDataCache *surf = static_cast<const GeometryInstanceSurfaceDataCache *>(request.surf);
	SceneShaderForwardClustered::ShaderData *shader = surf->shader;
	const SceneShaderForwardClustered::PipelineVersion version = SceneShaderForwardClustered::PIPELINE_VERSION_RT_CAPTURE;
	if (!_rt_capture_variant_ready(shader)) {
		return false;
	}

	const uint64_t input_mask = _rt_capture_input_mask(shader);
	RID vertex_array;
	RD::VertexFormatID vertex_format = 0;
	if (surf->owner->mesh_instance.is_valid()) {
		mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(surf->owner->mesh_instance, surf->surface_index, input_mask, false, false, vertex_array, vertex_format);
	} else {
		mesh_storage->mesh_surface_get_vertex_arrays_and_format(surf->surface, input_mask, false, false, vertex_array, vertex_format);
	}

	const SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key = _rt_capture_pipeline_key(surf, vertex_format);
	const RID pipeline = shader->pipeline_hash_map.get_pipeline(pipeline_key, pipeline_key.hash(), false, RSE::PIPELINE_SOURCE_SPECIALIZATION);
	if (pipeline.is_null()) {
		return false;
	}

	RD *rd = RD::get_singleton();
	rd->draw_list_bind_render_pipeline(p_draw_list, pipeline);
	rd->draw_list_bind_vertex_array(p_draw_list, vertex_array);
	rd->draw_list_bind_uniform_set(p_draw_list, surf->owner->transforms_uniform_set.is_valid() ? surf->owner->transforms_uniform_set : scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);
	if (surf->material_uniform_set.is_valid() && rd->uniform_set_is_valid(surf->material_uniform_set)) {
		rd->draw_list_bind_uniform_set(p_draw_list, surf->material_uniform_set, MATERIAL_UNIFORM_SET);
	}
	const RID capture_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader->get_shader_variant(version, 0, false), RT_CAPTURE_UNIFORM_SET,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, raytracing->get_capture_params_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, raytracing->get_capture_arena()));
	rd->draw_list_bind_uniform_set(p_draw_list, capture_uniform_set, RT_CAPTURE_UNIFORM_SET);

	SceneState::PushConstant push_constant = {};
	push_constant.base_index = p_element;
	if (surf->owner->base_flags & INSTANCE_DATA_FLAG_PARTICLES) {
		RendererRD::ParticlesStorage::get_singleton()->particles_get_instance_buffer_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
	} else if (surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) {
		mesh_storage->_multimesh_get_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
	}
	rd->draw_list_set_push_constant(p_draw_list, &push_constant, sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader));
	rd->draw_list_draw(p_draw_list, false, request.layout.instance_count, request.layout.vertex_count);
	return true;
}

void RenderForwardClusteredPT::_update_pt_volumetric_fog(RenderDataRD *p_render_data, RTViewportState *p_rt_state, uint32_t p_directional_light_count, uint32_t p_positional_light_count) {
	// Like raster, keep the froxels when fog is off: a shared viewport without fog renders
	// into the same buffers, and freeing them would reset the temporal history every frame.
	if (p_render_data->environment.is_null() || !environment_get_volumetric_fog_enabled(p_render_data->environment)) {
		return;
	}

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;

	// Shadow maps aren't rendered when path tracing, so there is no raster fallback.
	RendererRD::Fog::VolumetricFogRayQueryShadows ray_query_shadows;
	if (!fog_shadows->prepare(raytracing, p_rt_state, p_render_data, ray_query_shadows)) {
		rb->set_custom_data(RB_SCOPE_FOG, Ref<RenderBufferCustomDataRD>());
		return;
	}

	gi.setup_voxel_gi_instances(p_render_data, rb, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances, p_render_data->voxel_gi_count);

	_update_volumetric_fog(rb, p_render_data->environment, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform, p_render_data->scene_data->prev_cam_transform.affine_inverse(), RID(), p_directional_light_count, false, p_positional_light_count, p_render_data->voxel_gi_count, *p_render_data->fog_volumes, &ray_query_shadows);
}

void RenderForwardClusteredPT::_age_out_motion_vectors(const RenderDataRD *p_render_data) {
	if (!p_render_data) {
		return;
	}
	const uint64_t frame = RSG::rasterizer->get_frame_number();

	// Process RT-only instances
	if (p_render_data->rt_instances) {
		for (uint32_t i = 0; i < (uint32_t)p_render_data->rt_instances->size(); i++) {
			GeometryInstanceForwardClustered *inst =
					static_cast<GeometryInstanceForwardClustered *>((*p_render_data->rt_instances)[i]);
			if (inst) {
				inst->age_out_motion(frame);
			}
		}
	}

	// Process raster instances
	if (p_render_data->instances) {
		for (uint32_t i = 0; i < (uint32_t)p_render_data->instances->size(); i++) {
			GeometryInstanceForwardClustered *inst =
					static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);
			if (inst) {
				inst->age_out_motion(frame);
			}
		}
	}
}

void RenderForwardClusteredPT::_free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);
	p_render_buffers->clear_context(RB_SCOPE_DLSS_RR);
	if (raytracing) {
		raytracing->free_viewport_state(p_render_buffers);
	}
}

void RenderForwardClusteredPT::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RenderForwardClustered::_render_buffers_debug_draw(p_render_data);

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RID render_target = rb->get_render_target();

	// DLSS Ray Reconstruction debug views
	if (raytracing && raytracing->dlss_rr_has_buffers(rb.ptr())) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID fb = texture_storage->render_target_get_rd_framebuffer(render_target);

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_DIFFUSE_ALBEDO) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_diffuse_albedo(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_SPECULAR_ALBEDO) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_specular_albedo(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_NORMAL_ROUGHNESS) {
			copy_effects->copy_to_fb_rect(raytracing->dlss_rr_get_normal_roughness(rb.ptr()), fb, Rect2(Vector2(), rtsize), false, false);
		}

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DLSS_RR_SPECULAR_MOTION_VECTORS) {
			const RenderSceneDataRD *scene_data = p_render_data->scene_data;
			debug_effects->draw_motion_vectors(raytracing->dlss_rr_get_specular_motion_vectors(rb.ptr()), rb->get_depth_texture(), fb,
					scene_data->cam_projection, scene_data->cam_transform, scene_data->prev_cam_projection, scene_data->prev_cam_transform, rb->get_internal_size(),
					_render_buffers_get_velocity_texture(rb));
		}
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_RECONSTRUCTED_DEPTH && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_RECONSTRUCTED_DEPTH)) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID fb = texture_storage->render_target_get_rd_framebuffer(render_target);
		RID depth_tex = rb->get_texture(RB_SCOPE_BUFFERS, RB_TEX_RECONSTRUCTED_DEPTH);
		copy_effects->copy_to_fb_rect(depth_tex, fb, Rect2(Vector2(), rtsize), false, true, false, false, RID(), false, false, false, false, Rect2(), 1.0, true, RendererRD::CopyEffects::COPY_TO_FB_FLAG_MODE_LOG_LUMINANCE);
	}
}

RenderForwardClusteredPT::RenderForwardClusteredPT() {
}

RenderForwardClusteredPT::~RenderForwardClusteredPT() {
	if (rt_capture_framebuffer.is_valid()) {
		RD::get_singleton()->free_rid(rt_capture_framebuffer);
	}
	if (fog_shadows) {
		memdelete(fog_shadows);
	}
	if (raytracing) {
		memdelete(raytracing);
	}
}
