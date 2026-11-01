/**************************************************************************/
/*  test_render_raytracing_scheduler.cpp                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_render_raytracing_scheduler)

#ifdef RD_ENABLED

#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_scheduler.h"

namespace TestRenderRaytracingScheduler {

class MockEstimator : public RTVisibilityEstimator {
public:
	HashMap<uint32_t, float> visibility;
	HashSet<uint32_t> held;
	float default_visibility = 0.5f;

	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) override {}
	virtual float get_visibility(const RTSchedulerItem &p_item) const override {
		const float *value = visibility.getptr(p_item.slot);
		return value ? *value : default_visibility;
	}
	virtual bool should_hold(const RTSchedulerItem &p_item) const override { return held.has(p_item.slot); }
};

static RTSchedulerItem make_item(uint32_t p_slot, uint32_t p_triangles = 100, uint8_t p_gen = 1) {
	RTSchedulerItem item;
	item.slot = p_slot;
	item.gen = p_gen;
	item.triangle_count = p_triangles;
	item.in_frustum = true;
	item.aabb = AABB(Vector3(p_slot * 10.0f, 0, 0), Vector3(1, 1, 1));
	return item;
}

static LocalVector<RTSchedulerItem> make_items(uint32_t p_count, uint32_t p_triangles = 100) {
	LocalVector<RTSchedulerItem> items;
	for (uint32_t i = 0; i < p_count; i++) {
		items.push_back(make_item(i, p_triangles));
	}
	return items;
}

struct Harness {
	RTDynamicScheduler scheduler;
	MockEstimator estimator;
	RTSchedulerFrameContext context;
	LocalVector<RTSchedulerAction> actions;

	const LocalVector<RTSchedulerAction> &run(const LocalVector<RTSchedulerItem> &p_items, uint32_t p_viewport = 0) {
		context.viewport_id = p_viewport;
		scheduler.schedule(estimator, context, p_items, actions);
		return actions;
	}
	const LocalVector<RTSchedulerAction> &frame(const LocalVector<RTSchedulerItem> &p_items) {
		context.frame++;
		scheduler.begin_frame(context.frame);
		return run(p_items);
	}
};

static uint32_t count_actions(const LocalVector<RTSchedulerAction> &p_actions, RTSchedulerAction p_action) {
	uint32_t count = 0;
	for (RTSchedulerAction action : p_actions) {
		count += action == p_action ? 1 : 0;
	}
	return count;
}

TEST_CASE("[RTDynamicScheduler] New items build, then one in every rebuild_interval_frames updates is a rebuild") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.rebuild_interval_frames = 3;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(2);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
	const uint64_t build_frame = h.context.frame;

	LocalVector<uint64_t> rebuild_frames[2];
	for (uint32_t f = 0; f < 12; f++) {
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_HOLD) == 0);
		for (uint32_t i = 0; i < 2; i++) {
			if (actions[i] == RT_SCHEDULER_REBUILD) {
				rebuild_frames[i].push_back(h.context.frame);
			}
		}
	}
	for (uint32_t i = 0; i < 2; i++) {
		REQUIRE(rebuild_frames[i].size() >= 2);
		CHECK_MESSAGE(rebuild_frames[i][0] >= build_frame + 3, "No periodic rebuild within an interval of the first build.");
		for (uint32_t k = 1; k < rebuild_frames[i].size(); k++) {
			CHECK_MESSAGE(rebuild_frames[i][k] - rebuild_frames[i][k - 1] == 3, "Updated every frame, an item rebuilds every rebuild_interval_frames frames.");
		}
	}
}

TEST_CASE("[RTDynamicScheduler] Periodic rebuilds of items created together are staggered") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.rebuild_interval_frames = 4;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(400);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 400);

	for (uint32_t f = 0; f < 3; f++) {
		CHECK_MESSAGE(count_actions(h.frame(items), RT_SCHEDULER_REFIT) == 400, "No periodic rebuild within an interval of the first build.");
	}
	HashSet<uint32_t> rebuilt;
	for (uint32_t f = 0; f < 4; f++) {
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		const uint32_t frame_rebuilds = count_actions(actions, RT_SCHEDULER_REBUILD);
		CHECK_MESSAGE(frame_rebuilds >= 95, vformat("%d rebuilds on one frame.", frame_rebuilds));
		CHECK(frame_rebuilds <= 105);
		CHECK(count_actions(actions, RT_SCHEDULER_HOLD) == 0);
		for (uint32_t i = 0; i < actions.size(); i++) {
			if (actions[i] == RT_SCHEDULER_REBUILD) {
				rebuilt.insert(i);
			}
		}
	}
	CHECK_MESSAGE(rebuilt.size() == 400, "Every item gets its periodic rebuild within one interval.");
}

TEST_CASE("[RTDynamicScheduler] Rebuilds stay steady when max_operations can't update every item") {
	Harness h;
	const LocalVector<RTSchedulerItem> items = make_items(5000);
	for (uint32_t i = 0; i < items.size(); i++) {
		h.estimator.visibility[i] = float((i * 7919) % 1000) / 1000.0f;
	}
	for (uint32_t f = 0; f < 40; f++) {
		h.frame(items); // Initial builds take three frames, then periodic rebuilds ramp up.
	}
	uint32_t min_rebuilds = UINT32_MAX;
	uint32_t max_rebuilds = 0;
	for (uint32_t f = 0; f < 16; f++) {
		const uint32_t rebuilds = count_actions(h.frame(items), RT_SCHEDULER_REBUILD);
		min_rebuilds = MIN(min_rebuilds, rebuilds);
		max_rebuilds = MAX(max_rebuilds, rebuilds);
	}
	CHECK_MESSAGE(max_rebuilds - min_rebuilds < max_rebuilds / 4, vformat("Rebuilds per frame swing between %d and %d.", min_rebuilds, max_rebuilds));
}

static LocalVector<RTSchedulerItem> make_items_with(uint32_t p_count, RSE::RTBlasUpdate p_update, uint32_t p_triangles = 100) {
	LocalVector<RTSchedulerItem> items = make_items(p_count, p_triangles);
	for (RTSchedulerItem &item : items) {
		item.blas_update = p_update;
	}
	return items;
}

TEST_CASE("[RTDynamicScheduler] Refit-only items never rebuild after their first build") {
	Harness h;
	LocalVector<RTSchedulerItem> items = make_items_with(4, RSE::RT_BLAS_UPDATE_REFIT);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 4);
	for (uint32_t f = 0; f < 12; f++) {
		CHECK(count_actions(h.frame(items), RT_SCHEDULER_REFIT) == 4);
	}
	items[0].needs_build = true;
	CHECK_MESSAGE(h.frame(items)[0] == RT_SCHEDULER_REBUILD, "Structure changes still build.");
}

TEST_CASE("[RTDynamicScheduler] Rebuild-only items never refit") {
	SUBCASE("every update is a rebuild") {
		Harness h;
		const LocalVector<RTSchedulerItem> items = make_items_with(4, RSE::RT_BLAS_UPDATE_REBUILD);
		for (uint32_t f = 0; f < 8; f++) {
			CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 4);
		}
	}
	SUBCASE("a spent build budget holds them instead of refitting") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.build_triangles = 250;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items_with(4, RSE::RT_BLAS_UPDATE_REBUILD);
		h.frame(items);
		h.frame(items);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REBUILD) == 2);
		CHECK(count_actions(actions, RT_SCHEDULER_REFIT) == 0);
		CHECK(h.scheduler.get_stats().budget_limited == 2);
	}
	SUBCASE("one alone over the build budget still rebuilds when it comes first") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.build_triangles = 100;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items_with(1, RSE::RT_BLAS_UPDATE_REBUILD, 1000);
		for (uint32_t f = 0; f < 3; f++) {
			CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
		}
	}
}

TEST_CASE("[RTDynamicScheduler] An instance's BLAS update mode overrides its shader's unless it is auto") {
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_AUTO, RSE::RT_BLAS_UPDATE_REFIT) == RSE::RT_BLAS_UPDATE_REFIT);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_AUTO, RSE::RT_BLAS_UPDATE_AUTO) == RSE::RT_BLAS_UPDATE_AUTO);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_REBUILD, RSE::RT_BLAS_UPDATE_REFIT) == RSE::RT_BLAS_UPDATE_REBUILD);
	CHECK(rt_resolve_blas_update(RSE::RT_BLAS_UPDATE_REFIT, RSE::RT_BLAS_UPDATE_REBUILD) == RSE::RT_BLAS_UPDATE_REFIT);
}

TEST_CASE("[RTDynamicScheduler] Structure changes force a build") {
	Harness h;
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.frame(items);
	h.frame(items);
	items[0].needs_build = true;
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
}

TEST_CASE("[RTDynamicScheduler] Budgets cap operations and triangles") {
	SUBCASE("max_operations") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.max_operations = 3;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(10);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_HOLD) == 7);
		CHECK(h.scheduler.get_stats().get_updated() == 3);
		CHECK(h.scheduler.get_stats().budget_limited == 7);
	}
	SUBCASE("build triangles downgrade rebuild-due items to refits") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.build_triangles = 250;
		budgets.rebuild_interval_frames = 1;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(4);
		// Mandatory builds spread over two frames under the 250-triangle budget.
		CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
		CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REBUILD) == 2);
		CHECK(count_actions(actions, RT_SCHEDULER_REFIT) == 2);
		CHECK(h.scheduler.get_stats().build_triangles <= 250);
	}
	SUBCASE("update triangles") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.update_triangles = 300;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(5);
		h.frame(items);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REFIT) == 3);
		CHECK(h.scheduler.get_stats().refit_triangles == 300);
		CHECK(h.scheduler.get_stats().budget_limited == 2);
	}
}

TEST_CASE("[RTDynamicScheduler] An oversized new item still builds when it comes first") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.build_triangles = 500000;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(3000, 10);
	RTSchedulerItem big = make_item(5000, 400000 * 2);
	h.estimator.visibility[5000] = 1.0f;
	items.push_back(big);
	const LocalVector<RTSchedulerAction> &actions = h.frame(items);
	CHECK(actions[items.size() - 1] == RT_SCHEDULER_REBUILD);
	CHECK(h.scheduler.get_stats().get_updated() <= budgets.max_operations);
}

TEST_CASE("[RTDynamicScheduler] New items build before structure changes of existing ones") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 1;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items;
	items.push_back(make_item(0));
	h.frame(items);

	items[0].needs_build = true;
	h.estimator.visibility[0] = 1.0f;
	items.push_back(make_item(1));
	h.estimator.visibility[1] = 0.0f;
	const LocalVector<RTSchedulerAction> &actions = h.frame(items);
	CHECK(actions[1] == RT_SCHEDULER_REBUILD);
	CHECK(actions[0] == RT_SCHEDULER_HOLD);
}

TEST_CASE("[RTDynamicScheduler] Higher visibility is updated first") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 1;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(3);
	h.estimator.visibility[0] = 0.1f;
	h.estimator.visibility[1] = 0.9f;
	h.estimator.visibility[2] = 0.5f;
	CHECK(h.frame(items)[1] == RT_SCHEDULER_REBUILD);
	CHECK(h.frame(items)[2] == RT_SCHEDULER_REBUILD);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
}

TEST_CASE("[RTDynamicScheduler] Equal scores take turns") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 2;
	budgets.max_stale_frames = 1000;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(8);
	for (int i = 0; i < 4; i++) {
		h.frame(items); // Initial builds, two per frame.
	}
	HashMap<uint32_t, int> updates;
	for (int f = 0; f < 8; f++) {
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		for (uint32_t i = 0; i < actions.size(); i++) {
			if (actions[i] != RT_SCHEDULER_HOLD) {
				updates[i]++;
			}
		}
	}
	for (uint32_t i = 0; i < items.size(); i++) {
		CHECK_MESSAGE(updates[i] == 2, vformat("Item %d was updated %d times in 8 frames.", i, updates[i]));
	}
}

TEST_CASE("[RTDynamicScheduler] Estimator holds and safety nets") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_stale_frames = 5;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.estimator.held.insert(0);

	CHECK_MESSAGE(h.frame(items)[0] == RT_SCHEDULER_REBUILD, "Mandatory builds ignore holds.");
	for (int i = 0; i < 4; i++) {
		CHECK(h.frame(items)[0] == RT_SCHEDULER_HOLD);
		CHECK(h.scheduler.get_stats().invisible == 1);
	}
	CHECK_MESSAGE(h.frame(items)[0] != RT_SCHEDULER_HOLD, "max_stale_frames forces an update.");

	CHECK(h.frame(items)[0] == RT_SCHEDULER_HOLD);
	items[0].aabb.position += Vector3(5, 0, 0);
	CHECK_MESSAGE(h.frame(items)[0] != RT_SCHEDULER_HOLD, "Moving more than its radius forces an update.");
}

TEST_CASE("[RTDynamicScheduler] A reused slot with a new generation starts over") {
	Harness h;
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.frame(items);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REFIT);
	items[0].gen = 2;
	CHECK(h.scheduler.get_frames_since_update(0, 2) == UINT32_MAX);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
	CHECK(h.scheduler.get_frames_since_update(0, 1) == UINT32_MAX);
	CHECK(h.scheduler.get_frames_since_update(0, 2) == 0);
}

TEST_CASE("[RTDynamicScheduler] Viewports share the frame's budgets and shared entries") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 5;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> shared = make_items(3);
	h.frame(shared);
	h.context.frame++;
	h.scheduler.begin_frame(h.context.frame);
	CHECK(count_actions(h.run(shared, 0), RT_SCHEDULER_REFIT) == 3);
	CHECK_MESSAGE(count_actions(h.run(shared, 1), RT_SCHEDULER_HOLD) == 3, "Entries updated by viewport 0 this frame are not redone.");

	LocalVector<RTSchedulerItem> own;
	for (uint32_t i = 0; i < 4; i++) {
		own.push_back(make_item(100 + i));
	}
	CHECK(count_actions(h.run(own, 1), RT_SCHEDULER_REBUILD) == 2);
	CHECK(h.scheduler.get_stats().get_updated() == 5);
}

TEST_CASE("[RTDynamicScheduler] CPU estimator ranks by apparent size") {
	RTCpuVisibilityEstimator estimator;
	RTSchedulerFrameContext context;
	estimator.begin_frame(context);

	RTSchedulerItem near_item = make_item(0);
	near_item.aabb = AABB(Vector3(-0.5, -0.5, -5.5), Vector3(1, 1, 1));
	RTSchedulerItem far_item = near_item;
	far_item.aabb.position.z = -50.5;
	CHECK(estimator.get_visibility(near_item) > estimator.get_visibility(far_item));

	RTSchedulerItem behind = near_item;
	behind.aabb.position.z = 4.5;
	behind.in_frustum = false;
	CHECK(estimator.get_visibility(behind) > 0.0f);
	CHECK(estimator.get_visibility(behind) == doctest::Approx(estimator.get_visibility(near_item) * estimator.get_offscreen_weight()));

	RTSchedulerItem inside = near_item;
	inside.aabb = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	CHECK_MESSAGE(estimator.get_visibility(inside) == doctest::Approx(1.0f), "A camera inside the bounds sees it fully.");
	CHECK(estimator.should_hold(near_item) == false);
}

} // namespace TestRenderRaytracingScheduler

#endif // RD_ENABLED
