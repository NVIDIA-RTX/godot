/**************************************************************************/
/*  render_raytracing_plan.cpp                                            */
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

#include "render_raytracing_plan.h"

#include "core/variant/variant.h"

#include <cstring>
#include <type_traits>

namespace RendererSceneRenderImplementation {

template <typename T>
static void _rt_plan_copy_range(const LocalVector<T> &p_src, uint32_t p_src_begin, LocalVector<T> &r_dst, uint32_t p_dst_begin, uint32_t p_count) {
	static_assert(std::is_trivially_copyable_v<T>, "Plan ranges are copied with memcpy.");
	if (p_count == 0) {
		return;
	}
	CRASH_COND(p_src_begin + p_count > p_src.size() || p_dst_begin + p_count > r_dst.size());
	memcpy(r_dst.ptr() + p_dst_begin, p_src.ptr() + p_src_begin, size_t(p_count) * sizeof(T));
}

void rt_plan_reserve(RTPlanArena &r_arena, Span<uint32_t> p_capacities) {
	r_arena.clear();
	const uint32_t count = uint32_t(p_capacities.size());
	r_arena.instances.resize(count);
	uint32_t total = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t capacity = p_capacities[i];
		if (unlikely(capacity > UINT16_MAX)) {
			ERR_PRINT_ONCE(vformat("RT plan capacity %d exceeds %d elements per instance; clamping.", capacity, UINT16_MAX));
			capacity = UINT16_MAX;
		}
		RTPlanInstance &record = r_arena.instances[i];
		record.range_begin = total;
		record.range_capacity = uint16_t(capacity);
		total += capacity;
	}
	// Records only read their ranges up to the counts they wrote, so the ranges skip initialization.
	r_arena.groups.resize_uninitialized(total);
	r_arena.surfaces.resize_uninitialized(total);
	r_arena.captures.resize_uninitialized(total);
	r_arena.capture_aabbs.resize_uninitialized(total);
	r_arena.capture_evals.resize_uninitialized(total);
}

bool rt_plan_copy_forward(const RTPlanArena &p_src, uint32_t p_src_index, RTPlanArena &r_dst, uint32_t p_dst_index) {
	const RTPlanInstance &src = p_src.instances[p_src_index];
	RTPlanInstance &dst = r_dst.instances[p_dst_index];
	if (src.group_count > dst.range_capacity || src.surface_count > dst.range_capacity || src.capture_count > dst.range_capacity) {
		return false;
	}

	dst.key = src.key;
	dst.group_count = src.group_count;
	dst.surface_count = src.surface_count;
	dst.capture_count = src.capture_count;
	dst.pending_surfaces = src.pending_surfaces;
	dst.flags = src.flags;

	_rt_plan_copy_range(p_src.groups, src.range_begin, r_dst.groups, dst.range_begin, src.group_count);
	_rt_plan_copy_range(p_src.surfaces, src.range_begin, r_dst.surfaces, dst.range_begin, src.surface_count);
	_rt_plan_copy_range(p_src.captures, src.range_begin, r_dst.captures, dst.range_begin, src.capture_count);
	_rt_plan_copy_range(p_src.capture_aabbs, src.range_begin, r_dst.capture_aabbs, dst.range_begin, src.capture_count);
	_rt_plan_copy_range(p_src.capture_evals, src.range_begin, r_dst.capture_evals, dst.range_begin, src.capture_count);
	return true;
}

RTPlanKeyMatch rt_plan_key_match(const RTPlanKey &p_cached, const RTPlanVersions &p_cached_versions, const RTPlanKey &p_current, const RTPlanVersions &p_current_versions) {
	if (p_cached.surfaces_version != p_current.surfaces_version ||
			p_cached.mask_opaque != p_current.mask_opaque ||
			p_cached.mask_shadow != p_current.mask_shadow ||
			p_cached.transparency != p_current.transparency ||
			p_cached.camera_visible != p_current.camera_visible ||
			p_cached_versions.ready_version != p_current_versions.ready_version ||
			p_cached_versions.capture_mode_version != p_current_versions.capture_mode_version) {
		return RT_PLAN_KEY_MISMATCH;
	}
	if (p_cached_versions.material_version != p_current_versions.material_version || p_cached_versions.mesh_version != p_current_versions.mesh_version) {
		return RT_PLAN_KEY_GLOBAL_ONLY;
	}
	return RT_PLAN_KEY_MATCH;
}

bool rt_plan_lookup(Span<uint32_t> p_lookup, const RTPlanArena &p_prev, uint32_t p_rt_index, uint64_t p_surfaces_version, uint32_t &r_prev_index) {
	if (p_rt_index >= p_lookup.size()) {
		return false;
	}
	const uint32_t index = p_lookup[p_rt_index];
	if (index >= p_prev.instances.size()) {
		return false;
	}
	const RTPlanInstance &record = p_prev.instances[index];
	if (!(record.flags & RT_PLAN_INSTANCE_VALID) || record.key.surfaces_version != p_surfaces_version) {
		return false;
	}
	r_prev_index = index;
	return true;
}

void rt_plan_set_lookup(LocalVector<uint32_t> &r_lookup, uint32_t p_rt_index, uint32_t p_index) {
	const uint32_t old_size = r_lookup.size();
	if (p_rt_index >= old_size) {
		r_lookup.resize(p_rt_index + 1);
		for (uint32_t i = old_size; i < p_rt_index; i++) {
			r_lookup[i] = UINT32_MAX;
		}
	}
	r_lookup[p_rt_index] = p_index;
}

} // namespace RendererSceneRenderImplementation
