/**************************************************************************/
/*  aftermath.h                                                           */
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

#include "core/object/object.h"
#include "drivers/aftermath/aftermath_data.h"

/**
 * Aftermath - thin singleton wrapper around AftermathContext.
 *
 * Activation: both build flag AFTERMATH_ENABLED and
 * Engine::is_generate_spirv_debug_info_enabled() must be true at runtime.
 * All public methods compile to inline no-ops when AFTERMATH_ENABLED is off.
 *
 * No project settings, no GDScript bindings, no editor UI.
 */
class Aftermath : public Object {
	GDCLASS(Aftermath, Object);

protected:
	static Aftermath *singleton;
	static void _bind_methods();

public:
	static Aftermath *get_singleton() { return singleton; }

	/**
	 * Returns true when Aftermath is both compiled in and the runtime
	 * debug-info flag is set.  All internal calls gate on this.
	 */
	static bool is_active();

	/**
	 * Lifecycle / backend-init markers.  Driver backends call these at
	 * the appropriate points.
	 */
	void emit_marker(AftermathMarkerType p_marker);

	/**
	 * Register a shader binary for Aftermath hash-lookup.
	 * Only effective when is_active() == true.
	 */
	void register_shader(AftermathShaderType p_type, const uint8_t *p_bytes, uint32_t p_size);

	/**
	 * Vulkan-specific: return extension names / pNext structure pointers
	 * needed during VkDevice creation.
	 */
	void *get_internal_parameter(AftermathInternalParameterType p_type);

	Aftermath();
	virtual ~Aftermath();
};
