/**************************************************************************/
/*  d3d12_pix_markers.cpp                                                 */
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

#include "d3d12_pix_markers.h"

#include "core/error/error_macros.h"

#include <d3d12.h>

// PIX V2 blob constants (PIXEventsCommon.h / pix3_win.h).
// Blob layout: [header][color][context ptr][label bytes...][0 terminator]
static const uint32_t PIX_EVENT_RECORD_QWORDS = 64;
static const uint32_t PIX_EVENT_TAIL_QWORDS = 2;

// Bits 0-6: size, bits 7-11: event type, bits 12-19: metadata, bits 20-63: timestamp.
static const uint64_t PIX_EVENT_BEGIN_TYPE = 0x01;
// ON_CONTEXT (0x01) | STRING_IS_ANSI (0x02) | HAS_COLOR (0xF0)
static const uint8_t PIX_EVENT_METADATA_BEGIN = 0xF3;

static uint64_t _pix_encode_event_info(uint8_t p_size_qwords) {
	return ((PIX_EVENT_BEGIN_TYPE & 0x1Full) << 7) |
			((static_cast<uint64_t>(PIX_EVENT_METADATA_BEGIN) & 0xFFull) << 12) |
			((static_cast<uint64_t>(p_size_qwords) & 0x7Full) << 0);
}

static void _pix_copy_ansi_string(uint64_t *&r_dest, const uint64_t *p_limit, const char *p_string) {
	while (r_dest < p_limit) {
		uint64_t x = 0;
		for (int i = 0; i < 8; i++) {
			const unsigned char c = (unsigned char)p_string[i];
			if (!c) {
				*r_dest++ = x;
				return;
			}
			x |= (uint64_t)c << (i * 8);
		}
		*r_dest++ = x;
		p_string += 8;
	}
}

void d3d12_pix_begin_event(ID3D12GraphicsCommandList *p_command_list, uint32_t p_bgra_color, const char *p_name) {
	ERR_FAIL_NULL(p_command_list);
	ERR_FAIL_NULL(p_name);

	uint64_t buffer[PIX_EVENT_RECORD_QWORDS] = {};
	uint64_t *dest = buffer;
	const uint64_t *limit = buffer + PIX_EVENT_RECORD_QWORDS - PIX_EVENT_TAIL_QWORDS;

	uint64_t *header = dest++;
	*dest++ = (uint64_t)p_bgra_color;
	*dest++ = (uint64_t)(uintptr_t)p_command_list;
	_pix_copy_ansi_string(dest, limit, p_name);
	if (dest < limit) {
		*dest++ = 0;
	}

	*header = _pix_encode_event_info((uint8_t)(dest - buffer));

	p_command_list->BeginEvent(D3D12_PIX_EVENT_METADATA_V2, buffer, (UINT)((dest - buffer) * sizeof(uint64_t)));
}

void d3d12_pix_end_event(ID3D12GraphicsCommandList *p_command_list) {
	ERR_FAIL_NULL(p_command_list);
	p_command_list->EndEvent();
}
