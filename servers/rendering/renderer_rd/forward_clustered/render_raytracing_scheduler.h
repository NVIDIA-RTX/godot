/**************************************************************************/
/*  render_raytracing_scheduler.h                                         */
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

#include "render_raytracing_visibility.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

enum RTSchedulerAction : uint8_t {
	RT_SCHEDULER_HOLD,
	RT_SCHEDULER_REFIT,
	RT_SCHEDULER_REBUILD,
};

// Shared by every viewport scheduled in the same frame.
struct RTSchedulerBudgets {
	uint32_t max_operations = 2000;
	uint64_t build_triangles = 500000;
	uint64_t update_triangles = 1500000;
	uint32_t rebuild_interval_frames = 4; // Counted in updates of an item, phase-shifted per slot.
	uint32_t max_stale_frames = 30;
};

struct RTSchedulerStats {
	uint32_t builds = 0;
	uint32_t refits = 0;
	uint64_t build_triangles = 0;
	uint64_t refit_triangles = 0;
	uint32_t held = 0;
	uint32_t budget_limited = 0; // Subset of held.
	uint32_t invisible = 0; // Subset of held.

	uint32_t get_updated() const { return builds + refits; }
};

// Picks REBUILD / REFIT / HOLD for every dynamic item under the per-frame budgets, ranked by an RTVisibilityEstimator.
class RTDynamicScheduler {
	RTSchedulerBudgets budgets;
	RTSchedulerStats stats;
	uint64_t frame = 0;

	// Indexed by RTSchedulerItem::slot; slot_gen 0 means no history.
	LocalVector<uint8_t> slot_gen;
	LocalVector<uint64_t> slot_last_update;
	LocalVector<uint16_t> slot_rebuild_countdown;
	LocalVector<Vector3> slot_last_center;

	HashMap<uint32_t, uint32_t> viewport_cursor;

	struct Candidate {
		float score = 0.0f;
		uint32_t order = 0;
		uint32_t index = 0;
		bool forced = false;
	};
	LocalVector<Candidate> mandatory;
	LocalVector<Candidate> optional;
	void _ensure_slot(uint32_t p_slot);
	bool _has_history(const RTSchedulerItem &p_item) const;
	bool _try_rebuild(const RTSchedulerItem &p_item, bool p_ignore_triangle_budget);
	bool _try_refit(const RTSchedulerItem &p_item);
	RTSchedulerAction _try_update(const RTSchedulerItem &p_item, uint32_t p_interval);
	void _record(const RTSchedulerItem &p_item, uint16_t p_rebuild_countdown);

public:
	void set_budgets(const RTSchedulerBudgets &p_budgets) { budgets = p_budgets; }
	const RTSchedulerBudgets &get_budgets() const { return budgets; }

	void begin_frame(uint64_t p_frame);
	const RTSchedulerStats &get_stats() const { return stats; }

	// Budgets consumed here are no longer available to later viewports of the same frame.
	void schedule(RTVisibilityEstimator &p_estimator, const RTSchedulerFrameContext &p_frame, const LocalVector<RTSchedulerItem> &p_items, LocalVector<RTSchedulerAction> &r_actions);

	// Returns UINT32_MAX without history for this generation.
	uint32_t get_frames_since_update(uint32_t p_slot, uint8_t p_gen) const;
};
