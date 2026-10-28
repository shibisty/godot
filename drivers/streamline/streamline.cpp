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
	ClassDB::bind_method(D_METHOD("get_parameter", "parameter_type"), &Streamline::get_parameter);
	ClassDB::bind_method(D_METHOD("get_capability", "capability_type"), &Streamline::get_capability);
	ClassDB::bind_method(D_METHOD("get_reflex_mode"), &Streamline::get_reflex_mode);
	ClassDB::bind_method(D_METHOD("is_frame_generation_active"), &Streamline::is_frame_generation_active);
	ClassDB::bind_method(D_METHOD("get_frame_generation_max_frames"), &Streamline::get_frame_generation_max_frames);
	ClassDB::bind_method(D_METHOD("get_frame_generation_status"), &Streamline::get_frame_generation_status);

	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_MODE);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_PRESET);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_RR_PRESET);
	BIND_ENUM_CONSTANT(STREAMLINE_PARAM_DLSS_G_FRAMES);

	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_G);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_DLSS_RR);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_NIS);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_REFLEX);
	BIND_ENUM_CONSTANT(STREAMLINE_CAPABILITY_PCL);

	BIND_ENUM_CONSTANT(STREAMLINE_REFLEX_MODE_OFF);
	BIND_ENUM_CONSTANT(STREAMLINE_REFLEX_MODE_LOW_LATENCY);
	BIND_ENUM_CONSTANT(STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);
}

void Streamline::register_singleton() {
	GDREGISTER_CLASS(Streamline);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Streamline", Streamline::get_singleton()));

	GLOBAL_DEF("rendering/streamline/streamline_log", false);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/streamline/dlss_preset", PROPERTY_HINT_ENUM, "?,F,G,H,I,J,K,L,M,N,O"), "?");
	// Mariner E3 (DLSS Frame Generation + Reflex, from NVIDIA's fork). Reflex, PCL and DLSS-G are only loaded for the
	// running game, never in the editor or project manager.
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_mode", PROPERTY_HINT_ENUM, "Off,Low Latency,Low Latency + Boost"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/reflex_frame_limit_us", PROPERTY_HINT_RANGE, "0,1000000,1,suffix:us"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/streamline/frame_generation_frames", PROPERTY_HINT_RANGE, "1,3,1"), 1);
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
	set_parameter(STREAMLINE_PARAM_REFLEX_MODE, (double)GLOBAL_GET("rendering/streamline/reflex_mode"));
	set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, (double)GLOBAL_GET("rendering/streamline/reflex_frame_limit_us"));
	set_parameter(STREAMLINE_PARAM_DLSS_PRESET, GLOBAL_GET("rendering/streamline/dlss_preset"));
	set_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES, GLOBAL_GET("rendering/streamline/frame_generation_frames"));
}

void Streamline::emit_marker(StreamlineMarkerType p_marker) {
#ifdef STREAMLINE_ENABLED
	StreamlineContext &sl_context = StreamlineContext::get();

	switch (p_marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_VULKAN:
		case StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12:
			sl_context.initialize(p_marker == StreamlineMarkerType::STREAMLINE_MARKER_INITIALIZE_D3D12);
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_AFTER_DEVICE_CREATION:
			if (!sl_context.initialized || sl_context.post_init_done) {
				return; // Not initialized, or already done for the main device.
			}
			sl_context.post_init_done = true;
			sl_context.load_functions_post_init();
			print_verbose(vformat("Streamline: NVIDIA DLSS is %s on this device.", sl_context.streamline_capabilities.dlss_available ? "available" : "not available"));
			if (sl_context.is_game) {
				print_verbose(vformat("Streamline: DLSS Frame Generation %s, Reflex %s, PCL %s.",
						sl_context.streamline_capabilities.dlss_g_available ? "available" : "not available",
						sl_context.streamline_capabilities.reflex_available ? "available" : "not available",
						sl_context.streamline_capabilities.pcl_available ? "available" : "not available"));
			}

			if (sl_context.streamline_capabilities.pcl_available) {
				sl::PCLOptions pcl_options = {};
				// Latency pings arrive as F13 key presses (handled by the Windows display server).
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

			// Load initial project settings (Reflex mode and frame limit are applied with the next frame).
			update_project_settings();
			return;
		case StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY:
			sl_context.dlssg_disable("the rendering device is destroyed");
			if (sl_context.initialized && sl_context.slShutdown) {
				sl_context.slShutdown();
				sl_context.slShutdown = nullptr;
			}
			sl_context.reset_after_shutdown();
			return;
		default:
			break;
	}

	if (!sl_context.initialized) {
		return;
	}

	if (!sl_context.is_game || !sl_context.streamline_capabilities.reflex_available) {
		// Without Reflex (editor, unsupported GPU) only the frame tokens are needed (DLSS).
		if (p_marker == StreamlineMarkerType::STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP) {
			sl_context.get_new_frame_token();
		}
		return;
	}

	sl::PCLMarker sl_marker = sl::PCLMarker::eMaximum;
	switch (p_marker) {
		case StreamlineMarkerType::STREAMLINE_MARKER_MODIFY_SWAPCHAIN:
			sl_context.dlssg_disable("the swap chain is resized or recreated");
			break;
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
			// DLSS-G requires slReflexSleep to be called every frame while it is active.
			if (sl_context.reflex_is_active() || sl_context.reflex_options.frameLimitUs > 0 || sl_context.dlssg_active()) {
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
			// Mariner: frame generation must not keep interpolating with stale tags when the viewport that enabled it
			// was not evaluated by DLSS this frame (no camera, upscaler switched, DLSS warming up).
			sl_context.dlssg_check_evaluated_before_present();
			sl_marker = sl::PCLMarker::ePresentStart;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_END_PRESENT:
			sl_marker = sl::PCLMarker::ePresentEnd;
			break;
		case StreamlineMarkerType::STREAMLINE_MARKER_PC_PING:
			sl_marker = sl::PCLMarker::ePCLatencyPing;
			break;
		default:
			break;
	}

	if (sl_context.last_token && sl_marker != sl::PCLMarker::eMaximum && sl_context.streamline_capabilities.pcl_available) {
		sl_context.pcl_marker(sl_context.last_token, sl_marker);
	}
#endif
}

void Streamline::set_parameter(StreamlineParameterType p_parameter_type, const Variant &p_value) {
	_THREAD_SAFE_METHOD_
	switch (p_parameter_type) {
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_MODE: {
			reflex_mode = streamline_reflex_mode_from_value((double)p_value);
#ifdef STREAMLINE_ENABLED
			StreamlineContext::get().reflex_request_mode((sl::ReflexMode)(int)reflex_mode);
#endif
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US: {
			double value = p_value;
			reflex_frame_limit_us = value > 0.0 ? (uint32_t)MIN(value, 10000000.0) : 0;
#ifdef STREAMLINE_ENABLED
			StreamlineContext &sl_context = StreamlineContext::get();
			if (sl_context.reflex_options.frameLimitUs != reflex_frame_limit_us) {
				sl_context.reflex_options.frameLimitUs = reflex_frame_limit_us;
				sl_context.reflex_options_dirty = true;
			}
#endif
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_PRESET: {
			if (p_value.is_string()) {
				String preset = p_value;
				if (preset.length() == 1) {
					dlss_preset = preset;
#ifdef STREAMLINE_ENABLED
					StreamlineContext::get().dlss_default_preset = (char)preset.get(0);
#endif
				}
			}
			break;
		}
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_G_FRAMES: {
			frame_generation_frames = CLAMP((int)p_value, 1, STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES);
#ifdef STREAMLINE_ENABLED
			StreamlineContext::get().dlssg_requested_frames = frame_generation_frames;
#endif
			break;
		}
		default:
			// The Ray Reconstruction preset is accepted for API compatibility with NVIDIA's fork, but Ray
			// Reconstruction is not part of this build.
			break;
	}
}

Variant Streamline::get_parameter(StreamlineParameterType p_parameter_type) const {
	switch (p_parameter_type) {
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_MODE:
			return (int)reflex_mode;
		case StreamlineParameterType::STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US:
			return (int64_t)reflex_frame_limit_us;
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_PRESET:
			return dlss_preset;
		case StreamlineParameterType::STREAMLINE_PARAM_DLSS_G_FRAMES:
			return frame_generation_frames;
		default:
			return Variant();
	}
}

bool Streamline::get_capability(StreamlineCapabilityType p_capability_type) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	const StreamlineContext &sl_context = StreamlineContext::get();
	switch (p_capability_type) {
		case STREAMLINE_CAPABILITY_DLSS:
			return sl_context.streamline_capabilities.dlss_available;
		case STREAMLINE_CAPABILITY_DLSS_G:
			// DLSS-G only works together with Reflex and its PCL markers (magenta frames otherwise).
			return sl_context.streamline_capabilities.dlss_g_available && sl_context.streamline_capabilities.reflex_available && sl_context.streamline_capabilities.pcl_available;
		case STREAMLINE_CAPABILITY_DLSS_RR:
			return sl_context.streamline_capabilities.dlss_rr_available;
		case STREAMLINE_CAPABILITY_NIS:
			return sl_context.streamline_capabilities.nis_available;
		case STREAMLINE_CAPABILITY_REFLEX:
			return sl_context.streamline_capabilities.reflex_available;
		case STREAMLINE_CAPABILITY_PCL:
			return sl_context.streamline_capabilities.pcl_available;
	}
#endif
	return false;
}

int Streamline::get_reflex_mode() const {
#ifdef STREAMLINE_ENABLED
	const StreamlineContext &sl_context = StreamlineContext::get();
	if (sl_context.is_game && sl_context.streamline_capabilities.reflex_available) {
		return (int)sl_context.reflex_options.mode;
	}
#endif
	return STREAMLINE_REFLEX_MODE_OFF;
}

bool Streamline::is_frame_generation_active() const {
#ifdef STREAMLINE_ENABLED
	return StreamlineContext::get().dlssg_active();
#else
	return false;
#endif
}

int Streamline::get_frame_generation_max_frames() const {
#ifdef STREAMLINE_ENABLED
	const StreamlineContext &sl_context = StreamlineContext::get();
	if (sl_context.is_game && sl_context.streamline_capabilities.dlss_g_available) {
		return sl_context.dlssg_max_frames;
	}
#endif
	return 0;
}

int Streamline::get_frame_generation_status() const {
#ifdef STREAMLINE_ENABLED
	return (int)StreamlineContext::get().dlssg_last_status;
#else
	return 0;
#endif
}

void Streamline::set_internal_parameter(const char *p_key, void *p_value) {
#ifdef STREAMLINE_ENABLED
	_THREAD_SAFE_METHOD_
	StreamlineContext &sl_context = StreamlineContext::get();
	if (!sl_context.initialized) {
		return;
	}
#if STREAMLINE_ENABLED_VULKAN
	if (strcmp(p_key, "vulkan_physical_device") == 0 && sl_context.is_initialized_for(false) && sl_context.registered_device == nullptr) {
		sl_context.streamline_capabilities = sl_context.enumerate_support_vulkan(p_value);
	}
	if (strcmp(p_key, "vulkan_device") == 0 && sl_context.is_initialized_for(false) && sl_context.registered_device == nullptr) {
		sl_context.registered_device = p_value;
	}
#endif
#if STREAMLINE_ENABLED_D3D12
	if (strcmp(p_key, "d3d12_adapter_luid") == 0 && sl_context.is_initialized_for(true) && sl_context.registered_device == nullptr) {
		sl_context.streamline_capabilities = sl_context.enumerate_support_d3d12(p_value);
	}
	if (strcmp(p_key, "d3d12_device") == 0 && sl_context.is_initialized_for(true) && sl_context.registered_device == nullptr) {
		sl_context.registered_device = p_value;
		sl_context.init_device_d3d12(p_value);
	}
#endif
	if (strcmp(p_key, "device_destroyed") == 0 && p_value != nullptr && p_value == sl_context.registered_device) {
		emit_marker(STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY);
	}
#endif
}

void *Streamline::get_internal_parameter(StreamlineInternalParameterType p_internal_parameter_type) {
#ifdef STREAMLINE_ENABLED
	StreamlineContext &sl_context = StreamlineContext::get();
#if STREAMLINE_ENABLED_D3D12
	// Proxies are only handed out until the main device is registered (see registered_device).
	if (sl_context.is_initialized_for(true) && sl_context.registered_device == nullptr) {
		switch (p_internal_parameter_type) {
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12GetInterface:
				return sl_context.func_D3D12GetInterface;
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12CreateDevice:
				return sl_context.func_D3D12CreateDevice;
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_DXGIGetDebugInterface1:
				return sl_context.func_DXGIGetDebugInterface1;
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory:
				return sl_context.func_CreateDXGIFactory;
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory1:
				return sl_context.func_CreateDXGIFactory1;
			case STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2:
				return sl_context.func_CreateDXGIFactory2;
			default:
				break;
		}
	}
#endif
#if STREAMLINE_ENABLED_VULKAN
	if (p_internal_parameter_type == STREAMLINE_INTERNAL_PARAMETER_FUNC_vkGetInstanceProcAddr && sl_context.is_initialized_for(false)) {
		return sl_context.func_vkGetInstanceProcAddr;
	}
#endif
#endif
	return nullptr;
}
