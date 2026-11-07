/**************************************************************************/
/*  hiz_occlusion_depth.h                                                 */
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

#include "servers/rendering/renderer_rd/shaders/effects/hiz_occlusion_depth.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Mariner E13: reduces the depth buffer to the grid of distances that RendererSceneHiZOcclusion reprojects.
class HiZOcclusionDepth {
	struct PushConstant {
		float inv_projection[16];
		int32_t depth_size[2];
		int32_t cell_count[2];
		uint32_t flip_y;
		uint32_t pad[3];
	};
	static_assert(sizeof(PushConstant) == 96);

	HizOcclusionDepthShaderRD shader;
	RID shader_version;
	RID pipeline;

public:
	HiZOcclusionDepth();
	~HiZOcclusionDepth();

	// p_depth: the depth buffer (sampled, p_depth_size), p_projection: as used for it (depth correction and jitter
	// included). Writes p_cell_count floats per row into p_cells (storage buffer), row 0 at the bottom of the view.
	void process(RID p_depth, const Size2i &p_depth_size, const Projection &p_projection, RID p_cells, const Size2i &p_cell_count);
};

} // namespace RendererRD
