/**************************************************************************/
/*  particles_screen_collision.h                                          */
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

#include "core/math/basis.h"
#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/particles_screen_collision.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Mariner E22: the depth and surface normal of a view at half resolution, for the screen space collision of GPU
// particles (read in the next frame by the particles' collision step).
class ParticlesScreenCollision {
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		float source_size[2];
		float dest_size[2];
	};
	static_assert(sizeof(Params) == 144);

	ParticlesScreenCollisionShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffer;

public:
	ParticlesScreenCollision();
	~ParticlesScreenCollision();

	// p_depth: the depth buffer (reverse Z) of p_size; p_dest: RGBA32F storage image, half of p_size (rounded up).
	void process(RID p_depth, const Size2i &p_size, const Projection &p_projection, const Basis &p_view_to_world, RID p_dest, const Size2i &p_dest_size);
};

} // namespace RendererRD
