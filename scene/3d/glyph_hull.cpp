/**************************************************************************/
/*  glyph_hull.cpp                                                        */
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

#include "glyph_hull.h"

#include "core/math/geometry_2d.h"
#include "core/templates/local_vector.h"

// Keeps grown edges strictly outside the vertices they clear, and covers Clipper's fixed-point rounding.
static constexpr real_t GLYPH_HULL_GROW_EPSILON = 0.001;

// In grow distances; longer corners are bevelled.
static constexpr real_t GLYPH_HULL_MITER_LIMIT = 2.0;

// The MSDF fill ramp reaches half a screen pixel past the edge: at most half a texel whenever edges are visible.
static constexpr float GLYPH_HULL_MSDF_FILL_RAMP_TEXELS = 0.5f;

// Coverage needs no subdivision: bilinear weights inside a cell are positive, so it _passes where a corner does.
static constexpr int GLYPH_HULL_MSDF_SUBDIVISIONS = 4;

static constexpr int _cell_subdivisions(GlyphHull::Channel p_channel) {
	return p_channel == GlyphHull::CHANNEL_ALPHA ? 1 : GLYPH_HULL_MSDF_SUBDIVISIONS;
}

static constexpr int _tested_channel_count(GlyphHull::Channel p_channel) {
	return p_channel == GlyphHull::CHANNEL_ALPHA ? 1 : (p_channel == GlyphHull::CHANNEL_MSDF ? 3 : 4);
}

template <GlyphHull::Channel C>
static int _passing_channels(const uint8_t *p_texel, int p_limit) {
	int channels = 0;
	for (int c = 0; c < _tested_channel_count(C); c++) {
		channels |= (p_texel[c] > p_limit) << c;
	}
	return channels;
}

// Median and min are monotonic, so the set of passing channels decides the test.
template <GlyphHull::Channel C>
static bool _passes(int p_channels) {
	if constexpr (C == GlyphHull::CHANNEL_ALPHA) {
		return p_channels & 1;
	} else {
		const bool median = (p_channels & 3) == 3 || (p_channels & 5) == 5 || (p_channels & 6) == 6;
		return C == GlyphHull::CHANNEL_MSDF ? median : median && (p_channels & 8);
	}
}

static Rect2 _polygons_bounds(const Vector<Vector<Vector2>> &p_polygons) {
	Rect2 bounds(p_polygons[0][0], Size2());
	for (const Vector<Vector2> &polygon : p_polygons) {
		for (const Vector2 &point : polygon) {
			bounds.expand_to(point);
		}
	}
	return bounds;
}

// p_corners are ordered (0, 0), (1, 0), (0, 1), (1, 1). Values are scaled by S * S to keep the tests exact.
template <GlyphHull::Channel C>
static void _flag_subcells(const uint8_t *const p_corners[4], int p_mixed_channels, int p_passing_channels, int p_limit, int p_row_size, uint8_t *r_flags) {
	constexpr int S = _cell_subdivisions(C);
	int channels[S][S]; // Channels that can pass somewhere in each subcell.
	for (int j = 0; j < S; j++) {
		for (int i = 0; i < S; i++) {
			channels[j][i] = p_passing_channels;
		}
	}
	for (int c = 0; c < _tested_channel_count(C); c++) {
		if (!((p_mixed_channels >> c) & 1)) {
			continue;
		}
		int grid[S + 1][S + 1];
		for (int i = 0; i <= S; i++) {
			const int top = p_corners[0][c] * (S - i) + p_corners[1][c] * i;
			const int bottom = p_corners[2][c] * (S - i) + p_corners[3][c] * i;
			for (int j = 0; j <= S; j++) {
				grid[j][i] = top * (S - j) + bottom * j;
			}
		}
		for (int j = 0; j < S; j++) {
			for (int i = 0; i < S; i++) {
				const int peak = MAX(MAX(grid[j][i], grid[j][i + 1]), MAX(grid[j + 1][i], grid[j + 1][i + 1]));
				channels[j][i] |= (peak > p_limit) << c;
			}
		}
	}
	for (int j = 0; j < S; j++) {
		for (int i = 0; i < S; i++) {
			r_flags[j * p_row_size + i] = _passes<C>(channels[j][i]);
		}
	}
}

// A cell _passes nowhere unless its corners pass together, everywhere if each corner _passes, and is split otherwise.
template <GlyphHull::Channel C>
static void _flag_cells(const uint8_t *const *p_rows, const int *p_columns, const Size2i &p_cells, float p_threshold, uint8_t *r_flags) {
	constexpr int S = _cell_subdivisions(C);
	const int limit = (int)Math::floor(p_threshold * (255 * S * S));
	const int row_size = p_cells.x * S;
	LocalVector<uint8_t> top;
	LocalVector<uint8_t> bottom;
	top.resize(p_cells.x + 1);
	bottom.resize(p_cells.x + 1);
	for (int x = 0; x <= p_cells.x; x++) {
		bottom[x] = _passing_channels<C>(p_rows[0] + p_columns[x], limit / (S * S));
	}
	for (int y = 0; y < p_cells.y; y++) {
		SWAP(top, bottom);
		for (int x = 0; x <= p_cells.x; x++) {
			bottom[x] = _passing_channels<C>(p_rows[y + 1] + p_columns[x], limit / (S * S));
		}
		uint8_t *flags = r_flags + y * S * row_size;
		for (int x = 0; x < p_cells.x; x++) {
			const int any = top[x] | top[x + 1] | bottom[x] | bottom[x + 1];
			if (!_passes<C>(any)) {
				continue; // Flags start cleared.
			}
			if constexpr (S > 1) {
				const int all = top[x] & top[x + 1] & bottom[x] & bottom[x + 1];
				if (!_passes<C>(all)) {
					const uint8_t *const corners[4] = { p_rows[y] + p_columns[x], p_rows[y] + p_columns[x + 1], p_rows[y + 1] + p_columns[x], p_rows[y + 1] + p_columns[x + 1] };
					_flag_subcells<C>(corners, any & ~all, all, limit, row_size, flags + x * S);
					continue;
				}
			}
			// Passes everywhere, or is a single subcell passing somewhere.
			for (int j = 0; j < S; j++) {
				memset(flags + j * row_size + x * S, 1, S);
			}
		}
	}
}

// Cells span the texel centers of p_rect plus a one texel apron.
static bool _find_lit_cells(const Image &p_atlas, const Rect2i &p_rect, const GlyphHull::AlphaTest &p_test, Vector<uint8_t> &r_cells) {
	const Image::Format format = p_atlas.get_format();
	const bool coverage = p_test.channel == GlyphHull::CHANNEL_ALPHA;
	if (format != Image::FORMAT_RGBA8 && !(format == Image::FORMAT_LA8 && coverage)) {
		return false;
	}

	// Texels at the cell corners, clamped to the atlas like the sampler.
	const int stride = Image::get_format_pixel_size(format);
	const int width = p_atlas.get_width();
	const int height = p_atlas.get_height();
	const Size2i cells = p_rect.size + Size2i(1, 1);
	LocalVector<const uint8_t *> rows;
	rows.resize(cells.y + 1);
	for (int y = 0; y <= cells.y; y++) {
		rows[y] = p_atlas.ptr() + (int64_t)CLAMP(p_rect.position.y - 1 + y, 0, height - 1) * width * stride + (coverage ? stride - 1 : 0);
	}
	LocalVector<int> columns;
	columns.resize(cells.x + 1);
	for (int x = 0; x <= cells.x; x++) {
		columns[x] = CLAMP(p_rect.position.x - 1 + x, 0, width - 1) * stride;
	}

	const int subdivisions = _cell_subdivisions(p_test.channel);
	r_cells.resize_initialized(cells.x * cells.y * subdivisions * subdivisions);
	switch (p_test.channel) {
		case GlyphHull::CHANNEL_ALPHA:
			_flag_cells<GlyphHull::CHANNEL_ALPHA>(rows.ptr(), columns.ptr(), cells, p_test.threshold, r_cells.ptrw());
			break;
		case GlyphHull::CHANNEL_MSDF:
			_flag_cells<GlyphHull::CHANNEL_MSDF>(rows.ptr(), columns.ptr(), cells, p_test.threshold, r_cells.ptrw());
			break;
		case GlyphHull::CHANNEL_MSDF_OUTLINE:
			_flag_cells<GlyphHull::CHANNEL_MSDF_OUTLINE>(rows.ptr(), columns.ptr(), cells, p_test.threshold, r_cells.ptrw());
			break;
	}
	return true;
}

// Eight cells at once, for skipping empty stretches.
static uint64_t _load_cells(const uint8_t *p_cells) {
	uint64_t cells;
	memcpy(&cells, p_cells, sizeof(cells));
	return cells;
}

// Holes are dropped and diagonal neighbors are separate components. Outlines wind like Clipper's outer polygons.
static Vector<Vector<Vector2>> _trace_outlines(const Vector<uint8_t> &p_cells, const Size2i &p_size, real_t p_cell_size) {
	const uint8_t *cells = p_cells.ptr();
	const auto flagged = [&](int p_x, int p_y) -> bool {
		return p_x >= 0 && p_y >= 0 && p_x < p_size.x && p_y < p_size.y && cells[p_y * p_size.x + p_x];
	};
	// Clockwise, so the flagged cell beside an edge is on the side of the next direction.
	static const Point2i DIRECTIONS[4] = { Point2i(1, 0), Point2i(0, 1), Point2i(-1, 0), Point2i(0, -1) };
	constexpr int RIGHT = 0;

	Vector<Vector<Vector2>> outlines;
	LocalVector<uint64_t> traced; // Bit per cell whose top edge is on a traced outline.
	traced.resize_initialized((p_cells.size() + 63) / 64);
	LocalVector<uint8_t> clear_row;
	clear_row.resize_initialized(p_size.x);
	LocalVector<Vector2> ring;
	for (int y = 0; y < p_size.y; y++) {
		const uint8_t *row = cells + y * p_size.x;
		const uint8_t *above = y > 0 ? row - p_size.x : clear_row.ptr();
		for (int x = 0; x < p_size.x; x++) {
			// Every outline, outer or hole, contains the top edge of a flagged cell.
			if (x + 8 <= p_size.x && !(_load_cells(row + x) & ~_load_cells(above + x))) {
				x += 7;
				continue;
			}
			const int index = y * p_size.x + x;
			if (!row[x] || above[x] || ((traced[index / 64] >> (index % 64)) & 1)) {
				continue;
			}
			const Point2i start(x, y);
			Point2i vertex = start;
			int dir = RIGHT;
			ring.clear();
			do {
				if (dir == RIGHT) {
					const int top_edge = vertex.y * p_size.x + vertex.x;
					traced[top_edge / 64] |= uint64_t(1) << (top_edge % 64);
				}
				vertex += DIRECTIONS[dir];
				const bool up_left = flagged(vertex.x - 1, vertex.y - 1);
				const bool up_right = flagged(vertex.x, vertex.y - 1);
				const bool down_left = flagged(vertex.x - 1, vertex.y);
				const bool down_right = flagged(vertex.x, vertex.y);
				// Directions whose leaving edge has the flagged cell on its side only.
				const bool leaving[4] = { down_right && !up_right, down_left && !down_right, up_left && !down_left, up_right && !up_left };
				// Turning towards the flagged side first keeps diagonal neighbors apart.
				const int side = (dir + 1) & 3;
				const int next = leaving[side] ? side : (leaving[dir] ? dir : (dir + 3) & 3);
				if (next != dir) {
					ring.push_back(Vector2(vertex) * p_cell_size);
				}
				dir = next;
			} while (vertex != start || dir != RIGHT);

			real_t area = 0.0;
			for (uint32_t i = 0, prev = ring.size() - 1; i < ring.size(); prev = i++) {
				area += ring[prev].cross(ring[i]);
			}
			if (area > 0.0) {
				outlines.push_back(Vector<Vector2>(ring));
			}
		}
	}
	return outlines;
}

// Douglas-Peucker on a closed ring. Keeps every vertex when fewer than three would remain.
static LocalVector<int> _simplify_ring(const Vector<Vector2> &p_ring, real_t p_tolerance) {
	const Vector2 *ring = p_ring.ptr();
	const int count = p_ring.size();
	int far_index = 0;
	for (int i = 1; i < count; i++) {
		if (ring[i].distance_squared_to(ring[0]) > ring[far_index].distance_squared_to(ring[0])) {
			far_index = i;
		}
	}

	LocalVector<bool> keep;
	keep.resize_initialized(count);
	keep[0] = true;
	keep[far_index] = true;
	// Index count is vertex 0 closing the ring.
	LocalVector<Vector2i> spans;
	spans.reserve(count);
	spans.push_back(Vector2i(0, far_index));
	spans.push_back(Vector2i(far_index, count));
	while (!spans.is_empty()) {
		const Vector2i span = spans[spans.size() - 1];
		spans.remove_at(spans.size() - 1);
		const Vector2 a = ring[span.x];
		const Vector2 ab = ring[span.y % count] - a;
		const real_t length_squared = ab.length_squared();
		int split = -1;
		real_t split_distance = p_tolerance * p_tolerance;
		for (int i = span.x + 1; i < span.y; i++) {
			const Vector2 ap = ring[i] - a;
			const real_t t = length_squared > 0.0 ? CLAMP(ap.dot(ab) / length_squared, (real_t)0.0, (real_t)1.0) : (real_t)0.0;
			const real_t distance = (ap - ab * t).length_squared();
			if (distance > split_distance) {
				split = i;
				split_distance = distance;
			}
		}
		if (split >= 0) {
			keep[split] = true;
			spans.push_back(Vector2i(span.x, split));
			spans.push_back(Vector2i(split, span.y));
		}
	}

	LocalVector<int> kept;
	kept.reserve(count);
	for (int i = 0; i < count; i++) {
		if (keep[i]) {
			kept.push_back(i);
		}
	}
	if (kept.size() < 3) {
		kept.resize(count);
		for (int i = 0; i < count; i++) {
			kept[i] = i;
		}
	}
	return kept;
}

// Moves each simplified edge outwards until it clears the ring vertices it cuts off.
// At convex corners both edges cut the vertices near it, so both have to clear them.
static Vector<Vector2> _grow_edges(const Vector<Vector2> &p_ring, const LocalVector<int> &p_kept) {
	const Vector2 *ring = p_ring.ptr();
	const int ring_size = p_ring.size();
	const int count = p_kept.size();
	LocalVector<Vector2> normals;
	normals.resize(count);
	for (int j = 0; j < count; j++) {
		// Outlines wind like Clipper's outer polygons, so this points outwards.
		normals[j] = (ring[p_kept[(j + 1) % count]] - ring[p_kept[j]]).orthogonal().normalized();
	}
	// Ring vertices strictly between kept vertices p_span and p_span + 1, measured from edge p_edge.
	const auto bulge = [&](int p_edge, int p_span) {
		const Vector2 origin = ring[p_kept[p_edge]];
		const int end = p_span + 1 < count ? p_kept[p_span + 1] : ring_size + p_kept[0];
		real_t distance = 0.0;
		for (int i = p_kept[p_span] + 1; i < end; i++) {
			distance = MAX(distance, normals[p_edge].dot(ring[i < ring_size ? i : i - ring_size] - origin));
		}
		return distance;
	};

	LocalVector<real_t> offsets;
	offsets.resize(count);
	for (int j = 0; j < count; j++) {
		offsets[j] = bulge(j, j);
	}
	for (int j = 0; j < count; j++) {
		const int prev = (j + count - 1) % count;
		if (normals[prev].cross(normals[j]) > 0.0) {
			offsets[prev] = MAX(offsets[prev], bulge(prev, j));
			offsets[j] = MAX(offsets[j], bulge(j, prev));
		}
	}

	Vector<Vector2> grown;
	grown.reserve(count * 2);
	for (int j = 0; j < count; j++) {
		const int prev = (j + count - 1) % count;
		const Vector2 corner = ring[p_kept[j]];
		const real_t prev_offset = offsets[prev] + GLYPH_HULL_GROW_EPSILON;
		const real_t offset = offsets[j] + GLYPH_HULL_GROW_EPSILON;
		// Solves normals[prev].dot(miter) == prev_offset and normals[j].dot(miter) == offset.
		const real_t sine = normals[prev].cross(normals[j]);
		if (!Math::is_zero_approx(sine)) {
			const Vector2 miter = (normals[j].orthogonal() * prev_offset - normals[prev].orthogonal() * offset) / sine;
			const real_t limit = GLYPH_HULL_MITER_LIMIT * MAX(prev_offset, offset);
			// At reflex corners the grown edges cross, so a bevel would loop.
			if (sine < 0.0 || miter.length_squared() <= limit * limit) {
				grown.push_back(corner + miter);
				continue;
			}
		}
		const Vector2 bevel_start = corner + normals[prev] * prev_offset;
		const Vector2 bevel_end = corner + normals[j] * offset;
		grown.push_back(bevel_start);
		if (!bevel_end.is_equal_approx(bevel_start)) {
			grown.push_back(bevel_end);
		}
	}
	return grown;
}

struct GlyphHullSegmentBounds {
	Vector2 min;
	Vector2 max;

	bool overlaps(const GlyphHullSegmentBounds &p_other) const {
		return min.x <= p_other.max.x && p_other.min.x <= max.x && min.y <= p_other.max.y && p_other.min.y <= max.y;
	}
};

// Edge i ends at vertex i.
static LocalVector<GlyphHullSegmentBounds> _edge_bounds(const Vector<Vector2> &p_polygon) {
	const Vector2 *points = p_polygon.ptr();
	LocalVector<GlyphHullSegmentBounds> bounds;
	bounds.resize(p_polygon.size());
	for (int i = 0, prev = p_polygon.size() - 1; i < p_polygon.size(); prev = i++) {
		bounds[i] = { points[prev].min(points[i]), points[prev].max(points[i]) };
	}
	return bounds;
}

// Callers must check that the bounds overlap, which makes the side tests exact for collinear segments.
static bool _segments_touch(const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c, const Vector2 &p_d) {
	const Vector2 ab = p_b - p_a;
	const Vector2 cd = p_d - p_c;
	return ab.cross(p_c - p_a) * ab.cross(p_d - p_a) <= 0.0 && cd.cross(p_a - p_c) * cd.cross(p_b - p_c) <= 0.0;
}

static bool _is_polygon_simple(const Vector<Vector2> &p_polygon) {
	const Vector2 *points = p_polygon.ptr();
	const int count = p_polygon.size();
	const LocalVector<GlyphHullSegmentBounds> bounds = _edge_bounds(p_polygon);
	for (int i = 0; i < count; i++) {
		// Edge i ends at vertex i, so edge 0 neighbors the last edge.
		const int end = i == 0 ? count - 1 : count;
		for (int j = i + 2; j < end; j++) {
			if (bounds[i].overlaps(bounds[j]) && _segments_touch(points[i > 0 ? i - 1 : count - 1], points[i], points[j - 1], points[j])) {
				return false;
			}
		}
	}
	return true;
}

// p_hull must be simple.
static bool _contains_outline(const Vector<Vector2> &p_hull, const Vector<Vector2> &p_outline) {
	const Vector2 *hull = p_hull.ptr();
	const Vector2 *outline = p_outline.ptr();
	const int hull_size = p_hull.size();
	const LocalVector<GlyphHullSegmentBounds> hull_bounds = _edge_bounds(p_hull);
	for (int k = 0, prev = p_outline.size() - 1; k < p_outline.size(); prev = k++) {
		const GlyphHullSegmentBounds bounds = { outline[prev].min(outline[k]), outline[prev].max(outline[k]) };
		for (int i = 0; i < hull_size; i++) {
			if (bounds.overlaps(hull_bounds[i]) && _segments_touch(hull[i > 0 ? i - 1 : hull_size - 1], hull[i], outline[prev], outline[k])) {
				return false;
			}
		}
	}
	return Geometry2D::is_point_in_polygon(outline[0], p_hull);
}

// Covers every cell even where growing went wrong.
static Vector<Vector<Vector2>> _merge_with_outlines(const Vector<Vector<Vector2>> &p_outlines, const Vector<Vector<Vector2>> &p_hulls) {
	Vector<Vector<Vector2>> parts = p_outlines;
	Vector<Vector<Vector2>> outers;
	Vector<Vector<Vector2>> holes;
	for (const Vector<Vector2> &hull : p_hulls) {
		// Merged on their own, loops turned inside out add area instead of canceling it.
		Geometry2D::merge_many_polygons({ hull }, outers, holes);
		parts.append_array(outers);
	}
	Geometry2D::merge_many_polygons(parts, outers, holes);
	return outers;
}

// Sutherland-Hodgman.
static Vector<Vector2> _clip_to_rect(const Vector<Vector2> &p_polygon, const Rect2 &p_rect) {
	LocalVector<Vector2> polygon;
	for (const Vector2 &point : p_polygon) {
		polygon.push_back(point);
	}
	LocalVector<Vector2> clipped;
	for (int side = 0; side < 4; side++) {
		const int axis = side & 1;
		const bool upper = side >= 2;
		const real_t limit = upper ? p_rect.get_end()[axis] : p_rect.position[axis];
		const auto inside = [&](const Vector2 &p_point) {
			return upper ? p_point[axis] <= limit : p_point[axis] >= limit;
		};
		clipped.clear();
		for (uint32_t i = 0; i < polygon.size(); i++) {
			const Vector2 &a = polygon[i];
			const Vector2 &b = polygon[(i + 1) % polygon.size()];
			if (inside(a)) {
				clipped.push_back(a);
			}
			if (inside(a) != inside(b)) {
				clipped.push_back(a.lerp(b, (limit - a[axis]) / (b[axis] - a[axis])));
			}
		}
		SWAP(polygon, clipped);
	}
	return Vector<Vector2>(polygon);
}

static bool _append_polygon(const Vector<Vector2> &p_polygon, const Vector2 &p_offset, GlyphHull &r_hull) {
	if (p_polygon.size() < 3 || Geometry2D::is_polygon_clockwise(p_polygon)) {
		return true; // Degenerate, or wound like a hole.
	}
	const Vector<int> triangles = Geometry2D::triangulate_polygon(p_polygon);
	if (triangles.is_empty()) {
		return false;
	}
	const int first_point = r_hull.points.size();
	r_hull.points.resize(first_point + p_polygon.size());
	Vector2 *points = r_hull.points.ptrw() + first_point;
	for (int i = 0; i < p_polygon.size(); i++) {
		points[i] = p_polygon[i] + p_offset;
	}
	const int first_index = r_hull.indices.size();
	r_hull.indices.resize(first_index + triangles.size());
	int *indices = r_hull.indices.ptrw() + first_index;
	for (int i = 0; i < triangles.size(); i++) {
		indices[i] = first_point + triangles[i];
	}
	return true;
}

GlyphHull::AlphaTest GlyphHull::make_alpha_test(bool p_msdf, float p_msdf_pixel_range, float p_msdf_outline_size) {
	AlphaTest test;
	if (!p_msdf) {
		return test; // Any coverage can draw.
	}
	// BaseMaterial3D: fill alpha = clamp((median - 0.5) * px_size + 0.5), outline alpha =
	// clamp((min(median, a) - 0.5 + cr) * px_size) with cr = clamp(outline, 0, range / 2 - 1) / range.
	const float range = MAX(p_msdf_pixel_range, 1.0f);
	float edge = 0.5f - GLYPH_HULL_MSDF_FILL_RAMP_TEXELS / range;
	test.channel = CHANNEL_MSDF;
	if (p_msdf_outline_size > 0.0f) {
		edge = 0.5f - CLAMP(p_msdf_outline_size, 0.0f, MAX(range * 0.5f - 1.0f, 0.0f)) / range;
		test.channel = CHANNEL_MSDF_OUTLINE;
	}
	test.threshold = CLAMP(edge, 0.0f, 1.0f);
	return test;
}

bool GlyphHull::build(const Ref<Image> &p_atlas, const Rect2i &p_rect, const AlphaTest &p_test, real_t p_tolerance, GlyphHull &r_hull) {
	r_hull = GlyphHull();
	Vector<uint8_t> cells;
	if (p_atlas.is_null() || p_atlas->is_empty() || !p_rect.has_area() || !_find_lit_cells(*p_atlas.ptr(), p_rect, p_test, cells)) {
		return false;
	}
	// Cells span texel centers; cell (0, 0) starts at the center of the apron texel.
	const int subdivisions = _cell_subdivisions(p_test.channel);
	const Size2i size = (p_rect.size + Size2i(1, 1)) * subdivisions;
	return build_from_cells(cells, size, Vector2(p_rect.position) - Vector2(0.5, 0.5), 1.0 / subdivisions, Rect2(p_rect), p_tolerance, r_hull);
}

bool GlyphHull::build_from_cells(const Vector<uint8_t> &p_cells, const Size2i &p_size, const Vector2 &p_origin, real_t p_cell_size, const Rect2 &p_clip, real_t p_tolerance, GlyphHull &r_hull) {
	r_hull = GlyphHull();
	ERR_FAIL_COND_V(p_size.x < 0 || p_size.y < 0 || p_cells.size() != p_size.x * p_size.y || p_cell_size <= 0, false);

	// Built relative to p_origin, keeping float error low in large atlases.
	const Vector<Vector<Vector2>> outlines = _trace_outlines(p_cells, p_size, p_cell_size);
	if (outlines.is_empty()) {
		return false;
	}

	// Growing is local, so short edges can turn inside out or leave cells uncovered.
	Vector<Vector<Vector2>> hulls;
	bool covered = true;
	for (const Vector<Vector2> &outline : outlines) {
		const Vector<Vector2> hull = _grow_edges(outline, _simplify_ring(outline, p_tolerance));
		covered = covered && _is_polygon_simple(hull) && _contains_outline(hull, outline);
		hulls.push_back(hull);
	}
	if (!covered) {
		hulls = _merge_with_outlines(outlines, hulls);
	}

	// Never past the cells' extent, where corners could cover a neighboring glyph's ink.
	const Rect2 clip = Rect2(p_clip.position - p_origin, p_clip.size).intersection(_polygons_bounds(outlines));
	for (const Vector<Vector2> &hull : hulls) {
		if (!_append_polygon(_clip_to_rect(hull, clip), p_origin, r_hull)) {
			r_hull = GlyphHull();
			return false;
		}
	}
	return !r_hull.indices.is_empty();
}
