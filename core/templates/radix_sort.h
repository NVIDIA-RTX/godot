/**************************************************************************/
/*  radix_sort.h                                                          */
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

#include <cstring>
#include <type_traits>

// Maps a float to an unsigned key with the same ordering, negatives and -0.0 included. NaN sorts past +inf.
_FORCE_INLINE_ uint32_t radix_sort_key_float(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits ^ ((bits & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u);
}

// Maps a signed integer to an unsigned key with the same ordering.
_FORCE_INLINE_ uint32_t radix_sort_key_int32(int32_t p_value) {
	return uint32_t(p_value) ^ 0x80000000u;
}

// Stable LSD radix sort, one pass per key byte. KeyExtractor maps an element to an unsigned integer
// key (see the radix_sort_key_* helpers). Passes where every element shares the same byte are skipped.
template <typename T, typename KeyExtractor, typename Key = uint32_t>
class RadixSort {
	static_assert(std::is_unsigned_v<Key>, "RadixSort keys must be unsigned integers.");

public:
	KeyExtractor get_key;

	// Sorts p_array in place. p_scratch must hold p_len elements; its contents are clobbered.
	void sort(T *p_array, T *p_scratch, int64_t p_len) const {
		if (p_len < 2) {
			return;
		}

		T *src = p_array;
		T *dst = p_scratch;
		for (uint32_t shift = 0; shift < sizeof(Key) * 8; shift += 8) {
			int64_t offsets[256] = {};
			for (int64_t i = 0; i < p_len; i++) {
				offsets[_digit(src[i], shift)]++;
			}
			if (offsets[_digit(src[0], shift)] == p_len) {
				continue;
			}

			int64_t sum = 0;
			for (int64_t &offset : offsets) {
				const int64_t bucket_size = offset;
				offset = sum;
				sum += bucket_size;
			}
			for (int64_t i = 0; i < p_len; i++) {
				dst[offsets[_digit(src[i], shift)]++] = src[i];
			}
			SWAP(src, dst);
		}

		if (src != p_array) {
			for (int64_t i = 0; i < p_len; i++) {
				p_array[i] = src[i];
			}
		}
	}

private:
	_FORCE_INLINE_ uint32_t _digit(const T &p_element, uint32_t p_shift) const {
		return uint32_t((Key(get_key(p_element)) >> p_shift) & 0xFF);
	}
};
