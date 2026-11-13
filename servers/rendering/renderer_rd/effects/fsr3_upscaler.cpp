/**************************************************************************/
/*  fsr3_upscaler.cpp                                                     */
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

#include "fsr3_upscaler.h"

#include "ffx3_backend.h"

#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_accumulate_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_autogen_reactive_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_debug_view_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_luma_instability_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_luma_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_prepare_inputs_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_prepare_reactivity_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_rcas_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_shading_change_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/fsr3_upscaler_shading_change_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_server_globals.h"

// clang-format off
// The upscaler component (its static helpers stay in this translation unit).
FFX3_LIBRARY_BEGIN
namespace FFX3 {
#include "thirdparty/amd-fsr3/ffx_fsr3upscaler.cpp"
} // namespace FFX3
FFX3_LIBRARY_END
// clang-format on

using namespace RendererRD;
using namespace FFX3;

struct FSR3UpscalerEffect::Device {
	FFX3Device base;
	FFX3Pass passes[FFX_FSR3UPSCALER_PASS_COUNT];

	struct {
		Fsr3UpscalerPrepareInputsPassShaderRD prepare_inputs;
		Fsr3UpscalerLumaPyramidPassShaderRD luma_pyramid;
		Fsr3UpscalerShadingChangePyramidPassShaderRD shading_change_pyramid;
		Fsr3UpscalerShadingChangePassShaderRD shading_change;
		Fsr3UpscalerPrepareReactivityPassShaderRD prepare_reactivity;
		Fsr3UpscalerLumaInstabilityPassShaderRD luma_instability;
		Fsr3UpscalerAccumulatePassShaderRD accumulate;
		Fsr3UpscalerRcasPassShaderRD rcas;
		Fsr3UpscalerDebugViewPassShaderRD debug_view;
		Fsr3UpscalerAutogenReactivePassShaderRD autogen_reactive;
	} shaders;
};

namespace RendererRD {

class FSR3UpscalerContext {
public:
	FFX3Context backend;
	FfxFsr3UpscalerContextDescription fsr_desc = {};
	FfxFsr3UpscalerContext *fsr_context = nullptr; // About 4 MB: allocated on the heap.
	bool created = false;
	bool first_dispatch = true;
	FSR3UpscalerEffect::SharedSurfaces shared;

	~FSR3UpscalerContext() {
		if (created) {
			ffxFsr3UpscalerContextDestroy(fsr_context);
		}
		if (fsr_context) {
			memfree(fsr_context);
		}
		backend.release();
		RenderingDevice *rd = RD::get_singleton();
		for (RID rid : { shared.dilated_depth, shared.dilated_motion_vectors, shared.reconstructed_prev_nearest_depth }) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	}
};

} // namespace RendererRD

// Godot can't reflect binding names, so these mirror the FSR3UPSCALER_BIND_* defines of each
// thirdparty/amd-fsr3/shaders/ffx_fsr3upscaler_*_pass.glsl (with low resolution motion vectors), under the names the
// upscaler's binding tables use (ffx_fsr3upscaler.cpp). They must be revised whenever the library is updated: a slot
// mismatch shows up as a uniform set error, a name mismatch fails context creation (and the E24 doctest).
static void fsr3_fill_layouts(FFX3Pass *r_passes) {
	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS], true,
			{ { 0, L"r_input_motion_vectors" }, { 1, L"r_input_depth" }, { 2, L"r_input_color_jittered" } },
			{ { 3, L"rw_dilated_motion_vectors" }, { 4, L"rw_dilated_depth" }, { 5, L"rw_reconstructed_previous_nearest_depth" }, { 6, L"rw_farthest_depth" }, { 7, L"rw_current_luma" } },
			{},
			{},
			{ { 8, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID], false,
			{ { 0, L"r_current_luma" }, { 1, L"r_farthest_depth" } },
			{ { 2, L"rw_spd_global_atomic" }, { 3, L"rw_frame_info" }, { 4, L"rw_spd_mip0" }, { 5, L"rw_spd_mip1" }, { 6, L"rw_spd_mip2" }, { 7, L"rw_spd_mip3" }, { 8, L"rw_spd_mip4" }, { 9, L"rw_spd_mip5" }, { 10, L"rw_farthest_depth_mip1" } },
			{},
			{},
			{ { 11, L"cbFSR3Upscaler" }, { 12, L"cbSPD" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID], false,
			{ { 0, L"r_current_luma" }, { 1, L"r_previous_luma" }, { 2, L"r_dilated_motion_vectors" }, { 3, L"r_input_exposure" } },
			{ { 4, L"rw_spd_global_atomic" }, { 5, L"rw_spd_mip0" }, { 6, L"rw_spd_mip1" }, { 7, L"rw_spd_mip2" }, { 8, L"rw_spd_mip3" }, { 9, L"rw_spd_mip4" }, { 10, L"rw_spd_mip5" } },
			{},
			{},
			{ { 11, L"cbFSR3Upscaler" }, { 12, L"cbSPD" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_SHADING_CHANGE], true,
			{ { 0, L"r_spd_mips" } },
			{ { 1, L"rw_shading_change" } },
			{},
			{},
			{ { 2, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY], true,
			{ { 0, L"r_reconstructed_previous_nearest_depth" }, { 1, L"r_dilated_motion_vectors" }, { 2, L"r_dilated_depth" }, { 3, L"r_reactive_mask" }, { 4, L"r_transparency_and_composition_mask" }, { 5, L"r_accumulation" }, { 6, L"r_shading_change" }, { 7, L"r_current_luma" }, { 8, L"r_input_exposure" } },
			{ { 9, L"rw_dilated_reactive_masks" }, { 10, L"rw_new_locks" }, { 11, L"rw_accumulation" } },
			{},
			{},
			{ { 12, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY], true,
			{ { 0, L"r_input_exposure" }, { 1, L"r_dilated_reactive_masks" }, { 2, L"r_dilated_motion_vectors" }, { 3, L"r_frame_info" }, { 4, L"r_luma_history" }, { 5, L"r_farthest_depth_mip1" }, { 6, L"r_current_luma" } },
			{ { 7, L"rw_luma_history" }, { 8, L"rw_luma_instability" } },
			{},
			{},
			{ { 9, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE], true,
			{ { 0, L"r_input_exposure" }, { 1, L"r_dilated_reactive_masks" }, { 2, L"r_dilated_motion_vectors" }, { 3, L"r_internal_upscaled_color" }, { 4, L"r_lanczos_lut" }, { 5, L"r_farthest_depth_mip1" }, { 6, L"r_current_luma" }, { 7, L"r_luma_instability" }, { 8, L"r_input_color_jittered" } },
			{ { 9, L"rw_internal_upscaled_color" }, { 10, L"rw_upscaled_output" }, { 11, L"rw_new_locks" } },
			{},
			{},
			{ { 12, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_RCAS], false,
			{ { 0, L"r_input_exposure" }, { 1, L"r_rcas_input" } },
			{ { 2, L"rw_upscaled_output" } },
			{},
			{},
			{ { 3, L"cbFSR3Upscaler" }, { 4, L"cbRCAS" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_DEBUG_VIEW], false,
			{ { 0, L"r_dilated_reactive_masks" }, { 1, L"r_dilated_motion_vectors" }, { 2, L"r_dilated_depth" }, { 3, L"r_internal_upscaled_color" }, { 4, L"r_input_exposure" } },
			{ { 5, L"rw_upscaled_output" } },
			{},
			{},
			{ { 6, L"cbFSR3Upscaler" } });

	ffx3_layout(r_passes[FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE], true,
			{ { 0, L"r_input_opaque_only" }, { 1, L"r_input_color_jittered" } },
			{ { 2, L"rw_output_autoreactive" } },
			{},
			{},
			{ { 4, L"cbFSR3Upscaler" }, { 5, L"cbGenerateReactive" } });
	// FSR3UPSCALER_BIND_UAV_AUTOCOMPOSITION: written by the GLSL, deprecated (no resource) in the library.
	r_passes[FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE].placeholder_uavs.clear();
	r_passes[FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE].placeholder_uavs.push_back(3);

	// The sharpening variant of the accumulate pass shares its shader (variants 1 and 3), see fsr3_select_variant().
	FFX3Pass &sharpen = r_passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN];
	const FFX3Pass &accumulate = r_passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE];
	sharpen.has_fp16_variant = accumulate.has_fp16_variant;
	sharpen.srvs = accumulate.srvs;
	sharpen.uavs = accumulate.uavs;
	sharpen.cbs = accumulate.cbs;
}

static uint32_t fsr3_select_variant(uint32_t p_pass, uint32_t p_permutation, const FFX3Pass &p_layout) {
	const bool fp16 = (p_permutation & FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16) && p_layout.has_fp16_variant && p_layout.fp16_allowed;
	if (p_pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE || p_pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN) {
		return ((p_permutation & FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING) ? 1 : 0) + (fp16 ? 2 : 0);
	}
	return fp16 ? 1 : 0;
}

static String fsr3_check_layouts() {
	FFX3Effect effect;
	FFX3Pass passes[FFX_FSR3UPSCALER_PASS_COUNT];
	fsr3_fill_layouts(passes);
	effect.passes = passes;
	effect.pass_count = FFX_FSR3UPSCALER_PASS_COUNT;
	LocalVector<const wchar_t *> srv, uav, cb;
	for (const ResourceBinding &b : srvTextureBindingTable) {
		srv.push_back(b.name);
	}
	for (const ResourceBinding &b : uavTextureBindingTable) {
		uav.push_back(b.name);
	}
	for (const ResourceBinding &b : constantBufferBindingTable) {
		cb.push_back(b.name);
	}
	return ffx3_check_layout_names(effect, srv.ptr(), srv.size(), uav.ptr(), uav.size(), cb.ptr(), cb.size(), FFX_FSR3UPSCALER_PASS_TCR_AUTOGENERATE);
}

// Godot's addition to the constant buffer (0002-godot-fsr3-options.patch) must match the std140 layout of
// cbFSR3Upscaler in ffx_fsr3upscaler_callbacks_glsl.h: the matrix starts at byte 160.
static_assert(offsetof(Fsr3UpscalerConstants, reprojectionMatrix) == 160);
static_assert(sizeof(Fsr3UpscalerConstants) == 224);
static_assert(sizeof(Fsr3UpscalerConstants) <= FFX3Context::UBO_SIZE);

// Effect.

FSR3UpscalerEffect::FSR3UpscalerEffect() {
	device = memnew(Device);
	RenderingDevice *rd = RD::get_singleton();

	const String defines =
			"\n#define FFX_GPU\n"
			"\n#define FFX_GLSL 1\n"
			"\n#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
			"\n#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1\n"
			"\n#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 1\n"
			"\n#define FFX_FSR3UPSCALER_OPTION_GODOT_REACTIVE_MASK_CLAMP 1\n"
			"\n#define FFX_FSR3UPSCALER_OPTION_GODOT_DERIVE_INVALID_MOTION_VECTORS 1\n";

	Vector<String> modes_single;
	modes_single.push_back("");
	Vector<String> modes_fp16;
	modes_fp16.push_back("");
	modes_fp16.push_back("\n#define FFX_HALF 1\n");
	Vector<String> modes_accumulate;
	modes_accumulate.push_back("");
	modes_accumulate.push_back("\n#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 1\n");
	modes_accumulate.push_back("\n#define FFX_HALF 1\n");
	modes_accumulate.push_back("\n#define FFX_HALF 1\n#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 1\n");

	fsr3_fill_layouts(device->passes);
	auto shader = [&](FfxFsr3UpscalerPass p_pass, ShaderRD *p_shader, const Vector<String> &p_modes) {
		FFX3Pass &pass = device->passes[p_pass];
		pass.shader = p_shader;
		p_shader->initialize(p_modes, defines);
		pass.shader_version = p_shader->version_create();
	};
	shader(FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, &device->shaders.prepare_inputs, modes_fp16);
	shader(FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID, &device->shaders.luma_pyramid, modes_single);
	shader(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, &device->shaders.shading_change_pyramid, modes_single);
	shader(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE, &device->shaders.shading_change, modes_fp16);
	shader(FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, &device->shaders.prepare_reactivity, modes_fp16);
	shader(FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY, &device->shaders.luma_instability, modes_fp16);
	shader(FFX_FSR3UPSCALER_PASS_ACCUMULATE, &device->shaders.accumulate, modes_accumulate);
	shader(FFX_FSR3UPSCALER_PASS_RCAS, &device->shaders.rcas, modes_single);
	shader(FFX_FSR3UPSCALER_PASS_DEBUG_VIEW, &device->shaders.debug_view, modes_single);
	shader(FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE, &device->shaders.autogen_reactive, modes_fp16);
	{
		FFX3Pass &sharpen = device->passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN];
		const FFX3Pass &accumulate = device->passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE];
		sharpen.shader = accumulate.shader;
		sharpen.shader_version = accumulate.shader_version;
		sharpen.shares_shader = true;
	}
	// Same as FSR 2: the FP16 accumulate pass is slower on NVIDIA (occupancy, VRAM throughput).
	const bool fp16_accumulate = rd->get_device_vendor_name() != "NVIDIA";
	device->passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE].fp16_allowed = fp16_accumulate;
	device->passes[FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN].fp16_allowed = fp16_accumulate;

	FFX3Effect effect;
	effect.passes = device->passes;
	effect.pass_count = FFX_FSR3UPSCALER_PASS_COUNT;
	effect.samplers.push_back({ 900, false }); // s_PointClamp (below 1000: the D3D12 register encoding).
	effect.samplers.push_back({ 901, true }); // s_LinearClamp
	effect.select_variant = fsr3_select_variant;
	device->base.effects[FFX_EFFECT_FSR3UPSCALER] = effect;
}

FSR3UpscalerEffect::~FSR3UpscalerEffect() {
	device->base.free_shaders(); // Before the ShaderRD members go.
	memdelete(device);
}

FSR3UpscalerContext *FSR3UpscalerEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	ERR_FAIL_COND_V(p_internal_size.x <= 0 || p_internal_size.y <= 0 || p_target_size.x <= 0 || p_target_size.y <= 0, nullptr);
	RenderingDevice *rd = RD::get_singleton();

	FSR3UpscalerContext *context = memnew(FSR3UpscalerContext);
	context->backend.device = &device->base;
	context->fsr_context = reinterpret_cast<FfxFsr3UpscalerContext *>(memalloc(sizeof(FfxFsr3UpscalerContext)));

	FfxFsr3UpscalerContextDescription &desc = context->fsr_desc;
	desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
	desc.maxRenderSize = { uint32_t(p_internal_size.x), uint32_t(p_internal_size.y) };
	desc.maxUpscaleSize = { uint32_t(p_target_size.x), uint32_t(p_target_size.y) };
	desc.fpMessage = nullptr;
	context->backend.setup_interface(desc.backendInterface, FFX_EFFECT_FSR3UPSCALER);

	// The shared surfaces (ffxFsr3UpscalerGetSharedResourceDescriptions(): dilated depth R32F, dilated motion vectors
	// RG16F and reconstructed previous nearest depth R32UI, all at the maximum render size) are owned by the caller;
	// FSR 3 frame generation reads them too.
	auto make_shared = [&](RD::DataFormat p_format, const String &p_name) {
		RD::TextureFormat tf;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.format = p_format;
		tf.width = p_internal_size.x;
		tf.height = p_internal_size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		RID rid = rd->texture_create(tf, RD::TextureView());
		if (rid.is_valid()) {
			rd->set_resource_name(rid, p_name);
		}
		return rid;
	};
	SharedSurfaces &shared = context->shared;
	shared.dilated_depth = make_shared(RD::DATA_FORMAT_R32_SFLOAT, "FSR3 Dilated Depth");
	shared.dilated_motion_vectors = make_shared(RD::DATA_FORMAT_R16G16_SFLOAT, "FSR3 Dilated Motion Vectors");
	shared.reconstructed_prev_nearest_depth = make_shared(RD::DATA_FORMAT_R32_UINT, "FSR3 Reconstructed Previous Nearest Depth");
	if (shared.dilated_depth.is_null() || shared.dilated_motion_vectors.is_null() || shared.reconstructed_prev_nearest_depth.is_null()) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "FSR 3.1: couldn't create the shared surfaces.");
	}

	const FfxErrorCode result = ffxFsr3UpscalerContextCreate(context->fsr_context, &desc);
	if (result != FFX_OK) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, vformat("FSR 3.1: context creation failed (error 0x%x).", uint32_t(result)));
	}
	context->created = true;
	return context;
}

void FSR3UpscalerEffect::free_context(FSR3UpscalerContext *p_context) {
	if (p_context) {
		memdelete(p_context);
	}
}

const FSR3UpscalerEffect::SharedSurfaces *FSR3UpscalerEffect::get_shared_surfaces(const FSR3UpscalerContext *p_context) {
	return (p_context && p_context->shared.valid) ? &p_context->shared : nullptr;
}

void FSR3UpscalerEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	FSR3UpscalerContext &ctx = *p_params.context;

	// FfxResource keeps pointers to the RIDs until the dispatch returns.
	RID color = p_params.color;
	RID depth = p_params.depth;
	RID velocity = p_params.velocity;
	RID reactive = p_params.reactive;
	RID exposure = p_params.exposure;
	RID output = p_params.output;

	FfxFsr3UpscalerDispatchDescription dispatch = {};
	dispatch.commandList = &ctx; // Unused by this backend, but must not be null for the debug checks.
	dispatch.color = FFX3Context::wrap_texture(&color, L"color");
	dispatch.depth = FFX3Context::wrap_texture(&depth, L"depth");
	dispatch.motionVectors = FFX3Context::wrap_texture(&velocity, L"velocity");
	dispatch.reactive = FFX3Context::wrap_texture(&reactive, L"reactive");
	dispatch.exposure = FFX3Context::wrap_texture(&exposure, L"exposure");
	dispatch.transparencyAndComposition = {}; // Not provided by Godot (same as FSR 2).
	dispatch.dilatedDepth = FFX3Context::wrap_texture(&ctx.shared.dilated_depth, L"dilated_depth");
	dispatch.dilatedMotionVectors = FFX3Context::wrap_texture(&ctx.shared.dilated_motion_vectors, L"dilated_motion_vectors");
	dispatch.reconstructedPrevNearestDepth = FFX3Context::wrap_texture(&ctx.shared.reconstructed_prev_nearest_depth, L"reconstructed_prev_nearest_depth");
	dispatch.output = FFX3Context::wrap_texture(&output, L"output");
	dispatch.jitterOffset.x = p_params.jitter.x;
	dispatch.jitterOffset.y = p_params.jitter.y;
	dispatch.motionVectorScale.x = float(p_params.internal_size.width);
	dispatch.motionVectorScale.y = float(p_params.internal_size.height);
	dispatch.renderSize = { uint32_t(p_params.internal_size.width), uint32_t(p_params.internal_size.height) };
	dispatch.upscaleSize = ctx.fsr_desc.maxUpscaleSize;
	dispatch.enableSharpening = p_params.sharpness > 1e-6f;
	dispatch.sharpness = CLAMP(p_params.sharpness, 0.0f, 1.0f);
	dispatch.frameTimeDelta = p_params.delta_time * 1000.0f; // FSR 3.1 expects milliseconds.
	dispatch.preExposure = 1.0f;
	dispatch.reset = p_params.reset_accumulation || ctx.first_dispatch;
	dispatch.cameraNear = p_params.z_near;
	dispatch.cameraFar = p_params.z_far;
	dispatch.cameraFovAngleVertical = p_params.fovy;
	dispatch.viewSpaceToMetersFactor = 1.0f;
	dispatch.flags = 0;
	RendererRD::MaterialStorage::store_camera(p_params.reprojection, dispatch.reprojectionMatrix);
	ctx.first_dispatch = false;

	const FfxErrorCode result = ffxFsr3UpscalerContextDispatch(ctx.fsr_context, &dispatch);
	ERR_FAIL_COND_MSG(result != FFX_OK, vformat("FSR 3.1: dispatch failed (error 0x%x).", uint32_t(result)));

	// For frame generation (same frame): what the shared surfaces now hold.
	SharedSurfaces &shared = ctx.shared;
	shared.valid = true;
	shared.render_size = p_params.internal_size;
	shared.z_near = p_params.z_near;
	shared.z_far = p_params.z_far;
	shared.fovy = p_params.fovy;
	shared.delta_time = p_params.delta_time;
	shared.reset = dispatch.reset;
	shared.frame = RSG::rasterizer->get_frame_number();
}

uint32_t FSR3UpscalerEffect::get_sdk_version() {
	return FFX_SDK_MAKE_VERSION(1, 1, 4);
}

String FSR3UpscalerEffect::check_binding_layouts() {
	return fsr3_check_layouts();
}

int32_t FSR3UpscalerEffect::get_jitter_phase_count(int32_t p_render_width, int32_t p_display_width) {
	return ffxFsr3UpscalerGetJitterPhaseCount(p_render_width, p_display_width);
}
