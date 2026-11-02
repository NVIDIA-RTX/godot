/**************************************************************************/
/*  render_raytracing_debug.h                                             */
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

#include "core/templates/hash_map.h"
#include "core/templates/rid.h"

// Bookkeeping for the dynamic geometry debug view (RT_DEBUG_DYNAMIC_GEOMETRY): BLAS work fades out over
// FADE_FRAMES so items the scheduler only updates on some frames stay visible.
class RTDynamicGeometryDebug {
public:
	// Must match RT_DYNAMIC_GEOMETRY_* in raytracing_closest_hit_common_inc.glsl.
	enum Category : uint32_t {
		CATEGORY_STATIC,
		CATEGORY_MOVED,
		CATEGORY_REFIT,
		CATEGORY_BUILD,
		CATEGORY_CAPTURED_REFIT,
		CATEGORY_CAPTURED_BUILD,
		CATEGORY_BUILT_IDLE,
		CATEGORY_CAPTURED_BUILT_IDLE,
	};

	// Must match RT_DYNAMIC_GEOMETRY_FADE_FRAMES, _REST_SHIFT and _AGE_SHIFT.
	static constexpr uint32_t FADE_FRAMES = 60;
	static constexpr uint32_t REST_SHIFT = 4;
	static constexpr uint32_t CATEGORY_MASK = (1u << REST_SHIFT) - 1u;
	static constexpr uint32_t AGE_SHIFT = 8;
	static constexpr uint64_t NEVER = UINT64_MAX;

	struct Stamps {
		uint64_t last_build = NEVER;
		uint64_t last_refit = NEVER;
		uint64_t last_seen = NEVER;
		bool dynamic = false; // Only dynamic BLASes settle on CATEGORY_BUILT_IDLE.
	};

	struct State {
		Category category = CATEGORY_STATIC;
		Category rest = CATEGORY_STATIC;
		uint32_t age = 0;

		bool operator==(const State &p_other) const { return category == p_other.category && rest == p_other.rest && age == p_other.age; }
	};

	static State classify(const Stamps &p_stamps, bool p_captured, bool p_moved, uint64_t p_frame);
	static uint32_t pack(const State &p_state) {
		return uint32_t(p_state.category) | (uint32_t(p_state.rest) << REST_SHIFT) | (p_state.age << AGE_SHIFT);
	}

	void begin_frame(uint64_t p_frame);
	void record_build(RID p_blas);
	void record_refit(RID p_blas);
	State observe(RID p_blas, bool p_captured, bool p_moved, bool p_dynamic);
	void clear() { blas_stamps.clear(); }
	uint32_t get_tracked_count() const { return blas_stamps.size(); }

private:
	HashMap<uint64_t, Stamps> blas_stamps;
	uint64_t frame = 0;

	static uint32_t _age(uint64_t p_stamp, uint64_t p_frame);
	static bool _settles(const Stamps &p_stamps);
};
