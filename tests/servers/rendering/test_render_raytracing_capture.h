/**************************************************************************/
/*  test_render_raytracing_capture.h                                      */
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

#include "core/config/engine.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_capture.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"

#include "tests/test_macros.h"

namespace TestRenderRaytracingCapture {

static RTVertexCapture::ShaderTraits make_traits(RTVertexCapture::ShaderOverride p_override, bool p_writes_vertex, bool p_writes_modelview, bool p_trails) {
	RTVertexCapture::ShaderTraits traits;
	traits.override_mode = p_override;
	traits.writes_vertex = p_writes_vertex;
	traits.writes_modelview = p_writes_modelview;
	traits.particle_trails = p_trails;
	return traits;
}

TEST_CASE("[RTVertexCapture] needs_capture truth table") {
	// Bit (writes_vertex | writes_modelview << 1 | particle_trails << 2) is set when the combination is captured.
	struct Row {
		RTVertexCapture::Mode mode;
		RTVertexCapture::ShaderOverride override_mode;
		uint8_t captured;
	};
	const Row rows[] = {
		{ RTVertexCapture::MODE_AUTOMATIC, RTVertexCapture::OVERRIDE_AUTO, 0b11111110 },
		{ RTVertexCapture::MODE_AUTOMATIC, RTVertexCapture::OVERRIDE_DYNAMIC, 0b11111111 },
		{ RTVertexCapture::MODE_AUTOMATIC, RTVertexCapture::OVERRIDE_STATIC, 0b00000000 },
		{ RTVertexCapture::MODE_OPT_IN, RTVertexCapture::OVERRIDE_AUTO, 0b11110000 },
		{ RTVertexCapture::MODE_OPT_IN, RTVertexCapture::OVERRIDE_DYNAMIC, 0b11111111 },
		{ RTVertexCapture::MODE_OPT_IN, RTVertexCapture::OVERRIDE_STATIC, 0b00000000 },
		{ RTVertexCapture::MODE_DISABLED, RTVertexCapture::OVERRIDE_AUTO, 0b00000000 },
		{ RTVertexCapture::MODE_DISABLED, RTVertexCapture::OVERRIDE_DYNAMIC, 0b00000000 },
		{ RTVertexCapture::MODE_DISABLED, RTVertexCapture::OVERRIDE_STATIC, 0b00000000 },
		{ RTVertexCapture::MODE_AUTOMATIC, RTVertexCapture::OVERRIDE_FROZEN, 0b11111111 },
		{ RTVertexCapture::MODE_OPT_IN, RTVertexCapture::OVERRIDE_FROZEN, 0b11111111 },
		{ RTVertexCapture::MODE_DISABLED, RTVertexCapture::OVERRIDE_FROZEN, 0b00000000 },
	};
	for (const Row &row : rows) {
		for (int combination = 0; combination < 8; combination++) {
			const RTVertexCapture::ShaderTraits traits = make_traits(row.override_mode, (combination & 1) != 0, (combination & 2) != 0, (combination & 4) != 0);
			const bool expected = ((row.captured >> combination) & 1) != 0;
			CHECK_MESSAGE(RTVertexCapture::needs_capture(traits, row.mode) == expected, vformat("mode %d, override %d, combination %d", int(row.mode), int(row.override_mode), combination));
		}
	}
}

TEST_CASE("[RTVertexCapture] Layout sizes follow the deformed vertex buffer layout") {
	RTVertexCapture::Layout layout;
	layout.vertex_count = 10;
	CHECK(layout.tbn_stride() == 0);
	CHECK(layout.total_bytes() == 120);

	layout.has_tangent = true;
	CHECK_MESSAGE(layout.tbn_stride() == 0, "Tangents are only stored together with normals.");

	layout.has_normal = true;
	CHECK(layout.tbn_stride() == 8);
	CHECK(layout.position_bytes() == 120);
	CHECK(layout.total_bytes() == 200);

	layout.has_tangent = false;
	layout.instance_count = 3;
	CHECK(layout.point_count() == 30);
	CHECK(layout.tbn_stride() == 4);
	CHECK(layout.total_bytes() == 30 * 16);
}

TEST_CASE("[RTVertexCapture] Draw parameter offsets and flags") {
	RTVertexCapture::Layout layout;
	layout.vertex_count = 4;
	layout.instance_count = 2;
	layout.has_normal = true;
	layout.has_tangent = true;

	const RTVertexCapture::DrawParams params = RTVertexCapture::make_draw_params(layout, 256, Transform3D(), Transform3D(), Vector3(1.0, -2.0, 3.0));
	CHECK(params.position_offset == 64);
	CHECK(params.tbn_offset == (256 + 8 * 12) / 4);
	CHECK(params.vertex_count == 4);
	CHECK(params.flags == (RTVertexCapture::DRAW_FLAG_NORMAL | RTVertexCapture::DRAW_FLAG_TANGENT));

	layout.has_normal = false;
	CHECK_MESSAGE(RTVertexCapture::make_draw_params(layout, 0, Transform3D(), Transform3D(), Vector3()).flags == 0, "No TBN block without normals.");
}

static Vector3 apply_view_to_local(const RTVertexCapture::DrawParams &p_params, const Vector3 &p_view, real_t p_w) {
	Vector3 result;
	for (int row = 0; row < 3; row++) {
		const float *r = &p_params.view_to_local[row * 4];
		result[row] = r[0] * p_view.x + r[1] * p_view.y + r[2] * p_view.z + r[3] * p_w;
	}
	return result;
}

TEST_CASE("[RTVertexCapture] view_to_local inverts the raster modelview") {
	const Transform3D local_to_world(Basis(Vector3(0.3, 1.0, -0.2).normalized(), 0.7).scaled(Vector3(2.0, 0.5, 3.0)), Vector3(4.0, -1.0, 2.5));
	const Transform3D camera = Transform3D(Basis(Vector3(0.0, 1.0, 0.0), 1.1), Vector3(-3.0, 2.0, 8.0)).looking_at(Vector3(4.0, -1.0, 2.5));
	const RTVertexCapture::DrawParams params = RTVertexCapture::make_draw_params(RTVertexCapture::Layout(), 0, local_to_world, camera, Vector3());
	const Transform3D modelview = camera.affine_inverse() * local_to_world;

	const Vector3 points[] = { Vector3(), Vector3(1.0, 2.0, -3.0), Vector3(-0.25, 0.5, 4.0) };
	for (const Vector3 &local : points) {
		CHECK(apply_view_to_local(params, modelview.xform(local), 1.0).distance_to(local) < 1e-4);
	}

	// The raster pass maps normals with the inverse transpose; the hit shader maps captured ones with the model basis.
	const Basis modelview_normal = camera.basis.inverse() * local_to_world.basis.inverse().transposed();
	const Vector3 local_normal = Vector3(0.2, 0.9, -0.4).normalized();
	const Vector3 captured = apply_view_to_local(params, modelview_normal.xform(local_normal), 0.0);
	const Vector3 hit_world = local_to_world.basis.xform(captured).normalized();
	const Vector3 raster_world = local_to_world.basis.inverse().transposed().xform(local_normal).normalized();
	CHECK(hit_world.distance_to(raster_world) < 1e-4);
}

TEST_CASE("[RTVertexCapture] A shader with only the fixed-function billboard isn't captured") {
	CHECK_FALSE(RTVertexCapture::needs_capture(RTVertexCapture::ShaderTraits(), RTVertexCapture::MODE_AUTOMATIC));
	CHECK_FALSE(RTVertexCapture::needs_capture(RTVertexCapture::ShaderTraits(), RTVertexCapture::MODE_OPT_IN));
}

static RTVertexCapture::Stamps make_stamps() {
	RTVertexCapture::Stamps stamps;
	stamps.frame = 10;
	stamps.global_uniforms = 15;
	stamps.camera = 20;
	stamps.model_transform = 30;
	stamps.instance_uniforms = 40;
	stamps.material = 50;
	stamps.deformation = 60;
	stamps.source = 70;
	stamps.mode_version = 1;
	return stamps;
}

TEST_CASE("[RTVertexCapture] First capture and structural changes rebuild") {
	const RTVertexCapture::Stamps previous = make_stamps();
	CHECK(RTVertexCapture::needs_recapture(false, previous, previous, 0) == RTVertexCapture::RECAPTURE_REBUILD);
	CHECK(RTVertexCapture::needs_recapture(false, previous, previous, RTVertexCapture::DEP_ALL) == RTVertexCapture::RECAPTURE_REBUILD);

	RTVertexCapture::Stamps mode_changed = previous;
	mode_changed.mode_version++;
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, mode_changed, 0) == RTVertexCapture::RECAPTURE_REBUILD, "A vertex_capture_mode change drops the capture.");

	RTVertexCapture::Stamps source_changed = previous;
	source_changed.source++;
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, source_changed, 0) == RTVertexCapture::RECAPTURE_REBUILD, "A mesh surface or shader change rebuilds.");
}

TEST_CASE("[RTVertexCapture] Captures without dependencies are kept") {
	const RTVertexCapture::Stamps previous = make_stamps();
	RTVertexCapture::Stamps everything_changed = previous;
	everything_changed.frame++;
	everything_changed.global_uniforms++;
	everything_changed.camera++;
	everything_changed.model_transform++;
	everything_changed.instance_uniforms++;
	everything_changed.material++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, previous, RTVertexCapture::DEP_ALL) == RTVertexCapture::RECAPTURE_NONE);
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, everything_changed, 0) == RTVertexCapture::RECAPTURE_NONE, "Inputs vertex() doesn't read don't matter.");
}

// Each dependency bit and the stamp of its input.
struct DependencyStamp {
	uint32_t dependency;
	uint64_t RTVertexCapture::Stamps::*stamp;
};

static const DependencyStamp dependency_stamps[] = {
	{ RTVertexCapture::DEP_TIME, &RTVertexCapture::Stamps::frame },
	{ RTVertexCapture::DEP_GLOBAL_UNIFORMS, &RTVertexCapture::Stamps::global_uniforms },
	{ RTVertexCapture::DEP_CAMERA, &RTVertexCapture::Stamps::camera },
	{ RTVertexCapture::DEP_MODEL_TRANSFORM, &RTVertexCapture::Stamps::model_transform },
	{ RTVertexCapture::DEP_INSTANCE_UNIFORMS, &RTVertexCapture::Stamps::instance_uniforms },
	{ RTVertexCapture::DEP_MATERIAL_UNIFORMS, &RTVertexCapture::Stamps::material },
};

TEST_CASE("[RTVertexCapture] Each dependency recaptures only when its input changed") {
	const RTVertexCapture::Stamps previous = make_stamps();
	for (const DependencyStamp &row : dependency_stamps) {
		RTVertexCapture::Stamps current = previous;
		current.*row.stamp += 1;
		const uint32_t changed = RTVertexCapture::changed_dependencies(previous, current);
		CHECK((changed & row.dependency) != 0);
		const String name = vformat("dependency %d", row.dependency);
		CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, row.dependency) == RTVertexCapture::RECAPTURE_UPDATE, name);
		CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, uint32_t(RTVertexCapture::DEP_ALL) & ~changed) == RTVertexCapture::RECAPTURE_NONE, name);
		CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, previous, row.dependency) == RTVertexCapture::RECAPTURE_NONE, name);
	}
}

TEST_CASE("[RTVertexCapture] Multiple dependencies") {
	const RTVertexCapture::Stamps previous = make_stamps();
	const uint32_t dependencies = RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_INSTANCE_UNIFORMS;
	RTVertexCapture::Stamps current = previous;
	current.material++;
	current.model_transform++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_NONE);
	current.instance_uniforms++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_UPDATE);
	current = previous;
	current.camera++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_UPDATE);
	CHECK(RTVertexCapture::changed_dependencies(previous, current) == RTVertexCapture::DEP_CAMERA);
}

TEST_CASE("[RTVertexCapture] Deformation recaptures every shader") {
	const RTVertexCapture::Stamps previous = make_stamps();
	RTVertexCapture::Stamps current = previous;
	current.deformation++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, 0) == RTVertexCapture::RECAPTURE_UPDATE);
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, RTVertexCapture::DEP_MATERIAL_UNIFORMS) == RTVertexCapture::RECAPTURE_UPDATE);
}

TEST_CASE("[RTVertexCapture] Instance record changes recapture every shader") {
	const RTVertexCapture::Stamps previous = make_stamps();
	RTVertexCapture::Stamps current = previous;
	current.instances++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, 0) == RTVertexCapture::RECAPTURE_UPDATE);
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, RTVertexCapture::DEP_ALL) == RTVertexCapture::RECAPTURE_UPDATE);
	CHECK(RTVertexCapture::changed_dependencies(previous, current) == 0);
	current = previous;
	current.frame++;
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, 0) == RTVertexCapture::RECAPTURE_NONE, "A static MultiMesh is captured once.");
}

TEST_CASE("[RTVertexCapture] Point index of an instanced capture matches the merged MultiMesh layout") {
	RTVertexCapture::Layout layout;
	layout.vertex_count = 5;
	layout.instance_count = 3;
	layout.has_normal = true;
	CHECK(layout.point_index(0, 0) == 0);
	CHECK(layout.point_index(0, 4) == 4);
	CHECK(layout.point_index(1, 0) == 5);
	CHECK(layout.point_index(2, 4) == layout.point_count() - 1);

	// Merged buffer: [float3 position x N*V][TBN x N*V]; a point's TBN follows all positions at the same index.
	const RTVertexCapture::DrawParams params = RTVertexCapture::make_draw_params(layout, 64, Transform3D(), Transform3D(), Vector3());
	const uint64_t point = layout.point_index(2, 1);
	CHECK(params.position_offset + point * 3 == (64 + point * 12) / 4);
	CHECK(params.tbn_offset + point * (layout.tbn_stride() / 4) == (64 + layout.position_bytes() + point * layout.tbn_stride()) / 4);
}

TEST_CASE("[RTVertexCapture] Drawn instances of MultiMeshes and particle trails") {
	const RTVertexCapture::Instances plain = RTVertexCapture::drawn_instances(12, 20, 1, false);
	CHECK(plain.count == 12);
	CHECK(plain.records_per_instance == 1);
	CHECK(plain.stride == 20);

	const RTVertexCapture::Instances trails = RTVertexCapture::drawn_instances(12, 20, 4, true);
	CHECK_MESSAGE(trails.count == 3, "A particle_trails shader draws one instance per particle.");
	CHECK(trails.records_per_instance == 4);
	CHECK(trails.stride == 80);

	const RTVertexCapture::Instances trail_records = RTVertexCapture::drawn_instances(12, 20, 4, false);
	CHECK_MESSAGE(trail_records.count == 12, "Other shaders draw every trail record as an instance.");
	CHECK(trail_records.stride == 20);

	CHECK(RTVertexCapture::drawn_instances(12, 20, 1, true).count == 12);
}

static RTVertexCapture::ShaderTraits make_traits_with_dependencies(RTVertexCapture::ShaderOverride p_override, uint32_t p_dependencies) {
	RTVertexCapture::ShaderTraits traits = make_traits(p_override, true, false, false);
	traits.dependencies.mask = p_dependencies;
	return traits;
}

TEST_CASE("[RTVertexCapture] Camera-dependent billboards add camera and model dependencies") {
	const RTVertexCapture::ShaderOverride overrides[] = { RTVertexCapture::OVERRIDE_AUTO, RTVertexCapture::OVERRIDE_DYNAMIC };
	for (const RTVertexCapture::ShaderOverride override_mode : overrides) {
		CHECK(RTVertexCapture::surface_dependencies(make_traits_with_dependencies(override_mode, 0), false) == 0);
		CHECK(RTVertexCapture::surface_dependencies(make_traits_with_dependencies(override_mode, RTVertexCapture::DEP_TIME), false) == RTVertexCapture::DEP_TIME);
		CHECK(RTVertexCapture::surface_dependencies(make_traits_with_dependencies(override_mode, RTVertexCapture::DEP_TIME), true) == (RTVertexCapture::DEP_TIME | RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_MODEL_TRANSFORM));
	}
}

TEST_CASE("[RTVertexCapture] Frozen surfaces ignore every vertex() input") {
	const RTVertexCapture::ShaderTraits frozen = make_traits_with_dependencies(RTVertexCapture::OVERRIDE_FROZEN, RTVertexCapture::DEP_ALL);
	CHECK(RTVertexCapture::surface_dependencies(frozen, false) == 0);
	const uint32_t dependencies = RTVertexCapture::surface_dependencies(frozen, true);
	CHECK_MESSAGE((dependencies & RTVertexCapture::DEP_CAMERA) == 0, "Frozen camera billboards share one capture between viewports.");

	const RTVertexCapture::Stamps previous = make_stamps();
	RTVertexCapture::Stamps everything_changed = previous;
	for (const DependencyStamp &row : dependency_stamps) {
		RTVertexCapture::Stamps current = previous;
		current.*row.stamp += 1;
		everything_changed.*row.stamp += 1;
		CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_NONE, vformat("dependency %d", row.dependency));
	}
	CHECK(RTVertexCapture::needs_recapture(true, previous, everything_changed, dependencies) == RTVertexCapture::RECAPTURE_NONE);
}

TEST_CASE("[RTVertexCapture] Frozen surfaces still follow their source, deformation and instances") {
	const uint32_t dependencies = RTVertexCapture::surface_dependencies(make_traits_with_dependencies(RTVertexCapture::OVERRIDE_FROZEN, RTVertexCapture::DEP_ALL), true);
	const RTVertexCapture::Stamps previous = make_stamps();
	CHECK(RTVertexCapture::needs_recapture(false, previous, previous, dependencies) == RTVertexCapture::RECAPTURE_REBUILD);

	RTVertexCapture::Stamps current = previous;
	current.source++;
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_REBUILD, "Mesh, shader code and instance count changes rebuild.");
	current = previous;
	current.mode_version++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_REBUILD);
	current = previous;
	current.deformation++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_UPDATE);
	current = previous;
	current.instances++;
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, dependencies) == RTVertexCapture::RECAPTURE_UPDATE);
}

// Global uniform buffer stand-in: name -> floats; sampler globals aren't needed here.
using GlobalStore = HashMap<StringName, Vector<float>>;

static uint64_t globals_stamp(const Vector<StringName> &p_names, const GlobalStore &p_store) {
	return RTVertexCapture::global_uniforms_stamp(p_names, [&p_store](const StringName &p_name) {
		const Vector<float> *values = p_store.getptr(p_name);
		return RTVertexCapture::GlobalUniformValue{ values ? Span<const float>(values->ptr(), values->size()) : Span<const float>() };
	});
}

// Whether a surface reading the globals p_names, captured with p_before, is captured again with p_after.
static RTVertexCapture::Recapture recapture_for_globals(const Vector<StringName> &p_names, const GlobalStore &p_before, const GlobalStore &p_after) {
	RTVertexCapture::Stamps previous = make_stamps();
	previous.global_uniforms = globals_stamp(p_names, p_before);
	RTVertexCapture::Stamps current = previous;
	current.frame++;
	current.global_uniforms = globals_stamp(p_names, p_after);
	return RTVertexCapture::needs_recapture(true, previous, current, RTVertexCapture::DEP_GLOBAL_UNIFORMS);
}

TEST_CASE("[RTVertexCapture] Global uniforms recapture only when a global vertex() reads changes") {
	const Vector<StringName> read = { "wind", "gust" };
	GlobalStore before;
	before["wind"] = { 1.0f, 0.0f, 0.0f, 0.0f };
	before["gust"] = { 0.5f, 0.0f, 0.0f, 0.0f };
	before["unread"] = { 2.0f, 0.0f, 0.0f, 0.0f };

	CHECK_MESSAGE(recapture_for_globals(read, before, before) == RTVertexCapture::RECAPTURE_NONE, "Unchanged globals keep the capture across frames.");

	GlobalStore changed = before;
	changed["gust"].write[0] = 0.75f;
	CHECK(recapture_for_globals(read, before, changed) == RTVertexCapture::RECAPTURE_UPDATE);

	GlobalStore unread_changed = before;
	unread_changed["unread"].write[0] = 3.0f;
	CHECK_MESSAGE(recapture_for_globals(read, before, unread_changed) == RTVertexCapture::RECAPTURE_NONE, "Globals vertex() doesn't read don't matter.");

	GlobalStore removed = before;
	removed.erase("wind");
	CHECK_MESSAGE(recapture_for_globals(read, before, removed) == RTVertexCapture::RECAPTURE_UPDATE, "A removed global changes what vertex() reads.");

	CHECK(globals_stamp(Vector<StringName>(), before) == globals_stamp(Vector<StringName>(), changed));
}

TEST_CASE("[RTVertexCapture] Frames alone don't recapture global uniform dependencies") {
	const RTVertexCapture::Stamps previous = make_stamps();
	RTVertexCapture::Stamps current = previous;
	current.frame++;
	CHECK(RTVertexCapture::changed_dependencies(previous, current) == RTVertexCapture::DEP_TIME);
	CHECK(RTVertexCapture::needs_recapture(true, previous, current, RTVertexCapture::DEP_GLOBAL_UNIFORMS) == RTVertexCapture::RECAPTURE_NONE);
	CHECK_MESSAGE(RTVertexCapture::needs_recapture(true, previous, current, RTVertexCapture::DEP_ALL) == RTVertexCapture::RECAPTURE_UPDATE,
			"Code that doesn't parse depends on everything, so it is still recaptured every frame.");
}

static RTVertexCapture::VertexDependencies vertex_dependencies(const String &p_code) {
	return RendererSceneRenderImplementation::SceneShaderRaytracing::compute_vertex_capture_dependencies("shader_type spatial;\n" + p_code);
}

static uint32_t deps(const String &p_code) {
	return vertex_dependencies(p_code).mask;
}

// ShaderLanguage reads RSG::utilities, which only a live RenderingServer keeps valid.
TEST_CASE("[RTVertexCapture][SceneTree] rt_vertex render modes are one exclusive group") {
	CHECK(deps("render_mode rt_vertex_auto;\nvoid vertex() {}") == 0);
	CHECK(deps("render_mode rt_vertex_dynamic, billboard;\nvoid vertex() {}") == 0);
	CHECK(deps("render_mode rt_vertex_static;\nvoid vertex() {}") == 0);
	CHECK(deps("render_mode rt_vertex_frozen;\nvoid vertex() {}") == 0);
	CHECK(deps("render_mode rt_vertex_dynamic, rt_vertex_static;\nvoid vertex() {}") == RTVertexCapture::DEP_ALL);
	CHECK(deps("render_mode rt_vertex_frozen, rt_vertex_dynamic;\nvoid vertex() {}") == RTVertexCapture::DEP_ALL);
	CHECK(deps("render_mode rt_vertex_auto, rt_vertex_dynamic;\nvoid vertex() {}") == RTVertexCapture::DEP_ALL);
}

TEST_CASE("[RTVertexCapture][SceneTree] TIME dependencies") {
	CHECK(deps("void vertex() { VERTEX.y += sin(TIME); }") == RTVertexCapture::DEP_TIME);
	CHECK(deps(R"(
float wave(float x) { return sin(x + TIME); }
void vertex() { VERTEX.y += wave(VERTEX.x); }
)") == RTVertexCapture::DEP_TIME);
	CHECK_MESSAGE(deps(R"(
float inner() { return TIME; }
float outer() { return inner() + inner(); }
void vertex() { VERTEX.y += outer(); }
)") == RTVertexCapture::DEP_TIME,
			"Calls are followed transitively.");
	CHECK_MESSAGE(deps(R"(
float pulse() { return sin(TIME); }
void vertex() {}
void fragment() { ALBEDO = vec3(pulse()); }
)") == 0,
			"Functions only reached from fragment() don't count.");
}

TEST_CASE("[RTVertexCapture][SceneTree] Camera and model dependencies") {
	CHECK(deps("void vertex() { VERTEX += INV_VIEW_MATRIX[2].xyz; }") == RTVertexCapture::DEP_CAMERA);
	CHECK(deps("void vertex() { VERTEX += CAMERA_POSITION_WORLD * 0.0; }") == RTVertexCapture::DEP_CAMERA);
	CHECK(deps("void vertex() { MODELVIEW_MATRIX = mat4(1.0); }") == (RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_MODEL_TRANSFORM));
	CHECK(deps("void vertex() { VERTEX += NODE_POSITION_WORLD * 0.0; }") == RTVertexCapture::DEP_MODEL_TRANSFORM);
	CHECK(deps("void vertex() { VERTEX = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz; }") == RTVertexCapture::DEP_MODEL_TRANSFORM);
	CHECK_MESSAGE(deps("render_mode world_vertex_coords;\nvoid vertex() { VERTEX.y += 1.0; }") == RTVertexCapture::DEP_MODEL_TRANSFORM,
			"World-space vertex() output depends on the model transform it is mapped back through.");
	CHECK_MESSAGE(deps("render_mode skip_vertex_transform;\nvoid vertex() { VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz; }") == (RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_MODEL_TRANSFORM),
			"View-space vertex() output reads the modelview matrix.");
}

TEST_CASE("[RTVertexCapture][SceneTree] Camera-facing game effects") {
	// pvkk flare_glow: grows toward the main camera, scales with its distance and flickers.
	CHECK(deps(R"(
uniform float grow;
uniform float flicker_speed;
void vertex() {
	vec3 camera_pos = MAIN_CAM_INV_VIEW_MATRIX[3].xyz;
	vec3 object_pos = MODEL_MATRIX[3].xyz;
	float flicker = sin(dot(NODE_POSITION_WORLD, vec3(100.0)) + TIME * flicker_speed);
	vec3 offset_position = object_pos + normalize(camera_pos - object_pos) * grow;
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(MAIN_CAM_INV_VIEW_MATRIX[0], MAIN_CAM_INV_VIEW_MATRIX[1], MAIN_CAM_INV_VIEW_MATRIX[2], vec4(offset_position, 1.0));
	MODELVIEW_MATRIX = MODELVIEW_MATRIX * mat4(vec4(flicker, 0.0, 0.0, 0.0), vec4(0.0, flicker, 0.0, 0.0), vec4(0.0, 0.0, flicker, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
}
)") == (RTVertexCapture::DEP_TIME | RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_MODEL_TRANSFORM | RTVertexCapture::DEP_MATERIAL_UNIFORMS));
	// pvkk lightbeam_optimized: turned around its Z axis to face the camera.
	CHECK(deps(R"(
void vertex() {
	vec3 local_z = normalize(MODEL_MATRIX[2].xyz);
	vec3 local_x = normalize(cross(normalize(CAMERA_POSITION_WORLD - NODE_POSITION_WORLD), local_z));
	vec3 local_y = normalize(cross(local_z, local_x));
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(vec4(local_x * length(MODEL_MATRIX[0].xyz), 0.0), vec4(local_y * length(MODEL_MATRIX[1].xyz), 0.0), MODEL_MATRIX[2], MODEL_MATRIX[3]);
}
)") == (RTVertexCapture::DEP_CAMERA | RTVertexCapture::DEP_MODEL_TRANSFORM));
}

TEST_CASE("[RTVertexCapture][SceneTree] Uniform dependencies") {
	CHECK(deps("instance uniform float height;\nvoid vertex() { VERTEX.y += height; }") == RTVertexCapture::DEP_INSTANCE_UNIFORMS);
	CHECK(deps("uniform float amount;\nvoid vertex() { VERTEX.y += amount; }") == RTVertexCapture::DEP_MATERIAL_UNIFORMS);
	CHECK(deps("uniform sampler2D height_map;\nvoid vertex() { VERTEX.y += texture(height_map, UV).r; }") == RTVertexCapture::DEP_MATERIAL_UNIFORMS);
	CHECK_MESSAGE(deps("uniform float amount;\nvoid vertex() {}\nvoid fragment() { ALBEDO = vec3(amount); }") == 0,
			"Uniforms only read in fragment() don't count.");

	// Global uniforms are type-checked against the project in the editor; an unknown one fails the parse conservatively.
	const uint32_t global_deps = deps("global uniform float wind;\nvoid vertex() { VERTEX.x += wind; }");
	CHECK((global_deps & RTVertexCapture::DEP_GLOBAL_UNIFORMS) != 0);
	if (!Engine::get_singleton()->is_editor_hint()) {
		CHECK(global_deps == RTVertexCapture::DEP_GLOBAL_UNIFORMS);
	}
}

TEST_CASE("[RTVertexCapture][SceneTree] Global uniforms read by vertex() are collected through function calls") {
	const RTVertexCapture::VertexDependencies unparsed = vertex_dependencies("void vertex() { VERTEX += ; }");
	CHECK(unparsed.mask == RTVertexCapture::DEP_ALL);
	CHECK(unparsed.global_uniforms.is_empty());

	// The editor type-checks global uniforms against the project, where these don't exist.
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const RTVertexCapture::VertexDependencies dependencies = vertex_dependencies(R"(
global uniform float wind;
global uniform float gust;
global uniform vec4 tint;
float gusting() { return gust * wind; }
float sway() { return wind + gusting(); }
void vertex() { VERTEX.x += sway(); }
void fragment() { ALBEDO = tint.rgb; }
)");
	CHECK(dependencies.mask == RTVertexCapture::DEP_GLOBAL_UNIFORMS);
	CHECK_MESSAGE(dependencies.global_uniforms == Vector<StringName>({ "wind", "gust" }), "Each global once, in first-read order; tint is only read by fragment().");
}

TEST_CASE("[RTVertexCapture][SceneTree] Static vertex code and parse errors") {
	CHECK(deps("void vertex() { VERTEX *= 2.0; UV = UV * 0.5; }") == 0);
	CHECK(deps("void fragment() { ALBEDO = vec3(1.0); }") == 0);
	CHECK(deps("void vertex() { VERTEX += ; }") == RTVertexCapture::DEP_ALL);
	CHECK(deps("void vertex() { VERTEX += undefined_identifier; }") == RTVertexCapture::DEP_ALL);
}

TEST_CASE("[RTVertexCapture][SceneTree] BaseMaterial3D grow only depends on material uniforms") {
	CHECK(deps(R"(
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;
uniform vec4 albedo : source_color;
uniform sampler2D texture_albedo : source_color, filter_linear_mipmap, repeat_enable;
uniform float grow;
uniform vec3 uv1_scale;
uniform vec3 uv1_offset;
void vertex() {
	UV = UV * uv1_scale.xy + uv1_offset.xy;
	VERTEX += NORMAL * grow;
}
void fragment() {
	vec4 albedo_tex = texture(texture_albedo, UV);
	ALBEDO = albedo.rgb * albedo_tex.rgb;
}
)") == RTVertexCapture::DEP_MATERIAL_UNIFORMS);
}

TEST_CASE("[SceneShaderRaytracing] Instance uniforms and samplers are not material buffer members") {
	using Uniform = ShaderLanguage::ShaderNode::Uniform;
	using RendererSceneRenderImplementation::SceneShaderRaytracing;
	const Vector<uint32_t> offsets = { 0, 16 };
	Uniform local;
	local.type = ShaderLanguage::TYPE_VEC4;
	local.order = 1;
	Uniform global;
	global.type = ShaderLanguage::TYPE_FLOAT;
	global.scope = Uniform::SCOPE_GLOBAL;
	global.order = 0;
	// The parser leaves an instance uniform's order at 0, the first material uniform's slot.
	Uniform instance;
	instance.type = ShaderLanguage::TYPE_VEC3;
	instance.scope = Uniform::SCOPE_INSTANCE;
	Uniform sampler;
	sampler.type = ShaderLanguage::TYPE_SAMPLER2D;
	sampler.order = -1;
	Uniform past_offsets = local;
	past_offsets.order = offsets.size();

	CHECK(SceneShaderRaytracing::is_material_buffer_uniform(local, offsets));
	CHECK(SceneShaderRaytracing::is_material_buffer_uniform(global, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(instance, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(sampler, offsets));
	CHECK_FALSE(SceneShaderRaytracing::is_material_buffer_uniform(past_offsets, offsets));
}

} // namespace TestRenderRaytracingCapture
