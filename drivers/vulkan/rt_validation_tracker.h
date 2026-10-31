/**************************************************************************/
/*  rt_validation_tracker.h                                               */
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

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/span.h"

// Remembers where each acceleration structure was last built, so that driver-side ray tracing validation messages,
// which only carry device addresses and arrive after the work was submitted, can be traced back to the code that
// queued the build. Thread-safe.
class RTValidationTracker {
public:
	// Upper bound of call stack frames kept per acceleration structure.
	static constexpr uint32_t MAX_STACK_FRAMES = 32;

	// Kind of build passed to record_build().
	enum BuildOperation {
		BUILD_OPERATION_BUILD,
		BUILD_OPERATION_UPDATE,
	};

	// Starts tracking the acceleration structure whose memory begins at p_address.
	void add(uint64_t p_address, bool p_top_level);
	// Names a tracked acceleration structure; ignored if p_address isn't tracked.
	void set_name(uint64_t p_address, const String &p_name);
	// Records that a build of a tracked acceleration structure was queued in frame p_frame from p_stack (innermost
	// frame first, as returned by backtrace_capture()). Ignored if p_address isn't tracked.
	void record_build(uint64_t p_address, BuildOperation p_operation, uint64_t p_frame, Span<void *> p_stack);
	// Stops tracking the acceleration structure at p_address; call when its memory is released.
	void erase(uint64_t p_address);

	// Describes every acceleration structure whose address follows "address: 0x" in p_message. Each distinct call stack
	// is printed in full the first time and referred to by number afterwards. Returns an empty string if nothing is
	// tracked or no address was found.
	String annotate(const char *p_message);

	// Extracts the unique hexadecimal values following "address: 0x" in p_message, in order of appearance.
	static void parse_addresses(const char *p_message, LocalVector<uint64_t> &r_addresses);

private:
	struct Record {
		String name;
		bool top_level = false;
		bool built = false;
		BuildOperation last_operation = BUILD_OPERATION_BUILD;
		uint64_t last_frame = 0;
		uint32_t builds_in_last_frame = 0;
		uint32_t stack_size = 0;
		void *stack[MAX_STACK_FRAMES] = {};
	};

	// Snapshot of one annotated address, taken under the lock so symbols can be resolved without holding it.
	struct Annotation {
		String summary;
		uint32_t stack_size = 0;
		void *stack[MAX_STACK_FRAMES] = {};
	};

	Mutex mutex;
	HashMap<uint64_t, Record> records;
	// Call stack hash to the number shown in annotations.
	HashMap<uint32_t, uint32_t> stack_numbers;
	uint64_t latest_frame = 0;

	void _snapshot(uint64_t p_address, Annotation &r_annotation);
};
