/**************************************************************************/
/*  motion_blur.h                                                         */
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

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/motion_blur.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner E15a: motion blur by the motion vectors (CameraAttributes.motion_blur_*).
class MotionBlur {
	struct Params {
		float reprojection[16];
		float inv_projection[16];
		float color_size[2];
		float tile_count[2];
		float intensity;
		float camera_motion;
		float max_length;
		float tile_size;
		uint32_t mode;
		uint32_t samples;
		uint32_t frame;
		uint32_t pad;
	};
	static_assert(sizeof(Params) == 176);

	MotionBlurShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffers[5]; // One per pass (the passes are recorded before the GPU runs them).

public:
	struct Settings {
		Projection projection; // Current, as rendered (depth correction, jitter).
		Projection reprojection; // Current clip -> previous clip (static world).
		float intensity = 0.5;
		float camera_motion = 1.0;
		float max_length = 0.04; // Fraction of the view height.
		int samples = 12;
		uint64_t frame = 0;
	};

	MotionBlur();
	~MotionBlur();

	// Blurs p_color (storage, RGBA16F, p_color_size) in place by p_velocity / p_depth (internal size).
	void process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_velocity, RID p_depth, const Settings &p_settings);
};

} // namespace RendererRD
