/**************************************************************************/
/*  gather_dof.h                                                          */
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

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/gather_dof.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner F2: depth of field by gathering with two layers and CoC tiles (gather_dof.glsl). Forward+, one view;
// rendering/camera/depth_of_field/depth_of_field_mode = Gather. The CoC, shape and quality settings are Godot's.
class GatherDOF {
public:
	enum Mode {
		MODE_SETUP,
		MODE_TILES,
		MODE_DILATE,
		MODE_GATHER,
		MODE_POSTFILTER,
		MODE_COMPOSITE,
		MODE_MAX
	};

	struct PushConstant {
		int32_t full_size[2];
		int32_t half_size[2];

		int32_t tile_count[2];
		float z_near;
		float z_far;

		uint32_t flags;
		float near_begin;
		float near_end;
		float far_begin;

		float far_end;
		float blur_size;
		float blur_size_near;
		float blur_size_far;

		float max_radius;
		uint32_t rings;
		uint32_t sides;
		float jitter;
	};
	static_assert(sizeof(PushConstant) == 80);

	enum {
		FLAG_ORTHOGONAL = 1,
		FLAG_NEAR = 2,
		FLAG_FAR = 4,
		FLAG_PHYSICAL_NEAR = 8,
		FLAG_PHYSICAL_FAR = 16,
	};

	static constexpr int TILE = 8;

	// Rings of the gather by DOF quality (very low .. high): 1 + 8 + 16 + ... samples.
	static uint32_t rings_for_quality(int p_quality);
	// Sides of the aperture by the bokeh shape (box 4, hexagon 6, circle 0).
	static uint32_t sides_for_shape(int p_shape);

private:
	GatherDofShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];

public:
	GatherDOF();
	~GatherDOF();

	// Blurs p_color (storage, RGBA16F, p_color_size) in place; p_depth — the depth buffer (any size).
	void process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_depth, RID p_camera_attributes, float p_z_near, float p_z_far, bool p_orthogonal, uint64_t p_frame);
};

} // namespace RendererRD
