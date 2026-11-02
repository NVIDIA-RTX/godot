/**************************************************************************/
/*  test_concave_polygon_shape_3d.cpp                                     */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_concave_polygon_shape_3d)

#ifndef PHYSICS_3D_DISABLED

#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "tests/signal_watcher.h"

#include "modules/godot_physics_3d/godot_shape_3d.h"

namespace TestConcavePolygonShape3D {

static Vector<Vector3> make_one_triangle() {
	Vector<Vector3> f;
	f.push_back(Vector3(0, 0, 0));
	f.push_back(Vector3(1, 0, 0));
	f.push_back(Vector3(0, 0, 1));
	return f;
}

static Vector<Vector3> make_two_triangles() {
	Vector<Vector3> f;
	f.push_back(Vector3(0, 0, 0));
	f.push_back(Vector3(1, 0, 0));
	f.push_back(Vector3(1, 0, 1));
	f.push_back(Vector3(0, 0, 0));
	f.push_back(Vector3(1, 0, 1));
	f.push_back(Vector3(0, 0, 1));
	return f;
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] Constructor defaults") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	CHECK(shape->get_faces().is_empty());
	CHECK(shape->is_backface_collision_enabled() == false);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_faces / get_faces round-trip") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	Vector<Vector3> tri = make_one_triangle();
	shape->set_faces(tri);

	CHECK(shape->get_faces() == tri);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_faces emits changed signal") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	Array two_signals_no_args = { Array(), Array() };

	SIGNAL_WATCH(shape.ptr(), "changed");
	shape->set_faces(make_one_triangle());
	SIGNAL_CHECK("changed", two_signals_no_args);
	SIGNAL_UNWATCH(shape.ptr(), "changed");
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_enclosing_radius with empty faces") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	CHECK(shape->get_enclosing_radius() == 0.0f);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_enclosing_radius with faces") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_one_triangle());

	CHECK(Math::is_equal_approx(shape->get_enclosing_radius(), (real_t)1.0));
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_backface_collision_enabled toggle") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_one_triangle());

	CHECK(shape->is_backface_collision_enabled() == false);

	shape->set_backface_collision_enabled(true);
	CHECK(shape->is_backface_collision_enabled() == true);

	shape->set_backface_collision_enabled(false);
	CHECK(shape->is_backface_collision_enabled() == false);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_backface_collision_enabled emits changed when toggled") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_one_triangle());

	Array two_signals_no_args = { Array(), Array() };

	SIGNAL_WATCH(shape.ptr(), "changed");

	shape->set_backface_collision_enabled(true);
	SIGNAL_CHECK("changed", two_signals_no_args);

	shape->set_backface_collision_enabled(false);
	SIGNAL_CHECK("changed", two_signals_no_args);

	SIGNAL_UNWATCH(shape.ptr(), "changed");
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_backface_collision_enabled no-op when value unchanged") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_one_triangle());

	SIGNAL_WATCH(shape.ptr(), "changed");

	shape->set_backface_collision_enabled(false);
	SIGNAL_CHECK_FALSE("changed");

	shape->set_backface_collision_enabled(true);
	SIGNAL_DISCARD("changed");

	shape->set_backface_collision_enabled(true);
	SIGNAL_CHECK_FALSE("changed");

	SIGNAL_UNWATCH(shape.ptr(), "changed");
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] set_backface_collision_enabled no changed when faces empty") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	SIGNAL_WATCH(shape.ptr(), "changed");

	shape->set_backface_collision_enabled(true);
	SIGNAL_CHECK_FALSE("changed");

	SIGNAL_UNWATCH(shape.ptr(), "changed");
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_debug_mesh_lines deduplicates shared edges") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_two_triangles());

	Vector<Vector3> lines = shape->get_debug_mesh_lines();

	CHECK(lines.size() == 10);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_debug_mesh_lines empty on empty faces") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();

	CHECK(shape->get_debug_mesh_lines().is_empty());
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_debug_mesh_lines is stable across repeated calls") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_two_triangles());

	const Vector<Vector3> first = shape->get_debug_mesh_lines();
	const Vector<Vector3> second = shape->get_debug_mesh_lines();

	REQUIRE(first.size() == second.size());
	for (int i = 0; i < first.size(); i++) {
		CHECK(first[i] == second[i]);
	}
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_debug_mesh_lines invalidates on geometry change") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_one_triangle());

	const Vector<Vector3> before = shape->get_debug_mesh_lines();
	CHECK(before.size() == 6);

	shape->set_faces(make_two_triangles());

	const Vector<Vector3> after = shape->get_debug_mesh_lines();
	CHECK(after.size() == 10);
}

TEST_CASE("[SceneTree][ConcavePolygonShape3D] get_debug_mesh_lines invalidates on clear") {
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(make_two_triangles());
	CHECK(shape->get_debug_mesh_lines().size() == 10);

	shape->set_faces(Vector<Vector3>());
	CHECK(shape->get_debug_mesh_lines().is_empty());
}

static Vector<Vector3> make_grid(int p_cols, int p_rows) {
	Vector<Vector3> f;
	for (int r = 0; r < p_rows; r++) {
		for (int c = 0; c < p_cols; c++) {
			Vector3 v00(c, 0, r);
			Vector3 v10(c + 1, 0, r);
			Vector3 v01(c, 0, r + 1);
			Vector3 v11(c + 1, 0, r + 1);
			f.push_back(v00);
			f.push_back(v10);
			f.push_back(v11);
			f.push_back(v00);
			f.push_back(v11);
			f.push_back(v01);
		}
	}
	return f;
}

static void check_bvh_invariants(const GodotConcavePolygonShape3D &p_shape) {
	const int face_count = p_shape.faces.size();
	REQUIRE(face_count > 0);

	const int expected_node_count = 2 * face_count - 1;
	REQUIRE(p_shape.bvh.size() == expected_node_count);

	const GodotConcavePolygonShape3D::BVH *bvh = p_shape.bvh.ptr();
	const GodotConcavePolygonShape3D::Face *faces = p_shape.faces.ptr();
	const Vector3 *verts = p_shape.vertices.ptr();

	const real_t EPS = 1e-4f;

	for (int i = 0; i < expected_node_count; i++) {
		const GodotConcavePolygonShape3D::BVH &node = bvh[i];
		if (node.face_index >= 0) {
			CHECK(node.left == -1);
			CHECK(node.right == -1);
			CHECK(node.face_index < face_count);
			AABB grown = node.aabb;
			grown.grow_by(EPS);
			const GodotConcavePolygonShape3D::Face &f = faces[node.face_index];
			for (int j = 0; j < 3; j++) {
				CHECK(grown.has_point(verts[f.indices[j]]));
			}
		} else {
			CHECK(node.left >= 0);
			CHECK(node.right >= 0);
			CHECK(node.left < expected_node_count);
			CHECK(node.right < expected_node_count);
			AABB merged = bvh[node.left].aabb;
			merged.merge_with(bvh[node.right].aabb);
			CHECK(node.aabb.is_equal_approx(merged));
		}
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH: single triangle invariants") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_one_triangle(), false);
	shape._wait_for_bvh_build();
	REQUIRE(shape.bvh.size() == 1);
	CHECK(shape.bvh[0].face_index == 0);
	CHECK(shape.bvh[0].left == -1);
	CHECK(shape.bvh[0].right == -1);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH: two triangle invariants") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_two_triangles(), false);
	shape._wait_for_bvh_build();
	check_bvh_invariants(shape);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH: grid 3x3 invariants") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_grid(3, 3), false);
	shape._wait_for_bvh_build();
	check_bvh_invariants(shape);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH: node count equals 2N-1") {
	for (int cols : { 1, 2, 3, 4, 7, 8, 15, 16 }) {
		GodotConcavePolygonShape3D shape;
		shape._setup(make_grid(cols, cols), false);
		shape._wait_for_bvh_build();
		const int face_count = shape.faces.size();
		CHECK(shape.bvh.size() == 2 * face_count - 1);
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH intersect_segment: hit above each grid cell") {
	const int N = 5;
	Vector<Vector3> faces = make_grid(N, N);
	GodotConcavePolygonShape3D shape;
	shape._setup(faces, false);

	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			Vector3 center(c + 0.5f, 0, r + 0.5f);
			Vector3 from = center + Vector3(0, 2, 0);
			Vector3 to = center - Vector3(0, 2, 0);

			Vector3 result, normal;
			int face_index;
			bool hit = shape.intersect_segment(from, to, result, normal, face_index, false);
			CHECK_MESSAGE(hit, "Expected hit above grid cell");
			CHECK_MESSAGE(Math::is_equal_approx(result.y, real_t(0)), "Hit point should be on y=0 plane");
		}
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH intersect_segment: miss when segment is above mesh") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_grid(4, 4), false);

	Vector3 result, normal;
	int face_index;
	bool hit = shape.intersect_segment(Vector3(2, 5, 2), Vector3(2, 1, 2), result, normal, face_index, false);
	CHECK_FALSE(hit);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH intersect_segment: miss when segment is beside mesh") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_grid(4, 4), false);

	Vector3 result, normal;
	int face_index;
	bool hit = shape.intersect_segment(Vector3(10, 2, 2), Vector3(10, -2, 2), result, normal, face_index, false);
	CHECK_FALSE(hit);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH intersect_segment: empty mesh always misses") {
	GodotConcavePolygonShape3D shape;
	shape._setup(Vector<Vector3>(), false);

	Vector3 result, normal;
	int face_index;
	CHECK_FALSE(shape.intersect_segment(Vector3(0, 1, 0), Vector3(0, -1, 0), result, normal, face_index, false));
}

struct SimpleLCG {
	uint64_t state;
	explicit SimpleLCG(uint64_t p_seed) :
			state(p_seed) {}
	float next_float() {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return (float)((state >> 33) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
	}
	float next_range(float lo, float hi) { return lo + next_float() * (hi - lo); }
};

static Vector<Vector3> scatter_unit_quads(int p_num_quads, SimpleLCG &p_rng,
		float p_y_extent, Vector<Vector3> &r_centers) {
	Vector<Vector3> faces;
	faces.resize(p_num_quads * 6);
	r_centers.resize(p_num_quads);

	const float CELL = 4.0f;
	const int cols = (int)Math::ceil(Math::sqrt((real_t)p_num_quads));

	for (int q = 0; q < p_num_quads; q++) {
		const int row = q / cols;
		const int col = q % cols;

		const float jitter = (CELL - 1.5f) * 0.5f;

		Vector3 c(
				col * CELL + p_rng.next_range(-jitter, jitter),
				p_rng.next_range(-p_y_extent, p_y_extent),
				row * CELL + p_rng.next_range(-jitter, jitter));
		r_centers.set(q, c);

		const Vector3 v00 = c + Vector3(-0.5f, 0, -0.5f);
		const Vector3 v10 = c + Vector3(0.5f, 0, -0.5f);
		const Vector3 v01 = c + Vector3(-0.5f, 0, 0.5f);
		const Vector3 v11 = c + Vector3(0.5f, 0, 0.5f);

		faces.set(q * 6 + 0, v00);
		faces.set(q * 6 + 1, v10);
		faces.set(q * 6 + 2, v11);
		faces.set(q * 6 + 3, v00);
		faces.set(q * 6 + 4, v11);
		faces.set(q * 6 + 5, v01);
	}
	return faces;
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH fuzz: scattered quads must be hit from above") {
	SimpleLCG rng(0xDEADBEEFCAFE1234ULL);

	const int QUAD_COUNTS[] = { 1, 2, 3, 7, 8, 15, 16, 31, 32, 64, 128, 256 };
	const float Y_EXTENT = 20.0f;

	for (int num_quads : QUAD_COUNTS) {
		Vector<Vector3> centers;
		Vector<Vector3> faces = scatter_unit_quads(num_quads, rng, Y_EXTENT, centers);

		GodotConcavePolygonShape3D shape;
		shape._setup(faces, false);
		shape._wait_for_bvh_build();

		check_bvh_invariants(shape);
		CHECK(shape.bvh.size() == 2 * num_quads * 2 - 1);

		for (int q = 0; q < num_quads; q++) {
			const Vector3 &c = centers[q];
			Vector3 from = c + Vector3(0, Y_EXTENT + 10, 0);
			Vector3 to = c - Vector3(0, Y_EXTENT + 10, 0);

			Vector3 result, normal;
			int face_index;
			bool hit = shape.intersect_segment(from, to, result, normal, face_index, false);
			CHECK_MESSAGE(hit, "Ray through quad center must hit");
			CHECK_MESSAGE(Math::is_equal_approx(result.y, c.y),
					"Hit point y must match quad y");
		}
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH fuzz: rays outside scene AABB must miss") {
	SimpleLCG rng(0xFEEDFACEBEEFC0DEULL);

	Vector<Vector3> centers;
	Vector<Vector3> faces = scatter_unit_quads(64, rng, 10.0f, centers);

	GodotConcavePolygonShape3D shape;
	shape._setup(faces, false);
	shape._wait_for_bvh_build();

	AABB scene;
	for (int i = 0; i < faces.size(); i++) {
		if (i == 0) {
			scene = AABB(faces[0], Vector3());
		} else {
			scene.expand_to(faces[i]);
		}
	}

	for (int i = 0; i < 64; i++) {
		Vector3 from(scene.position.x + scene.size.x + 100.0f + rng.next_range(0, 10),
				rng.next_range(-500, 500),
				rng.next_range(-500, 500));
		Vector3 to = from + Vector3(rng.next_range(1, 10), 0, 0);

		Vector3 result, normal;
		int face_index;
		CHECK_FALSE_MESSAGE(shape.intersect_segment(from, to, result, normal, face_index, false),
				"Ray entirely outside scene must not hit");
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH fuzz: random rays don't crash, hit points valid") {
	SimpleLCG rng(0xBADC0FFEE0DDF00DULL);

	Vector<Vector3> centers;
	Vector<Vector3> faces = scatter_unit_quads(64, rng, 10.0f, centers);

	GodotConcavePolygonShape3D shape;
	shape._setup(faces, false);
	shape._wait_for_bvh_build();

	AABB scene;
	for (int i = 0; i < faces.size(); i++) {
		if (i == 0) {
			scene = AABB(faces[0], Vector3());
		} else {
			scene.expand_to(faces[i]);
		}
	}
	scene.grow_by(0.01f);

	const int RAYS = 512;
	for (int r = 0; r < RAYS; r++) {
		Vector3 from(rng.next_range(scene.position.x - 5, scene.position.x + scene.size.x + 5),
				rng.next_range(scene.position.y - 5, scene.position.y + scene.size.y + 5),
				rng.next_range(scene.position.z - 5, scene.position.z + scene.size.z + 5));
		Vector3 to(rng.next_range(scene.position.x - 5, scene.position.x + scene.size.x + 5),
				rng.next_range(scene.position.y - 5, scene.position.y + scene.size.y + 5),
				rng.next_range(scene.position.z - 5, scene.position.z + scene.size.z + 5));

		Vector3 result, normal;
		int face_index;
		bool hit = shape.intersect_segment(from, to, result, normal, face_index, false);
		if (hit) {
			CHECK(scene.has_point(result));
			CHECK(face_index >= 0);
			CHECK(face_index < (int)shape.faces.size());
		}
	}
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH async: re-setup flushes previous build") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_grid(8, 8), false);
	shape._setup(make_grid(4, 4), false);
	shape._wait_for_bvh_build();

	const int expected_faces = (int)shape.faces.size();
	CHECK(shape.bvh.size() == 2 * expected_faces - 1);
	check_bvh_invariants(shape);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH async: query immediately after setup") {
	GodotConcavePolygonShape3D shape;
	shape._setup(make_grid(16, 16), false);

	Vector3 result, normal;
	int face_index;
	bool hit = shape.intersect_segment(
			Vector3(8, 2, 8), Vector3(8, -2, 8), result, normal, face_index, false);
	CHECK(hit);
	CHECK(Math::is_equal_approx(result.y, real_t(0)));
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH async: destructor while build in-flight") {
	{
		GodotConcavePolygonShape3D shape;
		shape._setup(make_grid(32, 32), false);
	}
	CHECK(true);
}

TEST_CASE("[GodotConcavePolygonShape3D] BVH async: repeated rapid re-setups") {
	GodotConcavePolygonShape3D shape;
	for (int i = 1; i <= 8; i++) {
		shape._setup(make_grid(i, i), false);
	}
	shape._wait_for_bvh_build();
	check_bvh_invariants(shape);
}

} // namespace TestConcavePolygonShape3D

#endif // PHYSICS_3D_DISABLED
