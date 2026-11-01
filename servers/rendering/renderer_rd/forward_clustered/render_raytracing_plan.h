/**************************************************************************/
/*  render_raytracing_plan.h                                              */
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

#include "core/math/aabb.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/span.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_capture.h"
#include "servers/rendering/rendering_billboard.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererSceneRenderImplementation {

// Surfaces with equal keys can share one multi-geometry BLAS.
struct RTSurfaceGroupKey {
	RSE::CullMode cull_mode = RSE::CULL_MODE_BACK;
	uint32_t sbt_offset = 0;
	uint8_t tlas_mask = 0;
	RenderingBillboard::Settings billboard;
	bool opaque = true; // Not part of the key: opacity is per geometry in merged BLASes.
	bool groupable = false;

	bool operator==(const RTSurfaceGroupKey &p_other) const {
		return groupable && p_other.groupable && cull_mode == p_other.cull_mode &&
				sbt_offset == p_other.sbt_offset && tlas_mask == p_other.tlas_mask && billboard == p_other.billboard;
	}
};

struct RTPlanVersions {
	uint64_t material_version = 0;
	uint64_t mesh_version = 0;
	uint32_t ready_version = 0;
	uint32_t capture_mode_version = 0;
};

struct RTPlanKey {
	uint64_t surfaces_version = 0; // Unique, so it also identifies the instance.
	uint8_t mask_opaque = 0;
	uint8_t mask_shadow = 0;
	uint8_t transparency = 0;
	bool camera_visible = false;
};
static_assert(sizeof(RTPlanKey) == 16, "RTPlanKey must stay 16 bytes to keep RTPlanInstance at 32.");

enum RTPlanKeyMatch : uint8_t {
	RT_PLAN_KEY_MATCH,
	RT_PLAN_KEY_GLOBAL_ONLY, // Reuse after checking each surface's invalidation counters.
	RT_PLAN_KEY_MISMATCH,
};

enum RTPlanInstanceFlags : uint8_t {
	RT_PLAN_INSTANCE_VALID = 1 << 0,
	RT_PLAN_INSTANCE_HIT_GROUPS_READY = 1 << 1,
	RT_PLAN_INSTANCE_DYNAMIC = 1 << 2,
	RT_PLAN_INSTANCE_TLAS_STALE = 1 << 3,
	RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES = 1 << 4,
};

// Groups, surfaces and captures of one instance share one reserved range.
struct RTPlanInstance {
	RTPlanKey key;
	uint32_t range_begin = 0;
	uint16_t range_capacity = 0;
	uint16_t group_count = 0;
	uint16_t surface_count = 0;
	uint16_t capture_count = 0;
	uint16_t pending_surfaces = 0; // Left out until their hit group or capture is ready.
	uint8_t flags = 0;
};
static_assert(sizeof(RTPlanInstance) == 32, "RTPlanInstance must stay compact.");

enum RTPlanDeformedKind : uint8_t {
	RT_PLAN_DEFORMED_NONE,
	RT_PLAN_DEFORMED_MESH_INSTANCE,
	RT_PLAN_DEFORMED_CAPTURED,
};

enum RTPlanSurfaceFlags : uint8_t {
	RT_PLAN_SURFACE_TRANSPARENT = 1 << 0,
};

// Members of one group are contiguous and in surface order.
struct RTPlanSurface {
	const void *surf = nullptr; // GeometryInstanceSurfaceDataCache; only read on a BLAS miss or for captures.
	uint32_t material_slot = UINT32_MAX; // UINT32_MAX is the default material.
	uint32_t cache_key = 0;
	uint32_t mesh_rid_version = 0;
	uint32_t surface_counter = 0;
	uint32_t material_flags = 0;
	uint16_t material_counter = 0;
	uint8_t surface_index = 0;
	uint8_t deformed_kind = RT_PLAN_DEFORMED_NONE;
	uint8_t flags = 0;
};
static_assert(sizeof(RTPlanSurface) <= 48, "RTPlanSurface must stay compact.");

struct RTPlanGroup {
	RTSurfaceGroupKey key;
	uint64_t surface_mask = 0;
	uint64_t opaque_mask = 0;
	uint16_t surface_begin = 0; // Relative to the instance's range_begin.
	uint16_t surface_count = 0;
	bool transparent = false;
};
static_assert(sizeof(RTPlanGroup) <= 64, "RTPlanGroup must stay within a cache line.");

enum RTPlanCaptureFlags : uint8_t {
	RT_PLAN_CAPTURE_MERGED = 1 << 0,
	RT_PLAN_CAPTURE_CAMERA_DEPENDENT = 1 << 1,
	RT_PLAN_CAPTURE_EVALUATED = 1 << 2,
	RT_PLAN_CAPTURE_IN_FRUSTUM = 1 << 3,
	RT_PLAN_CAPTURE_IDENTITY_FLAGS = RT_PLAN_CAPTURE_MERGED | RT_PLAN_CAPTURE_CAMERA_DEPENDENT,
};

struct RTPlanCaptureEval {
	RTVertexCapture::Stamps inputs;
	RTVertexCapture::Recapture recapture = RTVertexCapture::RECAPTURE_REBUILD;
};

struct RTPlanCapture {
	const void *surf = nullptr;
	RID entry;
	RTVertexCapture::Layout layout;
	uint32_t dependencies = 0;
	uint32_t triangle_count = 0;
	uint32_t surface_key = 0;
	uint8_t flags = 0;
};
static_assert(sizeof(RTPlanCapture) <= 64, "RTPlanCapture must stay within a cache line.");

// One record per instance of a view, in rt_instances order.
struct RTPlanArena {
	RTPlanVersions versions; // Kept by clear().
	LocalVector<RTPlanInstance> instances;
	LocalVector<RTPlanGroup> groups;
	LocalVector<RTPlanSurface> surfaces;
	LocalVector<RTPlanCapture> captures;
	LocalVector<AABB> capture_aabbs; // Parallel to captures.
	LocalVector<RTPlanCaptureEval> capture_evals; // Parallel to captures; valid with RT_PLAN_CAPTURE_EVALUATED.

	void clear() {
		instances.clear();
		groups.clear();
		surfaces.clear();
		captures.clear();
		capture_aabbs.clear();
		capture_evals.clear();
	}
};

void rt_plan_reserve(RTPlanArena &r_arena, Span<uint32_t> p_capacities);
// Leaves the destination untouched and returns false when the counts exceed its range_capacity.
bool rt_plan_copy_forward(const RTPlanArena &p_src, uint32_t p_src_index, RTPlanArena &r_dst, uint32_t p_dst_index);
RTPlanKeyMatch rt_plan_key_match(const RTPlanKey &p_cached, const RTPlanVersions &p_cached_versions, const RTPlanKey &p_current, const RTPlanVersions &p_current_versions);
bool rt_plan_lookup(Span<uint32_t> p_lookup, const RTPlanArena &p_prev, uint32_t p_rt_index, uint64_t p_surfaces_version, uint32_t &r_prev_index);
void rt_plan_set_lookup(LocalVector<uint32_t> &r_lookup, uint32_t p_rt_index, uint32_t p_index);

} // namespace RendererSceneRenderImplementation
