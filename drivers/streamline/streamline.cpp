/**************************************************************************/
/*  streamline.cpp                                                        */
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

#include "streamline.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/object.h"
#ifdef STREAMLINE_ENABLED
#include "drivers/streamline/streamline_context.h"
#endif

Streamline *Streamline::singleton = nullptr;

void Streamline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_parameter", "parameter_type", "value"), &Streamline::set_parameter);
	ClassDB::bind_method(D_METHOD("get_capability", "capability_type"), &Streamline::get_capability);

	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_MODE);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_PRESET);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_RR_PRESET);

	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_G);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_RR);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_NIS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_REFLEX);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_PCL);
}

void Streamline::register_singleton() {
	GDREGISTER_CLASS(Streamline);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Streamline", Streamline::get_singleton()));

	GLOBAL_DEF("rendering/streamline/streamline_log", false);
	GLOBAL_DEF("rendering/streamline/streamline_imgui", false);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_mode", PROPERTY_HINT_RANGE, "0,2,1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_frame_limit_us", PROPERTY_HINT_RANGE, "0,1000000,1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/streamline/dlss_preset", PROPERTY_HINT_ENUM, "?,F,G,H,I,J,K,L,M,N,O"), "?");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/streamline/dlss_ray_reconstruction_preset", PROPERTY_HINT_ENUM, "?,D,E,F,G,H,I,J,K,L,M,N,O"), "?");
}

Streamline *Streamline::get_singleton() {
	return singleton;
}

Streamline::Streamline() {
	singleton = this;
}

Streamline::~Streamline() {
	singleton = nullptr;
}

void Streamline::update_project_settings() {
#ifdef STREAMLINE_ENABLED
	set_parameter(STREAMLINE_PARAM_REFLEX_MODE, (double)GLOBAL_GET("rendering/streamline/reflex_mode"));
	set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, (double)GLOBAL_GET("rendering/streamline/reflex_frame_limit_us"));
	set_parameter(STREAMLINE_PARAM_DLSS_PRESET, GLOBAL_GET("rendering/streamline/dlss_preset"));
	set_parameter(STREAMLINE_PARAM_DLSS_RR_PRESET, GLOBAL_GET("rendering/streamline/dlss_ray_reconstruction_preset"));
#endif
}

void Streamline::emit_marker(StreamlineMarkerType marker) {
#ifdef STREAMLINE_ENABLED
	StreamlineContext &sl_context = StreamlineContext::get();

	switch (marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_VULKAN:
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12:
			sl_context.initialize(marker == StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12);
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_AFTER_DEVICE_CREATION:
			sl_context.load_functions_post_init();

			if (sl_context.streamline_capabilities.pcl_available) {
				sl::PCLOptions pcl_options = {};
				pcl_options.virtualKey = sl::PCLHotKey::eVK_F13;
				pcl_options.idThread = 0;
				sl_context.pcl_set_options(pcl_options);
			}

			if (sl_context.streamline_capabilities.reflex_available) {
				sl::ReflexOptions reflex_options = {};
				reflex_options.frameLimitUs = 0;
				reflex_options.virtualKey = (uint16_t)sl::PCLHotKey::eVK_F13;
				reflex_options.mode = sl::ReflexMode::eOff;
				reflex_options.useMarkersToOptimize = false;
				reflex_options.idThread = 0;
				sl_context.reflex_set_options(reflex_options);
			}

			// load initial project settings
			update_project_settings();
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY:
			if (sl_context.slShutdown) {
				sl_context.slShutdown();
				sl_context.slShutdown = nullptr;
			}
			return;
		default:
			break;
	}

	if (!sl_context.is_game || !sl_context.streamline_capabilities.reflex_available) {
		// Make sure we still get frame tokens, needed for DLSS.
		if (marker == StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP) {
			sl_context.get_new_frame_token();
		}
		return;
	}

	sl::PCLMarker sl_marker = sl::PCLMarker::eMaximum;
	switch (marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_MODIFY_SWAPCHAIN:
			sl_context.dlssg_disable();
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP:
			if (sl_context.dlssg_delay > 0) {
				--sl_context.dlssg_delay;
			}
			if (sl_context.pcl_options_dirty) {
				sl_context.pcl_set_options(sl_context.pcl_options);
			}
			if (sl_context.reflex_options_dirty) {
				sl_context.reflex_set_options(sl_context.reflex_options);
			}

			sl_context.get_new_frame_token();
			if (sl_context.reflex_options.mode != sl::ReflexMode::eOff || sl_context.reflex_options.frameLimitUs > 0) {
				sl_context.reflex_sleep(sl_context.last_token);
			}
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_RENDER:
			sl_marker = sl::PCLMarker::eRenderSubmitStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_RENDER:
			sl_marker = sl::PCLMarker::eRenderSubmitEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_SIMULATION:
			sl_marker = sl::PCLMarker::eSimulationStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_SIMULATION:
			sl_marker = sl::PCLMarker::eSimulationEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEGIN_PRESENT:
			sl_marker = sl::PCLMarker::ePresentStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_PRESENT:
			sl_marker = sl::PCLMarker::ePresentEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_PC_PING:
			sl_marker = sl::PCLMarker::ePCLatencyPing;
			break;
		default:
			return;
	}

	if (sl_context.last_token && sl_marker != sl::PCLMarker::eMaximum) {
		sl_context.pcl_marker(sl_context.last_token, sl_marker);
	}
#endif
}
void Streamline::set_parameter(StreamlineParameterType p_parameter_type, const Variant &p_value) {
	_THREAD_SAFE_METHOD_
	switch (p_parameter_type) {
#ifdef STREAMLINE_ENABLED
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_MODE: {
			double val = p_value;
			sl::ReflexMode newMode;
			if (val > 1.0) {
				newMode = sl::ReflexMode::eLowLatencyWithBoost;
			} else if (val > 0.0) {
				newMode = sl::ReflexMode::eLowLatency;
			} else {
				newMode = sl::ReflexMode::eOff;
			}

			if (StreamlineContext::get().reflex_options.mode != newMode) {
				StreamlineContext::get().reflex_options_dirty = true;
			}
			StreamlineContext::get().reflex_options.mode = newMode;
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US: {
			double val = p_value;
			bool updated = StreamlineContext::get().reflex_options.frameLimitUs != (unsigned int)val;
			StreamlineContext::get().reflex_options.frameLimitUs = (unsigned int)val;
			if (updated) {
				StreamlineContext::get().reflex_options_dirty = true;
			}
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_PRESET: {
			if (p_value.is_string()) {
				String preset = p_value;
				if (preset.length() == 1) {
					StreamlineContext::get().dlss_default_preset = (char)preset.get(0);
				}
			}
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_RR_PRESET: {
			if (p_value.is_string()) {
				String preset = p_value;
				if (preset.length() == 1) {
					StreamlineContext::get().dlss_rr_default_preset = (char)preset.get(0);
				}
			}
			break;
		}
#endif
		default:
			break;
	}
}

bool Streamline::get_capability(StreamlineCapabilityType p_capability_type) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	switch (p_capability_type) {
		case STREAMLINE_CAPABILITY_DLSS:
			return StreamlineContext::get().streamline_capabilities.dlss_available;
		case STREAMLINE_CAPABILITY_DLSS_G:
			return StreamlineContext::get().streamline_capabilities.dlss_g_available;
		case STREAMLINE_CAPABILITY_DLSS_RR:
			return StreamlineContext::get().streamline_capabilities.dlss_rr_available;
		case STREAMLINE_CAPABILITY_NIS:
			return StreamlineContext::get().streamline_capabilities.nis_available;
		case STREAMLINE_CAPABILITY_REFLEX:
			return StreamlineContext::get().streamline_capabilities.reflex_available;
		case STREAMLINE_CAPABILITY_PCL:
			return StreamlineContext::get().streamline_capabilities.pcl_available;
	}
#endif
	return false;
}
void Streamline::set_internal_parameter(const char *key, void *value) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
#if STREAMLINE_ENABLED_VULKAN
	if (strcmp(key, "vulkan_physical_device") == 0) {
		void *phys_device = (void *)value;
		StreamlineContext::get().streamline_capabilities = StreamlineContext::get().enumerate_support_vulkan(phys_device);
	}
#endif
#if STREAMLINE_ENABLED_D3D12
	if (strcmp(key, "d3d12_adapter_luid") == 0) {
		void *adapter_luid = (void *)value;
		StreamlineContext::get().streamline_capabilities = StreamlineContext::get().enumerate_support_d3d12(adapter_luid);
	}
	if (strcmp(key, "d3d12_device") == 0) {
		void *d3d_device = (void *)value;
		StreamlineContext::get().init_device_d3d12(d3d_device);
	}
#endif
#endif
}

void *Streamline::get_internal_parameter(StreamlineInternalParameterType p_internal_parameter_type) {
#ifdef STREAMLINE_ENABLED
#if STREAMLINE_ENABLED_D3D12
	switch (p_internal_parameter_type) {
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12GetInterface:
			return StreamlineContext::get().func_D3D12GetInterface;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12CreateDevice:
			return StreamlineContext::get().func_D3D12CreateDevice;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_DXGIGetDebugInterface1:
			return StreamlineContext::get().func_DXGIGetDebugInterface1;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory:
			return StreamlineContext::get().func_CreateDXGIFactory;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory1:
			return StreamlineContext::get().func_CreateDXGIFactory1;
		case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2:
			return StreamlineContext::get().func_CreateDXGIFactory2;
	}
#endif
#endif
	return nullptr;
}
