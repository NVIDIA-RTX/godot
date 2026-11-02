/**************************************************************************/
/*  glyph_hull.h                                                          */
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

#include "core/io/image.h"
#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/templates/vector.h"

// Triangulated outline of everything a glyph atlas region can draw, so glyphs can be meshed tighter than
// their padded atlas rect. Coordinates are atlas texels, y-down.
struct GlyphHull {
	enum Channel : uint8_t {
		CHANNEL_ALPHA, // a.
		CHANNEL_MSDF, // median(r, g, b).
		CHANNEL_MSDF_OUTLINE, // min(median(r, g, b), a).
	};

	// Passes where the bilinearly filtered channel exceeds threshold (0..1).
	struct AlphaTest {
		Channel channel = CHANNEL_ALPHA;
		float threshold = 0.0f;
	};

	Vector<Vector2> points;
	Vector<int> indices; // Wound like a glyph quad.

	// Must match BaseMaterial3D's glyph shading. Pixel range and outline size are in atlas texels.
	static AlphaTest make_alpha_test(bool p_msdf, float p_msdf_pixel_range, float p_msdf_outline_size);

	// Adds up to p_tolerance texels of area, but never past the bounds of what can draw.
	// Returns false with an empty hull when nothing can draw or the format is unsupported; draw the full rect then.
	static bool build(const Ref<Image> &p_atlas, const Rect2i &p_rect, const AlphaTest &p_test, real_t p_tolerance, GlyphHull &r_hull);

	// p_cells is row-major; a non-zero cell (x, y) covers the square at p_origin + (x, y) * p_cell_size.
	static bool build_from_cells(const Vector<uint8_t> &p_cells, const Size2i &p_size, const Vector2 &p_origin, real_t p_cell_size, const Rect2 &p_clip, real_t p_tolerance, GlyphHull &r_hull);
};
