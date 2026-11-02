/**************************************************************************/
/*  rendering_device_rt_scratch_sizer.h                                   */
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

#include "core/typedefs.h"

// Picks a smaller size for the shared acceleration structure scratch buffer from the batch peaks of the last minute.
class RenderingDeviceRTScratchSizer {
	static constexpr uint64_t BUCKET_MSEC = 10000;
	static constexpr uint64_t BUCKET_COUNT = 6; // Peaks are forgotten after 50-60 seconds.
	static constexpr uint64_t SHRINK_DELAY_MSEC = 10000;
	static constexpr uint64_t PEAK_HEADROOM = 2;

	struct Bucket {
		uint64_t index = 0; // Msec / BUCKET_MSEC of the peaks it holds.
		uint64_t peak = 0;
	};

	Bucket buckets[BUCKET_COUNT];
	uint64_t resize_msec = 0;

public:
	static constexpr uint64_t MIN_SIZE = 32 * 1000 * 1000;

	_FORCE_INLINE_ void record_peak(uint64_t p_bytes, uint64_t p_msec) {
		const uint64_t index = p_msec / BUCKET_MSEC;
		Bucket &bucket = buckets[index % BUCKET_COUNT];
		if (bucket.index != index) {
			bucket = { index, 0 };
		}
		bucket.peak = MAX(bucket.peak, p_bytes);
	}

	_FORCE_INLINE_ void record_resize(uint64_t p_msec) { resize_msec = p_msec; }

	uint64_t get_window_peak(uint64_t p_msec) const {
		const uint64_t index = p_msec / BUCKET_MSEC;
		uint64_t peak = 0;
		for (const Bucket &bucket : buckets) {
			if (bucket.index <= index && index - bucket.index < BUCKET_COUNT) {
				peak = MAX(peak, bucket.peak);
			}
		}
		return peak;
	}

	// Returns 0 to keep the p_size buffer.
	uint64_t get_shrink_size(uint64_t p_size, uint64_t p_msec) const {
		if (p_msec - resize_msec < SHRINK_DELAY_MSEC) {
			return 0;
		}
		const uint64_t size = MAX(MIN_SIZE, get_window_peak(p_msec) * PEAK_HEADROOM);
		return size < p_size ? size : 0;
	}
};
