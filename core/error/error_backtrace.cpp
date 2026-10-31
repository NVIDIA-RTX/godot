/**************************************************************************/
/*  error_backtrace.cpp                                                   */
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

#include "core/error/error_backtrace.h"

#include "core/string/ustring.h"

#include <cstring>

#if defined(ERROR_BACKTRACE_ENABLED)

#include <cstdio>

static constexpr uint32_t BACKTRACE_DUMP_MAX_FRAMES = 62;

static String _backtrace_frame_line(uint32_t p_index, const String &p_text) {
	return "   [" + String::num_int64((int64_t)p_index) + "] " + p_text + "\n";
}

static String _backtrace_address_text(const void *p_address) {
	return "0x" + String::num_uint64((uint64_t)(uintptr_t)p_address, 16, true);
}

static String _backtrace_header(uint32_t p_frame_count) {
	return "   Native C++ backtrace (" + String::num_int64((int64_t)p_frame_count) + " frames):\n";
}

static constexpr const char *BACKTRACE_FOOTER = "   -- end native C++ backtrace --\n";

#ifdef WINDOWS_ENABLED

#include "core/os/mutex.h"

#include <windows.h>

// Some versions of imagehlp.dll lack the proper packing directives themselves
// so we need to do it.
#pragma pack(push, before_imagehlp, 8)
#include <imagehlp.h>
#pragma pack(pop, before_imagehlp)

// DbgHelp is single-threaded; every Sym* call must hold this lock.
static Mutex sym_mutex;
static bool sym_initialized = false;

static void _ensure_sym_initialized() {
	if (sym_initialized) {
		return;
	}
	HANDLE process = GetCurrentProcess();
	if (SymInitialize(process, nullptr, TRUE)) {
		SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
		sym_initialized = true;
	}
}

static String _backtrace_symbol_text(HANDLE p_process, void *p_frame) {
	const DWORD64 address = reinterpret_cast<DWORD64>(p_frame);
	DWORD64 displacement = 0;
	char symbol_buffer[sizeof(IMAGEHLP_SYMBOL64) + 1024];
	IMAGEHLP_SYMBOL64 *symbol = reinterpret_cast<IMAGEHLP_SYMBOL64 *>(symbol_buffer);
	memset(symbol, 0, sizeof(IMAGEHLP_SYMBOL64));
	symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
	symbol->MaxNameLength = 1024;

	if (!SymGetSymFromAddr64(p_process, address, &displacement, symbol) || symbol->Name[0] == '\0') {
		return _backtrace_address_text(p_frame);
	}

	char und_name[1024];
	if (UnDecorateSymbolName(symbol->Name, und_name, sizeof(und_name), UNDNAME_COMPLETE) == 0) {
		snprintf(und_name, sizeof(und_name), "%s", symbol->Name);
	}
	DWORD offset_line = 0;
	IMAGEHLP_LINE64 line = {};
	line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
	if (SymGetLineFromAddr64(p_process, address, &offset_line, &line) && line.FileName) {
		return String::utf8(und_name) + " (" + String::utf8(line.FileName) + ":" + String::num_uint64((uint64_t)line.LineNumber) + ")";
	}
	return String::utf8(und_name);
}

static String _backtrace_format_native(void *const *p_frames, uint32_t p_frame_count) {
	MutexLock lock(sym_mutex);
	_ensure_sym_initialized();

	HANDLE process = GetCurrentProcess();
	String out = _backtrace_header(p_frame_count);
	for (uint32_t i = 0; i < p_frame_count; i++) {
		out += _backtrace_frame_line(i, sym_initialized ? _backtrace_symbol_text(process, p_frames[i]) : _backtrace_address_text(p_frames[i]));
	}
	return out + BACKTRACE_FOOTER;
}

#elif defined(__has_include) && __has_include(<execinfo.h>)

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <cstdlib>

static String _backtrace_symbol_text(char **p_symbols, uint32_t p_index, void *p_frame) {
	char fname[1024] = "???";
	if (p_symbols && p_symbols[p_index]) {
		snprintf(fname, sizeof(fname), "%s", p_symbols[p_index]);
	}

	Dl_info info;
	if (dladdr(p_frame, &info) && info.dli_sname && info.dli_sname[0] == '_') {
		int status = 0;
		char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
		if (status == 0 && demangled) {
			snprintf(fname, sizeof(fname), "%s", demangled);
		}
		if (demangled) {
			free(demangled);
		}
	}
	return String::utf8(fname);
}

static String _backtrace_format_native(void *const *p_frames, uint32_t p_frame_count) {
	char **symbols = backtrace_symbols(p_frames, (int)p_frame_count);
	String out = _backtrace_header(p_frame_count);
	for (uint32_t i = 0; i < p_frame_count; i++) {
		out += _backtrace_frame_line(i, _backtrace_symbol_text(symbols, i, p_frames[i]));
	}
	if (symbols) {
		free(symbols);
	}
	return out + BACKTRACE_FOOTER;
}

#else

static String _backtrace_format_native(void *const *p_frames, uint32_t p_frame_count) {
	return String("   Native C++ backtrace: (not available on this platform or compiler)\n");
}

#endif

// Kept out of line, and never ending in a tail call, so it is always exactly one frame to skip.
_NO_INLINE_ uint32_t backtrace_capture(void **r_frames, uint32_t p_max_frames, uint32_t p_skip_frames) {
	if (r_frames == nullptr || p_max_frames == 0) {
		return 0;
	}
	const uint32_t skip = p_skip_frames + 1;
#ifdef WINDOWS_ENABLED
	const uint32_t captured = CaptureStackBackTrace(skip, p_max_frames, r_frames, nullptr);
	return MIN(captured, p_max_frames);
#elif defined(__has_include) && __has_include(<execinfo.h>)
	// backtrace() can't skip frames, so capture into a scratch buffer first.
	void *all_frames[128];
	const int captured = backtrace(all_frames, (int)MIN(p_max_frames + skip, (uint32_t)std_size(all_frames)));
	if (captured <= (int)skip) {
		return 0;
	}
	const uint32_t count = (uint32_t)captured - skip;
	memcpy(r_frames, all_frames + skip, count * sizeof(void *));
	return count;
#else
	return 0;
#endif
}

String backtrace_format(void *const *p_frames, uint32_t p_frame_count) {
	if (p_frame_count == 0) {
		return String("   Native C++ backtrace: (unavailable)\n");
	}
	return _backtrace_format_native(p_frames, p_frame_count);
}

String backtrace_dump() {
	void *frames[BACKTRACE_DUMP_MAX_FRAMES];
	return backtrace_format(frames, backtrace_capture(frames, BACKTRACE_DUMP_MAX_FRAMES, 1));
}

#else

String backtrace_dump() {
	return String();
}

uint32_t backtrace_capture(void **r_frames, uint32_t p_max_frames, uint32_t p_skip_frames) {
	return 0;
}

String backtrace_format(void *const *p_frames, uint32_t p_frame_count) {
	return String();
}

#endif
