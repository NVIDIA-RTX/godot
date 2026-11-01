/**************************************************************************/
/*  render_raytracing_scheduler.cpp                                       */
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

#include "render_raytracing_scheduler.h"

#include "core/templates/sort_array.h"

// Keeps items the estimator rates at 0 ageing, so they still get their turn.
static constexpr float RT_SCHEDULER_VISIBILITY_FLOOR = 1e-4f;
// Safety-net items rank above everything the estimator scores.
static constexpr float RT_SCHEDULER_FORCED_SCORE_BONUS = 1e6f;
// New items aren't traced until built. Mandatory scores are 0..1, so 1 ranks them first.
static constexpr float RT_SCHEDULER_NEW_ITEM_SCORE_BONUS = 1.0f;

struct RTCandidateSort {
	template <typename T>
	_FORCE_INLINE_ bool operator()(const T &p_a, const T &p_b) const {
		if (p_a.score != p_b.score) {
			return p_a.score > p_b.score;
		}
		return p_a.order < p_b.order;
	}
};

struct RTCandidateHeapOrder {
	template <typename T>
	_FORCE_INLINE_ bool operator()(const T &p_a, const T &p_b) const {
		return RTCandidateSort()(p_b, p_a);
	}
};

// Heap-ordered lazily, so stopping early skips ordering the rest. Returns how many were not visited.
template <typename T, typename F>
static uint32_t _rt_visit_in_order(LocalVector<T> &r_candidates, const F &p_visit) {
	SortArray<T, RTCandidateHeapOrder> heap;
	heap.make_heap(0, r_candidates.size(), r_candidates.ptr());
	for (uint32_t end = r_candidates.size(); end > 0; end--) {
		heap.pop_heap(0, end, r_candidates.ptr());
		if (!p_visit(r_candidates[end - 1])) {
			return end;
		}
	}
	return 0;
}

// Returns [interval, 2 * interval). Fibonacci hashing spreads the rebuild phase of dense slots.
static uint16_t _rt_initial_rebuild_countdown(uint32_t p_slot, uint64_t p_frame, uint32_t p_interval) {
	const uint32_t phase = uint32_t((uint64_t(p_slot * 2654435769u) * p_interval) >> 32);
	return uint16_t(p_interval + (p_interval - (p_frame + phase) % p_interval) % p_interval);
}

void RTDynamicScheduler::begin_frame(uint64_t p_frame) {
	frame = p_frame;
	stats = RTSchedulerStats();
}

void RTDynamicScheduler::_ensure_slot(uint32_t p_slot) {
	if (p_slot < slot_gen.size()) {
		return;
	}
	const uint32_t size = MAX(p_slot + 1, slot_gen.size() * 2);
	const uint32_t old_size = slot_gen.size();
	slot_gen.resize(size);
	slot_last_update.resize(size);
	slot_rebuild_countdown.resize(size);
	slot_last_center.resize(size);
	for (uint32_t i = old_size; i < size; i++) {
		slot_gen[i] = 0;
	}
}

bool RTDynamicScheduler::_has_history(const RTSchedulerItem &p_item) const {
	return p_item.slot < slot_gen.size() && slot_gen[p_item.slot] == p_item.gen;
}

uint32_t RTDynamicScheduler::get_frames_since_update(uint32_t p_slot, uint8_t p_gen) const {
	if (p_slot >= slot_gen.size() || slot_gen[p_slot] != p_gen) {
		return UINT32_MAX;
	}
	return uint32_t(MIN(frame - slot_last_update[p_slot], uint64_t(UINT32_MAX - 1)));
}

bool RTDynamicScheduler::_try_rebuild(const RTSchedulerItem &p_item, bool p_ignore_triangle_budget) {
	if (stats.get_updated() >= budgets.max_operations) {
		return false;
	}
	if (!p_ignore_triangle_budget && stats.build_triangles + p_item.triangle_count > budgets.build_triangles) {
		return false;
	}
	stats.builds++;
	stats.build_triangles += p_item.triangle_count;
	return true;
}

bool RTDynamicScheduler::_try_refit(const RTSchedulerItem &p_item) {
	if (stats.get_updated() >= budgets.max_operations) {
		return false;
	}
	if (stats.refit_triangles + p_item.triangle_count > budgets.update_triangles) {
		return false;
	}
	stats.refits++;
	stats.refit_triangles += p_item.triangle_count;
	return true;
}

RTSchedulerAction RTDynamicScheduler::_try_update(const RTSchedulerItem &p_item, uint32_t p_interval) {
	switch (p_item.blas_update) {
		case RSE::RT_BLAS_UPDATE_REBUILD:
			return _try_rebuild(p_item, stats.build_triangles == 0) ? RT_SCHEDULER_REBUILD : RT_SCHEDULER_HOLD;
		case RSE::RT_BLAS_UPDATE_REFIT:
			return _try_refit(p_item) ? RT_SCHEDULER_REFIT : RT_SCHEDULER_HOLD;
		default:
			break;
	}
	if (slot_rebuild_countdown[p_item.slot] <= 1 && _try_rebuild(p_item, false)) {
		return RT_SCHEDULER_REBUILD;
	}
	return _try_refit(p_item) ? RT_SCHEDULER_REFIT : RT_SCHEDULER_HOLD;
}

void RTDynamicScheduler::_record(const RTSchedulerItem &p_item, uint16_t p_rebuild_countdown) {
	slot_gen[p_item.slot] = p_item.gen;
	slot_last_update[p_item.slot] = frame;
	slot_last_center[p_item.slot] = p_item.aabb.get_center();
	slot_rebuild_countdown[p_item.slot] = p_rebuild_countdown;
}

void RTDynamicScheduler::schedule(RTVisibilityEstimator &p_estimator, const RTSchedulerFrameContext &p_frame, const LocalVector<RTSchedulerItem> &p_items, LocalVector<RTSchedulerAction> &r_actions) {
	r_actions.resize(p_items.size());
	mandatory.clear();
	optional.clear();
	p_estimator.begin_frame(p_frame);

	uint32_t &cursor = viewport_cursor[p_frame.viewport_id];

	for (uint32_t i = 0; i < p_items.size(); i++) {
		const RTSchedulerItem &item = p_items[i];
		r_actions[i] = RT_SCHEDULER_HOLD;
		_ensure_slot(item.slot);

		Candidate candidate;
		candidate.index = i;
		candidate.order = item.slot - cursor;
		const float visibility = MAX(p_estimator.get_visibility(item), RT_SCHEDULER_VISIBILITY_FLOOR);

		if (item.needs_build || !_has_history(item)) {
			candidate.score = visibility + (_has_history(item) ? 0.0f : RT_SCHEDULER_NEW_ITEM_SCORE_BONUS);
			mandatory.push_back(candidate);
			continue;
		}

		const uint64_t since_update = frame - slot_last_update[item.slot];
		if (since_update == 0) {
			continue; // Already updated this frame by another viewport (shared entry).
		}
		const real_t radius = item.aabb.size.length() * 0.5f;
		candidate.forced = since_update >= budgets.max_stale_frames || slot_last_center[item.slot].distance_to(item.aabb.get_center()) > radius;
		if (!candidate.forced && p_estimator.should_hold(item)) {
			stats.held++;
			stats.invisible++;
			continue;
		}
		candidate.score = visibility * float(1 + MIN(since_update, uint64_t(budgets.max_stale_frames)));
		if (candidate.forced) {
			candidate.score += RT_SCHEDULER_FORCED_SCORE_BONUS;
		}
		optional.push_back(candidate);
	}

	const uint32_t updated_before = stats.get_updated();
	const auto budget_left = [this]() { return stats.get_updated() < budgets.max_operations; };
	uint32_t limited = 0;
	const uint32_t interval = MAX(budgets.rebuild_interval_frames, 1u);

	// The first build of the frame ignores the triangle budget, so oversized items are never starved.
	const uint32_t mandatory_unvisited = _rt_visit_in_order(mandatory, [&](const Candidate &p_candidate) {
		if (!budget_left()) {
			return false;
		}
		const RTSchedulerItem &item = p_items[p_candidate.index];
		if (_try_rebuild(item, stats.build_triangles == 0)) {
			r_actions[p_candidate.index] = RT_SCHEDULER_REBUILD;
			_record(item, _rt_initial_rebuild_countdown(item.slot, frame, interval));
		} else {
			limited++;
		}
		return true;
	});

	// Cadence counts updates, not frames, so budget-starved items don't come due together.
	const uint32_t optional_unvisited = _rt_visit_in_order(optional, [&](const Candidate &p_candidate) {
		if (!budget_left()) {
			return false;
		}
		const RTSchedulerItem &item = p_items[p_candidate.index];
		const RTSchedulerAction action = _try_update(item, interval);
		if (action == RT_SCHEDULER_REBUILD) {
			_record(item, uint16_t(interval));
		} else if (action == RT_SCHEDULER_REFIT) {
			_record(item, uint16_t(MAX(slot_rebuild_countdown[item.slot], uint16_t(2)) - 1));
		} else {
			limited++;
		}
		r_actions[p_candidate.index] = action;
		return true;
	});
	limited += mandatory_unvisited + optional_unvisited;
	stats.held += limited;
	stats.budget_limited += limited;

	cursor += stats.get_updated() - updated_before;
}
