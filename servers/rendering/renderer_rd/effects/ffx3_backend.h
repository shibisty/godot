/**************************************************************************/
/*  ffx3_backend.h                                                        */
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

// Mariner E24: RenderingDevice backend (FfxInterface) for the AMD FidelityFX SDK 1.1.4 components in
// thirdparty/amd-fsr3 (FSR 3.1 upscaler, frame interpolation, optical flow).
//
// Internal header: only for the translation units that compile those components. Their types live in "namespace
// FFX3" (FSR 2.2 in thirdparty/amd-fsr2 declares the same names globally); these files are kept out of the SCU
// batches (scu_builders.py) so that the library macros never meet FSR 2's.

#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/rendering_device.h"

// clang-format off
// Standard headers the library uses, included outside the namespace so that their include guards keep the copies
// inside "namespace FFX3" empty.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef FFX_GCC
#define FFX_GCC // Only decides whether symbols are exported, which Godot doesn't need.
#endif

#if defined(__clang__)
#define FFX3_LIBRARY_BEGIN _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Weverything\"")
#define FFX3_LIBRARY_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define FFX3_LIBRARY_BEGIN _Pragma("GCC diagnostic push") \
	_Pragma("GCC diagnostic ignored \"-Wall\"") \
	_Pragma("GCC diagnostic ignored \"-Wextra\"") \
	_Pragma("GCC diagnostic ignored \"-Wunused-function\"") \
	_Pragma("GCC diagnostic ignored \"-Wunused-variable\"") \
	_Pragma("GCC diagnostic ignored \"-Wunused-but-set-variable\"") \
	_Pragma("GCC diagnostic ignored \"-Wunknown-pragmas\"") \
	_Pragma("GCC diagnostic ignored \"-Wsign-compare\"") \
	_Pragma("GCC diagnostic ignored \"-Wmissing-field-initializers\"") \
	_Pragma("GCC diagnostic ignored \"-Wshadow\"") \
	_Pragma("GCC diagnostic ignored \"-Wnarrowing\"") \
	_Pragma("GCC diagnostic ignored \"-Wparentheses\"")
#define FFX3_LIBRARY_END _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
#define FFX3_LIBRARY_BEGIN __pragma(warning(push, 0))
#define FFX3_LIBRARY_END __pragma(warning(pop))
#else
#define FFX3_LIBRARY_BEGIN
#define FFX3_LIBRARY_END
#endif

FFX3_LIBRARY_BEGIN
namespace FFX3 {
#include "thirdparty/amd-fsr3/ffx_interface.h"
#include "thirdparty/amd-fsr3/ffx_util.h"
} // namespace FFX3
FFX3_LIBRARY_END
// clang-format on

namespace RendererRD {

struct FFX3Binding {
	uint32_t slot;
	const wchar_t *name;
};

struct FFX3Pass;

struct FFX3PipelineVariant {
	RID shader;
	RID pipeline;
	const FFX3Pass *pass = nullptr;
};

// One compute pass of an FFX component: its shader (owned by the effect) and its hand-written binding layout.
struct FFX3Pass {
	ShaderRD *shader = nullptr;
	RID shader_version;
	bool shares_shader = false; // Uses another pass' shader version (not freed with this pass).
	bool has_fp16_variant = false; // Variant +1 (+2 for the upscaler's accumulate pass) defines FFX_HALF.
	bool fp16_allowed = true;
	FFX3PipelineVariant variants[4];
	LocalVector<FFX3Binding> srvs; // Sampled textures.
	LocalVector<FFX3Binding> uavs; // Storage images.
	LocalVector<FFX3Binding> srv_buffers; // Read-only storage buffers.
	LocalVector<FFX3Binding> uav_buffers; // Storage buffers.
	LocalVector<FFX3Binding> cbs; // Uniform buffers.
	LocalVector<uint32_t> placeholder_uavs; // Storage images declared by the GLSL with no library resource.
};

// The passes of one FfxEffect, plus how a permutation maps to a shader variant.
struct FFX3Effect {
	FFX3Pass *passes = nullptr;
	uint32_t pass_count = 0;
	// Samplers the GLSL declares (binding, linear or point); they differ between components.
	LocalVector<Pair<uint32_t, bool>> samplers;
	uint32_t (*select_variant)(uint32_t p_pass, uint32_t p_permutation, const FFX3Pass &p_layout) = nullptr;
};

struct FFX3Device {
	FFX3::FfxDeviceCapabilities capabilities = {};
	RID point_clamp_sampler;
	RID linear_clamp_sampler;
	HashMap<uint32_t, FFX3Effect> effects; // By FfxEffect.

	FFX3Device();
	~FFX3Device();
	// Frees the shader versions (and with them the pipelines). Call before the ShaderRD objects are destroyed.
	void free_shaders();
};

// One FfxInterface instance (one context of one component).
class FFX3Context {
public:
	struct Resources {
		LocalVector<RID> rids;
		LocalVector<bool> owned;
		LocalVector<bool> is_buffer;
		LocalVector<LocalVector<RID>> mip_slice_rids;
		LocalVector<FFX3::FfxResourceDescription> descriptions;
		LocalVector<uint32_t> dynamic_list;
		LocalVector<uint32_t> free_list;

		uint32_t add(RID p_rid, bool p_owned, bool p_buffer, const FFX3::FfxResourceDescription &p_description);
		void remove(uint32_t p_index);
		bool is_valid(int32_t p_index) const {
			return p_index >= 0 && uint32_t(p_index) < rids.size() && rids[p_index].is_valid();
		}
	};

	struct Job {
		FFX3::FfxGpuJobType type = FFX3::FFX_GPU_JOB_COMPUTE;
		int32_t target = -1;
		int32_t source = -1;
		float color[4] = {};
		const FFX3PipelineVariant *pipeline = nullptr;
		const FFX3Effect *effect = nullptr;
		uint32_t dimensions[3] = {};
		struct Binding {
			uint32_t slot;
			int32_t resource;
			uint32_t mip;
		};
		LocalVector<Binding> srvs;
		LocalVector<Binding> uavs;
		LocalVector<Binding> srv_buffers;
		LocalVector<Binding> uav_buffers;
		struct CB {
			uint32_t slot;
			uint32_t size; // In 32-bit entries.
			const uint32_t *data;
		};
		LocalVector<CB> cbs;
	};

	static constexpr uint32_t UBO_RING_SIZE = 96;
	static constexpr uint32_t UBO_SIZE = 1024; // Bytes; the largest block is the frame interpolation's (< 512).
	static constexpr uint32_t CB_STAGING_COUNT = 24;

	FFX3Device *device = nullptr;
	uint32_t effect = 0;
	Resources resources;
	LocalVector<Job> jobs;
	RID ubo_ring[UBO_RING_SIZE];
	uint32_t ubo_ring_index = 0;
	uint32_t cb_staging[CB_STAGING_COUNT][UBO_SIZE / sizeof(uint32_t)] = {};
	uint32_t cb_staging_index = 0;
	RID placeholder_uav; // 1x1 R32F.

	// Fills the callbacks; the context must outlive every library call made through the interface.
	void setup_interface(FFX3::FfxInterface &r_interface, uint32_t p_effect);
	// Frees what the library left (after its own context was destroyed, or when its creation failed).
	void release();
	~FFX3Context() { release(); }

	// An RD texture as an FfxResource; the RID must stay alive (and at the same address) until the dispatch returns.
	static FFX3::FfxResource wrap_texture(RID *p_rid, const wchar_t *p_name);
	static RD::DataFormat to_rd_format(FFX3::FfxSurfaceFormat p_format);
	static FFX3::FfxSurfaceFormat to_ffx_format(RD::DataFormat p_format);
};

// Every binding name of the effect's hand-written layouts must be one of the library's (else "pass N: unknown ...").
String ffx3_check_layout_names(const FFX3Effect &p_effect, const wchar_t *const *p_srv_names, uint32_t p_srv_count, const wchar_t *const *p_uav_names, uint32_t p_uav_count, const wchar_t *const *p_cb_names, uint32_t p_cb_count, uint32_t p_skip_pass = UINT32_MAX);

// Fills a pass layout (used by the effects' layout tables).
void ffx3_layout(FFX3Pass &r_pass, bool p_fp16_variant, std::initializer_list<FFX3Binding> p_srvs, std::initializer_list<FFX3Binding> p_uavs, std::initializer_list<FFX3Binding> p_srv_buffers, std::initializer_list<FFX3Binding> p_uav_buffers, std::initializer_list<FFX3Binding> p_cbs);

} // namespace RendererRD
