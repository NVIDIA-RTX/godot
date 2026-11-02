/**************************************************************************/
/*  render_raytracing_debug.cpp                                           */
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

#include "render_raytracing_debug.h"

#include "core/templates/local_vector.h"

uint32_t RTDynamicGeometryDebug::_age(uint64_t p_stamp, uint64_t p_frame) {
	if (p_stamp == NEVER || p_stamp > p_frame || p_frame - p_stamp >= FADE_FRAMES) {
		return UINT32_MAX;
	}
	return uint32_t(p_frame - p_stamp);
}

bool RTDynamicGeometryDebug::_settles(const Stamps &p_stamps) {
	return p_stamps.dynamic && p_stamps.last_build != NEVER && (p_stamps.last_refit == NEVER || p_stamps.last_build >= p_stamps.last_refit);
}

RTDynamicGeometryDebug::State RTDynamicGeometryDebug::classify(const Stamps &p_stamps, bool p_captured, bool p_moved, uint64_t p_frame) {
	static constexpr Category work_categories[2][2] = {
		{ CATEGORY_REFIT, CATEGORY_BUILD },
		{ CATEGORY_CAPTURED_REFIT, CATEGORY_CAPTURED_BUILD },
	};
	static constexpr Category idle_categories[2] = { CATEGORY_BUILT_IDLE, CATEGORY_CAPTURED_BUILT_IDLE };
	const uint32_t build_age = _age(p_stamps.last_build, p_frame);
	const uint32_t refit_age = _age(p_stamps.last_refit, p_frame);
	State state;
	state.rest = _settles(p_stamps) ? idle_categories[p_captured] : CATEGORY_STATIC;
	if (build_age == UINT32_MAX && refit_age == UINT32_MAX) {
		state.category = p_moved ? CATEGORY_MOVED : state.rest;
		return state;
	}
	const bool built = build_age <= refit_age;
	state.category = work_categories[p_captured][built];
	state.age = built ? build_age : refit_age;
	return state;
}

void RTDynamicGeometryDebug::begin_frame(uint64_t p_frame) {
	if (p_frame == frame) {
		return;
	}
	frame = p_frame;
	LocalVector<uint64_t> faded;
	for (const KeyValue<uint64_t, Stamps> &kv : blas_stamps) {
		const Stamps &stamps = kv.value;
		const bool working = _age(stamps.last_build, frame) != UINT32_MAX || _age(stamps.last_refit, frame) != UINT32_MAX;
		const bool idling = _settles(stamps) && _age(stamps.last_seen, frame) != UINT32_MAX;
		if (!working && !idling) {
			faded.push_back(kv.key);
		}
	}
	for (uint64_t id : faded) {
		blas_stamps.erase(id);
	}
}

void RTDynamicGeometryDebug::record_build(RID p_blas) {
	blas_stamps[p_blas.get_id()].last_build = frame;
}

void RTDynamicGeometryDebug::record_refit(RID p_blas) {
	blas_stamps[p_blas.get_id()].last_refit = frame;
}

RTDynamicGeometryDebug::State RTDynamicGeometryDebug::observe(RID p_blas, bool p_captured, bool p_moved, bool p_dynamic) {
	Stamps *stamps = blas_stamps.getptr(p_blas.get_id());
	if (!stamps) {
		return classify(Stamps(), p_captured, p_moved, frame);
	}
	stamps->last_seen = frame;
	stamps->dynamic = p_dynamic;
	return classify(*stamps, p_captured, p_moved, frame);
}
