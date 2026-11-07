/**************************************************************************/
/*  gtao.h                                                                */
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

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/gtao.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner E18: ground truth ambient occlusion with bent normals (Environment.ssao_gtao), ported from Spartan Engine
// (MIT, commit cb258a150), see shaders/effects/gtao.glsl.
class GTAO {
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		float size[2];
		float radius;
		float intensity;
		uint32_t directions;
		uint32_t steps;
		uint32_t frame;
		uint32_t mode;
		uint32_t history_valid;
		uint32_t use_velocity;
		float sharpness;
		float thickness; // Mariner F1: 0 = horizon-based.
	};
	static_assert(sizeof(Params) == 176);

	GtaoShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffers[2];

public:
	struct Settings {
		Projection projection; // As rasterized (depth correction, jitter).
		Basis view_to_world;
		float radius = 1.0;
		float intensity = 1.0; // Exponent of the visibility.
		float sharpness = 0.98;
		float thickness = 0.5; // Mariner F1: assumed thickness of what is on screen [m]; 0 = horizon-based GTAO.
		int quality = 2; // RSE::EnvironmentSSAOQuality.
		uint64_t frame = 0;
		bool camera_cut = false; // Throw the history away.
	};

	// Slices and horizon steps per pixel for an SSAO quality level.
	static void quality_samples(int p_quality, uint32_t &r_directions, uint32_t &r_steps);

	GTAO();
	~GTAO();

	// Writes p_final (RGBA8: world bent normal * 0.5 + 0.5, visibility) from the depth and normal-roughness buffers;
	// p_velocity may be null (no motion vectors this frame).
	void process(RenderSceneBuffersRD *p_buffers, RID p_depth, RID p_normal_roughness, RID p_velocity, RID p_final, const Size2i &p_size, const Settings &p_settings);
};

} // namespace RendererRD
