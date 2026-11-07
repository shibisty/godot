/**************************************************************************/
/*  lens_flare.h                                                          */
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

#include "servers/rendering/renderer_rd/shaders/effects/lens_flare.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner E15b: lens flare (ghosts, halo, sun starburst) and lens dirt (CameraAttributes.lens_*).
class LensFlare {
	struct Params {
		float dest_size[2];
		float source_size[2];
		float sun_uv[2];
		float sun_in_front;
		float sun_radius;
		float sun_color[4];
		float intensity;
		float threshold;
		float ghost_spacing;
		float halo_width;
		float chromatic;
		float dirt_intensity;
		float aspect;
		float streak_count;
		uint32_t mode;
		uint32_t ghosts;
		uint32_t has_dirt;
		uint32_t pad;
	};
	static_assert(sizeof(Params) == 96);

	LensFlareShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffers[6]; // One per pass.
	RID sun_buffer;

public:
	struct Settings {
		float intensity = 0.02;
		float threshold = 4.0;
		int ghosts = 4;
		float ghost_spacing = 0.35;
		float halo_width = 0.45;
		float chromatic = 0.006;
		RID dirt_texture; // RD texture, or none.
		float dirt_intensity = 1.0;
		// The sun: its position in the image (UV), whether it is in front of the camera, its angular radius (UV of the
		// height) and color (linear, energy and streak intensity included).
		bool sun = false;
		Vector2 sun_uv;
		bool sun_in_front = false;
		float sun_radius = 0.01;
		Color sun_color;
		float streak_count = 6;
	};

	LensFlare();
	~LensFlare();

	// Adds the flare to p_color (storage, RGBA16F, p_color_size) in place; p_depth: the depth buffer.
	void process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_depth, const Settings &p_settings);
};

} // namespace RendererRD
