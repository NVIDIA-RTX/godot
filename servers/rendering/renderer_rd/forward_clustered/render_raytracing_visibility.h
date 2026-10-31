/**************************************************************************/
/*  render_raytracing_visibility.h                                        */
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
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_server.h"

struct RTSchedulerFrameContext {
	uint64_t frame = 0;
	uint32_t viewport_id = 0;
	Transform3D camera;
	Transform3D prev_camera;
};

struct RTSchedulerItem {
	uint32_t slot = 0;
	uint8_t gen = 1; // Bumped when the slot gets a new owner, never 0.
	bool needs_build = false;
	bool in_frustum = false;
	RS::RTBlasUpdate blas_update = RS::RT_BLAS_UPDATE_AUTO;
	AABB aabb;
	uint32_t triangle_count = 0;
};

constexpr RS::RTBlasUpdate rt_resolve_blas_update(RS::RTBlasUpdate p_instance, RS::RTBlasUpdate p_shader) {
	return p_instance != RS::RT_BLAS_UPDATE_AUTO ? p_instance : p_shader;
}

// Items are keyed by (slot, gen); estimators keep no per-item registration.
class RTVisibilityEstimator {
public:
	virtual ~RTVisibilityEstimator() = default;

	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) = 0;
	// Returns 0..1, higher is updated first.
	virtual float get_visibility(const RTSchedulerItem &p_item) const = 0;
	virtual bool should_hold(const RTSchedulerItem &p_item) const { return false; }
	virtual bool wants_feedback() const { return false; }
	virtual void record_gpu_work(uint32_t p_viewport_id) {}
	virtual void viewport_freed(uint32_t p_viewport_id) {}
};

class RTCpuVisibilityEstimator : public RTVisibilityEstimator {
	Vector3 camera_position;
	float offscreen_weight = 0.25f;

public:
	void set_offscreen_weight(float p_weight) { offscreen_weight = p_weight; }
	float get_offscreen_weight() const { return offscreen_weight; }

	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) override { camera_position = p_frame.camera.origin; }

	// Squared apparent size (r / max(d, r))^2, weighted down outside the frustum.
	virtual float get_visibility(const RTSchedulerItem &p_item) const override {
		const real_t radius = p_item.aabb.size.length() * 0.5f;
		if (radius <= 0) {
			return 0.0f;
		}
		const real_t distance = MAX(camera_position.distance_to(p_item.aabb.get_center()), radius);
		const real_t ratio = radius / distance;
		return float(ratio * ratio) * (p_item.in_frustum ? 1.0f : offscreen_weight);
	}
};
