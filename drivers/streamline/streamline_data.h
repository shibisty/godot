/**************************************************************************/
/*  streamline_data.h                                                     */
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

enum StreamlineMarkerType {
	STREAMLINE_MARKER_INITIALIZE_VULKAN,
	STREAMLINE_MARKER_INITIALIZE_D3D12,
	STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP,
	STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY,
	STREAMLINE_MARKER_AFTER_DEVICE_CREATION,
	STREAMLINE_MARKER_MODIFY_SWAPCHAIN,
	STREAMLINE_MARKER_BEGIN_SIMULATION,
	STREAMLINE_MARKER_END_SIMULATION,
	STREAMLINE_MARKER_BEGIN_RENDER,
	STREAMLINE_MARKER_END_RENDER,
	STREAMLINE_MARKER_BEGIN_PRESENT,
	STREAMLINE_MARKER_END_PRESENT,
	STREAMLINE_MARKER_PC_PING
};

enum StreamlineCapabilityType {
	STREAMLINE_CAPABILITY_DLSS,
	STREAMLINE_CAPABILITY_DLSS_G,
	STREAMLINE_CAPABILITY_DLSS_RR,
	STREAMLINE_CAPABILITY_REFLEX,
	STREAMLINE_CAPABILITY_PCL,
	STREAMLINE_CAPABILITY_NIS
};

enum StreamlineInternalParameterType {
	STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12GetInterface,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12CreateDevice,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_DXGIGetDebugInterface1,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory1,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2,
	STREAMLINE_INTERNAL_PARAMETER_FUNC_vkGetInstanceProcAddr, // Mariner: interposer entry point for volk (Vulkan).
};

struct StreamlineCapabilities {
	bool dlss_available = false;
	bool dlss_g_available = false;
	bool dlss_rr_available = false;
	bool reflex_available = false;
	bool pcl_available = false;
	bool nis_available = false;
};

enum StreamlineParameterType {
	STREAMLINE_PARAM_REFLEX_MODE, // [double] 0.0 = off, 1.0 = on, 2.0 = on with boost
	STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, // [double] frame limit in microseconds
	STREAMLINE_PARAM_DLSS_PRESET, // [string] DLSS preset. Defaults to "?" (auto). Can be overridden to "J" or "K" for transformer models.
	STREAMLINE_PARAM_DLSS_RR_PRESET, // [string] DLSS RR preset. Defaults to "?" (auto). Can be overridden to "D" or "E" for transformer models.
	STREAMLINE_PARAM_DLSS_G_FRAMES, // [int] Mariner: frames DLSS-G generates per rendered frame (1 = 2x, 2 = 3x, 3 = 4x multi frame generation).
	STREAMLINE_PARAM_COUNT,
};

// Reflex modes as exposed to scripts and the rendering/streamline/reflex_mode project setting
// (same values as sl::ReflexMode: off, low latency, low latency with boost).
enum StreamlineReflexMode {
	STREAMLINE_REFLEX_MODE_OFF = 0,
	STREAMLINE_REFLEX_MODE_LOW_LATENCY = 1,
	STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST = 2,
};

// Upper bound of DLSSGOptions::numFramesToGenerate in the Streamline SDK (4x frame multiplier, RTX 50 series).
static constexpr int STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES = 3;

// Mariner E3: platform independent rules shared by the Streamline integration and its tests.

// STREAMLINE_PARAM_REFLEX_MODE value (a double, like NVIDIA's fork) to a Reflex mode.
inline StreamlineReflexMode streamline_reflex_mode_from_value(double p_value) {
	if (p_value > 1.0) {
		return STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST;
	}
	if (p_value > 0.0) {
		return STREAMLINE_REFLEX_MODE_LOW_LATENCY;
	}
	return STREAMLINE_REFLEX_MODE_OFF;
}

// DLSS-G presents magenta frames unless Reflex is running (DLSSGStatus::eFailReflexNotDetectedAtRuntime), so while
// frame generation is wanted an "off" request is promoted to low latency. Boost is kept.
inline StreamlineReflexMode streamline_effective_reflex_mode(StreamlineReflexMode p_requested, bool p_required_by_frame_generation) {
	if (p_required_by_frame_generation && p_requested == STREAMLINE_REFLEX_MODE_OFF) {
		return STREAMLINE_REFLEX_MODE_LOW_LATENCY;
	}
	return p_requested;
}

// Frames to generate per rendered frame: the request clamped to [1, what the GPU supports]. p_max_supported is
// DLSSGState::numFramesToGenerateMax (1 = 2x only, 3 = up to 4x on RTX 50); 0 means not known yet (treated as 1).
inline int streamline_clamp_generated_frames(int p_requested, int p_max_supported) {
	int max_frames = p_max_supported < 1 ? 1 : (p_max_supported > STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES ? STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES : p_max_supported);
	if (p_requested < 1) {
		return 1;
	}
	return p_requested > max_frames ? max_frames : p_requested;
}
