#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 vertex_base;
	if (gl_VertexIndex == 0) {
		vertex_base = vec2(-1.0, -1.0);
	} else if (gl_VertexIndex == 1) {
		vertex_base = vec2(-1.0, 3.0);
	} else {
		vertex_base = vec2(3.0, -1.0);
	}
	gl_Position = vec4(vertex_base, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// Mariner E23: contrast adaptive sharpening, the no-scaling path of AMD FidelityFX CAS (ffx_cas.h 1.20190610, with
// CAS_BETTER_DIAGONALS and CAS_GO_SLOWER), as used by Spartan Engine (data/shaders/amd_fidelity_fx/cas.hlsl,
// commit cb258a150). Translated from HLSL / ffx_a.h to GLSL for a Godot raster pass; values are normalized by the
// output max value so that HDR output keeps its range.
//
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	float peak; // -1 / mix(8, 5, sharpness) × amount: weight of the 4 neighbours at full contrast headroom.
	float inv_max_value;
}
params;

layout(location = 0) out vec4 frag_color;

vec3 cas_load(ivec2 p) {
	return clamp(texelFetch(source_color, clamp(p, ivec2(0), params.size - 1), 0).rgb * params.inv_max_value, vec3(0.0), vec3(1.0));
}

void main() {
	ivec2 sp = ivec2(gl_FragCoord.xy);

	// a b c
	// d e f
	// g h i
	vec3 a = cas_load(sp + ivec2(-1, -1));
	vec3 b = cas_load(sp + ivec2(0, -1));
	vec3 c = cas_load(sp + ivec2(1, -1));
	vec3 d = cas_load(sp + ivec2(-1, 0));
	vec4 e4 = texelFetch(source_color, sp, 0);
	vec3 e = clamp(e4.rgb * params.inv_max_value, vec3(0.0), vec3(1.0));
	vec3 f = cas_load(sp + ivec2(1, 0));
	vec3 g = cas_load(sp + ivec2(-1, 1));
	vec3 h = cas_load(sp + ivec2(0, 1));
	vec3 i = cas_load(sp + ivec2(1, 1));

	// Soft min and max (cross + full 3x3, 2.0x bigger).
	vec3 mn = min(min(min(d, e), min(f, b)), h);
	vec3 mn2 = min(min(min(mn, a), min(c, g)), i);
	mn = mn + mn2;
	vec3 mx = max(max(max(d, e), max(f, b)), h);
	vec3 mx2 = max(max(max(mx, a), max(c, g)), i);
	mx = mx + mx2;

	// Smooth minimum distance to the signal limit divided by the smooth max.
	vec3 rcp_m = 1.0 / max(mx, vec3(1e-5));
	vec3 amp = clamp(min(mn, 2.0 - mx) * rcp_m, vec3(0.0), vec3(1.0));
	amp = sqrt(amp);

	// Filter shape:
	// 0 w 0
	// w 1 w
	// 0 w 0
	float w = amp.g * params.peak;
	float rcp_weight = 1.0 / (1.0 + 4.0 * w);
	vec3 pix = clamp((b * w + d * w + f * w + h * w + e) * rcp_weight, vec3(0.0), vec3(1.0));

	frag_color = vec4(pix / params.inv_max_value, e4.a);
}
