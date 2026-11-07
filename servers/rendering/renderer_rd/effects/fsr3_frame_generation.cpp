/**************************************************************************/
/*  fsr3_frame_generation.cpp                                             */
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

#include "fsr3_frame_generation.h"

#include "ffx3_backend.h"

#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_compute_game_vector_field_inpainting_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_compute_inpainting_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_debug_view_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_disocclusion_mask_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_game_motion_vector_field_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_inpainting_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_interpolation_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_optical_flow_vector_field_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_reconstruct_and_dilate_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_reconstruct_previous_depth_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_fi_setup_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_compute_luminance_pyramid_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_compute_optical_flow_advanced_v5_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_compute_scd_divergence_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_filter_optical_flow_v5_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_generate_scd_histogram_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_prepare_luma_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/fsr3/ffx3_of_scale_optical_flow_advanced_v5_pass.glsl.gen.h"

// clang-format off
// The frame interpolation component; optical flow is compiled in ffx3_opticalflow_lib.cpp (same static names).
FFX3_LIBRARY_BEGIN
namespace FFX3 {
#include "thirdparty/amd-fsr3/ffx_opticalflow.h"
#include "thirdparty/amd-fsr3/ffx_frameinterpolation.cpp"
} // namespace FFX3
FFX3_LIBRARY_END
// clang-format on

using namespace RendererRD;
using namespace FFX3;

namespace RendererRD {
void ffx3_opticalflow_fill_layouts(FFX3Pass *r_passes); // ffx3_opticalflow_lib.cpp
String ffx3_opticalflow_check_layouts();
} // namespace RendererRD

struct FSR3FrameGenerationEffect::Device {
	FFX3Device base;
	FFX3Pass fi_passes[FFX_FRAMEINTERPOLATION_PASS_COUNT];
	FFX3Pass of_passes[FFX_OPTICALFLOW_PASS_COUNT];

	struct {
		Ffx3FiReconstructAndDilatePassShaderRD reconstruct_and_dilate;
		Ffx3FiSetupPassShaderRD setup;
		Ffx3FiReconstructPreviousDepthPassShaderRD reconstruct_previous_depth;
		Ffx3FiGameMotionVectorFieldPassShaderRD game_motion_vector_field;
		Ffx3FiOpticalFlowVectorFieldPassShaderRD optical_flow_vector_field;
		Ffx3FiDisocclusionMaskPassShaderRD disocclusion_mask;
		Ffx3FiInterpolationPassShaderRD interpolation;
		Ffx3FiComputeInpaintingPyramidPassShaderRD inpainting_pyramid;
		Ffx3FiInpaintingPassShaderRD inpainting;
		Ffx3FiComputeGameVectorFieldInpaintingPyramidPassShaderRD game_vector_field_inpainting_pyramid;
		Ffx3FiDebugViewPassShaderRD debug_view;

		Ffx3OfPrepareLumaPassShaderRD prepare_luma;
		Ffx3OfComputeLuminancePyramidPassShaderRD luminance_pyramid;
		Ffx3OfGenerateScdHistogramPassShaderRD scd_histogram;
		Ffx3OfComputeScdDivergencePassShaderRD scd_divergence;
		Ffx3OfComputeOpticalFlowAdvancedV5PassShaderRD optical_flow;
		Ffx3OfFilterOpticalFlowV5PassShaderRD filter_optical_flow;
		Ffx3OfScaleOpticalFlowAdvancedV5PassShaderRD scale_optical_flow;
	} shaders;
};

namespace RendererRD {

class FSR3FrameGenerationContext {
public:
	FFX3Context of_backend;
	FFX3Context fi_backend;
	FfxOpticalflowContextDescription of_desc = {};
	FfxFrameInterpolationContextDescription fi_desc = {};
	FfxOpticalflowContext *of_context = nullptr;
	FfxFrameInterpolationContext *fi_context = nullptr;
	bool of_created = false;
	bool fi_created = false;
	RID optical_flow_vector; // Shared by optical flow (writes) and interpolation (reads).
	RID optical_flow_scd;
	Size2i optical_flow_size;
	Size2i display_size;
	Size2i max_render_size;
	uint64_t frame_id = 0;

	~FSR3FrameGenerationContext() {
		if (fi_created) {
			ffxFrameInterpolationContextDestroy(fi_context);
		}
		if (of_created) {
			ffxOpticalflowContextDestroy(of_context);
		}
		if (fi_context) {
			memfree(fi_context);
		}
		if (of_context) {
			memfree(of_context);
		}
		fi_backend.release();
		of_backend.release();
		RenderingDevice *rd = RD::get_singleton();
		for (RID rid : { optical_flow_vector, optical_flow_scd }) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	}
};

} // namespace RendererRD

// Hand-written layouts of thirdparty/amd-fsr3/shaders/ffx_frameinterpolation_*_pass.glsl
// (FFX_FRAMEINTERPOLATION_BIND_* defines) under the names of the component's binding tables (ffx_frameinterpolation.cpp);
// see fsr3_upscaler.cpp. Generated from the GLSL by engine/fsr3/gen_layouts.py.
static void fsr3_fi_fill_layouts(FFX3Pass *r_passes) {
	// ffx_frameinterpolation_reconstruct_and_dilate_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE], false,
			{ { 0, L"r_input_motion_vectors" }, { 1, L"r_input_depth" } },
			{ { 2, L"rw_reconstructed_depth_previous_frame" }, { 3, L"rw_dilated_motion_vectors" }, { 4, L"rw_dilated_depth" } },
			{},
			{},
			{ { 5, L"cbFI" } });
	// ffx_frameinterpolation_setup_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_SETUP], false,
			{ { 0, L"r_optical_flow_scd" } },
			{ { 1, L"rw_game_motion_vector_field_x" }, { 2, L"rw_game_motion_vector_field_y" }, { 3, L"rw_optical_flow_motion_vector_field_x" }, { 4, L"rw_optical_flow_motion_vector_field_y" }, { 5, L"rw_disocclusion_mask" } },
			{},
			{ { 6, L"rw_counters" } },
			{ { 7, L"cbFI" } });
	// ffx_frameinterpolation_reconstruct_previous_depth_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH], false,
			{ { 0, L"r_dilated_motion_vectors" }, { 1, L"r_dilated_depth" }, { 2, L"r_current_interpolation_source" }, { 3, L"r_input_distortion_field" } },
			{ { 4, L"rw_reconstructed_depth_interpolated_frame" } },
			{},
			{},
			{ { 5, L"cbFI" } });
	// ffx_frameinterpolation_game_motion_vector_field_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD], false,
			{ { 0, L"r_dilated_motion_vectors" }, { 1, L"r_dilated_depth" }, { 2, L"r_previous_interpolation_source" }, { 3, L"r_current_interpolation_source" }, { 4, L"r_input_distortion_field" } },
			{ { 5, L"rw_game_motion_vector_field_x" }, { 6, L"rw_game_motion_vector_field_y" } },
			{},
			{},
			{ { 7, L"cbFI" } });
	// ffx_frameinterpolation_optical_flow_vector_field_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD], false,
			{ { 0, L"r_optical_flow" }, { 1, L"r_optical_flow_confidence" }, { 2, L"r_dilated_depth" }, { 3, L"r_previous_interpolation_source" }, { 4, L"r_current_interpolation_source" } },
			{ { 6, L"rw_optical_flow_motion_vector_field_x" }, { 7, L"rw_optical_flow_motion_vector_field_y" } },
			{},
			{},
			{ { 8, L"cbFI" } });
	// ffx_frameinterpolation_disocclusion_mask_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK], false,
			{ { 0, L"r_game_motion_vector_field_x" }, { 1, L"r_game_motion_vector_field_y" }, { 2, L"r_reconstructed_depth_previous_frame" }, { 3, L"r_dilated_depth" }, { 4, L"r_reconstructed_depth_interpolated_frame" }, { 5, L"r_inpainting_pyramid" }, { 6, L"r_input_distortion_field" } },
			{ { 7, L"rw_disocclusion_mask" } },
			{},
			{},
			{ { 8, L"cbFI" } });
	// ffx_frameinterpolation_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION], false,
			{ { 0, L"r_game_motion_vector_field_x" }, { 1, L"r_game_motion_vector_field_y" }, { 2, L"r_optical_flow_motion_vector_field_x" }, { 3, L"r_optical_flow_motion_vector_field_y" }, { 4, L"r_previous_interpolation_source" }, { 5, L"r_current_interpolation_source" }, { 6, L"r_disocclusion_mask" }, { 7, L"r_inpainting_pyramid" } },
			{ { 9, L"rw_output" } },
			{ { 8, L"r_counters" } },
			{},
			{ { 10, L"cbFI" } });
	// ffx_frameinterpolation_compute_inpainting_pyramid_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID], false,
			{ { 0, L"r_output" } },
			{ { 2, L"rw_inpainting_pyramid0" }, { 3, L"rw_inpainting_pyramid1" }, { 4, L"rw_inpainting_pyramid2" }, { 5, L"rw_inpainting_pyramid3" }, { 6, L"rw_inpainting_pyramid4" }, { 7, L"rw_inpainting_pyramid5" }, { 8, L"rw_inpainting_pyramid6" }, { 9, L"rw_inpainting_pyramid7" }, { 10, L"rw_inpainting_pyramid8" }, { 11, L"rw_inpainting_pyramid9" }, { 12, L"rw_inpainting_pyramid10" }, { 13, L"rw_inpainting_pyramid11" }, { 14, L"rw_inpainting_pyramid12" } },
			{},
			{ { 1, L"rw_counters" } },
			{ { 15, L"cbFI" }, { 16, L"cbInpaintingPyramid" } });
	// ffx_frameinterpolation_inpainting_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_INPAINTING], false,
			{ { 0, L"r_optical_flow_scd" }, { 1, L"r_inpainting_pyramid" }, { 2, L"r_present_backbuffer" }, { 3, L"r_current_interpolation_source" } },
			{ { 7, L"rw_output" } },
			{},
			{},
			{ { 8, L"cbFI" } });
	// ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID], false,
			{ { 0, L"r_game_motion_vector_field_x" }, { 1, L"r_game_motion_vector_field_y" } },
			{ { 3, L"rw_inpainting_pyramid0" }, { 4, L"rw_inpainting_pyramid1" }, { 5, L"rw_inpainting_pyramid2" }, { 6, L"rw_inpainting_pyramid3" }, { 7, L"rw_inpainting_pyramid4" }, { 8, L"rw_inpainting_pyramid5" }, { 9, L"rw_inpainting_pyramid6" }, { 10, L"rw_inpainting_pyramid7" }, { 11, L"rw_inpainting_pyramid8" }, { 12, L"rw_inpainting_pyramid9" }, { 13, L"rw_inpainting_pyramid10" }, { 14, L"rw_inpainting_pyramid11" }, { 15, L"rw_inpainting_pyramid12" } },
			{},
			{ { 2, L"rw_counters" } },
			{ { 16, L"cbFI" }, { 17, L"cbInpaintingPyramid" } });
	// ffx_frameinterpolation_debug_view_pass.glsl
	ffx3_layout(r_passes[FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW], false,
			{ { 0, L"r_game_motion_vector_field_x" }, { 1, L"r_game_motion_vector_field_y" }, { 2, L"r_optical_flow_motion_vector_field_x" }, { 3, L"r_optical_flow_motion_vector_field_y" }, { 4, L"r_disocclusion_mask" }, { 5, L"r_present_backbuffer" }, { 6, L"r_inpainting_pyramid" }, { 7, L"r_current_interpolation_source" }, { 8, L"r_input_distortion_field" } },
			{ { 9, L"rw_output" } },
			{},
			{},
			{ { 10, L"cbFI" } });
}

static String fsr3_fi_check_layouts() {
	FFX3Effect effect;
	FFX3Pass passes[FFX_FRAMEINTERPOLATION_PASS_COUNT];
	fsr3_fi_fill_layouts(passes);
	effect.passes = passes;
	effect.pass_count = FFX_FRAMEINTERPOLATION_PASS_COUNT;
	LocalVector<const wchar_t *> srv, uav, cb;
	for (const ResourceBinding &b : srvResourceBindingTable) {
		srv.push_back(b.name);
	}
	for (const ResourceBinding &b : uavResourceBindingTable) {
		uav.push_back(b.name);
	}
	for (const ResourceBinding &b : cbResourceBindingTable) {
		cb.push_back(b.name);
	}
	return ffx3_check_layout_names(effect, srv.ptr(), srv.size(), uav.ptr(), uav.size(), cb.ptr(), cb.size());
}

static_assert(sizeof(FrameInterpolationConstants) <= FFX3Context::UBO_SIZE);

// Effect.

FSR3FrameGenerationEffect::FSR3FrameGenerationEffect() {
	device = memnew(Device);

	Vector<String> modes_single;
	modes_single.push_back("");
	const String fi_defines =
			"\n#define FFX_GPU\n"
			"\n#define FFX_GLSL 1\n"
			"\n#define FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH 1\n"
			"\n#define FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS 1\n";
	const String of_defines =
			"\n#define FFX_GPU\n"
			"\n#define FFX_GLSL 1\n";

	fsr3_fi_fill_layouts(device->fi_passes);
	ffx3_opticalflow_fill_layouts(device->of_passes);
	auto shader = [&](FFX3Pass &r_pass, ShaderRD *p_shader, const String &p_defines) {
		r_pass.shader = p_shader;
		p_shader->initialize(modes_single, p_defines);
		r_pass.shader_version = p_shader->version_create();
	};
	FFX3Pass *fi = device->fi_passes;
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE], &device->shaders.reconstruct_and_dilate, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_SETUP], &device->shaders.setup, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH], &device->shaders.reconstruct_previous_depth, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD], &device->shaders.game_motion_vector_field, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD], &device->shaders.optical_flow_vector_field, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK], &device->shaders.disocclusion_mask, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION], &device->shaders.interpolation, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID], &device->shaders.inpainting_pyramid, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_INPAINTING], &device->shaders.inpainting, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID], &device->shaders.game_vector_field_inpainting_pyramid, fi_defines);
	shader(fi[FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW], &device->shaders.debug_view, fi_defines);
	FFX3Pass *of = device->of_passes;
	shader(of[FFX_OPTICALFLOW_PASS_PREPARE_LUMA], &device->shaders.prepare_luma, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID], &device->shaders.luminance_pyramid, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM], &device->shaders.scd_histogram, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE], &device->shaders.scd_divergence, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5], &device->shaders.optical_flow, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5], &device->shaders.filter_optical_flow, of_defines);
	shader(of[FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5], &device->shaders.scale_optical_flow, of_defines);

	FFX3Effect fi_effect;
	fi_effect.passes = device->fi_passes;
	fi_effect.pass_count = FFX_FRAMEINTERPOLATION_PASS_COUNT;
	fi_effect.samplers.push_back({ 1000, true }); // s_LinearClamp
	device->base.effects[FFX_EFFECT_FRAMEINTERPOLATION] = fi_effect;

	FFX3Effect of_effect;
	of_effect.passes = device->of_passes;
	of_effect.pass_count = FFX_OPTICALFLOW_PASS_COUNT;
	device->base.effects[FFX_EFFECT_OPTICALFLOW] = of_effect;
}

FSR3FrameGenerationEffect::~FSR3FrameGenerationEffect() {
	device->base.free_shaders(); // Before the ShaderRD members go.
	memdelete(device);
}

FSR3FrameGenerationContext *FSR3FrameGenerationEffect::create_context(Size2i p_display_size, Size2i p_max_render_size) {
	ERR_FAIL_COND_V(p_display_size.x <= 0 || p_display_size.y <= 0 || p_max_render_size.x <= 0 || p_max_render_size.y <= 0, nullptr);
	RenderingDevice *rd = RD::get_singleton();

	FSR3FrameGenerationContext *context = memnew(FSR3FrameGenerationContext);
	context->display_size = p_display_size;
	context->max_render_size = p_max_render_size;
	context->of_backend.device = &device->base;
	context->fi_backend.device = &device->base;
	context->of_context = reinterpret_cast<FfxOpticalflowContext *>(memalloc(sizeof(FfxOpticalflowContext)));
	context->fi_context = reinterpret_cast<FfxFrameInterpolationContext *>(memalloc(sizeof(FfxFrameInterpolationContext)));

	// Optical flow on the display-size HUD-less image.
	FfxOpticalflowContextDescription &of_desc = context->of_desc;
	context->of_backend.setup_interface(of_desc.backendInterface, FFX_EFFECT_OPTICALFLOW);
	of_desc.flags = 0;
	of_desc.resolution = { uint32_t(p_display_size.x), uint32_t(p_display_size.y) };
	FfxErrorCode result = ffxOpticalflowContextCreate(context->of_context, &of_desc);
	if (result != FFX_OK) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, vformat("FSR 3 frame generation: optical flow context creation failed (error 0x%x).", uint32_t(result)));
	}
	context->of_created = true;

	// Its outputs, owned by the caller and read by the interpolation.
	FfxOpticalflowSharedResourceDescriptions of_shared = {};
	result = ffxOpticalflowGetSharedResourceDescriptions(context->of_context, &of_shared);
	if (result != FFX_OK) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "FSR 3 frame generation: no optical flow resource descriptions.");
	}
	auto make = [&](const FfxCreateResourceDescription &p_desc) {
		RD::TextureFormat tf;
		tf.format = FFX3Context::to_rd_format(p_desc.resourceDescription.format);
		tf.width = MAX(p_desc.resourceDescription.width, 1u);
		tf.height = MAX(p_desc.resourceDescription.height, 1u);
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		ERR_FAIL_COND_V(tf.format == RD::DATA_FORMAT_MAX, RID());
		RID rid = rd->texture_create(tf, RD::TextureView());
		if (rid.is_valid() && p_desc.name) {
			rd->set_resource_name(rid, String(p_desc.name));
		}
		return rid;
	};
	context->optical_flow_vector = make(of_shared.opticalFlowVector);
	context->optical_flow_scd = make(of_shared.opticalFlowSCD);
	context->optical_flow_size = Size2i(of_shared.opticalFlowVector.resourceDescription.width, of_shared.opticalFlowVector.resourceDescription.height);
	if (context->optical_flow_vector.is_null() || context->optical_flow_scd.is_null()) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "FSR 3 frame generation: couldn't create the optical flow textures.");
	}

	// Interpolation: inverted depth, render-size (dilated) motion vectors of the FSR 3.1 upscaler.
	FfxFrameInterpolationContextDescription &fi_desc = context->fi_desc;
	context->fi_backend.setup_interface(fi_desc.backendInterface, FFX_EFFECT_FRAMEINTERPOLATION);
	fi_desc.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
	fi_desc.maxRenderSize = { uint32_t(p_max_render_size.x), uint32_t(p_max_render_size.y) };
	fi_desc.displaySize = { uint32_t(p_display_size.x), uint32_t(p_display_size.y) };
	fi_desc.backBufferFormat = FFX3Context::to_ffx_format(FORMAT);
	fi_desc.previousInterpolationSourceFormat = FFX3Context::to_ffx_format(FORMAT);
	result = ffxFrameInterpolationContextCreate(context->fi_context, &fi_desc);
	if (result != FFX_OK) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, vformat("FSR 3 frame generation: interpolation context creation failed (error 0x%x).", uint32_t(result)));
	}
	context->fi_created = true;
	return context;
}

void FSR3FrameGenerationEffect::free_context(FSR3FrameGenerationContext *p_context) {
	if (p_context) {
		memdelete(p_context);
	}
}

Size2i FSR3FrameGenerationEffect::get_context_display_size(const FSR3FrameGenerationContext *p_context) {
	return p_context ? p_context->display_size : Size2i();
}

Size2i FSR3FrameGenerationEffect::get_context_max_render_size(const FSR3FrameGenerationContext *p_context) {
	return p_context ? p_context->max_render_size : Size2i();
}

bool FSR3FrameGenerationEffect::dispatch(const Parameters &p_params) {
	ERR_FAIL_NULL_V(p_params.context, false);
	ERR_FAIL_NULL_V(p_params.surfaces, false);
	FSR3FrameGenerationContext &ctx = *p_params.context;
	const FSR3UpscalerEffect::SharedSurfaces &s = *p_params.surfaces;
	ERR_FAIL_COND_V(s.render_size.x > ctx.max_render_size.x || s.render_size.y > ctx.max_render_size.y, false);

	// FfxResource keeps pointers to the RIDs until the dispatches return.
	RID hudless = p_params.hudless;
	RID final_color = p_params.final_color;
	RID output = p_params.output;
	RID dilated_depth = s.dilated_depth;
	RID dilated_motion_vectors = s.dilated_motion_vectors;
	RID reconstructed_prev_depth = s.reconstructed_prev_nearest_depth;
	const bool reset = p_params.reset || s.reset;

	FfxOpticalflowDispatchDescription of = {};
	of.commandList = &ctx;
	of.color = FFX3Context::wrap_texture(&hudless, L"hudless");
	of.opticalFlowVector = FFX3Context::wrap_texture(&ctx.optical_flow_vector, L"optical_flow_vector");
	of.opticalFlowSCD = FFX3Context::wrap_texture(&ctx.optical_flow_scd, L"optical_flow_scd");
	of.reset = reset;
	of.backbufferTransferFunction = int(p_params.transfer_function);
	of.minMaxLuminance = { p_params.min_luminance, p_params.max_luminance };
	FfxErrorCode result = ffxOpticalflowContextDispatch(ctx.of_context, &of);
	ERR_FAIL_COND_V_MSG(result != FFX_OK, false, vformat("FSR 3 frame generation: optical flow failed (error 0x%x).", uint32_t(result)));

	FfxFrameInterpolationDispatchDescription fi = {};
	fi.flags = 0;
	fi.commandList = &ctx;
	fi.displaySize = { uint32_t(ctx.display_size.x), uint32_t(ctx.display_size.y) };
	fi.renderSize = { uint32_t(s.render_size.x), uint32_t(s.render_size.y) };
	fi.currentBackBuffer = FFX3Context::wrap_texture(&final_color, L"final");
	fi.currentBackBuffer_HUDLess = FFX3Context::wrap_texture(&hudless, L"hudless");
	fi.output = FFX3Context::wrap_texture(&output, L"output");
	fi.interpolationRect = { 0, 0, ctx.display_size.x, ctx.display_size.y };
	fi.opticalFlowVector = FFX3Context::wrap_texture(&ctx.optical_flow_vector, L"optical_flow_vector");
	fi.opticalFlowSceneChangeDetection = FFX3Context::wrap_texture(&ctx.optical_flow_scd, L"optical_flow_scd");
	fi.opticalFlowBufferSize = { uint32_t(ctx.optical_flow_size.x), uint32_t(ctx.optical_flow_size.y) };
	fi.opticalFlowScale = { 1.0f / ctx.display_size.x, 1.0f / ctx.display_size.y };
	fi.opticalFlowBlockSize = 8;
	fi.cameraNear = s.z_near;
	fi.cameraFar = s.z_far;
	fi.cameraFovAngleVertical = s.fovy;
	fi.viewSpaceToMetersFactor = 1.0f;
	fi.frameTimeDelta = s.delta_time * 1000.0f;
	fi.reset = reset;
	fi.backBufferTransferFunction = FfxBackbufferTransferFunction(p_params.transfer_function);
	fi.minMaxLuminance[0] = p_params.min_luminance;
	fi.minMaxLuminance[1] = p_params.max_luminance;
	fi.frameID = ++ctx.frame_id;
	fi.dilatedDepth = FFX3Context::wrap_texture(&dilated_depth, L"dilated_depth");
	fi.dilatedMotionVectors = FFX3Context::wrap_texture(&dilated_motion_vectors, L"dilated_motion_vectors");
	fi.reconstructedPrevDepth = FFX3Context::wrap_texture(&reconstructed_prev_depth, L"reconstructed_prev_depth");
	fi.distortionField = {};
	result = ffxFrameInterpolationDispatch(ctx.fi_context, &fi);
	ERR_FAIL_COND_V_MSG(result != FFX_OK, false, vformat("FSR 3 frame generation: interpolation failed (error 0x%x).", uint32_t(result)));
	return true;
}

uint32_t FSR3FrameGenerationEffect::get_frame_interpolation_version() {
	return ffxFrameInterpolationGetEffectVersion();
}

uint32_t FSR3FrameGenerationEffect::get_optical_flow_version() {
	return ffxOpticalflowGetEffectVersion();
}

String FSR3FrameGenerationEffect::check_binding_layouts() {
	return fsr3_fi_check_layouts() + ffx3_opticalflow_check_layouts();
}
