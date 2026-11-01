/**************************************************************************/
/*  rendering_billboard.h                                                 */
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

#include "core/math/transform_3d.h"

// CPU mirror of billboard_inc.glsl; keep both in sync.
class RenderingBillboard {
public:
	enum Mode : uint8_t {
		MODE_DISABLED,
		MODE_ENABLED,
		MODE_FIXED_Y,
		MODE_PARTICLES,
	};

	struct Camera {
		Transform3D transform;
		bool orthogonal = false;
		real_t projection_y_scale = 1.0;
	};

	struct Settings {
		Mode mode = MODE_DISABLED;
		bool keep_scale = false;
		bool fixed_size = false;

		_FORCE_INLINE_ bool is_camera_dependent() const { return mode != MODE_DISABLED || fixed_size; }

		_FORCE_INLINE_ bool operator==(const Settings &p_other) const {
			return mode == p_other.mode && keep_scale == p_other.keep_scale && fixed_size == p_other.fixed_size;
		}
	};

	// Each horizontal axis falls back to the other when its cross product degenerates.
	static _FORCE_INLINE_ Basis fixed_y_basis(const Basis &p_camera_basis) {
		const Vector3 up(0, 1, 0);
		Vector3 right = up.cross(p_camera_basis.get_column(2));
		Vector3 back = p_camera_basis.get_column(0).cross(up);
		if (right.length_squared() < CMP_EPSILON2) {
			right = up.cross(back);
		}
		if (back.length_squared() < CMP_EPSILON2) {
			back = right.cross(up);
		}
		return Basis(right.normalized(), up, back.normalized());
	}

	static _FORCE_INLINE_ Basis facing_basis(Mode p_mode, const Basis &p_camera_basis) {
		switch (p_mode) {
			case MODE_ENABLED:
				return p_camera_basis;
			case MODE_FIXED_Y:
				return fixed_y_basis(p_camera_basis);
			case MODE_PARTICLES:
				return Basis(p_camera_basis.get_column(0).normalized(), p_camera_basis.get_column(1).normalized(), p_camera_basis.get_column(2).normalized());
			case MODE_DISABLED:
				break;
		}
		return Basis();
	}

	static _FORCE_INLINE_ Basis particle_rotation(real_t p_rotation) {
		const real_t c = Math::cos(p_rotation);
		const real_t s = Math::sin(p_rotation);
		return Basis(Vector3(c, -s, 0), Vector3(s, c, 0), Vector3(0, 0, 1));
	}

	struct FixedSizeScale {
		Vector3 gradient;
		real_t offset = 1.0;

		_FORCE_INLINE_ real_t at(const Vector3 &p_origin) const { return gradient.dot(p_origin) + offset; }
	};

	static _FORCE_INLINE_ FixedSizeScale fixed_size_scale(const Camera &p_camera) {
		FixedSizeScale result;
		if (p_camera.orthogonal) {
			result.offset = Math::abs(1.0f / (2.0f * p_camera.projection_y_scale)) * 2.0f;
		} else {
			const Transform3D view = p_camera.transform.affine_inverse();
			result.gradient = -view.basis.rows[2];
			result.offset = -view.origin.z;
		}
		return result;
	}

	// Matches the raster vertex stage; p_rotation is INSTANCE_CUSTOM.x (particles mode only).
	static Transform3D compute_world_transform(const Settings &p_settings, const Transform3D &p_model, const Camera &p_camera, real_t p_rotation = 0.0) {
		Transform3D world = p_model;

		if (p_settings.mode != MODE_DISABLED) {
			world.basis = facing_basis(p_settings.mode, p_camera.transform.basis);
			if (p_settings.mode == MODE_PARTICLES) {
				world.basis = world.basis * particle_rotation(p_rotation);
			}
			if (p_settings.keep_scale) {
				world.basis.scale_local(p_model.basis.get_scale_abs());
			}
		}

		if (p_settings.fixed_size) {
			const real_t scale = fixed_size_scale(p_camera).at(world.origin);
			world.basis.scale(Vector3(scale, scale, scale));
		}

		return world;
	}
};
