/**************************************************************************/
/*  cas.h                                                                 */
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

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/cas.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner E23: contrast adaptive sharpening (AMD FidelityFX CAS) of the final image (CameraAttributes.image_sharpening).
class CAS {
	struct Params {
		int32_t size[2];
		float peak;
		float inv_max_value;
	};

	CasShaderRD shader;
	RID shader_version;
	PipelineCacheRD pipeline;

public:
	CAS();
	~CAS();

	// CAS peak weight for an amount (0 = off, 1 = the CAS maximum): -1 / lerp(8, 5, amount), scaled by the amount
	// so that small amounts sharpen little.
	static float peak_for_amount(float p_amount);

	// Sharpens the render target texture p_target (p_size) in place: copies it into a texture of p_buffers, then
	// draws the sharpened image into p_target_framebuffer.
	void process(RenderSceneBuffersRD *p_buffers, RID p_target, RID p_target_framebuffer, const Size2i &p_size, float p_amount, float p_max_value);
};

} // namespace RendererRD
