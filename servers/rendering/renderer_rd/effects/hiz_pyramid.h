/**************************************************************************/
/*  hiz_pyramid.h                                                         */
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

#include "servers/rendering/renderer_rd/shaders/effects/hiz_pyramid.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Mariner E13b: builds the depth pyramid of a frame (RenderSceneBuffersRD::get_hiz()).
class HiZPyramid {
	struct PushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
	};

	enum Mode {
		MODE_FROM_DEPTH,
		MODE_REDUCE,
		MODE_MAX,
	};

	static HiZPyramid *singleton;

	HizPyramidShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

public:
	static HiZPyramid *get_singleton() { return singleton; }

	HiZPyramid();
	~HiZPyramid();

	// p_depth: the depth (sampled) of one view; p_levels: the views of each mipmap of that view's layer (RG32F
	// storage), the first of the depth's size, each next half of the previous (rounded down, at least 1).
	void process(RID p_depth, const Vector<RID> &p_levels, const Vector<Size2i> &p_sizes);
};

} // namespace RendererRD
