/**************************************************************************/
/*  aftermath_context.cpp                                                 */
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

// All Godot and platform headers are only needed when Aftermath is compiled in.
// Godot headers come first so their typedefs don't clash with Windows/SDK macros.
// Platform headers must come BEFORE the SDK headers - the Aftermath SDK gates
// its D3D12/Vulkan PFN typedefs and structs on __d3d12_h__ / VULKAN_H_ being
// already defined. Without this, those typedefs simply don't exist.
#ifdef AFTERMATH_ENABLED
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef AFTERMATH_ENABLED_D3D12
#include <d3d12.h>
#endif
#ifdef AFTERMATH_ENABLED_VULKAN
#include <vulkan/vulkan.h>
#endif
#include "drivers/aftermath/aftermath_headers.h"
#endif // AFTERMATH_ENABLED

#include "aftermath_context.h"

// ---------------------------------------------------------------------------
// Singleton (always compiled)
// ---------------------------------------------------------------------------

AftermathContext &AftermathContext::get() {
	static AftermathContext ctx;
	return ctx;
}

// ---------------------------------------------------------------------------
// Stub implementations when AFTERMATH_ENABLED is off
// ---------------------------------------------------------------------------

#ifndef AFTERMATH_ENABLED

bool AftermathContext::load_functions() {
	return false;
}
void AftermathContext::register_shader(AftermathShaderType, const uint8_t *, uint32_t) {}
void AftermathContext::initialize_d3d12(void *) {}
void AftermathContext::wait_for_dump() {}
const char *const *AftermathContext::vk_required_extensions() const {
	return nullptr;
}
void *AftermathContext::vk_device_diagnostics_config() {
	return nullptr;
}

#else // AFTERMATH_ENABLED

// ---------------------------------------------------------------------------
// Macro: cast stored void* to the SDK function pointer type and call it.
// Usage: AM_CALL(PFN_GFSDK_Aftermath_Foo, fn_Foo, arg1, arg2, ...)
// ---------------------------------------------------------------------------
#define AM_CALL(PFNType, field, ...) \
	(reinterpret_cast<PFNType>(field)(__VA_ARGS__))

// ---------------------------------------------------------------------------
// Environment-variable feature toggles
//
// Defaults are tuned for "post-mortem mode": collect everything needed to
// debug a real crash, but DON'T add new fault sources of our own.
// Set to "0" to disable, anything else (or unset) keeps the default.
//   GODOT_AFTERMATH_SHADER_DEBUG_INFO         (default: on)
//   GODOT_AFTERMATH_RESOURCE_TRACKING         (default: on)
//   GODOT_AFTERMATH_AUTOMATIC_CHECKPOINTS     (default: on)
//   GODOT_AFTERMATH_CALLSTACK_CAPTURE         (default: on,  D3D12 only)
//   GODOT_AFTERMATH_MARKERS                   (default: on,  D3D12 only)
//   GODOT_AFTERMATH_SHADER_ERROR_REPORTING    (default: OFF; turns silent
//                                              shader UB into device-loss)
// ---------------------------------------------------------------------------
struct AftermathEnvOption {
	const char *name;
	bool default_on;
	const char *description;
};

static const AftermathEnvOption AFTERMATH_ENV_OPTIONS[] = {
	{ "GODOT_AFTERMATH_SHADER_DEBUG_INFO", true, "Embed shader debug info (.nvdbg) for source-level crash analysis" },
	{ "GODOT_AFTERMATH_RESOURCE_TRACKING", true, "Track resource usage for richer crash reports" },
	{ "GODOT_AFTERMATH_AUTOMATIC_CHECKPOINTS", true, "Vulkan only: insert automatic checkpoints" },
	{ "GODOT_AFTERMATH_CALLSTACK_CAPTURE", true, "D3D12 only: capture CPU callstacks at submit" },
	{ "GODOT_AFTERMATH_MARKERS", true, "D3D12 only: enable user event markers" },
	{ "GODOT_AFTERMATH_SHADER_ERROR_REPORTING", false, "Strict: turn silent shader UB into device-loss" },
};

static bool _aftermath_env_flag(const char *p_name, bool p_default) {
	OS *os = OS::get_singleton();
	if (os == nullptr || !os->has_environment(p_name)) {
		return p_default;
	}
	const String value = os->get_environment(p_name);
	const bool is_disabled = (value == "0");
	return !is_disabled;
}

static void _aftermath_log_env_options() {
	print_line("Aftermath: feature toggles (set env var to '0' to disable, anything else enables):");
	for (const AftermathEnvOption &opt : AFTERMATH_ENV_OPTIONS) {
		const bool active = _aftermath_env_flag(opt.name, opt.default_on);
		const char *state = active ? "ON " : "off";
		const char *def = opt.default_on ? "default ON " : "default OFF";
		print_line(String("  [") + state + "] " + opt.name + " (" + def + ") - " + opt.description);
	}
}

// ---------------------------------------------------------------------------
// Static data
// ---------------------------------------------------------------------------

const char *AftermathContext::vk_ext_names[3] = {
#ifdef AFTERMATH_ENABLED_VULKAN
	VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
	VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME,
#else
	nullptr,
	nullptr,
#endif
	nullptr,
};

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

bool AftermathContext::load_functions() {
	if (_initialized) {
		return true;
	}

#ifdef _WIN32
	HMODULE lib = LoadLibraryA("GFSDK_Aftermath_Lib.x64.dll");
	if (!lib) {
		WARN_PRINT("Aftermath: GFSDK_Aftermath_Lib.x64.dll not found - GPU crash dumps disabled. Place the DLL next to the executable.");
		return false;
	}

#define AM_GETPROC(field, export_name) \
	field = (void *)GetProcAddress(lib, export_name); \
	if (!field) { \
		print_verbose("Aftermath: missing export " export_name "."); \
		return false; \
	}

	AM_GETPROC(fn_EnableGpuCrashDumps, "GFSDK_Aftermath_EnableGpuCrashDumps")
	AM_GETPROC(fn_DisableGpuCrashDumps, "GFSDK_Aftermath_DisableGpuCrashDumps")
	AM_GETPROC(fn_GetCrashDumpStatus, "GFSDK_Aftermath_GetCrashDumpStatus")
	AM_GETPROC(fn_GetShaderDebugInfoIdentifier, "GFSDK_Aftermath_GetShaderDebugInfoIdentifier")
	AM_GETPROC(fn_GpuCrashDump_CreateDecoder, "GFSDK_Aftermath_GpuCrashDump_CreateDecoder")
	AM_GETPROC(fn_GpuCrashDump_GenerateJSON, "GFSDK_Aftermath_GpuCrashDump_GenerateJSON")
	AM_GETPROC(fn_GpuCrashDump_GetJSON, "GFSDK_Aftermath_GpuCrashDump_GetJSON")
	AM_GETPROC(fn_GpuCrashDump_DestroyDecoder, "GFSDK_Aftermath_GpuCrashDump_DestroyDecoder")

#ifdef AFTERMATH_ENABLED_D3D12
	AM_GETPROC(fn_DX12_Initialize, "GFSDK_Aftermath_DX12_Initialize")
	AM_GETPROC(fn_GetShaderHash, "GFSDK_Aftermath_GetShaderHash")
#endif
#ifdef AFTERMATH_ENABLED_VULKAN
	AM_GETPROC(fn_GetShaderHashSpirv, "GFSDK_Aftermath_GetShaderHashSpirv")
#endif

#undef AM_GETPROC

	_create_dump_dirs();

	GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_EnableGpuCrashDumps, fn_EnableGpuCrashDumps,
			GFSDK_Aftermath_Version_API,
			GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX | GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
			GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
			reinterpret_cast<PFN_GFSDK_Aftermath_GpuCrashDumpCb>(&AftermathContext::_cb_gpu_crash_dump),
			reinterpret_cast<PFN_GFSDK_Aftermath_ShaderDebugInfoCb>(&AftermathContext::_cb_shader_debug_info),
			reinterpret_cast<PFN_GFSDK_Aftermath_GpuCrashDumpDescriptionCb>(&AftermathContext::_cb_description),
			reinterpret_cast<PFN_GFSDK_Aftermath_ResolveMarkerCb>(&AftermathContext::_cb_resolve_marker),
			this);

	if (res != GFSDK_Aftermath_Result_Success) {
		WARN_PRINT("Aftermath: EnableGpuCrashDumps failed - GPU crash dumps disabled.");
		return false;
	}

	_initialized = true;
	print_line("Aftermath: GPU crash dump capture enabled.");
	print_line(String("Aftermath: Output directory -> ") + _dump_dir);
	print_line("Aftermath: Crash dumps, shader binaries and debug info are all colocated here. Open the .nv-gpudmp file in Nsight - shaders are auto-discovered.");
	_aftermath_log_env_options();
	return true;
#else
	return false;
#endif // _WIN32
}

// ---------------------------------------------------------------------------
// Dump directory creation
// ---------------------------------------------------------------------------

void AftermathContext::_create_dump_dirs() {
#ifdef _WIN32
	char tmp_path[MAX_PATH] = {};
	GetTempPathA(MAX_PATH, tmp_path);

	// GetTempFileNameA creates a uniquely named file; delete it and create a
	// directory with the same name so the path is unique per process.
	char tmp_file[MAX_PATH] = {};
	GetTempFileNameA(tmp_path, "gdam", 0, tmp_file);
	DeleteFileA(tmp_file);
	CreateDirectoryA(tmp_file, nullptr);

	// Use strncpy_s to avoid MSVC C4996.
	strncpy_s(_dump_dir, sizeof(_dump_dir), tmp_file, _TRUNCATE);
#endif
}

// ---------------------------------------------------------------------------
// Shader registration
// ---------------------------------------------------------------------------

void AftermathContext::register_shader(AftermathShaderType p_type, const uint8_t *p_bytes, uint32_t p_size) {
	if (!_initialized || !p_bytes || !p_size) {
		return;
	}

	switch (p_type) {
#ifdef AFTERMATH_ENABLED_D3D12
		case AFTERMATH_SHADER_DXIL: {
			D3D12_SHADER_BYTECODE shader_binary = {};
			shader_binary.pShaderBytecode = p_bytes;
			shader_binary.BytecodeLength = p_size;
			GFSDK_Aftermath_ShaderBinaryHash hash = {};
			GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_GetShaderHash, fn_GetShaderHash,
					GFSDK_Aftermath_Version_API, &shader_binary, &hash);
			if (res != GFSDK_Aftermath_Result_Success) {
				return;
			}
			uint64_t key = hash.hash;
			{
				MutexLock lock(shader_map_mutex);
				if (shader_map.has(key)) {
					return;
				}
				shader_map[key].resize(p_size);
				memcpy(shader_map[key].ptrw(), p_bytes, p_size);
			}
			char path[512] = {};
			snprintf(path, sizeof(path), "%s\\%016llx.cso", _dump_dir, (unsigned long long)key);
			_write_file(path, p_bytes, p_size);
			print_verbose(String("Aftermath: dumped DXIL shader hash=") + String::num_uint64(key, 16, true) + " -> " + path);
		} break;
#endif
#ifdef AFTERMATH_ENABLED_VULKAN
		case AFTERMATH_SHADER_SPIRV: {
			GFSDK_Aftermath_SpirvCode spirv;
			spirv.pData = p_bytes;
			spirv.size = p_size;
			GFSDK_Aftermath_ShaderBinaryHash hash = {};
			GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_GetShaderHashSpirv, fn_GetShaderHashSpirv,
					GFSDK_Aftermath_Version_API, &spirv, &hash);
			if (res != GFSDK_Aftermath_Result_Success) {
				return;
			}
			uint64_t key = hash.hash;
			{
				MutexLock lock(shader_map_mutex);
				if (shader_map.has(key)) {
					return;
				}
				shader_map[key].resize(p_size);
				memcpy(shader_map[key].ptrw(), p_bytes, p_size);
			}
			char path[512] = {};
			snprintf(path, sizeof(path), "%s\\%016llx.spv", _dump_dir, (unsigned long long)key);
			_write_file(path, p_bytes, p_size);
			print_verbose(String("Aftermath: dumped SPIR-V shader hash=") + String::num_uint64(key, 16, true) + " -> " + path);
		} break;
#endif
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Backend initialization
// ---------------------------------------------------------------------------

void AftermathContext::initialize_d3d12(void *p_d3d12_device) {
#ifdef AFTERMATH_ENABLED_D3D12
	if (!_initialized || !p_d3d12_device) {
		return;
	}
	ID3D12Device *device = static_cast<ID3D12Device *>(p_d3d12_device);
	uint32_t feature_flags = 0;
	if (_aftermath_env_flag("GODOT_AFTERMATH_MARKERS", true)) {
		feature_flags |= GFSDK_Aftermath_FeatureFlags_EnableMarkers;
	}
	if (_aftermath_env_flag("GODOT_AFTERMATH_RESOURCE_TRACKING", true)) {
		feature_flags |= GFSDK_Aftermath_FeatureFlags_EnableResourceTracking;
	}
	if (_aftermath_env_flag("GODOT_AFTERMATH_CALLSTACK_CAPTURE", true)) {
		feature_flags |= GFSDK_Aftermath_FeatureFlags_CallStackCapturing;
	}
	if (_aftermath_env_flag("GODOT_AFTERMATH_SHADER_DEBUG_INFO", true)) {
		feature_flags |= GFSDK_Aftermath_FeatureFlags_GenerateShaderDebugInfo;
	}
	if (_aftermath_env_flag("GODOT_AFTERMATH_SHADER_ERROR_REPORTING", false)) {
		feature_flags |= GFSDK_Aftermath_FeatureFlags_EnableShaderErrorReporting;
	}
	GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_DX12_Initialize, fn_DX12_Initialize,
			GFSDK_Aftermath_Version_API,
			feature_flags,
			device);
	if (res != GFSDK_Aftermath_Result_Success) {
		WARN_PRINT("Aftermath: DX12_Initialize failed. Shader-level crash data may be missing.");
	} else {
		WARN_PRINT("Aftermath: D3D12 active. Note: spirv2dxil strips DXIL debug info; only instruction-level crash dumps are available.");
	}
#endif
}

// ---------------------------------------------------------------------------
// Vulkan extension helpers
// ---------------------------------------------------------------------------

const char *const *AftermathContext::vk_required_extensions() const {
	return vk_ext_names;
}

void *AftermathContext::vk_device_diagnostics_config() {
#ifdef AFTERMATH_ENABLED_VULKAN
	static bool built = false;
	if (!built) {
		built = true;
		VkDeviceDiagnosticsConfigCreateInfoNV *info =
				reinterpret_cast<VkDeviceDiagnosticsConfigCreateInfoNV *>(vk_diag_config_storage);
		memset(info, 0, sizeof(*info));
		info->sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
		VkDeviceDiagnosticsConfigFlagsNV flags = 0;
		if (_aftermath_env_flag("GODOT_AFTERMATH_SHADER_DEBUG_INFO", true)) {
			flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV;
		}
		if (_aftermath_env_flag("GODOT_AFTERMATH_RESOURCE_TRACKING", true)) {
			flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV;
		}
		if (_aftermath_env_flag("GODOT_AFTERMATH_AUTOMATIC_CHECKPOINTS", true)) {
			flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV;
		}
		if (_aftermath_env_flag("GODOT_AFTERMATH_SHADER_ERROR_REPORTING", false)) {
			flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
		}
		info->flags = flags;
	}
	print_line("Aftermath: Vulkan GPU crash dump capture active (VK_NV_device_diagnostics_config enabled).");
	return vk_diag_config_storage;
#else
	return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Wait for dump flush
// ---------------------------------------------------------------------------

void AftermathContext::wait_for_dump() {
	if (!_initialized) {
		return;
	}
#ifdef _WIN32
	constexpr int MAX_WAIT_MS = 5000;
	constexpr int SLEEP_MS = 50;
	for (int elapsed = 0; elapsed < MAX_WAIT_MS; elapsed += SLEEP_MS) {
		GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
		AM_CALL(PFN_GFSDK_Aftermath_GetCrashDumpStatus, fn_GetCrashDumpStatus, &status);
		if (status == GFSDK_Aftermath_CrashDump_Status_Finished) {
			print_line("Aftermath: crash dump complete.");
			return;
		}
		Sleep(SLEEP_MS);
	}
	WARN_PRINT("Aftermath: timed out waiting for crash dump.");
#endif
}

// ---------------------------------------------------------------------------
// File helper
// ---------------------------------------------------------------------------

void AftermathContext::_write_file(const char *p_path, const void *p_data, uint32_t p_size) {
#ifdef _WIN32
	HANDLE f = CreateFileA(p_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(f, p_data, p_size, &written, nullptr);
	CloseHandle(f);
#endif
}

// ---------------------------------------------------------------------------
// Crash-dump callback: write .nv-gpudmp and JSON sidecar
// ---------------------------------------------------------------------------

void AftermathContext::_cb_gpu_crash_dump(const void *p_data, const uint32_t p_size, void *p_user) {
	AftermathContext *self = static_cast<AftermathContext *>(p_user);

	char dump_path[512] = {};
#ifdef _WIN32
	SYSTEMTIME st = {};
	GetLocalTime(&st);
	snprintf(dump_path, sizeof(dump_path), "%s\\%04d%02d%02d_%02d%02d%02d.nv-gpudmp",
			self->_dump_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#endif

	self->_write_file(dump_path, p_data, p_size);
	print_line(String("Aftermath: GPU crash dump written to: ") + dump_path);

	// Decode and write JSON sidecar.
	GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
	GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_GpuCrashDump_CreateDecoder,
			self->fn_GpuCrashDump_CreateDecoder,
			GFSDK_Aftermath_Version_API, p_data, p_size, &decoder);
	if (res != GFSDK_Aftermath_Result_Success) {
		return;
	}

	uint32_t json_size = 0;
	res = AM_CALL(PFN_GFSDK_Aftermath_GpuCrashDump_GenerateJSON,
			self->fn_GpuCrashDump_GenerateJSON,
			decoder,
			GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
			GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
			reinterpret_cast<PFN_GFSDK_Aftermath_ShaderDebugInfoLookupCb>(&AftermathContext::_cb_shader_debug_info_lookup),
			reinterpret_cast<PFN_GFSDK_Aftermath_ShaderLookupCb>(&AftermathContext::_cb_shader_lookup),
			reinterpret_cast<PFN_GFSDK_Aftermath_ShaderSourceDebugInfoLookupCb>(&AftermathContext::_cb_shader_source_debug_info_lookup),
			self,
			&json_size);

	if (res == GFSDK_Aftermath_Result_Success && json_size > 0) {
		Vector<char> json_buf;
		json_buf.resize(json_size);
		res = AM_CALL(PFN_GFSDK_Aftermath_GpuCrashDump_GetJSON,
				self->fn_GpuCrashDump_GetJSON,
				decoder, json_size, json_buf.ptrw());
		if (res == GFSDK_Aftermath_Result_Success) {
			char json_path[512] = {};
			snprintf(json_path, sizeof(json_path), "%s.json", dump_path);
			self->_write_file(json_path, json_buf.ptr(), json_size);
		}
	}

	AM_CALL(PFN_GFSDK_Aftermath_GpuCrashDump_DestroyDecoder,
			self->fn_GpuCrashDump_DestroyDecoder, decoder);
}

// ---------------------------------------------------------------------------
// Shader debug-info callback: write .nvdbg blobs
// ---------------------------------------------------------------------------

void AftermathContext::_cb_shader_debug_info(const void *p_data, const uint32_t p_size, void *p_user) {
	AftermathContext *self = static_cast<AftermathContext *>(p_user);

	GFSDK_Aftermath_ShaderDebugInfoIdentifier id = {};
	GFSDK_Aftermath_Result res = AM_CALL(PFN_GFSDK_Aftermath_GetShaderDebugInfoIdentifier,
			self->fn_GetShaderDebugInfoIdentifier,
			GFSDK_Aftermath_Version_API,
			p_data,
			p_size,
			&id);
	if (res != GFSDK_Aftermath_Result_Success) {
		return;
	}

	// Keep the blob in memory so _cb_shader_debug_info_lookup can hand it back
	// to the decoder at crash time; without this, the JSON has no PC->source map.
	{
		MutexLock lock(self->debug_info_mutex);
		ShaderDebugInfoBlob blob;
		blob.id0 = id.id[0];
		blob.id1 = id.id[1];
		blob.data.resize(p_size);
		memcpy(blob.data.ptrw(), p_data, p_size);
		self->debug_info_blobs.push_back(blob);
	}

	char path[512] = {};
	snprintf(path, sizeof(path), "%s\\shader-%016llx-%016llx.nvdbg",
			self->_dump_dir,
			(unsigned long long)id.id[0],
			(unsigned long long)id.id[1]);
	self->_write_file(path, p_data, p_size);
	print_line(String("Aftermath: wrote shader debug info -> ") + path);
}

// ---------------------------------------------------------------------------
// Description callback
// ---------------------------------------------------------------------------

void AftermathContext::_cb_description(void *p_add_value_fn, void *p_user) {
	auto add_value = reinterpret_cast<PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription>(p_add_value_fn);
	add_value(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "Godot Engine");
}

// ---------------------------------------------------------------------------
// Resolve marker callback (no custom markers)
// ---------------------------------------------------------------------------

void AftermathContext::_cb_resolve_marker(const void *, uint32_t, void *, void **pp_resolved, uint32_t *p_resolved_size) {
	*pp_resolved = nullptr;
	*p_resolved_size = 0;
}

// ---------------------------------------------------------------------------
// Shader lookup callbacks
// ---------------------------------------------------------------------------

void AftermathContext::_cb_shader_lookup(const void *p_hash, void *p_set_shader_fn, void *p_user) {
	AftermathContext *self = static_cast<AftermathContext *>(p_user);
	const GFSDK_Aftermath_ShaderBinaryHash *hash = static_cast<const GFSDK_Aftermath_ShaderBinaryHash *>(p_hash);
	auto set_binary = reinterpret_cast<PFN_GFSDK_Aftermath_SetData>(p_set_shader_fn);

	MutexLock lock(self->shader_map_mutex);
	uint64_t key = hash->hash;
	if (self->shader_map.has(key)) {
		const Vector<uint8_t> &bytes = self->shader_map[key];
		set_binary(bytes.ptr(), bytes.size());
		print_line(String("Aftermath: shader binary lookup HIT hash=") + String::num_uint64(key, 16, true) + " (" + itos(bytes.size()) + " bytes)");
	} else {
		print_line(String("Aftermath: shader binary lookup MISS hash=") + String::num_uint64(key, 16, true) + " (have " + itos(self->shader_map.size()) + " registered)");
	}
}

void AftermathContext::_cb_shader_debug_info_lookup(const void *p_id, void *p_set_debug_info_fn, void *p_user) {
	AftermathContext *self = static_cast<AftermathContext *>(p_user);
	const GFSDK_Aftermath_ShaderDebugInfoIdentifier *id =
			static_cast<const GFSDK_Aftermath_ShaderDebugInfoIdentifier *>(p_id);
	auto set_debug_info = reinterpret_cast<PFN_GFSDK_Aftermath_SetData>(p_set_debug_info_fn);

	char id_str[40] = {};
	snprintf(id_str, sizeof(id_str), "%016llx-%016llx",
			(unsigned long long)id->id[0], (unsigned long long)id->id[1]);

	MutexLock lock(self->debug_info_mutex);
	for (const ShaderDebugInfoBlob &blob : self->debug_info_blobs) {
		if (blob.id0 == id->id[0] && blob.id1 == id->id[1]) {
			set_debug_info(blob.data.ptr(), blob.data.size());
			print_line(String("Aftermath: debug info lookup HIT id=") + id_str + " (" + itos(blob.data.size()) + " bytes)");
			return;
		}
	}
	print_line(String("Aftermath: debug info lookup MISS id=") + id_str + " (have " + itos(self->debug_info_blobs.size()) + " registered)");
}

void AftermathContext::_cb_shader_source_debug_info_lookup(const void *, void *, void *) {
	// Source-debug-info is keyed by a separate ShaderDebugName (used with
	// externally stripped source, e.g. DXIL PDBs). We embed debug info directly
	// in the SPIR-V, so there is nothing to serve on this path.
}

#undef AM_CALL
#endif // AFTERMATH_ENABLED
