/**************************************************************************/
/*  render_raytracing_capture.h                                           */
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
#include "core/string/string_name.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/span.h"
#include "core/templates/vector.h"

#include <cstdint>

// Decides which surfaces the path tracer builds from their captured raster vertex() output.
class RTVertexCapture {
public:
	enum Mode : uint8_t {
		MODE_AUTOMATIC,
		MODE_OPT_IN,
		MODE_DISABLED,
	};

	// Must match the option order of the rt_vertex render mode group.
	enum ShaderOverride : uint8_t {
		OVERRIDE_AUTO,
		OVERRIDE_DYNAMIC,
		OVERRIDE_STATIC,
		OVERRIDE_FROZEN, // Recaptured only when its source, deformation or instances change.
	};

	enum Dependency : uint32_t {
		DEP_TIME = 1 << 0,
		DEP_CAMERA = 1 << 1,
		DEP_MODEL_TRANSFORM = 1 << 2,
		DEP_INSTANCE_UNIFORMS = 1 << 3,
		DEP_MATERIAL_UNIFORMS = 1 << 4,
		DEP_GLOBAL_UNIFORMS = 1 << 5,
		DEP_ALL = (1 << 6) - 1,
		DEP_UNKNOWN = 0xFFFFFFFFu,
	};

	struct VertexDependencies {
		uint32_t mask = DEP_UNKNOWN;
		Vector<StringName> global_uniforms;
	};

	// Recorded regardless of the mode, so the mode can change at runtime without recompiling.
	struct ShaderTraits {
		ShaderOverride override_mode = OVERRIDE_AUTO;
		bool writes_vertex = false; // The injected billboard code doesn't count.
		bool writes_modelview = false; // PROJECTION_MATRIX writes don't count: captures are in view space.
		bool particle_trails = false;
		VertexDependencies dependencies;
		uint32_t code_version = 0;
	};

	// The injected billboard code reads the camera and model transform. Frozen shaders have no dependencies.
	static constexpr uint32_t surface_dependencies(const ShaderTraits &p_traits, bool p_camera_billboard) {
		if (p_traits.override_mode == OVERRIDE_FROZEN) {
			return 0;
		}
		return p_traits.dependencies.mask | (p_camera_billboard ? uint32_t(DEP_CAMERA | DEP_MODEL_TRANSFORM) : 0u);
	}

	struct Stamps {
		uint64_t frame = 0;
		uint64_t global_uniforms = 0;
		uint64_t camera = 0;
		uint64_t model_transform = 0;
		uint64_t instance_uniforms = 0;
		uint64_t instance_uniforms_version = UINT64_MAX; // Not compared; allows reusing instance_uniforms.
		uint64_t material = 0;
		uint64_t deformation = 0; // Compared for every shader.
		uint64_t instances = 0; // Compared for every shader.
		uint64_t source = 0; // Any change rebuilds.
		uint32_t mode_version = 0; // Any change rebuilds.
	};

	enum Recapture : uint8_t {
		RECAPTURE_NONE,
		RECAPTURE_UPDATE,
		RECAPTURE_REBUILD,
	};

	static constexpr uint32_t changed_dependencies(const Stamps &p_previous, const Stamps &p_current) {
		return (p_previous.frame != p_current.frame ? uint32_t(DEP_TIME) : 0u) |
				(p_previous.global_uniforms != p_current.global_uniforms ? uint32_t(DEP_GLOBAL_UNIFORMS) : 0u) |
				(p_previous.camera != p_current.camera ? uint32_t(DEP_CAMERA) : 0u) |
				(p_previous.model_transform != p_current.model_transform ? uint32_t(DEP_MODEL_TRANSFORM) : 0u) |
				(p_previous.instance_uniforms != p_current.instance_uniforms ? uint32_t(DEP_INSTANCE_UNIFORMS) : 0u) |
				(p_previous.material != p_current.material ? uint32_t(DEP_MATERIAL_UNIFORMS) : 0u);
	}

	static constexpr Recapture needs_recapture(bool p_has_capture, const Stamps &p_previous, const Stamps &p_current, uint32_t p_dependencies) {
		if (!p_has_capture || p_previous.mode_version != p_current.mode_version || p_previous.source != p_current.source) {
			return RECAPTURE_REBUILD;
		}
		if ((changed_dependencies(p_previous, p_current) & p_dependencies) != 0 || p_previous.deformation != p_current.deformation ||
				p_previous.instances != p_current.instances) {
			return RECAPTURE_UPDATE;
		}
		return RECAPTURE_NONE;
	}

	struct GlobalUniformValue {
		Span<const float> buffer;
		uint64_t texture = 0;
	};

	// Names are looked up on every call, so globals added, removed or retyped since are read correctly.
	template <typename GetValue>
	static uint64_t global_uniforms_stamp(const Vector<StringName> &p_names, const GetValue &p_get_value) {
		uint32_t lanes[2] = { HASH_MURMUR3_SEED, 0x9E3779B9u };
		for (const StringName &name : p_names) {
			const GlobalUniformValue value = p_get_value(name);
			const int bytes = int(value.buffer.size() * sizeof(float));
			for (uint32_t &lane : lanes) {
				lane = hash_murmur3_one_64(value.texture, hash_murmur3_one_32(uint32_t(bytes), hash_murmur3_buffer(value.buffer.ptr(), bytes, lane)));
			}
		}
		return (uint64_t(lanes[1]) << 32) | lanes[0];
	}

	static constexpr bool needs_capture(const ShaderTraits &p_traits, Mode p_mode) {
		if (p_mode == MODE_DISABLED || p_traits.override_mode == OVERRIDE_STATIC) {
			return false;
		}
		if (p_traits.override_mode == OVERRIDE_DYNAMIC || p_traits.override_mode == OVERRIDE_FROZEN || p_traits.particle_trails) {
			return true;
		}
		return p_mode == MODE_AUTOMATIC && (p_traits.writes_vertex || p_traits.writes_modelview);
	}

	// float3 positions (instance * V + vertex), then the packed TBN block when the mesh has normals.
	// Must match the deformed and merged MultiMesh vertex buffers BLASes are built from.
	struct Layout {
		uint32_t vertex_count = 0;
		uint32_t instance_count = 1;
		bool has_normal = false;
		bool has_tangent = false; // Only stored together with the normal.

		constexpr uint64_t point_count() const { return uint64_t(vertex_count) * instance_count; }
		constexpr uint64_t point_index(uint32_t p_instance, uint32_t p_vertex) const { return uint64_t(p_instance) * vertex_count + p_vertex; }
		constexpr uint32_t tbn_stride() const { return has_normal ? (has_tangent ? 8u : 4u) : 0u; }
		constexpr uint64_t position_bytes() const { return point_count() * 12u; }
		constexpr uint64_t total_bytes() const { return point_count() * (12u + tbn_stride()); }
	};

	struct Instances {
		uint32_t count = 0;
		uint32_t records_per_instance = 1;
		uint32_t stride = 0; // In floats.
	};

	// A particle_trails shader draws one instance per particle and reads its trail_steps consecutive records.
	static constexpr Instances drawn_instances(uint32_t p_records, uint32_t p_record_stride, uint32_t p_trail_steps, bool p_uses_trails) {
		const uint32_t per_instance = p_uses_trails && p_trail_steps > 1 ? p_trail_steps : 1u;
		return Instances{ p_records / per_instance, per_instance, p_record_stride * per_instance };
	}

	enum DrawFlags : uint32_t {
		DRAW_FLAG_NORMAL = 1u << 0,
		DRAW_FLAG_TANGENT = 1u << 1,
	};

	// Must match RTCaptureDraw in rt_capture_inc.glsl (std430).
	struct DrawParams {
		float view_to_local[12] = {};
		uint32_t position_offset = 0; // In words.
		uint32_t tbn_offset = 0; // In words.
		uint32_t vertex_count = 0;
		uint32_t flags = 0;
		float dead_position[4] = {}; // Where culled instances and non-finite points collapse to.
	};
	static_assert(sizeof(DrawParams) == 80, "Must match RTCaptureDraw in rt_capture_inc.glsl.");

	// p_arena_offset must be 4-byte aligned. p_dead_position is in capture space.
	static DrawParams make_draw_params(const Layout &p_layout, uint32_t p_arena_offset, const Transform3D &p_local_to_world, const Transform3D &p_camera, const Vector3 &p_dead_position) {
		DrawParams params;
		const Transform3D view_to_local = p_local_to_world.affine_inverse() * p_camera;
		for (int row = 0; row < 3; row++) {
			params.view_to_local[row * 4 + 0] = float(view_to_local.basis.rows[row][0]);
			params.view_to_local[row * 4 + 1] = float(view_to_local.basis.rows[row][1]);
			params.view_to_local[row * 4 + 2] = float(view_to_local.basis.rows[row][2]);
			params.view_to_local[row * 4 + 3] = float(view_to_local.origin[row]);
		}
		params.position_offset = p_arena_offset / 4u;
		params.tbn_offset = uint32_t((p_arena_offset + p_layout.position_bytes()) / 4u);
		params.vertex_count = p_layout.vertex_count;
		params.flags = (p_layout.has_normal ? uint32_t(DRAW_FLAG_NORMAL) : 0u) | (p_layout.has_normal && p_layout.has_tangent ? uint32_t(DRAW_FLAG_TANGENT) : 0u);
		params.dead_position[0] = float(p_dead_position.x);
		params.dead_position[1] = float(p_dead_position.y);
		params.dead_position[2] = float(p_dead_position.z);
		return params;
	}
};
