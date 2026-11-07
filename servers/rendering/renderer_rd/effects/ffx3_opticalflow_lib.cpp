/**************************************************************************/
/*  ffx3_opticalflow_lib.cpp                                              */
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

// Mariner E24b: the FidelityFX optical flow component (FSR 3 frame generation), in its own translation unit: its
// static helpers have the same names as the frame interpolation's.

#include "ffx3_backend.h"

// clang-format off
FFX3_LIBRARY_BEGIN
namespace FFX3 {
#include "thirdparty/amd-fsr3/ffx_opticalflow.cpp"
} // namespace FFX3
FFX3_LIBRARY_END
// clang-format on

using namespace RendererRD;
using namespace FFX3;

namespace RendererRD {

// Hand-written layouts of thirdparty/amd-fsr3/shaders/ffx_opticalflow_*_pass*.glsl (FFX_OPTICALFLOW_BIND_* defines)
// under the names of the component's binding tables (ffx_opticalflow.cpp); see fsr3_upscaler.cpp.
void ffx3_opticalflow_fill_layouts(FFX3Pass *r_passes) {
	// ffx_opticalflow_prepare_luma_pass.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_PREPARE_LUMA], false,
			{ { 0, L"r_input_color" } },
			{ { 1, L"rw_optical_flow_input" } },
			{},
			{},
			{ { 2, L"cbOF" } });
	// ffx_opticalflow_compute_luminance_pyramid_pass.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID], false,
			{},
			{ { 0, L"rw_optical_flow_input" }, { 1, L"rw_optical_flow_input_level_1" }, { 2, L"rw_optical_flow_input_level_2" }, { 3, L"rw_optical_flow_input_level_3" }, { 4, L"rw_optical_flow_input_level_4" }, { 5, L"rw_optical_flow_input_level_5" }, { 6, L"rw_optical_flow_input_level_6" } },
			{},
			{},
			{ { 7, L"cbOF" }, { 8, L"cbOF_SPD" } });
	// ffx_opticalflow_generate_scd_histogram_pass.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM], false,
			{ { 0, L"r_optical_flow_input" } },
			{ { 1, L"rw_optical_flow_scd_histogram" } },
			{},
			{},
			{ { 2, L"cbOF" } });
	// ffx_opticalflow_compute_scd_divergence_pass.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE], false,
			{},
			{ { 0, L"rw_optical_flow_scd_histogram" }, { 1, L"rw_optical_flow_scd_previous_histogram" }, { 2, L"rw_optical_flow_scd_temp" }, { 3, L"rw_optical_flow_scd_output" } },
			{},
			{},
			{ { 4, L"cbOF" } });
	// ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl (FFX_WAVE)
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5], false,
			{ { 0, L"r_optical_flow_input" }, { 1, L"r_optical_flow_previous_input" } },
			{ { 2, L"rw_optical_flow" }, { 3, L"rw_optical_flow_scd_output" } },
			{},
			{},
			{ { 4, L"cbOF" } });
	// ffx_opticalflow_filter_optical_flow_pass_v5.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5], false,
			{ { 0, L"r_optical_flow_previous" } },
			{ { 1, L"rw_optical_flow" } },
			{},
			{},
			{ { 2, L"cbOF" } });
	// ffx_opticalflow_scale_optical_flow_advanced_pass_v5.glsl
	ffx3_layout(r_passes[FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5], false,
			{ { 0, L"r_optical_flow_input" }, { 1, L"r_optical_flow_previous_input" }, { 2, L"r_optical_flow" } },
			{ { 3, L"rw_optical_flow_next_level" }, { 4, L"rw_optical_flow_scd_output" } },
			{},
			{},
			{ { 5, L"cbOF" } });
}

String ffx3_opticalflow_check_layouts() {
	FFX3Effect effect;
	FFX3Pass passes[FFX_OPTICALFLOW_PASS_COUNT];
	ffx3_opticalflow_fill_layouts(passes);
	effect.passes = passes;
	effect.pass_count = FFX_OPTICALFLOW_PASS_COUNT;
	LocalVector<const wchar_t *> srv, uav, cb;
	for (const Binding &b : srvBindingNames) {
		srv.push_back(b.name);
	}
	for (const Binding &b : uavBindingNames) {
		uav.push_back(b.name);
	}
	for (const Binding &b : cbBindingNames) {
		cb.push_back(b.name);
	}
	return ffx3_check_layout_names(effect, srv.ptr(), srv.size(), uav.ptr(), uav.size(), cb.ptr(), cb.size());
}

} // namespace RendererRD
