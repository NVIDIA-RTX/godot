/**************************************************************************/
/*  aftermath.cpp                                                         */
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

#include "aftermath.h"

#ifdef AFTERMATH_ENABLED
#include "core/config/engine.h"
#include "drivers/aftermath/aftermath_context.h"
#endif

Aftermath *Aftermath::singleton = nullptr;

void Aftermath::_bind_methods() {
	// No GDScript bindings - Aftermath is an internal driver module.
}

Aftermath::Aftermath() {
	singleton = this;
}

Aftermath::~Aftermath() {
	singleton = nullptr;
}

bool Aftermath::is_active() {
#ifdef AFTERMATH_ENABLED
	return Engine::get_singleton()->is_generate_spirv_debug_info_enabled();
#else
	return false;
#endif
}

void Aftermath::emit_marker(AftermathMarkerType p_marker) {
#ifdef AFTERMATH_ENABLED
	AftermathContext &ctx = AftermathContext::get();
	switch (p_marker) {
		case AFTERMATH_MARKER_INITIALIZE_D3D12:
		case AFTERMATH_MARKER_INITIALIZE_VULKAN:
			if (!is_active()) {
				return;
			}
			ctx.load_functions();
			return;

		case AFTERMATH_MARKER_AFTER_DEVICE_CREATION:
			// D3D12: initialize_d3d12() is called separately via emit_marker(INITIALIZE_D3D12).
			// Vulkan: initialized implicitly via VkDeviceDiagnosticsConfigCreateInfoNV.
			return;

		case AFTERMATH_MARKER_ON_DEVICE_LOST:
			ctx.wait_for_dump();
			return;

		case AFTERMATH_MARKER_BEFORE_DEVICE_DESTROY:
			// Nothing to do; crash-dump callbacks are process-lifetime.
			return;
	}
#endif
}

void Aftermath::register_shader(AftermathShaderType p_type, const uint8_t *p_bytes, uint32_t p_size) {
#ifdef AFTERMATH_ENABLED
	if (!is_active()) {
		return;
	}
	AftermathContext::get().register_shader(p_type, p_bytes, p_size);
#endif
}

void *Aftermath::get_internal_parameter(AftermathInternalParameterType p_type) {
#ifdef AFTERMATH_ENABLED
	AftermathContext &ctx = AftermathContext::get();
	switch (p_type) {
		case AFTERMATH_INTERNAL_PARAM_VK_REQUIRED_EXTENSIONS:
			return (void *)ctx.vk_required_extensions();
		case AFTERMATH_INTERNAL_PARAM_VK_DEVICE_DIAGNOSTICS_CONFIG:
			return ctx.vk_device_diagnostics_config();
	}
#endif
	return nullptr;
}
