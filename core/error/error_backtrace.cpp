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

#ifdef WINDOWS_ENABLED

#include "core/os/mutex.h"

#include <windows.h>

// Some versions of imagehlp.dll lack the proper packing directives themselves
// so we need to do it.
#pragma pack(push, before_imagehlp, 8)
#include <imagehlp.h>
#pragma pack(pop, before_imagehlp)

static Mutex sym_init_mutex;
static bool sym_initialized = false;

static void _ensure_sym_initialized() {
	MutexLock lock(sym_init_mutex);
	if (sym_initialized) {
		return;
	}
	HANDLE process = GetCurrentProcess();
	if (SymInitialize(process, nullptr, TRUE)) {
		SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
		sym_initialized = true;
	}
}

static String _native_backtrace_string_windows() {
	_ensure_sym_initialized();

	void *stack[62];
	const USHORT frame_count = CaptureStackBackTrace(0, 62, stack, nullptr);

	String out = "   Native C++ backtrace (" + String::num_int64((int64_t)frame_count) + " frames):\n";

	HANDLE process = GetCurrentProcess();
	for (USHORT i = 0; i < frame_count; i++) {
		const DWORD64 address = reinterpret_cast<DWORD64>(stack[i]);
		if (!sym_initialized) {
			out += "   [" + String::num_int64((int64_t)i) + "] 0x" + String::num_uint64((uint64_t)(uintptr_t)stack[i], 16, true) + "\n";
			continue;
		}

		DWORD64 displacement = 0;
		char symbol_buffer[sizeof(IMAGEHLP_SYMBOL64) + 1024];
		IMAGEHLP_SYMBOL64 *symbol = reinterpret_cast<IMAGEHLP_SYMBOL64 *>(symbol_buffer);
		memset(symbol, 0, sizeof(IMAGEHLP_SYMBOL64));
		symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
		symbol->MaxNameLength = 1024;

		if (SymGetSymFromAddr64(process, address, &displacement, symbol) && symbol->Name[0] != '\0') {
			char und_name[1024];
			if (UnDecorateSymbolName(symbol->Name, und_name, sizeof(und_name), UNDNAME_COMPLETE) == 0) {
				snprintf(und_name, sizeof(und_name), "%s", symbol->Name);
			}
			DWORD offset_line = 0;
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			if (SymGetLineFromAddr64(process, address, &offset_line, &line) && line.FileName) {
				out += "   [" + String::num_int64((int64_t)i) + "] " + String::utf8(und_name) + " (" + String::utf8(line.FileName) + ":" + String::num_uint64((uint64_t)line.LineNumber) + ")\n";
			} else {
				out += "   [" + String::num_int64((int64_t)i) + "] " + String::utf8(und_name) + "\n";
			}
		} else {
			out += "   [" + String::num_int64((int64_t)i) + "] 0x" + String::num_uint64((uint64_t)(uintptr_t)stack[i], 16, true) + "\n";
		}
	}

	out += "   -- end native C++ backtrace --\n";
	return out;
}

#elif defined(__has_include)

#if __has_include(<execinfo.h>)

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <cstdlib>

static String _native_backtrace_string_unix() {
	void *frames[64];
	const int n = backtrace(frames, 64);
	if (n <= 0) {
		return String("   Native C++ backtrace: (unavailable)\n");
	}

	char **symbols = backtrace_symbols(frames, n);
	String out = "   Native C++ backtrace (" + String::num_int64((int64_t)n) + " frames):\n";

	for (int i = 0; i < n; i++) {
		char fname[1024] = "???";
		if (symbols && symbols[i]) {
			snprintf(fname, sizeof(fname), "%s", symbols[i]);
		}

		Dl_info info;
		if (dladdr(frames[i], &info) && info.dli_sname && info.dli_sname[0] == '_') {
			int status = 0;
			char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
			if (status == 0 && demangled) {
				snprintf(fname, sizeof(fname), "%s", demangled);
			}
			if (demangled) {
				free(demangled);
			}
		}

		out += "   [" + String::num_int64((int64_t)i) + "] " + String::utf8(fname) + "\n";
	}

	if (symbols) {
		free(symbols);
	}

	out += "   -- end native C++ backtrace --\n";
	return out;
}

#else

static String _native_backtrace_string_stub() {
	return String("   Native C++ backtrace: (execinfo.h not available on this platform)\n");
}

#endif

#else

static String _native_backtrace_string_stub() {
	return String("   Native C++ backtrace: (__has_include not supported; rebuild with a C++17 compiler)\n");
}

#endif

String backtrace_dump() {
#ifdef WINDOWS_ENABLED
	return _native_backtrace_string_windows();
#elif defined(__has_include) && __has_include(<execinfo.h>)
	return _native_backtrace_string_unix();
#else
	return _native_backtrace_string_stub();
#endif
}

#else

String backtrace_dump() {
	return String();
}

#endif
