/**************************************************************************/
/*  crash_handler_windows_seh.cpp                                         */
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

#include "crash_handler_windows.h"

#include "core/config/project_settings.h"
#include "core/object/script_language.h"
#include "core/os/main_loop.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "main/main.h"

#ifdef CRASH_HANDLER_EXCEPTION

// Backtrace code based on: https://stackoverflow.com/questions/6205981/windows-c-stack-trace-from-a-running-app

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include <intrin.h>
#include <psapi.h>

// Some versions of dbghelp.dll lack the proper packing directives themselves
// so we need to do it.
#pragma pack(push, before_dbghelp, 8)
#include <dbghelp.h>
#pragma pack(pop, before_dbghelp)

// Fatal CRT conditions that never raise a structured exception get synthetic exception codes.
static constexpr DWORD CRASH_CODE_ABORT = 0xE0474401;
static constexpr DWORD CRASH_CODE_PURE_CALL = 0xE0474402;
static constexpr DWORD CRASH_CODE_INVALID_PARAMETER = 0xE0474403;
static constexpr DWORD EXCEPTION_CODE_CPP = 0xE06D7363;
static constexpr DWORD EXCEPTION_CODE_HEAP_CORRUPTION = 0xC0000374;

// Stack the main thread keeps in reserve so the handler can run after a stack overflow.
static constexpr ULONG CRASH_STACK_GUARANTEE = 64 * 1024;
static constexpr DWORD CRASH_REPORT_TIMEOUT_MSEC = 60 * 1000;

static const char *_exception_name(DWORD p_code) {
	switch (p_code) {
		case EXCEPTION_ACCESS_VIOLATION:
			return "access violation";
		case EXCEPTION_STACK_OVERFLOW:
			return "stack overflow";
		case EXCEPTION_BREAKPOINT:
			return "breakpoint, CRASH_NOW or CRASH_COND";
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			return "illegal instruction";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			return "integer divide by zero";
		case EXCEPTION_IN_PAGE_ERROR:
			return "in-page error";
		case EXCEPTION_CODE_HEAP_CORRUPTION:
			return "heap corruption";
		case EXCEPTION_CODE_CPP:
			return "unhandled C++ exception";
		case CRASH_CODE_ABORT:
			return "abort()";
		case CRASH_CODE_PURE_CALL:
			return "pure virtual function call";
		case CRASH_CODE_INVALID_PARAMETER:
			return "invalid CRT parameter";
		default:
			return "unknown exception";
	}
}

struct module_data {
	std::string image_name;
	std::string module_name;
	void *base_address = nullptr;
	DWORD load_size;
};

class symbol {
	typedef IMAGEHLP_SYMBOL64 sym_type;
	sym_type *sym;
	static const int max_name_len = 1024;

public:
	symbol(HANDLE process, DWORD64 address) :
			sym((sym_type *)::operator new(sizeof(*sym) + max_name_len)) {
		memset(sym, '\0', sizeof(*sym) + max_name_len);
		sym->SizeOfStruct = sizeof(*sym);
		sym->MaxNameLength = max_name_len;
		DWORD64 displacement;

		SymGetSymFromAddr64(process, address, &displacement, sym);
	}

	std::string name() { return std::string(sym->Name); }
	std::string undecorated_name() {
		if (*sym->Name == '\0') {
			return "<couldn't map PC to fn name>";
		}
		std::vector<char> und_name(max_name_len);
		UnDecorateSymbolName(sym->Name, &und_name[0], max_name_len, UNDNAME_COMPLETE);
		return std::string(&und_name[0], strlen(&und_name[0]));
	}
};

class get_mod_info {
	HANDLE process;

public:
	get_mod_info(HANDLE h) :
			process(h) {}

	module_data operator()(HMODULE module) {
		module_data ret;
		char temp[4096];
		MODULEINFO mi;

		GetModuleInformation(process, module, &mi, sizeof(mi));
		ret.base_address = mi.lpBaseOfDll;
		ret.load_size = mi.SizeOfImage;

		GetModuleFileNameEx(process, module, temp, sizeof(temp));
		ret.image_name = temp;
		GetModuleBaseName(process, module, temp, sizeof(temp));
		ret.module_name = temp;
		SymLoadModule64(process, nullptr, ret.image_name.c_str(), ret.module_name.c_str(), (DWORD64)ret.base_address, ret.load_size);
		return ret;
	}
};

struct CrashReport {
	EXCEPTION_POINTERS *exception = nullptr;
	HANDLE thread = nullptr; // Real handle to the crashing thread, which waits while the report is written.
	DWORD thread_id = 0;
};

static String _minidump_path() {
	const String user_dir = OS::get_singleton()->get_user_data_dir();
	const String dir = user_dir.is_empty() ? OS::get_singleton()->get_executable_path().get_base_dir() : user_dir.path_join("logs");
	return dir.path_join(vformat("crash_%d_%d.dmp", (int64_t)GetCurrentProcessId(), (int64_t)GetTickCount64()));
}

static void _write_minidump(const CrashReport &p_report) {
	const String path = _minidump_path();
	CreateDirectoryW((LPCWSTR)path.get_base_dir().utf16().get_data(), nullptr);
	HANDLE file = CreateFileW((LPCWSTR)path.utf16().get_data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		print_error(vformat("Could not create the minidump at %s.", path));
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION info = {};
	info.ThreadId = p_report.thread_id;
	info.ExceptionPointers = p_report.exception;
	info.ClientPointers = FALSE;
	const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
	const bool written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
	CloseHandle(file);
	print_error(written ? vformat("Minidump written to %s", path) : vformat("Writing the minidump to %s failed.", path));
}

static void _print_native_backtrace(const CrashReport &p_report) {
	HANDLE process = GetCurrentProcess();
	DWORD offset_from_symbol = 0;
	IMAGEHLP_LINE64 line = {};
	std::vector<module_data> modules;
	DWORD cbNeeded;
	std::vector<HMODULE> module_handles(1);

	// Load the symbols:
	if (!SymInitialize(process, nullptr, false)) {
		return;
	}

	SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_EXACT_SYMBOLS);
	EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
	module_handles.resize(cbNeeded / sizeof(HMODULE));
	EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
	std::transform(module_handles.begin(), module_handles.end(), std::back_inserter(modules), get_mod_info(process));
	void *base = modules[0].base_address;

	// Setup stuff. StackWalk64 modifies the context, so walk a copy.
	CONTEXT context_copy = *p_report.exception->ContextRecord;
	CONTEXT *context = &context_copy;
	STACKFRAME64 frame;
	bool skip_first = false;

	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;

#if defined(_M_X64)
	frame.AddrPC.Offset = context->Rip;
	frame.AddrStack.Offset = context->Rsp;
	frame.AddrFrame.Offset = context->Rbp;
#elif defined(_M_ARM64) || defined(_M_ARM64EC)
	frame.AddrPC.Offset = context->Pc;
	frame.AddrStack.Offset = context->Sp;
	frame.AddrFrame.Offset = context->Fp;
#elif defined(_M_ARM)
	frame.AddrPC.Offset = context->Pc;
	frame.AddrStack.Offset = context->Sp;
	frame.AddrFrame.Offset = context->R11;
#else
	frame.AddrPC.Offset = context->Eip;
	frame.AddrStack.Offset = context->Esp;
	frame.AddrFrame.Offset = context->Ebp;

	// Skip the first one to avoid a duplicate on 32-bit mode
	skip_first = true;
#endif

	line.SizeOfStruct = sizeof(line);
	IMAGE_NT_HEADERS *h = ImageNtHeader(base);
	DWORD image_type = h->FileHeader.Machine;

	int n = 0;
	do {
		if (skip_first) {
			skip_first = false;
		} else {
			if (frame.AddrPC.Offset != 0) {
				std::string fnName = symbol(process, frame.AddrPC.Offset).undecorated_name();

				if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &offset_from_symbol, &line)) {
					print_error(vformat("[%d] %s (%s:%d)", n, fnName.c_str(), (char *)line.FileName, (int)line.LineNumber));
				} else {
					// Module and offset still locate frames in modules without symbols (drivers, system DLLs).
					String location = "?";
					for (const module_data &m : modules) {
						const DWORD64 module_base = (DWORD64)m.base_address;
						if (frame.AddrPC.Offset >= module_base && frame.AddrPC.Offset < module_base + m.load_size) {
							location = vformat("%s+0x%X", m.module_name.c_str(), (uint64_t)(frame.AddrPC.Offset - module_base));
							break;
						}
					}
					print_error(vformat("[%d] %s [%s]", n, fnName.c_str(), location));
				}
			} else {
				print_error(vformat("[%d] ???", n));
			}

			n++;
		}

		if (!StackWalk64(image_type, process, p_report.thread, &frame, context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
			break;
		}
	} while (frame.AddrReturn.Offset != 0 && n < 256);

	SymCleanup(process);
}

// Runs on a fresh thread: the crashing thread's stack may be exhausted, and MiniDumpWriteDump cannot reliably dump its own thread.
static DWORD WINAPI _crash_report_thread(LPVOID p_report) {
	const CrashReport &report = *static_cast<const CrashReport *>(p_report);
	_write_minidump(report);
	_print_native_backtrace(report);
	return 0;
}

static void _run_crash_report(EXCEPTION_POINTERS *ep) {
	CrashReport report;
	report.exception = ep;
	report.thread_id = GetCurrentThreadId();
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &report.thread, 0, FALSE, DUPLICATE_SAME_ACCESS);

	HANDLE reporter = CreateThread(nullptr, 0, _crash_report_thread, &report, 0, nullptr);
	if (reporter) {
		WaitForSingleObject(reporter, CRASH_REPORT_TIMEOUT_MSEC);
		CloseHandle(reporter);
	} else {
		print_error("Could not start the crash report thread.");
	}
	CloseHandle(report.thread);
}

DWORD CrashHandlerException(EXCEPTION_POINTERS *ep) {
	if (OS::get_singleton() == nullptr || OS::get_singleton()->is_disable_crash_handler() || IsDebuggerPresent()) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// A main-thread crash reaches both the __except filter in main and the unhandled exception filter;
	// concurrent crashes on other threads must not interleave their dumps either.
	static std::atomic_flag dumped = ATOMIC_FLAG_INIT;
	if (dumped.test_and_set()) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (OS::get_singleton()->is_crash_handler_silent()) {
		std::_Exit(0);
	}

	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	// Only the reserved stack is left; keep work on this thread minimal.
	const bool stack_overflow = code == EXCEPTION_STACK_OVERFLOW;

	String msg;
	if (ProjectSettings::get_singleton()) {
		msg = GLOBAL_GET("debug/settings/crash_handler/message");
	}

	// Tell MainLoop about the crash. This can be handled by users too in Node.
	// The scene tree can only be notified from the main thread.
	if (!stack_overflow && OS::get_singleton()->get_main_loop() && Thread::is_main_thread()) {
		OS::get_singleton()->get_main_loop()->notification(MainLoop::NOTIFICATION_CRASH);
	}

	print_error("\n================================================================");
	print_error(vformat("%s: Program crashed", __FUNCTION__));
	print_error(vformat("Exception 0x%08X (%s) at 0x%016X on %s thread %d", (uint64_t)code, _exception_name(code),
			(uint64_t)ep->ExceptionRecord->ExceptionAddress, Thread::is_main_thread() ? "main" : "worker", (int64_t)GetCurrentThreadId()));

	// Print the engine version just before, so that people are reminded to include the version in backtrace reports.
	if (String(GODOT_VERSION_HASH).is_empty()) {
		print_error(vformat("Engine version: %s", GODOT_VERSION_FULL_NAME));
	} else {
		print_error(vformat("Engine version: %s (%s)", GODOT_VERSION_FULL_NAME, GODOT_VERSION_HASH));
	}
	print_error(vformat("Dumping the backtrace. %s", msg));

	_run_crash_report(ep);

	print_error("-- END OF C++ BACKTRACE --");
	print_error("================================================================");

	if (!stack_overflow) {
		for (const Ref<ScriptBacktrace> &backtrace : ScriptServer::capture_script_backtraces(false)) {
			if (!backtrace->is_empty()) {
				print_error(backtrace->format());
				print_error(vformat("-- END OF %s BACKTRACE --", backtrace->get_language_name().to_upper()));
				print_error("================================================================");
			}
		}
	}

	// Pass the exception to the OS
	return EXCEPTION_CONTINUE_SEARCH;
}

// The __except block in main only covers the main thread; this catches crashes on every other thread.
static LONG WINAPI _unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
	return CrashHandlerException(ep);
}

// Reports a fatal CRT condition that never raises a structured exception, then hands it to Windows Error Reporting.
[[noreturn]] static void _report_fatal_condition(DWORD p_code) {
	CONTEXT context = {};
	RtlCaptureContext(&context);
	EXCEPTION_RECORD record = {};
	record.ExceptionCode = p_code;
	record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
	record.ExceptionAddress = _ReturnAddress();
	EXCEPTION_POINTERS pointers = { &record, &context };

	CrashHandlerException(&pointers);
	RaiseFailFastException(&record, &context, 0);
	std::_Exit(3);
}

static void __cdecl _on_abort_signal(int) {
	_report_fatal_condition(CRASH_CODE_ABORT);
}

static void __cdecl _on_pure_call() {
	_report_fatal_condition(CRASH_CODE_PURE_CALL);
}

static void __cdecl _on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t) {
	_report_fatal_condition(CRASH_CODE_INVALID_PARAMETER);
}
#endif

CrashHandler::CrashHandler() {
	disabled = false;
}

CrashHandler::~CrashHandler() {
}

void CrashHandler::disable() {
	if (disabled) {
		return;
	}

#ifdef CRASH_HANDLER_EXCEPTION
	SetUnhandledExceptionFilter(nullptr);
	signal(SIGABRT, SIG_DFL);
	_set_purecall_handler(nullptr);
	_set_invalid_parameter_handler(nullptr);
#endif
	disabled = true;
}

void CrashHandler::initialize() {
#ifdef CRASH_HANDLER_EXCEPTION
	SetUnhandledExceptionFilter(_unhandled_exception_filter);
	ULONG stack_guarantee = CRASH_STACK_GUARANTEE;
	SetThreadStackGuarantee(&stack_guarantee);
	// std::terminate and the default pure call handler end in abort(), so SIGABRT covers them on every thread.
	signal(SIGABRT, _on_abort_signal);
	_set_purecall_handler(_on_pure_call);
	_set_invalid_parameter_handler(_on_invalid_parameter);
#endif
}
