/**************************************************************************/
/*  render_raytracing_validation.h                                        */
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

// CPU-side checks of the device addresses the path tracer hands to hit shaders. Neither RD nor the draw graph
// sees those reads, so stale or freed buffers would otherwise show up as GPU faults.
// Enabled by rendering/pathtracing/debug/strict_validation. Every address written into RT_GeometryData must come
// from _rt_buffer_address(), and every custom material texture slot must go through check_texture_slot().

#ifdef DEV_ENABLED
#define RT_VALIDATION_ENABLED
#endif

#ifdef RT_VALIDATION_ENABLED

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/span.h"
#include "servers/rendering/rendering_device.h"

#define RT_VALIDATE(m_code)                                                \
	do {                                                                   \
		if (::RendererSceneRenderImplementation::rt_validation().strict) { \
			m_code;                                                        \
		}                                                                  \
	} while (false)

namespace RendererSceneRenderImplementation {

struct RT_GeometryData;
struct RT_MaterialData;
struct RTGeometrySources;

struct RTValidationView {
	Span<RT_GeometryData> geometries;
	Span<RTGeometrySources> geometry_sources; // Parallel to geometries.
	Span<RT_MaterialData> materials;
	Span<RID> material_sources; // Parallel to materials.
	uint32_t bindless_texture_count = 0;
	Span<RD::BufferAddressRange> buffers;
};

class RTValidation {
public:
	bool strict = false;

	void begin_view();
	void check_texture_slot(const String &p_name, uint32_t p_offset, uint32_t p_block_size, uint32_t p_index, uint32_t p_bindless_texture_count);

	void check_view(const RTValidationView &p_view, LocalVector<String> &r_issues) const;
	void check_tlas(Span<uint32_t> p_custom_indices, uint32_t p_geometry_count, const char *p_tlas_name, LocalVector<String> &r_issues) const;
	void check_tlas_blases(Span<RID> p_stale_blases, const char *p_tlas_name, LocalVector<String> &r_issues) const;

	// Returns true when there are no issues and the view may be traced.
	bool report(const LocalVector<String> &p_issues);
	bool is_new_issue(const String &p_issue);

private:
	LocalVector<String> pending;
	HashSet<String> reported;
};

RTValidation &rt_validation();

} // namespace RendererSceneRenderImplementation

#else

#define RT_VALIDATE(m_code) ((void)0)

#endif // RT_VALIDATION_ENABLED
