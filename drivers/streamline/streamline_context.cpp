/**************************************************************************/
/*  streamline_context.cpp                                                */
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

#include "streamline_context.h"

#ifdef STREAMLINE_ENABLED
#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#if STREAMLINE_ENABLED_VULKAN
#include <vulkan/vulkan.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/os/os.h"

StreamlineContext &StreamlineContext::get() {
	static StreamlineContext _context;
	return _context;
}

#ifdef _WIN32
static HMODULE _load_interposer() {
	// Load sl.interposer.dll from the executable's folder (Streamline also looks for its plugins and
	// nvngx_dlss.dll there), falling back to the regular DLL search order like NVIDIA's fork does.
	HMODULE module = nullptr;
	WCHAR exe_path[MAX_PATH];
	DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
	if (len > 0 && len < MAX_PATH) {
		static const WCHAR interposer_name[] = L"sl.interposer.dll";
		WCHAR *last_sep = wcsrchr(exe_path, L'\\');
		if (last_sep && (size_t)(last_sep + 1 - exe_path) + std_size(interposer_name) <= MAX_PATH) {
			// Bounded copy (the name and its terminator fit, checked above); avoids MSVC's C4996 on wcscpy.
			memcpy(last_sep + 1, interposer_name, sizeof(interposer_name));
			module = LoadLibraryW(exe_path);
		}
	}
	if (module == nullptr) {
		module = LoadLibraryA("sl.interposer.dll");
	}
	return module;
}
#endif

void StreamlineContext::load_functions(bool d3d12) {
#ifdef _WIN32
	HMODULE streamline = _load_interposer();
	if (streamline == nullptr) {
		print_verbose("Streamline: sl.interposer.dll not found next to the executable, NVIDIA DLSS is unavailable.");
		return;
	}
#define SL_GETPROC(type, name) (type *)(void *)GetProcAddress(streamline, name)
	this->slInit = SL_GETPROC(PFun_slInit, "slInit");
	this->slShutdown = SL_GETPROC(PFun_slShutdown, "slShutdown");
	this->slIsFeatureSupported = SL_GETPROC(PFun_slIsFeatureSupported, "slIsFeatureSupported");
	this->slGetFeatureFunction = SL_GETPROC(PFun_slGetFeatureFunction, "slGetFeatureFunction");
	this->slGetNewFrameToken = SL_GETPROC(PFun_slGetNewFrameToken, "slGetNewFrameToken");
	this->slSetFeatureLoaded = SL_GETPROC(PFun_slSetFeatureLoaded, "slSetFeatureLoaded");
	this->slSetD3DDevice = SL_GETPROC(PFun_slSetD3DDevice, "slSetD3DDevice");

	this->slAllocateResources = SL_GETPROC(PFun_slAllocateResources, "slAllocateResources");
	this->slFreeResources = SL_GETPROC(PFun_slFreeResources, "slFreeResources");
	this->slEvaluateFeature = SL_GETPROC(PFun_slEvaluateFeature, "slEvaluateFeature");
	this->slSetTag = SL_GETPROC(PFun_slSetTag, "slSetTag");
	this->slSetConstants = SL_GETPROC(PFun_slSetConstants, "slSetConstants");
#undef SL_GETPROC

#if STREAMLINE_ENABLED_VULKAN
	if (!d3d12) {
		this->func_vkGetInstanceProcAddr = (void *)GetProcAddress(streamline, "vkGetInstanceProcAddr");
	}
#endif
#if STREAMLINE_ENABLED_D3D12
	if (d3d12) {
		this->func_CreateDXGIFactory = (void *)GetProcAddress(streamline, "CreateDXGIFactory");
		this->func_CreateDXGIFactory1 = (void *)GetProcAddress(streamline, "CreateDXGIFactory1");
		this->func_CreateDXGIFactory2 = (void *)GetProcAddress(streamline, "CreateDXGIFactory2");
		this->func_D3D12CreateDevice = (void *)GetProcAddress(streamline, "D3D12CreateDevice");
		this->func_D3D12GetInterface = (void *)GetProcAddress(streamline, "D3D12GetInterface");
		this->func_DXGIGetDebugInterface1 = (void *)GetProcAddress(streamline, "DXGIGetDebugInterface1");
	}
#endif
#endif
}

void StreamlineContext::load_functions_post_init() {
	if (slGetFeatureFunction) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void *&)this->slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void *&)this->slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void *&)this->slDLSSSetOptions);

		// E3: Reflex, PCL and DLSS Frame Generation are only loaded for the running game (see initialize()).
		if (is_game) {
			if (streamline_capabilities.reflex_available) {
				slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void *&)this->slReflexSetOptions);
				slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void *&)this->slReflexSleep);
				slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void *&)this->slReflexGetState);
			}
			if (streamline_capabilities.pcl_available) {
				slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", (void *&)this->slPCLSetMarker);
				slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", (void *&)this->slPCLSetOptions);
			}
			if (streamline_capabilities.dlss_g_available) {
				slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void *&)this->slDLSSGGetState);
				slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void *&)this->slDLSSGSetOptions);
			}
		}
	}
}

static void _enumerate_support(StreamlineContext *self, sl::AdapterInfo &adapterInfo, StreamlineCapabilities &outSupport) {
	if (self->slIsFeatureSupported) {
		// DLSS Super Resolution (E2) everywhere; Reflex, PCL and DLSS-G (E3) only for the game. DLSS-RR and NIS of
		// NVIDIA's fork are not loaded and stay false.
		outSupport.dlss_available = self->slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo) == sl::Result::eOk;
		if (self->is_game) {
			outSupport.dlss_g_available = self->slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo) == sl::Result::eOk;
			outSupport.reflex_available = self->slIsFeatureSupported(sl::kFeatureReflex, adapterInfo) == sl::Result::eOk;
			outSupport.pcl_available = self->slIsFeatureSupported(sl::kFeaturePCL, adapterInfo) == sl::Result::eOk;
		}
	}
}

#if STREAMLINE_ENABLED_VULKAN
StreamlineCapabilities StreamlineContext::enumerate_support_vulkan(void *vk_physical_device) {
	VkPhysicalDevice device = (VkPhysicalDevice)vk_physical_device;
	StreamlineCapabilities support;
	sl::AdapterInfo adapterInfo;
	adapterInfo.vkPhysicalDevice = device;
	_enumerate_support(this, adapterInfo, support);
	return support;
}
#endif

#if STREAMLINE_ENABLED_D3D12
StreamlineCapabilities StreamlineContext::enumerate_support_d3d12(void *d3d_adapter_luid) {
	StreamlineCapabilities support;
	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t *)d3d_adapter_luid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
	_enumerate_support(this, adapterInfo, support);
	return support;
}

void StreamlineContext::init_device_d3d12(void *d3d12_device) {
	if (this->slSetD3DDevice) {
		sl::Result result = this->slSetD3DDevice(d3d12_device);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
	}
}
#endif

#define STREAMLINE_GAME_ONLY \
	if (!is_game) \
		return;

// Recomputes the Reflex mode that is actually applied. DLSS-G only works while Reflex is running, so frame
// generation promotes an "off" request to low latency instead (streamline_effective_reflex_mode()).
static sl::ReflexMode _effective_reflex_mode(sl::ReflexMode p_requested, bool p_required_by_dlssg) {
	return (sl::ReflexMode)(int)streamline_effective_reflex_mode((StreamlineReflexMode)(int)p_requested, p_required_by_dlssg);
}

void StreamlineContext::reflex_set_options(const sl::ReflexOptions &p_options) {
	STREAMLINE_GAME_ONLY; // No Reflex for the editor or project manager.

	reflex_options = p_options;
	reflex_options_dirty = false;
	sl::Result result = this->slReflexSetOptions ? this->slReflexSetOptions(p_options) : sl::Result::eOk;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::reflex_request_mode(sl::ReflexMode p_mode) {
	reflex_requested_mode = p_mode;

	sl::ReflexMode effective = _effective_reflex_mode(reflex_requested_mode, reflex_required_by_dlssg);
	if (reflex_options.mode != effective) {
		reflex_options.mode = effective;
		reflex_options_dirty = true;
	}
}

void StreamlineContext::reflex_set_required_by_dlssg(bool p_required) {
	if (reflex_required_by_dlssg == p_required) {
		return;
	}
	reflex_required_by_dlssg = p_required;

	sl::ReflexMode effective = _effective_reflex_mode(reflex_requested_mode, reflex_required_by_dlssg);
	if (reflex_options.mode != effective) {
		if (p_required) {
			print_verbose("Streamline: enabling Reflex low latency mode, required by DLSS Frame Generation.");
		}
		reflex_options.mode = effective;
		// The options are applied on the main thread in STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP, since this can be
		// called from the rendering thread.
		reflex_options_dirty = true;
	}
}

void StreamlineContext::reflex_sleep(sl::FrameToken *p_frame_token) {
	STREAMLINE_GAME_ONLY;

	if (p_frame_token == nullptr || this->slReflexSleep == nullptr) {
		return;
	}
	sl::Result result = this->slReflexSleep(*p_frame_token);
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::pcl_set_options(const sl::PCLOptions &p_options) {
	STREAMLINE_GAME_ONLY;

	pcl_options = p_options;
	pcl_options_dirty = false;
	sl::Result result = this->slPCLSetOptions ? this->slPCLSetOptions(p_options) : sl::Result::eOk;
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::pcl_marker(sl::FrameToken *p_frame_token, sl::PCLMarker p_marker) {
	STREAMLINE_GAME_ONLY;

	if (p_frame_token == nullptr || this->slPCLSetMarker == nullptr) {
		return;
	}
	sl::Result result = this->slPCLSetMarker(p_marker, *p_frame_token);
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, StreamlineContext::result_to_string(result));
}

void StreamlineContext::dlssg_query_max_frames(const sl::ViewportHandle &p_viewport) {
	if (dlssg_max_frames > 0 || slDLSSGGetState == nullptr) {
		return;
	}
	sl::DLSSGState state{};
	if (slDLSSGGetState(p_viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax >= 1) {
		dlssg_max_frames = MIN((int)state.numFramesToGenerateMax, STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES);
		print_verbose(vformat("Streamline: DLSS Frame Generation supports up to %d generated frame(s) per rendered frame (%dx).", dlssg_max_frames, dlssg_max_frames + 1));
	}
}

void StreamlineContext::dlssg_evaluate(const sl::ViewportHandle &p_viewport, bool p_want) {
	STREAMLINE_GAME_ONLY; // Frame generation only for the running game.

	if (slDLSSGSetOptions == nullptr || !streamline_capabilities.dlss_g_available) {
		return;
	}

	bool want = p_want;
	// DLSS-G presents magenta frames unless Reflex is running alongside it, so it needs both Reflex and PCL (which
	// provides the latency markers Reflex consumes) to be available.
	if (want && !(streamline_capabilities.reflex_available && streamline_capabilities.pcl_available)) {
		WARN_PRINT_ONCE("DLSS Frame Generation was requested but Reflex is not available on this system. Frame generation requires Reflex and has been disabled.");
		want = false;
	}

	// Reflex has to be turned on before frame generation starts, and stays on for as long as it runs.
	reflex_set_required_by_dlssg(want);
	if (want) {
		dlssg_query_max_frames(p_viewport);
	}

	// Disable the previous DLSS-G viewport if needed.
	if (dlssg_active() && ((!want && dlssg_viewport == p_viewport) || (want && dlssg_viewport != p_viewport))) {
		print_verbose(vformat("Streamline: disabling DLSS Frame Generation on viewport %d.", (uint32_t)dlssg_viewport));
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		sl::Result result = slDLSSGSetOptions(dlssg_viewport, options);
		dlssg_viewport = sl::ViewportHandle(-1);
		dlssg_active_frames = 0;
		dlssg_last_status = 0;
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	// Wait for Reflex to be running before starting frame generation, otherwise the first frames are presented as
	// magenta. This costs at most one frame.
	const bool can_activate = dlssg_delay == 0 && reflex_is_running();
	const int frames = streamline_clamp_generated_frames(dlssg_requested_frames, dlssg_max_frames);

	// Enable DLSS-G on this viewport, or change the number of generated frames (multi frame generation).
	if (want && can_activate && (dlssg_viewport != p_viewport || dlssg_active_frames != frames)) {
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOn;
		options.numFramesToGenerate = (uint32_t)frames;
		sl::Result result = slDLSSGSetOptions(p_viewport, options);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));

		print_verbose(vformat("Streamline: DLSS Frame Generation on viewport %d, %d generated frame(s) per rendered frame.", (uint32_t)p_viewport, frames));
		dlssg_viewport = p_viewport;
		dlssg_active_frames = frames;
	}

	if (dlssg_active() && dlssg_viewport == p_viewport) {
		dlssg_evaluated = true;
		// Report why frame generation is failing instead of silently presenting magenta frames.
		dlssg_report_status(p_viewport);
	}
}

void StreamlineContext::dlssg_disable(const char *p_reason) {
	STREAMLINE_GAME_ONLY;

	dlssg_delay = 10;
	dlssg_last_status = 0;
	dlssg_evaluated = false;
	reflex_set_required_by_dlssg(false);

	if (slDLSSGSetOptions && dlssg_active()) {
		print_verbose(vformat("Streamline: DLSS Frame Generation turned off on viewport %d (%s).", (uint32_t)dlssg_viewport, p_reason));

		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		slDLSSGSetOptions(dlssg_viewport, options);
	}
	dlssg_viewport = sl::ViewportHandle(-1);
	dlssg_active_frames = 0;
}

void StreamlineContext::dlssg_check_evaluated_before_present() {
	if (dlssg_active() && !dlssg_evaluated) {
		// The tags (depth, motion vectors, HUD-less colour) would be stale: interpolating them shows the last 3D
		// frame through menus or after the upscaler was switched off.
		dlssg_disable("its viewport was not evaluated by DLSS this frame");
	}
	dlssg_evaluated = false;
}

void StreamlineContext::dlssg_report_status(const sl::ViewportHandle &p_viewport) {
	if (slDLSSGGetState == nullptr) {
		return;
	}

	sl::DLSSGState state{};
	if (slDLSSGGetState(p_viewport, state, nullptr) != sl::Result::eOk) {
		return;
	}
	if (state.numFramesToGenerateMax >= 1) {
		dlssg_max_frames = MIN((int)state.numFramesToGenerateMax, STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES);
	}

	uint32_t status = (uint32_t)state.status;
	if (status == dlssg_last_status) {
		return; // Only report changes, this is polled every frame.
	}
	dlssg_last_status = status;

	if (status == (uint32_t)sl::DLSSGStatus::eOk) {
		return;
	}

	// DLSS-G presents magenta frames when any of these are set, so make the reason visible.
	String reasons;
	if (status & (uint32_t)sl::DLSSGStatus::eFailResolutionTooLow) {
		reasons += "\n- Output resolution is too low (minimum " + itos(state.minWidthOrHeight) + " px).";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) {
		reasons += "\n- Reflex is not running. Reflex must be active while DLSS Frame Generation is on.";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailHDRFormatNotSupported) {
		reasons += "\n- The swap chain back buffer format is not supported (DLSS-G does not support FP16/scRGB HDR output).";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailCommonConstantsInvalid) {
		reasons += "\n- Invalid common constants were provided (camera matrices, jitter or motion vectors).";
	}
	if (status & (uint32_t)sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) {
		reasons += "\n- IDXGISwapChain3::GetCurrentBackBufferIndex was not called before presenting.";
	}
	if (reasons.is_empty()) {
		reasons = "\n- Unknown status: " + itos(status) + ".";
	}

	ERR_PRINT("DLSS Frame Generation is not working and will present corrupted (magenta) frames:" + reasons);
}

void StreamlineContext::reset_after_shutdown() {
	initialized = false;
	post_init_done = false;
	registered_device = nullptr;
	streamline_capabilities = StreamlineCapabilities();
	slDLSSGetOptimalSettings = nullptr;
	slDLSSGetState = nullptr;
	slDLSSSetOptions = nullptr;
	slReflexGetState = nullptr;
	slReflexSetOptions = nullptr;
	slReflexSleep = nullptr;
	slPCLSetMarker = nullptr;
	slPCLSetOptions = nullptr;
	slDLSSGGetState = nullptr;
	slDLSSGSetOptions = nullptr;
	reflex_options = sl::ReflexOptions();
	reflex_options_dirty = true;
	reflex_required_by_dlssg = false;
	pcl_options_dirty = true;
	dlssg_viewport = sl::ViewportHandle(-1);
	dlssg_active_frames = 0;
	dlssg_max_frames = 0;
	dlssg_last_status = 0;
	dlssg_evaluated = false;
	last_token = nullptr;
}

sl::FrameToken *StreamlineContext::get_new_frame_token() {
	if (!initialized || this->slGetNewFrameToken == nullptr) {
		return nullptr;
	}
	sl::Result result = this->slGetNewFrameToken(last_token, nullptr);
	ERR_FAIL_COND_V_MSG(result != sl::Result::eOk, nullptr, StreamlineContext::result_to_string(result));
	return last_token;
}

const char *StreamlineContext::result_to_string(sl::Result result) {
	switch (result) {
		case sl::Result::eOk:
			return "sl::eOk";
		case sl::Result::eErrorIO:
			return "sl::eErrorIO";
		case sl::Result::eErrorDriverOutOfDate:
			return "sl::eErrorDriverOutOfDate";
		case sl::Result::eErrorOSOutOfDate:
			return "sl::eErrorOSOutOfDate";
		case sl::Result::eErrorOSDisabledHWS:
			return "sl::eErrorOSDisabledHWS";
		case sl::Result::eErrorDeviceNotCreated:
			return "sl::eErrorDeviceNotCreated";
		case sl::Result::eErrorNoSupportedAdapterFound:
			return "sl::eErrorNoSupportedAdapterFound";
		case sl::Result::eErrorAdapterNotSupported:
			return "sl::eErrorAdapterNotSupported";
		case sl::Result::eErrorNoPlugins:
			return "sl::eErrorNoPlugins";
		case sl::Result::eErrorVulkanAPI:
			return "sl::eErrorVulkanAPI";
		case sl::Result::eErrorDXGIAPI:
			return "sl::eErrorDXGIAPI";
		case sl::Result::eErrorD3DAPI:
			return "sl::eErrorD3DAPI";
		case sl::Result::eErrorNRDAPI:
			return "sl::eErrorNRDAPI";
		case sl::Result::eErrorNVAPI:
			return "sl::eErrorNVAPI";
		case sl::Result::eErrorReflexAPI:
			return "sl::eErrorReflexAPI";
		case sl::Result::eErrorNGXFailed:
			return "sl::eErrorNGXFailed";
		case sl::Result::eErrorJSONParsing:
			return "sl::eErrorJSONParsing";
		case sl::Result::eErrorMissingProxy:
			return "sl::eErrorMissingProxy";
		case sl::Result::eErrorMissingResourceState:
			return "sl::eErrorMissingResourceState";
		case sl::Result::eErrorInvalidIntegration:
			return "sl::eErrorInvalidIntegration";
		case sl::Result::eErrorMissingInputParameter:
			return "sl::eErrorMissingInputParameter";
		case sl::Result::eErrorNotInitialized:
			return "sl::eErrorNotInitialized";
		case sl::Result::eErrorComputeFailed:
			return "sl::eErrorComputeFailed";
		case sl::Result::eErrorInitNotCalled:
			return "sl::eErrorInitNotCalled";
		case sl::Result::eErrorExceptionHandler:
			return "sl::eErrorExceptionHandler";
		case sl::Result::eErrorInvalidParameter:
			return "sl::eErrorInvalidParameter";
		case sl::Result::eErrorMissingConstants:
			return "sl::eErrorMissingConstants";
		case sl::Result::eErrorDuplicatedConstants:
			return "sl::eErrorDuplicatedConstants";
		case sl::Result::eErrorMissingOrInvalidAPI:
			return "sl::eErrorMissingOrInvalidAPI";
		case sl::Result::eErrorCommonConstantsMissing:
			return "sl::eErrorCommonConstantsMissing";
		case sl::Result::eErrorUnsupportedInterface:
			return "sl::eErrorUnsupportedInterface";
		case sl::Result::eErrorFeatureMissing:
			return "sl::eErrorFeatureMissing";
		case sl::Result::eErrorFeatureNotSupported:
			return "sl::eErrorFeatureNotSupported";
		case sl::Result::eErrorFeatureMissingHooks:
			return "sl::eErrorFeatureMissingHooks";
		case sl::Result::eErrorFeatureFailedToLoad:
			return "sl::eErrorFeatureFailedToLoad";
		case sl::Result::eErrorFeatureWrongPriority:
			return "sl::eErrorFeatureWrongPriority";
		case sl::Result::eErrorFeatureMissingDependency:
			return "sl::eErrorFeatureMissingDependency";
		case sl::Result::eErrorFeatureManagerInvalidState:
			return "sl::eErrorFeatureManagerInvalidState";
		case sl::Result::eErrorInvalidState:
			return "sl::eErrorInvalidState";
		case sl::Result::eWarnOutOfVRAM:
			return "sl::eWarnOutOfVRAM";
		default:
			return "sl::eUnknown";
	}
}

void StreamlineContext::initialize(bool d3d12) {
	StreamlineContext &ctx = StreamlineContext::get();
	ctx.is_game = true;
	if (Engine::get_singleton()->is_editor_hint() || Engine::get_singleton()->is_project_manager_hint()) {
		ctx.is_game = false;
	}

	if (ctx.slInit != nullptr) {
		return; // Already initialized.
	}

	ctx.load_functions(d3d12);
	if (ctx.slInit == nullptr) {
		return;
	}

	sl::Preferences pref;

	// DLSS Super Resolution / DLAA (E2) always; PCL, Reflex and DLSS Frame Generation (E3) only for the running game,
	// like NVIDIA's fork. DLSS-RR, NIS and the ImGui plugin of the fork are not loaded.
	Vector<sl::Feature> featuresToLoad;
	if (ctx.is_game) {
		featuresToLoad.push_back(sl::kFeaturePCL);
		featuresToLoad.push_back(sl::kFeatureReflex);
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
	}
	featuresToLoad.push_back(sl::kFeatureDLSS);

	pref.featuresToLoad = featuresToLoad.ptr();
	pref.numFeaturesToLoad = featuresToLoad.size();

	pref.renderAPI = d3d12 ? sl::RenderAPI::eD3D12 : sl::RenderAPI::eVulkan;
	pref.applicationId = 0x90d07004; // Same NGX application ID as NVIDIA's Godot fork.
	pref.flags = sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins | sl::PreferenceFlags::eDisableCLStateTracking;

	if (bool(GLOBAL_GET("rendering/streamline/streamline_log"))) {
		pref.logLevel = sl::LogLevel::eVerbose;
		pref.showConsole = true;
	} else {
		pref.logLevel = sl::LogLevel::eOff;
		pref.showConsole = false;
	}
	sl::Result result = ctx.slInit(pref, sl::kSDKVersion);
	if (result != sl::Result::eOk) {
		// Leave the interposer unused: the rendering drivers only take its proxies when slInit() succeeded.
		ERR_PRINT(vformat("Streamline: slInit failed (%s), NVIDIA DLSS is unavailable.", StreamlineContext::result_to_string(result)));
		return;
	}
	ctx.initialized = true;
	ctx.initialized_d3d12 = d3d12;
	print_verbose(vformat("Streamline: initialized for %s.", d3d12 ? "D3D12" : "Vulkan"));
}

#endif
