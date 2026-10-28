/**************************************************************************/
/*  dlss.h                                                                */
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
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

// NVIDIA DLSS Super Resolution / DLAA (E2) and DLSS Frame Generation (E3) through NVIDIA Streamline.
// Ported from NVIDIA's Godot fork (https://github.com/NVIDIA-RTX/godot, branches nvidia-pt-dlss and
// worktree-dlss-fg-reflex). Ray Reconstruction and NIS sharpening of that fork are not included here.

namespace RendererRD {

class DLSSEffect;
class DLSSContext {
public:
	struct Parameters {
		DLSSContext *context = nullptr;
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
		bool reverse_depth = true;
		Vector2 jitter;
		float delta_time = 0.0f;
		float sharpness = 0.0f;
		char preset = '?';
		bool reset_accumulation = false;
		bool dlss_g = false; // Frame generation requested for this viewport (Viewport.frame_generation).
		Projection reprojection;
		Projection cam_projection;
		Transform3D cam_transform;
	} last_parameters;
	DLSSEffect *last_effect = nullptr;
	bool is_d3d12 = false;
	int delay = 4; // Warmup frames before DLSS evaluates (Vulkan stability workaround).

	virtual ~DLSSContext() {}
};

class DLSSEffect {
public:
	struct Shaders {
		MotionVectorDecodeShaderRD mvec_decode_shader;
		RID mvec_decode_version;
		RID mvec_decode_pipeline;
	} shaders;

	DLSSEffect();
	~DLSSEffect();
	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	void upscale(const DLSSContext::Parameters &p_params);
	bool is_ready(DLSSContext *p_context);

	// DLSS Frame Generation can run (game, DLSS-G + Reflex + PCL available and loaded).
	static bool frame_generation_supported();
	// Tags p_texture (the final 3D image without 2D/UI, same size and colour space as the swap chain) as the
	// HUD-less colour of the frame generation viewport, in render graph order.
	static void frame_generation_tag_hudless(RID p_texture);

private:
	void _upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params);
	static void _upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
	static void _tag_hudless_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
