/**************************************************************************/
/*  test_render_raytracing_plan.h                                         */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_plan.h"

#include "tests/test_macros.h"

namespace TestRenderRaytracingPlan {

using namespace RendererSceneRenderImplementation;

static Span<uint32_t> as_span(const LocalVector<uint32_t> &p_vector) {
	return Span<uint32_t>(p_vector.ptr(), p_vector.size());
}

static RTPlanKey make_key() {
	RTPlanKey key;
	key.surfaces_version = 11;
	key.mask_opaque = 0x3;
	key.mask_shadow = 0x2;
	key.transparency = 128;
	key.camera_visible = true;
	return key;
}

static RTPlanVersions make_versions() {
	RTPlanVersions versions;
	versions.material_version = 22;
	versions.mesh_version = 33;
	versions.ready_version = 44;
	versions.capture_mode_version = 55;
	return versions;
}

// Fills record p_index of r_arena with p_groups groups, p_surfaces surfaces and p_captures captures tagged with p_tag.
static void fill_record(RTPlanArena &r_arena, uint32_t p_index, uint16_t p_groups, uint16_t p_surfaces, uint16_t p_captures, uint32_t p_tag) {
	RTPlanInstance &record = r_arena.instances[p_index];
	record.key = make_key();
	record.key.surfaces_version = p_tag;
	record.group_count = p_groups;
	record.surface_count = p_surfaces;
	record.capture_count = p_captures;
	record.pending_surfaces = 1;
	record.flags = RT_PLAN_INSTANCE_VALID | RT_PLAN_INSTANCE_HIT_GROUPS_READY;
	for (uint16_t i = 0; i < p_groups; i++) {
		RTPlanGroup &group = r_arena.groups[record.range_begin + i];
		group.surface_begin = i;
		group.surface_count = 1;
		group.surface_mask = uint64_t(p_tag) << i;
	}
	for (uint16_t i = 0; i < p_surfaces; i++) {
		r_arena.surfaces[record.range_begin + i].cache_key = p_tag * 100 + i;
	}
	for (uint16_t i = 0; i < p_captures; i++) {
		r_arena.captures[record.range_begin + i].triangle_count = p_tag * 1000 + i;
		r_arena.capture_aabbs[record.range_begin + i] = AABB(Vector3(p_tag, i, 0), Vector3(1, 1, 1));
	}
}

TEST_CASE("[RTPlan] rt_plan_reserve assigns contiguous ranges in record order") {
	RTPlanArena arena;
	SUBCASE("prefix ranges and totals") {
		const uint32_t capacities[] = { 2, 0, 5, 1 };
		rt_plan_reserve(arena, capacities);
		REQUIRE(arena.instances.size() == 4);
		const uint32_t begins[] = { 0, 2, 2, 7 };
		for (uint32_t i = 0; i < 4; i++) {
			CHECK(arena.instances[i].range_begin == begins[i]);
			CHECK(arena.instances[i].range_capacity == capacities[i]);
			CHECK(arena.instances[i].flags == 0);
			CHECK(arena.instances[i].surface_count == 0);
		}
		CHECK(arena.groups.size() == 8);
		CHECK(arena.surfaces.size() == 8);
		CHECK(arena.captures.size() == 8);
		CHECK(arena.capture_aabbs.size() == 8);
		CHECK(arena.capture_evals.size() == 8);
	}
	SUBCASE("zero capacities") {
		const uint32_t capacities[] = { 0, 0, 0 };
		rt_plan_reserve(arena, capacities);
		CHECK(arena.instances.size() == 3);
		CHECK(arena.groups.size() == 0);
		CHECK(arena.surfaces.size() == 0);
		CHECK(arena.captures.size() == 0);
		CHECK(arena.capture_aabbs.size() == 0);
	}
	SUBCASE("empty input clears a used arena") {
		const uint32_t capacities[] = { 3 };
		rt_plan_reserve(arena, capacities);
		fill_record(arena, 0, 1, 2, 3, 5);
		rt_plan_reserve(arena, Span<uint32_t>());
		CHECK(arena.instances.is_empty());
		CHECK(arena.groups.is_empty());
		CHECK(arena.surfaces.is_empty());
		CHECK(arena.captures.is_empty());
		CHECK(arena.capture_aabbs.is_empty());
	}
	SUBCASE("reserving again resets reused instance records") {
		const uint32_t capacities[] = { 3 };
		rt_plan_reserve(arena, capacities);
		fill_record(arena, 0, 1, 2, 3, 5);
		rt_plan_reserve(arena, capacities);
		CHECK(arena.instances[0].flags == 0);
		CHECK(arena.instances[0].capture_count == 0);
		CHECK(arena.instances[0].surface_count == 0);
	}
	SUBCASE("capacities above UINT16_MAX are clamped") {
		const uint32_t capacities[] = { 1, UINT16_MAX + 10u, 2 };
		ERR_PRINT_OFF;
		rt_plan_reserve(arena, capacities);
		ERR_PRINT_ON;
		CHECK(arena.instances[1].range_capacity == UINT16_MAX);
		CHECK(arena.instances[2].range_begin == 1u + UINT16_MAX);
		CHECK(arena.surfaces.size() == 3u + UINT16_MAX);
	}
}

TEST_CASE("[RTPlan] rt_plan_copy_forward moves a record into a differently placed range") {
	RTPlanArena src;
	const uint32_t src_capacities[] = { 2, 3 };
	rt_plan_reserve(src, src_capacities);
	fill_record(src, 1, 2, 3, 2, 7);
	src.groups[src.instances[1].range_begin + 1].surface_begin = 1;
	src.groups[src.instances[1].range_begin + 1].surface_count = 2;

	RTPlanArena dst;
	const uint32_t dst_capacities[] = { 4, 3, 3 };
	rt_plan_reserve(dst, dst_capacities);
	REQUIRE(dst.instances[2].range_begin == 7);
	REQUIRE(src.instances[1].range_begin == 2);
	// Ranges aren't initialized by rt_plan_reserve, so mark the neighbors to check they stay untouched.
	dst.surfaces[6].cache_key = 0;
	dst.captures[9].triangle_count = 0;

	REQUIRE(rt_plan_copy_forward(src, 1, dst, 2));
	const RTPlanInstance &record = dst.instances[2];
	CHECK(record.range_begin == 7);
	CHECK(record.range_capacity == 3);
	CHECK(rt_plan_key_match(record.key, make_versions(), src.instances[1].key, make_versions()) == RT_PLAN_KEY_MATCH);
	CHECK(record.group_count == 2);
	CHECK(record.surface_count == 3);
	CHECK(record.capture_count == 2);
	CHECK(record.pending_surfaces == 1);
	CHECK(record.flags == (RT_PLAN_INSTANCE_VALID | RT_PLAN_INSTANCE_HIT_GROUPS_READY));

	CHECK(dst.groups[7].surface_begin == 0);
	CHECK(dst.groups[8].surface_begin == 1);
	CHECK(dst.groups[8].surface_count == 2);
	CHECK(dst.groups[8].surface_mask == uint64_t(7) << 1);
	for (uint32_t i = 0; i < 3; i++) {
		CHECK(dst.surfaces[7 + i].cache_key == 700 + i);
	}
	for (uint32_t i = 0; i < 2; i++) {
		CHECK(dst.captures[7 + i].triangle_count == 7000 + i);
		CHECK(dst.capture_aabbs[7 + i] == AABB(Vector3(7, i, 0), Vector3(1, 1, 1)));
	}
	CHECK_MESSAGE(dst.surfaces[6].cache_key == 0, "Neighboring ranges are untouched.");
	CHECK(dst.captures[9].triangle_count == 0);
}

TEST_CASE("[RTPlan] rt_plan_copy_forward rejects records that overflow the destination range") {
	RTPlanArena src;
	const uint32_t src_capacities[] = { 4 };
	rt_plan_reserve(src, src_capacities);

	RTPlanArena dst;
	const uint32_t dst_capacities[] = { 2 };
	rt_plan_reserve(dst, dst_capacities);
	fill_record(dst, 0, 1, 1, 1, 3);

	SUBCASE("groups") {
		fill_record(src, 0, 3, 1, 1, 9);
	}
	SUBCASE("surfaces") {
		fill_record(src, 0, 1, 3, 1, 9);
	}
	SUBCASE("captures") {
		fill_record(src, 0, 1, 1, 3, 9);
	}

	CHECK_FALSE(rt_plan_copy_forward(src, 0, dst, 0));
	const RTPlanInstance &record = dst.instances[0];
	CHECK(record.key.surfaces_version == 3);
	CHECK(record.group_count == 1);
	CHECK(record.surface_count == 1);
	CHECK(record.capture_count == 1);
	CHECK(record.range_capacity == 2);
	CHECK(dst.groups[0].surface_mask == 3);
	CHECK(dst.surfaces[0].cache_key == 300);
	CHECK(dst.captures[0].triangle_count == 3000);
	CHECK(dst.capture_aabbs[0] == AABB(Vector3(3, 0, 0), Vector3(1, 1, 1)));
}

TEST_CASE("[RTPlan] rt_plan_key_match separates global version moves from mismatches") {
	const RTPlanKey key = make_key();
	const RTPlanVersions versions = make_versions();
	CHECK(rt_plan_key_match(key, versions, key, versions) == RT_PLAN_KEY_MATCH);

	RTPlanVersions global = versions;
	global.material_version++;
	CHECK(rt_plan_key_match(key, versions, key, global) == RT_PLAN_KEY_GLOBAL_ONLY);
	global = versions;
	global.mesh_version++;
	CHECK(rt_plan_key_match(key, versions, key, global) == RT_PLAN_KEY_GLOBAL_ONLY);
	global.material_version++;
	CHECK(rt_plan_key_match(key, versions, key, global) == RT_PLAN_KEY_GLOBAL_ONLY);

	struct Mutation {
		void (*key)(RTPlanKey &);
		void (*versions)(RTPlanVersions &);
	};
	const Mutation mutations[] = {
		{ [](RTPlanKey &r_key) { r_key.surfaces_version++; }, [](RTPlanVersions &) {} },
		{ [](RTPlanKey &) {}, [](RTPlanVersions &r_versions) { r_versions.ready_version++; } },
		{ [](RTPlanKey &) {}, [](RTPlanVersions &r_versions) { r_versions.capture_mode_version++; } },
		{ [](RTPlanKey &r_key) { r_key.mask_opaque ^= 0x10; }, [](RTPlanVersions &) {} },
		{ [](RTPlanKey &r_key) { r_key.mask_shadow ^= 0x10; }, [](RTPlanVersions &) {} },
		{ [](RTPlanKey &r_key) { r_key.transparency++; }, [](RTPlanVersions &) {} },
		{ [](RTPlanKey &r_key) { r_key.camera_visible = !r_key.camera_visible; }, [](RTPlanVersions &) {} },
	};
	uint32_t i = 0;
	for (const Mutation &mutation : mutations) {
		RTPlanKey current = key;
		RTPlanVersions current_versions = versions;
		mutation.key(current);
		mutation.versions(current_versions);
		CHECK_MESSAGE(rt_plan_key_match(key, versions, current, current_versions) == RT_PLAN_KEY_MISMATCH, vformat("Field %d alone mismatches.", i));
		current_versions.material_version++;
		CHECK_MESSAGE(rt_plan_key_match(key, versions, current, current_versions) == RT_PLAN_KEY_MISMATCH, vformat("Field %d with a global move mismatches.", i));
		i++;
	}
}

TEST_CASE("[RTPlan] rt_plan_set_lookup grows with UINT32_MAX") {
	LocalVector<uint32_t> lookup;
	rt_plan_set_lookup(lookup, 3, 7);
	REQUIRE(lookup.size() == 4);
	CHECK(lookup[0] == UINT32_MAX);
	CHECK(lookup[1] == UINT32_MAX);
	CHECK(lookup[2] == UINT32_MAX);
	CHECK(lookup[3] == 7);

	rt_plan_set_lookup(lookup, 1, 2);
	CHECK(lookup.size() == 4);
	CHECK(lookup[1] == 2);
	CHECK(lookup[3] == 7);
}

TEST_CASE("[RTPlan] rt_plan_lookup finds only the current instance's valid record") {
	RTPlanArena prev;
	const uint32_t capacities[] = { 1, 1, 1 };
	rt_plan_reserve(prev, capacities);
	fill_record(prev, 0, 1, 1, 0, 40);
	fill_record(prev, 1, 1, 1, 0, 41);
	prev.instances[1].flags = 0;
	fill_record(prev, 2, 1, 1, 0, 42);

	LocalVector<uint32_t> lookup;
	rt_plan_set_lookup(lookup, 5, 2);
	rt_plan_set_lookup(lookup, 2, 0);
	rt_plan_set_lookup(lookup, 3, 1);
	rt_plan_set_lookup(lookup, 4, 9);

	uint32_t index = UINT32_MAX;
	CHECK(rt_plan_lookup(as_span(lookup), prev, 5, 42, index));
	CHECK(index == 2);
	CHECK(rt_plan_lookup(as_span(lookup), prev, 2, 40, index));
	CHECK(index == 0);

	index = 123;
	CHECK_FALSE_MESSAGE(rt_plan_lookup(as_span(lookup), prev, 6, 42, index), "rt_index past the lookup.");
	CHECK_FALSE_MESSAGE(rt_plan_lookup(as_span(lookup), prev, 0, 40, index), "rt_index without a record.");
	CHECK_FALSE_MESSAGE(rt_plan_lookup(as_span(lookup), prev, 4, 40, index), "Record index past the previous arena.");
	CHECK_FALSE_MESSAGE(rt_plan_lookup(as_span(lookup), prev, 3, 41, index), "Record without RT_PLAN_INSTANCE_VALID.");
	CHECK_FALSE_MESSAGE(rt_plan_lookup(as_span(lookup), prev, 5, 43, index), "Recycled rt_index with a new surfaces_version.");
	CHECK_FALSE(rt_plan_lookup(Span<uint32_t>(), prev, 0, 40, index));
	CHECK_MESSAGE(index == 123, "Misses leave r_prev_index untouched.");
}

} // namespace TestRenderRaytracingPlan
