/**************************************************************************/
/*  fsr3_upscaler.h                                                       */
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

#include "core/math/projection.h"
#include "servers/rendering/rendering_device.h"

// AMD FidelityFX Super Resolution 3.1 upscaler (FidelityFX SDK 1.1.4, MIT, see thirdparty/amd-fsr3).
//
// The library and its types live in fsr3_upscaler.cpp only, inside "namespace FFX3": FSR 2 (thirdparty/amd-fsr2)
// declares types and macros with the same names, so nothing from the FSR 3 headers may leak into this header.

namespace RendererRD {

class FSR3UpscalerContext;

class FSR3UpscalerEffect {
public:
	struct Device;

	struct Parameters {
		FSR3UpscalerContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID exposure;
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		Vector2 jitter;
		float delta_time = 0.0f; // Seconds.
		float sharpness = 0.0f; // 0 = off, 1 = maximum.
		bool reset_accumulation = false;
		Projection reprojection;
	};

	// What the last dispatch of a context left for FSR 3 frame generation: the upscaler's shared surfaces (render
	// size) and the camera of that frame.
	struct SharedSurfaces {
		RID dilated_depth; // R32F
		RID dilated_motion_vectors; // RG16F, UV units
		RID reconstructed_prev_nearest_depth; // R32UI
		bool valid = false;
		Size2i render_size;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		float delta_time = 0.0f;
		bool reset = false;
		uint64_t frame = 0; // RenderingServer frame number of the dispatch.
	};

	FSR3UpscalerEffect();
	~FSR3UpscalerEffect();

	FSR3UpscalerContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	static void free_context(FSR3UpscalerContext *p_context);
	static const SharedSurfaces *get_shared_surfaces(const FSR3UpscalerContext *p_context);
	void upscale(const Parameters &p_params);

	// Version of the vendored FidelityFX SDK, as FFX_SDK_MAKE_VERSION() packs it.
	static uint32_t get_sdk_version();
	// Empty if every binding name of the hand-written pass layouts is known to the library, else the mismatches.
	static String check_binding_layouts();
	// Jitter sequence length the upscaler expects for the given render / display widths.
	static int32_t get_jitter_phase_count(int32_t p_render_width, int32_t p_display_width);

private:
	Device *device = nullptr;
};

} // namespace RendererRD
