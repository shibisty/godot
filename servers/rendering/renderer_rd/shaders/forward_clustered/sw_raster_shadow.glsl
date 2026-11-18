#[vertex]

#version 450

#VERSION_DEFINES

// Mariner V5a: merges the software rasterized depth of a shadow pass (sw_raster.glsl, MODE_SHADOW: an R32UI of the
// pass's rectangle, reverse Z float bits, 0 empty) into the shadow map. One triangle over the draw region (the pass's
// rectangle), the depth test keeps the nearer of it and the hardware casters.

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// Read as a sampled texture: a storage image in a fragment shader breaks the command list on Direct3D 12 (Mariner V5a).
layout(set = 0, binding = 0) uniform utexture2D source_depth;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(push_constant, std430) uniform Params {
	ivec2 origin; // The rectangle's corner in the framebuffer.
	ivec2 size;
}
params;

void main() {
	ivec2 p = ivec2(gl_FragCoord.xy) - params.origin;
	if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, params.size))) {
		discard;
	}
	uint d = texelFetch(usampler2D(source_depth, source_sampler), p, 0).r;
	if (d == 0u) {
		discard; // Nothing drawn here.
	}
	gl_FragDepth = uintBitsToFloat(d);
}
