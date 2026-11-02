/**************************************************************************/
/*  rt_procedural_instance_3d.cpp                                         */
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

#include "rt_procedural_instance_3d.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

void RTProceduralInstance3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_procedural();
		} break;
	}
}

void RTProceduralInstance3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &RTProceduralInstance3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &RTProceduralInstance3D::get_size);
	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &RTProceduralInstance3D::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &RTProceduralInstance3D::get_bounds);
	ClassDB::bind_method(D_METHOD("is_multi_aabb"), &RTProceduralInstance3D::is_multi_aabb);
	ClassDB::bind_method(D_METHOD("set_expose_aabb_bounds", "expose"), &RTProceduralInstance3D::set_expose_aabb_bounds);
	ClassDB::bind_method(D_METHOD("get_expose_aabb_bounds"), &RTProceduralInstance3D::get_expose_aabb_bounds);
	ClassDB::bind_method(D_METHOD("set_custom_enclosing_aabb", "aabb"), &RTProceduralInstance3D::set_custom_enclosing_aabb);
	ClassDB::bind_method(D_METHOD("get_custom_enclosing_aabb"), &RTProceduralInstance3D::get_custom_enclosing_aabb);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bounds", PROPERTY_HINT_ARRAY_TYPE, vformat("%s/%s:%s", Variant::AABB, PROPERTY_HINT_NONE, String())), "set_bounds", "get_bounds");
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "custom_enclosing_aabb"), "set_custom_enclosing_aabb", "get_custom_enclosing_aabb");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "expose_aabb_bounds"), "set_expose_aabb_bounds", "get_expose_aabb_bounds");
}

PackedFloat32Array RTProceduralInstance3D::_pack_aabb_data() const {
	const int count = bounds.size();
	if (count == 0) {
		return PackedFloat32Array();
	}
	PackedFloat32Array packed;
	packed.resize(count * 6);
	float *w = packed.ptrw();
	const AABB *b = bounds.ptr();
	for (int i = 0; i < count; i++) {
		const Vector3 mn = b[i].position;
		const Vector3 mx = b[i].position + b[i].size;
		w[i * 6 + 0] = mn.x;
		w[i * 6 + 1] = mn.y;
		w[i * 6 + 2] = mn.z;
		w[i * 6 + 3] = mx.x;
		w[i * 6 + 4] = mx.y;
		w[i * 6 + 5] = mx.z;
	}
	return packed;
}

AABB RTProceduralInstance3D::_compute_enclosing_aabb() const {
	if (custom_enclosing_aabb.has_volume()) {
		return custom_enclosing_aabb;
	}
	if (bounds.is_empty()) {
		return AABB(-size * 0.5, size);
	}
	const AABB *b = bounds.ptr();
	AABB enclosing = b[0];
	for (int i = 1; i < bounds.size(); i++) {
		enclosing.merge_with(b[i]);
	}
	return enclosing;
}

void RTProceduralInstance3D::_update_procedural() {
	if (!is_inside_tree()) {
		return;
	}

	AABB culling_aabb = _compute_enclosing_aabb();
	set_custom_aabb(culling_aabb);
	RenderingServer::get_singleton()->instance_set_rt_procedural(get_instance(), true, culling_aabb);

	PackedFloat32Array aabb_data = _pack_aabb_data();
	RenderingServer::get_singleton()->instance_set_rt_procedural_bounds(get_instance(), aabb_data, expose_aabb_bounds);
}

void RTProceduralInstance3D::_ensure_null_base_mesh() {
	// A zero-surface ArrayMesh is the minimum object required to make
	// RendererSceneCull classify this instance as INSTANCE_MESH so that
	// instance_set_rt_procedural() can attach procedural RT data to it.
	// With no surfaces: no vertex/index buffers, no surface caches, no BLAS
	// built by the mesh pipeline, and nothing added to raster render lists.
	if (null_base_mesh.is_valid()) {
		return;
	}
	null_base_mesh.instantiate();
	set_base(null_base_mesh->get_rid());
}

void RTProceduralInstance3D::set_size(const Vector3 &p_size) {
	size = p_size.abs();
	_ensure_null_base_mesh();
	_update_procedural();
	update_gizmos();
}

Vector3 RTProceduralInstance3D::get_size() const {
	return size;
}

void RTProceduralInstance3D::set_bounds(const TypedArray<AABB> &p_bounds) {
	const int count = p_bounds.size();
	bounds.resize(count);
	AABB *w = bounds.ptrw();
	for (int i = 0; i < count; i++) {
		w[i] = p_bounds[i];
	}
	_ensure_null_base_mesh();
	_update_procedural();
	update_gizmos();
}

TypedArray<AABB> RTProceduralInstance3D::get_bounds() const {
	TypedArray<AABB> out;
	const int count = bounds.size();
	out.resize(count);
	const AABB *r = bounds.ptr();
	for (int i = 0; i < count; i++) {
		out[i] = r[i];
	}
	return out;
}

bool RTProceduralInstance3D::is_multi_aabb() const {
	return !bounds.is_empty();
}

void RTProceduralInstance3D::set_custom_enclosing_aabb(const AABB &p_aabb) {
	custom_enclosing_aabb = p_aabb;
	_update_procedural();
	update_gizmos();
}

AABB RTProceduralInstance3D::get_custom_enclosing_aabb() const {
	return custom_enclosing_aabb;
}

void RTProceduralInstance3D::set_expose_aabb_bounds(bool p_expose) {
	expose_aabb_bounds = p_expose;
	_update_procedural();
}

bool RTProceduralInstance3D::get_expose_aabb_bounds() const {
	return expose_aabb_bounds;
}

AABB RTProceduralInstance3D::get_aabb() const {
	return _compute_enclosing_aabb();
}

RTProceduralInstance3D::RTProceduralInstance3D() {
	_ensure_null_base_mesh();
}

RTProceduralInstance3D::~RTProceduralInstance3D() {
}
