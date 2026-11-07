/**************************************************************************/
/*  test_mariner_e24.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mariner_e24)

#include "core/object/class_db.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/effects/fsr3_frame_generation.h"
#include "servers/rendering/renderer_rd/effects/fsr3_upscaler.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patches E24a / E24b: AMD FSR 3.1 upscaler (Viewport.SCALING_3D_MODE_FSR3) and FSR 3 frame
// generation (Viewport.frame_generation in that mode), FidelityFX SDK 1.1.4 in thirdparty/amd-fsr3. Here: the new
// scaling mode, the vendored components' versions, the jitter sequence and the hand-written pass binding layouts; the
// images on lavapipe by engine/tests/e24/fsr3.gd (upscaler) and fg.gd (generated frames presented, half-way, UI).

namespace TestMarinerE24 {

TEST_CASE("[Mariner][E24a] FSR 3.1 scaling mode") {
	CHECK(int(Viewport::SCALING_3D_MODE_DLSS) == 6);
	CHECK(int(Viewport::SCALING_3D_MODE_FSR3) == 7);
	CHECK(int(Viewport::SCALING_3D_MODE_MAX) == 8);
	CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_FSR3) == int(Viewport::SCALING_3D_MODE_FSR3));
	CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_MAX) == int(Viewport::SCALING_3D_MODE_MAX));
	CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_FSR3) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);

	bool ok = false;
	CHECK(ClassDB::get_integer_constant("Viewport", "SCALING_3D_MODE_FSR3", &ok) == 7);
	CHECK(ok);
	CHECK(ClassDB::get_integer_constant_enum("Viewport", "SCALING_3D_MODE_FSR3") == StringName("Scaling3DMode"));
	CHECK(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_SCALING_3D_MODE_FSR3", &ok) == 7);
	CHECK(ok);

	PropertyInfo info;
	CHECK(ClassDB::get_property_info("Viewport", "scaling_3d_mode", &info));
	CHECK(info.hint_string.contains("FSR 3.1 (Slow):7"));
}

TEST_CASE("[Mariner][E24a] FSR 3.1 library") {
	// FFX_SDK_MAKE_VERSION(1, 1, 4): the upscaler refuses any other backend version.
	CHECK(RendererRD::FSR3UpscalerEffect::get_sdk_version() == ((1u << 22) | (1u << 12) | 4u));

	// Same jitter sequence length as the viewport's (8 * scale^-2, renderer_viewport.cpp).
	CHECK(RendererRD::FSR3UpscalerEffect::get_jitter_phase_count(1920, 1920) == 8);
	CHECK(RendererRD::FSR3UpscalerEffect::get_jitter_phase_count(1280, 1920) == 18);
	CHECK(RendererRD::FSR3UpscalerEffect::get_jitter_phase_count(960, 1920) == 32);

	const String errors = RendererRD::FSR3UpscalerEffect::check_binding_layouts();
	INFO(errors);
	CHECK(errors.is_empty());
}

TEST_CASE("[Mariner][E24b] FSR 3 frame generation library") {
	// Frame interpolation 1.1.3 and optical flow 1.1.2 of the FidelityFX SDK 1.1.4.
	CHECK(RendererRD::FSR3FrameGenerationEffect::get_frame_interpolation_version() == ((1u << 22) | (1u << 12) | 3u));
	CHECK(RendererRD::FSR3FrameGenerationEffect::get_optical_flow_version() == ((1u << 22) | (1u << 12) | 2u));

	const String errors = RendererRD::FSR3FrameGenerationEffect::check_binding_layouts();
	INFO(errors);
	CHECK(errors.is_empty());

	// Storage-capable and copyable (the interpolation keeps a copy of the HUD-less frame).
	CHECK(RendererRD::FSR3FrameGenerationEffect::FORMAT == RD::DATA_FORMAT_R16G16B16A16_SFLOAT);

	// Statistics for the game and the tests.
	CHECK(ClassDB::has_method("RenderingServer", "get_frame_generation_presented_count"));
	CHECK(ClassDB::has_method("RenderingServer", "get_frame_generation_output_texture"));
}

} // namespace TestMarinerE24
