/**************************************************************************/
/*  rt_procedural_instance_3d_gizmo_plugin.cpp                            */
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

#include "rt_procedural_instance_3d_gizmo_plugin.h"

#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/rt_procedural_instance_3d.h"

RTProceduralInstance3DGizmoPlugin::RTProceduralInstance3DGizmoPlugin() {
	helper.instantiate();
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/fog_volume");
	gizmo_color.g = 0.8f;
	gizmo_color.b = 1.0f;
	create_material("shape_material", gizmo_color);
	gizmo_color.a = 0.15f;
	create_material("shape_material_internal", gizmo_color);
	create_handle_material("handles");
}

bool RTProceduralInstance3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<RTProceduralInstance3D>(p_spatial) != nullptr;
}

String RTProceduralInstance3DGizmoPlugin::get_gizmo_name() const {
	return "RTProceduralInstance3D";
}

int RTProceduralInstance3DGizmoPlugin::get_priority() const {
	return -1;
}

String RTProceduralInstance3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant RTProceduralInstance3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	RTProceduralInstance3D *pi = Object::cast_to<RTProceduralInstance3D>(p_gizmo->get_node_3d());
	return pi->get_size();
}

void RTProceduralInstance3DGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void RTProceduralInstance3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	RTProceduralInstance3D *pi = Object::cast_to<RTProceduralInstance3D>(p_gizmo->get_node_3d());
	Vector3 size = pi->get_size();

	Vector3 sg[2];
	helper->get_segment(p_camera, p_point, sg);

	Vector3 position;
	helper->box_set_handle(sg, p_id, size, position);
	pi->set_size(size);
	pi->set_global_position(position);
}

void RTProceduralInstance3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	helper->box_commit_handle(TTR("Change RTProceduralInstance3D Size"), p_cancel, p_gizmo->get_node_3d());
}

void RTProceduralInstance3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	RTProceduralInstance3D *pi = Object::cast_to<RTProceduralInstance3D>(p_gizmo->get_node_3d());

	p_gizmo->clear();

	const Ref<Material> material = get_material("shape_material", p_gizmo);
	const Ref<Material> material_internal = get_material("shape_material_internal", p_gizmo);
	Ref<Material> handles_material = get_material("handles");

	Vector<Vector3> lines;

	if (pi->is_multi_aabb()) {
		const TypedArray<AABB> sub_bounds = pi->get_bounds();
		for (int n = 0; n < sub_bounds.size(); n++) {
			const AABB sub = sub_bounds[n];
			for (int i = 0; i < 12; i++) {
				Vector3 a, b;
				sub.get_edge(i, a, b);
				lines.push_back(a);
				lines.push_back(b);
			}
		}
	} else {
		AABB aabb;
		aabb.size = pi->get_size();
		aabb.position = aabb.size / -2;
		for (int i = 0; i < 12; i++) {
			Vector3 a, b;
			aabb.get_edge(i, a, b);
			lines.push_back(a);
			lines.push_back(b);
		}
	}

	Vector<Vector3> handles = helper->box_get_handles(pi->get_size());

	p_gizmo->add_lines(lines, material);
	p_gizmo->add_collision_segments(lines);
	p_gizmo->add_handles(handles, handles_material);
}
