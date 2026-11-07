/**************************************************************************/
/*  contact_shadows.h                                                     */
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

#include "servers/rendering/renderer_rd/shaders/effects/contact_shadows.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Mariner E11: screen space contact shadows of one directional light (compute over the depth pre-pass).
class ContactShadows {
	struct Params {
		float projection[16];
		float inv_projection[16];
		float light_length[4];
		float screen_steps[4];
		float fade[4];
	};

	ContactShadowsShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffer;

public:
	struct Settings {
		Projection projection; // As used for the depth buffer (depth correction, jitter).
		Vector3 light_direction; // View space, towards the light.
		float length = 0.0; // Meters.
		int steps = 16;
		float thickness = 0.02; // Fraction of the view distance.
		float min_thickness = 0.02; // Meters.
		float fade_from = 40.0; // Meters.
		float fade_to = 60.0;
		uint64_t frame = 0;
	};

	ContactShadows();
	~ContactShadows();

	// p_depth: the depth buffer (sampled), p_mask: R8 storage image of the same size.
	void process(RID p_depth, RID p_mask, const Size2i &p_size, const Settings &p_settings);
};

} // namespace RendererRD
