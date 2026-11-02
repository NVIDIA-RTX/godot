/**************************************************************************/
/*  test_glyph_hull.cpp                                                   */
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

TEST_FORCE_LINK(test_glyph_hull)

#ifndef _3D_DISABLED

#include "core/math/geometry_2d.h"
#include "scene/3d/glyph_hull.h"

namespace TestGlyphHull {

static constexpr int ATLAS_SIZE = 48;
static constexpr real_t TOLERANCE = 1.0;
static const Rect2i GLYPH_RECT = Rect2i(4, 4, 40, 40);

// Test shapes as signed distances in texels, positive inside.
static real_t sd_disk(const Vector2 &p_point) {
	return 9.0 - p_point.distance_to(Vector2(18, 20));
}

static real_t sd_bar(const Vector2 &p_point) {
	const Vector2 d = (p_point - Vector2(33, 11)).abs() - Vector2(7, 3);
	return -(d.maxf(0.0).length() + MIN(MAX(d.x, d.y), (real_t)0.0));
}

static real_t sd_stroke(const Vector2 &p_point) {
	return 1.5 - Geometry2D::get_distance_to_segment(p_point, Vector2(10, 38), Vector2(38, 30));
}

static real_t sd_shapes(const Vector2 &p_point) {
	return MAX(sd_disk(p_point), MAX(sd_bar(p_point), sd_stroke(p_point)));
}

static uint8_t to_byte(real_t p_value) {
	return (uint8_t)CLAMP(p_value * 256.0, 0.0, 255.0);
}

// Evaluates p_texel(center, channel) for every texel of a square atlas.
template <typename TexelFunc>
static Ref<Image> make_atlas(Image::Format p_format, TexelFunc p_texel) {
	const int channels = Image::get_format_pixel_size(p_format);
	Vector<uint8_t> data;
	data.resize(ATLAS_SIZE * ATLAS_SIZE * channels);
	for (int i = 0; i < ATLAS_SIZE * ATLAS_SIZE; i++) {
		const Vector2 center(i % ATLAS_SIZE + 0.5, i / ATLAS_SIZE + 0.5);
		for (int c = 0; c < channels; c++) {
			data.write[i * channels + c] = p_texel(center, c);
		}
	}
	return Image::create_from_data(ATLAS_SIZE, ATLAS_SIZE, false, p_format, data);
}

// LA8 coverage with one texel of anti-aliasing.
static Ref<Image> make_coverage_atlas() {
	return make_atlas(Image::FORMAT_LA8, [](const Vector2 &p_centre, int p_channel) {
		return p_channel == 0 ? (uint8_t)255 : to_byte(sd_shapes(p_centre) + 0.5);
	});
}

// RGBA8 MSDF-like atlas: each colour channel encodes a different pair of
// shapes, so the median differs from every single channel. Alpha holds the
// true distance.
static Ref<Image> make_msdf_atlas(real_t p_range) {
	return make_atlas(Image::FORMAT_RGBA8, [p_range](const Vector2 &p_centre, int p_channel) {
		const real_t distances[4] = {
			MAX(sd_disk(p_centre), sd_bar(p_centre)),
			MAX(sd_bar(p_centre), sd_stroke(p_centre)),
			MAX(sd_stroke(p_centre), sd_disk(p_centre)),
			sd_shapes(p_centre),
		};
		return to_byte(0.5 + distances[p_channel] / p_range);
	});
}

// Bilinear filtering with clamp-to-edge, as the glyph material samples the atlas.
static Color sample(const Ref<Image> &p_atlas, const Vector2 &p_point) {
	const Vector2 texel = p_point - Vector2(0.5, 0.5);
	const Vector2 t = texel - texel.floor();
	const Vector2i base = Vector2i(texel.floor());
	auto fetch = [&](int p_x, int p_y) {
		return p_atlas->get_pixel(CLAMP(base.x + p_x, 0, ATLAS_SIZE - 1), CLAMP(base.y + p_y, 0, ATLAS_SIZE - 1));
	};
	return fetch(0, 0).lerp(fetch(1, 0), t.x).lerp(fetch(0, 1).lerp(fetch(1, 1), t.x), t.y);
}

// BaseMaterial3D's MSDF alpha at one screen pixel per texel, where the fill
// ramp is widest.
static float msdf_alpha(const Color &p_texel, float p_range, float p_outline) {
	const float px_size = p_range;
	const float d = MAX(MIN(p_texel.r, p_texel.g), MIN(MAX(p_texel.r, p_texel.g), p_texel.b));
	if (p_outline > 0.0f) {
		const float cr = CLAMP(p_outline, 0.0f, p_range / 2.0f - 1.0f) / p_range;
		return CLAMP((MIN(d, p_texel.a) - 0.5f + cr) * px_size, 0.0f, 1.0f);
	}
	return CLAMP((d - 0.5f) * px_size + 0.5f, 0.0f, 1.0f);
}

static Vector2 triangle_vertex(const GlyphHull &p_hull, int p_index) {
	return p_hull.points[p_hull.indices[p_index]];
}

// Twice the signed area; positive for the winding of a y-down glyph quad.
static real_t triangle_cross(const GlyphHull &p_hull, int p_first) {
	const Vector2 a = triangle_vertex(p_hull, p_first);
	return (triangle_vertex(p_hull, p_first + 1) - a).cross(triangle_vertex(p_hull, p_first + 2) - a);
}

static bool hull_contains(const GlyphHull &p_hull, const Vector2 &p_point) {
	for (int i = 0; i + 2 < p_hull.indices.size(); i += 3) {
		const real_t cross = triangle_cross(p_hull, i);
		if (Math::is_zero_approx(cross)) {
			continue;
		}
		const Vector2 a = triangle_vertex(p_hull, i) - p_point;
		const Vector2 b = triangle_vertex(p_hull, i + 1) - p_point;
		const Vector2 c = triangle_vertex(p_hull, i + 2) - p_point;
		const real_t u = b.cross(c) / cross;
		const real_t v = c.cross(a) / cross;
		if (u >= -1e-4 && v >= -1e-4 && 1.0 - u - v >= -1e-4) {
			return true;
		}
	}
	return false;
}

static real_t hull_area(const GlyphHull &p_hull) {
	real_t area = 0.0;
	for (int i = 0; i + 2 < p_hull.indices.size(); i += 3) {
		area += triangle_cross(p_hull, i) * 0.5;
	}
	return area;
}

static bool hull_wound_like_quad(const GlyphHull &p_hull) {
	for (int i = 0; i + 2 < p_hull.indices.size(); i += 3) {
		if (triangle_cross(p_hull, i) < -1e-6) {
			return false;
		}
	}
	return true;
}

static bool hull_within(const GlyphHull &p_hull, const Rect2 &p_rect) {
	for (const Vector2 &point : p_hull.points) {
		if (!p_rect.grow(1e-4).has_point(point)) {
			return false;
		}
	}
	return true;
}

// Calls p_func(point) for sample points `p_step` apart across the glyph rect.
template <typename Func>
static void for_each_sample(real_t p_step, Func p_func) {
	for (real_t y = GLYPH_RECT.position.y + p_step * 0.5; y < GLYPH_RECT.get_end().y; y += p_step) {
		for (real_t x = GLYPH_RECT.position.x + p_step * 0.5; x < GLYPH_RECT.get_end().x; x += p_step) {
			p_func(Vector2(x, y));
		}
	}
}

// Counts sample points in the glyph rect that can draw but lie outside the hull.
template <typename AlphaFunc>
static int count_uncovered(const GlyphHull &p_hull, const Ref<Image> &p_atlas, AlphaFunc p_alpha) {
	int uncovered = 0;
	for_each_sample(0.125, [&](const Vector2 &p_point) {
		if (p_alpha(sample(p_atlas, p_point)) > 1e-4f && !hull_contains(p_hull, p_point)) {
			uncovered++;
		}
	});
	return uncovered;
}

// Bounds of the sample points in the glyph rect that can draw, grown by the
// sample spacing.
template <typename AlphaFunc>
static Rect2 drawable_bounds(const Ref<Image> &p_atlas, AlphaFunc p_alpha) {
	constexpr real_t STEP = 1.0 / 16.0;
	Rect2 bounds;
	bool found = false;
	for_each_sample(STEP, [&](const Vector2 &p_point) {
		if (p_alpha(sample(p_atlas, p_point)) > 0.0f) {
			bounds = found ? bounds.expand(p_point) : Rect2(p_point, Size2());
			found = true;
		}
	});
	return bounds.grow(STEP);
}

static Vector<uint8_t> make_cells(const Size2i &p_size, bool (*p_flag)(int p_x, int p_y)) {
	Vector<uint8_t> cells;
	cells.resize(p_size.x * p_size.y);
	for (int i = 0; i < cells.size(); i++) {
		cells.write[i] = p_flag(i % p_size.x, i / p_size.x);
	}
	return cells;
}

TEST_CASE("[GlyphHull] Alpha test matches BaseMaterial3D thresholds") {
	const GlyphHull::AlphaTest coverage = GlyphHull::make_alpha_test(false, 12, 4);
	CHECK(coverage.channel == GlyphHull::CHANNEL_ALPHA);
	CHECK(coverage.threshold == 0.0f);

	const GlyphHull::AlphaTest fill = GlyphHull::make_alpha_test(true, 12, 0);
	CHECK(fill.channel == GlyphHull::CHANNEL_MSDF);
	CHECK_MESSAGE(fill.threshold == doctest::Approx(0.5 - 0.5 / 12), "The fill ramp reaches half a texel past the edge.");

	const GlyphHull::AlphaTest outline = GlyphHull::make_alpha_test(true, 12, 3);
	CHECK(outline.channel == GlyphHull::CHANNEL_MSDF_OUTLINE);
	CHECK(outline.threshold == doctest::Approx(0.5 - 3.0 / 12));
	CHECK_MESSAGE(GlyphHull::make_alpha_test(true, 12, 10).threshold == doctest::Approx(0.5 - 5.0 / 12), "Outline size clamps to range / 2 - 1.");
}

TEST_CASE("[GlyphHull] Cell mask hull keeps concave outlines and clips to the rect") {
	// L shape: two full rows above a two cell wide stem.
	const Size2i size(6, 6);
	const Vector2 origin(10, 20);
	const Vector<uint8_t> cells = make_cells(size, [](int p_x, int p_y) { return p_y < 2 || p_x < 2; });

	GlyphHull hull;
	REQUIRE(GlyphHull::build_from_cells(cells, size, origin, 1.0, Rect2(0, 0, 100, 100), TOLERANCE, hull));
	for (int i = 0; i < cells.size(); i++) {
		if (cells[i]) {
			const Vector2 cell = origin + Vector2(i % size.x, i / size.x);
			CHECK(hull_contains(hull, cell + Vector2(0.01, 0.01)));
			CHECK(hull_contains(hull, cell + Vector2(0.99, 0.99)));
		}
	}
	CHECK_FALSE_MESSAGE(hull_contains(hull, origin + Vector2(4, 4)), "The notch of the L must stay open.");
	CHECK(hull_area(hull) == doctest::Approx(20.0).epsilon(0.01));
	CHECK(hull_wound_like_quad(hull));

	const Rect2 clip(origin + Vector2(1, 1), Vector2(3, 3));
	REQUIRE(GlyphHull::build_from_cells(cells, size, origin, 1.0, clip, TOLERANCE, hull));
	CHECK(hull_within(hull, clip));
	CHECK(hull_area(hull) == doctest::Approx(5.0).epsilon(0.01));
}

TEST_CASE("[GlyphHull] Cell mask hull stays within the bounds of the flagged cells") {
	// Half-size cells forming a staircase, which simplifies to a triangle that
	// must grow past the outer step corners to cover them.
	const Size2i size(8, 8);
	const Vector2 origin(10, 20);
	constexpr real_t CELL_SIZE = 0.5;
	const Vector<uint8_t> cells = make_cells(size, [](int p_x, int p_y) { return 1 <= p_x && p_x <= p_y && p_y <= 6; });

	GlyphHull hull;
	REQUIRE(GlyphHull::build_from_cells(cells, size, origin, CELL_SIZE, Rect2(0, 0, 100, 100), TOLERANCE, hull));
	for (int i = 0; i < cells.size(); i++) {
		if (cells[i]) {
			const Vector2 cell = origin + Vector2(i % size.x, i / size.x) * CELL_SIZE;
			CHECK(hull_contains(hull, cell + Vector2(0.01, 0.01)));
			CHECK(hull_contains(hull, cell + Vector2(0.49, 0.49)));
		}
	}
	CHECK(hull_within(hull, Rect2(origin + Vector2(1, 1) * CELL_SIZE, Vector2(6, 6) * CELL_SIZE)));
	CHECK(hull_wound_like_quad(hull));
}

// Cells of a 5 x 5 ring around a 3 x 3 hole.
static bool ring_cell(int p_x, int p_y) {
	const bool square = p_x >= 0 && p_x <= 4 && p_y >= 0 && p_y <= 4;
	const bool hole = p_x >= 1 && p_x <= 3 && p_y >= 1 && p_y <= 3;
	return square && !hole;
}

TEST_CASE("[GlyphHull] Cell mask hull fills holes and covers diagonal neighbors") {
	// A ring, a ring whose hole touches the outside at one corner, and a chain
	// of cells touching only at their corners.
	const Size2i size(17, 6);
	const Vector<uint8_t> cells = make_cells(size, [](int p_x, int p_y) {
		return ring_cell(p_x, p_y) || (ring_cell(p_x - 6, p_y) && !(p_x == 10 && p_y == 4)) || (p_x >= 12 && p_x - 12 == p_y);
	});

	GlyphHull hull;
	REQUIRE(GlyphHull::build_from_cells(cells, size, Vector2(), 1.0, Rect2(0, 0, 100, 100), TOLERANCE, hull));
	for (int i = 0; i < cells.size(); i++) {
		if (cells[i]) {
			const Vector2 cell(i % size.x, i / size.x);
			CHECK(hull_contains(hull, cell + Vector2(0.01, 0.01)));
			CHECK(hull_contains(hull, cell + Vector2(0.99, 0.99)));
		}
	}
	CHECK_MESSAGE(hull_contains(hull, Vector2(2.5, 2.5)), "Holes are filled.");
	CHECK(hull_wound_like_quad(hull));
}

TEST_CASE("[GlyphHull] Coverage atlas hull contains every drawable point") {
	const Ref<Image> atlas = make_coverage_atlas();
	const auto alpha = [](const Color &p_texel) { return p_texel.a; };
	GlyphHull hull;
	REQUIRE(GlyphHull::build(atlas, GLYPH_RECT, GlyphHull::make_alpha_test(false, 0, 0), TOLERANCE, hull));
	CHECK(count_uncovered(hull, atlas, alpha) == 0);
	CHECK_MESSAGE(hull_within(hull, drawable_bounds(atlas, alpha)), "Coverage is exact per cell, so the hull ends where coverage does.");
	CHECK(hull_wound_like_quad(hull));
	CHECK_MESSAGE(hull_area(hull) < 0.7 * GLYPH_RECT.get_area(), "The hull should be much tighter than the glyph rect.");
	CHECK(hull.points.size() < 80);
}

TEST_CASE("[GlyphHull] MSDF atlas hull contains every drawable point") {
	constexpr float RANGE = 8.0f;
	const Ref<Image> atlas = make_msdf_atlas(RANGE);
	for (const float outline : { 0.0f, 1.0f, 3.0f }) {
		const auto alpha = [&](const Color &p_texel) { return msdf_alpha(p_texel, RANGE, outline); };
		GlyphHull hull;
		REQUIRE(GlyphHull::build(atlas, GLYPH_RECT, GlyphHull::make_alpha_test(true, RANGE, outline), TOLERANCE, hull));
		CHECK(count_uncovered(hull, atlas, alpha) == 0);
		CHECK_MESSAGE(hull_within(hull, drawable_bounds(atlas, alpha).grow(0.5)), "Hulls end within half a texel of the ink, keeping neighboring glyphs apart.");
		CHECK(hull_wound_like_quad(hull));
		CHECK(hull_area(hull) < GLYPH_RECT.get_area());
	}
}

TEST_CASE("[GlyphHull] Unsupported or blank atlases fall back to the glyph rect") {
	const GlyphHull::AlphaTest coverage = GlyphHull::make_alpha_test(false, 0, 0);
	GlyphHull hull;
	CHECK_FALSE(GlyphHull::build(Ref<Image>(), GLYPH_RECT, coverage, TOLERANCE, hull));
	CHECK_FALSE(GlyphHull::build(Image::create_empty(ATLAS_SIZE, ATLAS_SIZE, false, Image::FORMAT_RGB8), GLYPH_RECT, coverage, TOLERANCE, hull));
	CHECK_FALSE_MESSAGE(GlyphHull::build(make_coverage_atlas(), GLYPH_RECT, GlyphHull::make_alpha_test(true, 8, 0), TOLERANCE, hull), "MSDF needs RGBA8.");

	const Ref<Image> blank = make_atlas(Image::FORMAT_LA8, [](const Vector2 &, int) { return (uint8_t)0; });
	CHECK_FALSE(GlyphHull::build(blank, GLYPH_RECT, coverage, TOLERANCE, hull));
	CHECK(hull.points.is_empty());
	CHECK(hull.indices.is_empty());
}

} // namespace TestGlyphHull

#endif // _3D_DISABLED
