/**************************************************************************/
/*  fsr3_frame_generation.h                                               */
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

#include "fsr3_upscaler.h"

// AMD FidelityFX Super Resolution 3 frame generation (FidelityFX SDK 1.1.4, MIT, see thirdparty/amd-fsr3): optical
// flow on the HUD-less image, then frame interpolation between the previous and the current frame, with the UI of the
// current frame on top. Works on the FSR 3.1 upscaler's shared surfaces of the same frame. Display-space textures
// only; presenting the interpolated image is RenderingDevice's job (screen_prepare_extra_for_drawing()).
//
// The library and its types live in the FidelityFX translation units (ffx3_*.cpp, fsr3_*.cpp), inside "namespace FFX3".

namespace RendererRD {

class FSR3FrameGenerationContext;

class FSR3FrameGenerationEffect {
public:
	struct Device;

	enum TransferFunction {
		TRANSFER_FUNCTION_SRGB, // UNORM swap chain: sRGB-encoded values.
		TRANSFER_FUNCTION_PQ, // HDR10.
		TRANSFER_FUNCTION_SCRGB, // Linear FP16.
	};

	struct Parameters {
		FSR3FrameGenerationContext *context = nullptr;
		const FSR3UpscalerEffect::SharedSurfaces *surfaces = nullptr; // Of this frame.
		RID hudless; // This frame without the UI, display size, FORMAT.
		RID final_color; // This frame with the UI, display size, FORMAT.
		RID output; // The interpolated frame (between the previous and this one), display size, FORMAT, storage.
		TransferFunction transfer_function = TRANSFER_FUNCTION_SRGB;
		float min_luminance = 0.0f;
		float max_luminance = 1.0f;
		bool reset = false;
	};

	// Format of the display-size textures (hudless, final, output): storage-capable and copyable.
	static constexpr RD::DataFormat FORMAT = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;

	FSR3FrameGenerationEffect();
	~FSR3FrameGenerationEffect();

	FSR3FrameGenerationContext *create_context(Size2i p_display_size, Size2i p_max_render_size);
	static void free_context(FSR3FrameGenerationContext *p_context);
	static Size2i get_context_display_size(const FSR3FrameGenerationContext *p_context);
	static Size2i get_context_max_render_size(const FSR3FrameGenerationContext *p_context);
	// Records optical flow and interpolation; false if nothing was written to the output.
	bool dispatch(const Parameters &p_params);

	// Versions of the vendored components, as FFX_SDK_MAKE_VERSION() packs them.
	static uint32_t get_frame_interpolation_version();
	static uint32_t get_optical_flow_version();
	// Empty if every binding name of the hand-written pass layouts is known to the library, else the mismatches.
	static String check_binding_layouts();

private:
	Device *device = nullptr;
};

} // namespace RendererRD
