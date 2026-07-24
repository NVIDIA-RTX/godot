/**************************************************************************/
/*  rt_procedural_instance_3d.h                                           */
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

#include "core/variant/typed_array.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/mesh.h"

// Ray-traced procedural geometry node. Supports a single centered AABB (via size)
// or an array of sub-AABBs (via bounds). Intersection logic requires a ShaderMaterial
// with a void intersection() entry point assigned via material_override.
class RTProceduralInstance3D : public GeometryInstance3D {
	GDCLASS(RTProceduralInstance3D, GeometryInstance3D);

	Vector3 size = Vector3(1, 1, 1);
	Vector<AABB> bounds;
	AABB custom_enclosing_aabb; // Zero-size = auto-compute from bounds.
	bool expose_aabb_bounds = false;
	// Empty ArrayMesh to satisfy the INSTANCE_MESH base-type gate in RendererSceneCull.
	Ref<ArrayMesh> null_base_mesh;

	void _update_procedural();
	void _ensure_null_base_mesh();
	PackedFloat32Array _pack_aabb_data() const;
	AABB _compute_enclosing_aabb() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_bounds(const TypedArray<AABB> &p_bounds);
	TypedArray<AABB> get_bounds() const;
	bool is_multi_aabb() const;

	void set_custom_enclosing_aabb(const AABB &p_aabb);
	AABB get_custom_enclosing_aabb() const;

	void set_expose_aabb_bounds(bool p_expose);
	bool get_expose_aabb_bounds() const;

	virtual AABB get_aabb() const override;

	RTProceduralInstance3D();
	~RTProceduralInstance3D();
};
