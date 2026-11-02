/**************************************************************************/
/*  rt_validation_tracker.cpp                                             */
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

#include "rt_validation_tracker.h"

#include "core/error/error_backtrace.h"
#include "core/templates/hashfuncs.h"
#include "core/variant/variant.h"

#include <cstdlib>
#include <cstring>

void RTValidationTracker::add(uint64_t p_address, bool p_top_level) {
	MutexLock lock(mutex);
	Record record;
	record.top_level = p_top_level;
	records.insert(p_address, record);
}

void RTValidationTracker::set_name(uint64_t p_address, const String &p_name) {
	MutexLock lock(mutex);
	Record *record = records.getptr(p_address);
	if (record) {
		record->name = p_name;
	}
}

void RTValidationTracker::record_build(uint64_t p_address, BuildOperation p_operation, uint64_t p_frame, Span<void *> p_stack) {
	MutexLock lock(mutex);
	Record *record = records.getptr(p_address);
	if (!record) {
		return;
	}
	record->builds_in_last_frame = (record->built && record->last_frame == p_frame) ? record->builds_in_last_frame + 1 : 1;
	record->built = true;
	record->last_operation = p_operation;
	record->last_frame = p_frame;
	record->stack_size = (uint32_t)MIN(p_stack.size(), (uint64_t)MAX_STACK_FRAMES);
	memcpy(record->stack, p_stack.ptr(), record->stack_size * sizeof(void *));
	latest_frame = MAX(latest_frame, p_frame);
}

void RTValidationTracker::erase(uint64_t p_address) {
	MutexLock lock(mutex);
	records.erase(p_address);
}

void RTValidationTracker::parse_addresses(const char *p_message, LocalVector<uint64_t> &r_addresses) {
	static constexpr char KEY[] = "address: 0x";
	if (p_message == nullptr) {
		return;
	}
	for (const char *cursor = strstr(p_message, KEY); cursor != nullptr; cursor = strstr(cursor, KEY)) {
		cursor += sizeof(KEY) - 1;
		char *end = nullptr;
		const uint64_t address = strtoull(cursor, &end, 16);
		if (end != cursor && !r_addresses.has(address)) {
			r_addresses.push_back(address);
		}
	}
}

void RTValidationTracker::_snapshot(uint64_t p_address, Annotation &r_annotation) {
	const String address_text = "0x" + String::num_uint64(p_address, 16);
	const Record *record = records.getptr(p_address);
	if (!record) {
		r_annotation.summary = address_text + ": not a live acceleration structure (freed, or not created through RenderingDevice)";
		return;
	}

	String summary = address_text + ": " + (record->top_level ? "TLAS" : "BLAS");
	summary += record->name.is_empty() ? String(" (unnamed)") : " \"" + record->name + "\"";
	if (!record->built) {
		r_annotation.summary = summary + ", never built";
		return;
	}

	summary += vformat(", last %s queued in frame %d", record->last_operation == BUILD_OPERATION_UPDATE ? "refit" : "build", record->last_frame);
	if (record->builds_in_last_frame > 1) {
		summary += vformat(" (%d times that frame)", record->builds_in_last_frame);
	}
	if (record->stack_size == 0) {
		r_annotation.summary = summary;
		return;
	}

	const uint32_t stack_hash = hash_murmur3_buffer(record->stack, record->stack_size * sizeof(void *));
	const uint32_t *known_number = stack_numbers.getptr(stack_hash);
	if (known_number) {
		r_annotation.summary = summary + vformat(", call stack #%d (printed earlier)", *known_number);
		return;
	}

	const uint32_t number = stack_numbers.size() + 1;
	stack_numbers.insert(stack_hash, number);
	r_annotation.summary = summary + vformat(", call stack #%d:", number);
	r_annotation.stack_size = record->stack_size;
	memcpy(r_annotation.stack, record->stack, record->stack_size * sizeof(void *));
}

String RTValidationTracker::annotate(const char *p_message) {
	LocalVector<uint64_t> addresses;
	parse_addresses(p_message, addresses);
	if (addresses.is_empty()) {
		return String();
	}

	LocalVector<Annotation> annotations;
	uint64_t frame = 0;
	{
		MutexLock lock(mutex);
		if (records.is_empty()) {
			return String();
		}
		frame = latest_frame;
		annotations.resize(addresses.size());
		for (uint32_t i = 0; i < addresses.size(); i++) {
			_snapshot(addresses[i], annotations[i]);
		}
	}

	String out = vformat("\n\tAcceleration structures (latest frame with a recorded build: %d):", frame);
	for (const Annotation &annotation : annotations) {
		out += "\n\t\t" + annotation.summary;
		if (annotation.stack_size > 0) {
			out += "\n" + backtrace_format(annotation.stack, annotation.stack_size);
		}
	}
	return out;
}
