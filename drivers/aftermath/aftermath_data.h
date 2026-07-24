/**************************************************************************/
/*  aftermath_data.h                                                      */
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

// Opaque marker types passed across the driver boundary.
// No Aftermath SDK types leak beyond aftermath_context.h.

enum AftermathMarkerType {
	AFTERMATH_MARKER_INITIALIZE_D3D12,
	AFTERMATH_MARKER_INITIALIZE_VULKAN,
	AFTERMATH_MARKER_AFTER_DEVICE_CREATION,
	AFTERMATH_MARKER_BEFORE_DEVICE_DESTROY,
	AFTERMATH_MARKER_ON_DEVICE_LOST,
};

// Shader bytecode types we receive from the driver backends.
enum AftermathShaderType {
	AFTERMATH_SHADER_DXIL, // DXIL bytecode (D3D12 final stage blob)
	AFTERMATH_SHADER_SPIRV, // SPIR-V bytecode (Vulkan)
};

// Keys for get_internal_parameter() - used by Vulkan to query
// the VkDeviceDiagnosticsConfigCreateInfoNV pNext structure.
enum AftermathInternalParameterType {
	// Returns a const char* const* - null-terminated array of extension name strings.
	AFTERMATH_INTERNAL_PARAM_VK_REQUIRED_EXTENSIONS,
	// Returns a void* pointing to a static VkDeviceDiagnosticsConfigCreateInfoNV
	// suitable for chaining into VkDeviceCreateInfo.pNext.
	AFTERMATH_INTERNAL_PARAM_VK_DEVICE_DIAGNOSTICS_CONFIG,
};
