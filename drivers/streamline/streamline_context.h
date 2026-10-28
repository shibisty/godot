/**************************************************************************/
/*  streamline_context.h                                                  */
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

#ifdef STREAMLINE_ENABLED
#include "drivers/streamline/streamline_data.h"
#include "drivers/streamline/streamline_headers.h"

class StreamlineContext {
public:
	// Interposer
	PFun_slInit *slInit = nullptr;
	PFun_slShutdown *slShutdown = nullptr;
	PFun_slIsFeatureSupported *slIsFeatureSupported = nullptr;
	PFun_slGetFeatureFunction *slGetFeatureFunction = nullptr;

	PFun_slGetNewFrameToken *slGetNewFrameToken = nullptr;
	PFun_slAllocateResources *slAllocateResources = nullptr;
	PFun_slFreeResources *slFreeResources = nullptr;
	PFun_slEvaluateFeature *slEvaluateFeature = nullptr;
	PFun_slSetTag *slSetTag = nullptr;
	PFun_slSetConstants *slSetConstants = nullptr;
	PFun_slSetFeatureLoaded *slSetFeatureLoaded = nullptr;
	PFun_slSetD3DDevice *slSetD3DDevice = nullptr;

	// Reflex (E3, game only).
	sl::ReflexOptions reflex_options;
	bool reflex_options_dirty = true;
	// Mode requested through project settings / the Streamline singleton. The mode actually applied to Reflex can be
	// higher than this, since DLSS-G requires Reflex to be active.
	sl::ReflexMode reflex_requested_mode = sl::ReflexMode::eOff;
	bool reflex_required_by_dlssg = false;
	PFun_slReflexGetState *slReflexGetState = nullptr;
	PFun_slReflexSetOptions *slReflexSetOptions = nullptr;
	PFun_slReflexSleep *slReflexSleep = nullptr;

	// PCL latency markers (E3, game only).
	sl::PCLOptions pcl_options;
	bool pcl_options_dirty = true;
	PFun_slPCLSetMarker *slPCLSetMarker = nullptr;
	PFun_slPCLSetOptions *slPCLSetOptions = nullptr;

	// DLSS Frame Generation (E3, game only).
	PFun_slDLSSGGetState *slDLSSGGetState = nullptr;
	PFun_slDLSSGSetOptions *slDLSSGSetOptions = nullptr;

	// DLSS Super Resolution
	PFun_slDLSSGetOptimalSettings *slDLSSGetOptimalSettings = nullptr;
	PFun_slDLSSGetState *slDLSSGetState = nullptr;
	PFun_slDLSSSetOptions *slDLSSSetOptions = nullptr;
	char dlss_default_preset = '?';

	// D3D12 proxies exported by sl.interposer.dll (only handed out after a successful slInit for D3D12).
	void *func_D3D12GetInterface = nullptr;
	void *func_D3D12CreateDevice = nullptr;
	void *func_DXGIGetDebugInterface1 = nullptr;
	void *func_CreateDXGIFactory = nullptr;
	void *func_CreateDXGIFactory1 = nullptr;
	void *func_CreateDXGIFactory2 = nullptr;

	// Vulkan entry point exported by sl.interposer.dll (only handed out after a successful slInit for Vulkan).
	void *func_vkGetInstanceProcAddr = nullptr;

	void load_functions(bool d3d12);
	void load_functions_post_init();
	static const char *result_to_string(sl::Result result);

	void reflex_set_options(const sl::ReflexOptions &p_options);
	void reflex_request_mode(sl::ReflexMode p_mode);
	// DLSS-G renders magenta frames when Reflex is not running (DLSSGStatus::eFailReflexNotDetectedAtRuntime),
	// so Reflex is forced on for as long as frame generation is wanted.
	void reflex_set_required_by_dlssg(bool p_required);
	bool reflex_is_active() const { return reflex_options.mode != sl::ReflexMode::eOff; }
	// True once the active Reflex options have actually been submitted to Streamline, which happens on the main
	// thread. DLSS-G must not be enabled before that.
	bool reflex_is_running() const { return reflex_is_active() && !reflex_options_dirty; }
	void reflex_sleep(sl::FrameToken *p_frame_token);
	void pcl_set_options(const sl::PCLOptions &p_options);
	void pcl_marker(sl::FrameToken *p_frame_token, sl::PCLMarker p_marker);

	sl::FrameToken *get_new_frame_token();

	// DLSS Frame Generation state. Only one viewport (the one with Viewport.frame_generation, evaluated by DLSS) can
	// have it, since all viewports present into the same swap chain.
	sl::ViewportHandle dlssg_viewport;
	int dlssg_delay = 0; // Frames to wait before (re-)enabling, after a swap chain change.
	int dlssg_requested_frames = 1; // Frames to generate per rendered frame (STREAMLINE_PARAM_DLSS_G_FRAMES).
	int dlssg_active_frames = 0; // numFramesToGenerate currently set (0 = off).
	int dlssg_max_frames = 0; // DLSSGState::numFramesToGenerateMax, 0 = not queried yet.
	uint32_t dlssg_last_status = 0; // Last reported DLSSGStatus bitmask, only status changes are logged.
	bool dlssg_evaluated = false; // The frame generation viewport was evaluated (tagged) since the last present.
	bool dlssg_hudless_reported = false;
	bool dlssg_active() const { return (uint32_t)dlssg_viewport != UINT_MAX; }
	// Called from the DLSS evaluation of p_viewport (render graph callback, render thread).
	void dlssg_evaluate(const sl::ViewportHandle &p_viewport, bool p_want);
	void dlssg_disable(const char *p_reason);
	void dlssg_check_evaluated_before_present();
	void dlssg_report_status(const sl::ViewportHandle &p_viewport);
	void dlssg_query_max_frames(const sl::ViewportHandle &p_viewport);

	static void initialize(bool d3d12);

	static StreamlineContext &get();

	// Set when slInit() succeeded; d3d12 tells which API Streamline was initialized for.
	bool initialized = false;
	bool initialized_d3d12 = false;
	// The main rendering device registered with Streamline. Local RenderingDevices (GPU texture compression,
	// lightmapper) created later are ignored so they don't re-target or shut down Streamline.
	void *registered_device = nullptr;
	bool is_initialized_for(bool p_d3d12) const { return initialized && initialized_d3d12 == p_d3d12; }

	sl::FrameToken *last_token = nullptr;
	bool is_game = false;
	bool post_init_done = false; // Feature functions loaded and options set for the main device.

	// Clears everything that refers to the Streamline instance after slShutdown().
	void reset_after_shutdown();

#if STREAMLINE_ENABLED_VULKAN
	StreamlineCapabilities enumerate_support_vulkan(void *vk_physical_device);
#endif
#if STREAMLINE_ENABLED_D3D12
	void init_device_d3d12(void *d3d_device);
	StreamlineCapabilities enumerate_support_d3d12(void *d3d_adapter_luid);
#endif

	StreamlineCapabilities streamline_capabilities;
};
#endif
