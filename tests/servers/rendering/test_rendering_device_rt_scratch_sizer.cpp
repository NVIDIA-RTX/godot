/**************************************************************************/
/*  test_rendering_device_rt_scratch_sizer.cpp                            */
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

TEST_FORCE_LINK(test_rendering_device_rt_scratch_sizer)

#include "servers/rendering/rendering_device_rt_scratch_sizer.h"

namespace TestRenderingDeviceRTScratchSizer {

constexpr uint64_t MB = 1000 * 1000;
constexpr uint64_t SEC = 1000;

TEST_CASE("[RenderingDeviceRTScratchSizer] Waits for the shrink delay after a resize") {
	RenderingDeviceRTScratchSizer sizer;
	sizer.record_resize(100 * SEC);
	CHECK(sizer.get_shrink_size(128 * MB, 109 * SEC) == 0);
	CHECK(sizer.get_shrink_size(128 * MB, 110 * SEC) == 32 * MB);
}

TEST_CASE("[RenderingDeviceRTScratchSizer] Shrinks to twice the window peak, at least the minimum") {
	RenderingDeviceRTScratchSizer sizer;
	sizer.record_peak(10 * MB, 100 * SEC);
	CHECK(sizer.get_shrink_size(128 * MB, 120 * SEC) == 32 * MB);
	sizer.record_peak(40 * MB, 105 * SEC);
	CHECK(sizer.get_shrink_size(128 * MB, 120 * SEC) == 80 * MB);
}

TEST_CASE("[RenderingDeviceRTScratchSizer] Keeps buffers the target doesn't fit under") {
	RenderingDeviceRTScratchSizer sizer;
	sizer.record_peak(70 * MB, 100 * SEC);
	CHECK(sizer.get_shrink_size(128 * MB, 120 * SEC) == 0);
	CHECK(sizer.get_shrink_size(32 * MB, 120 * SEC) == 0);
}

TEST_CASE("[RenderingDeviceRTScratchSizer] Forgets peaks older than the window") {
	RenderingDeviceRTScratchSizer sizer;
	sizer.record_peak(100 * MB, 100 * SEC);
	CHECK(sizer.get_window_peak(159 * SEC) == 100 * MB);
	CHECK(sizer.get_window_peak(160 * SEC) == 0);
	CHECK(sizer.get_shrink_size(256 * MB, 160 * SEC) == 32 * MB);
}

TEST_CASE("[RenderingDeviceRTScratchSizer] A reused bucket drops its stale peak") {
	RenderingDeviceRTScratchSizer sizer;
	sizer.record_peak(100 * MB, 100 * SEC);
	sizer.record_peak(5 * MB, 160 * SEC);
	CHECK(sizer.get_window_peak(160 * SEC) == 5 * MB);
}

} // namespace TestRenderingDeviceRTScratchSizer
