#[compute]

#version 450

#VERSION_DEFINES

// Mariner E13b: the depth pyramid (Hi-Z) of the frame, for effects and GPU culling (RenderSceneBuffersRD.get_hiz()).
// Each texel holds the range of the raw depths it covers: r the farthest (the smallest, reverse Z), g the nearest.
// Mode 0 copies the depth buffer into the first level; mode 1 reduces a level into the next, every source texel
// counted (a level of odd size gives its last row and column to the last texel of the next).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_FROM_DEPTH
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
#else
layout(rg32f, set = 0, binding = 0) uniform restrict readonly image2D source;
#endif

layout(rg32f, set = 1, binding = 0) uniform restrict writeonly image2D dest;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 dest_size;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.dest_size))) {
		return;
	}
#ifdef MODE_FROM_DEPTH
	float depth = texelFetch(depth_buffer, pos, 0).r;
	imageStore(dest, pos, vec4(depth, depth, 0.0, 0.0));
#else
	ivec2 from = pos * 2;
	// The last texel of a level of odd size also takes the source's last row / column.
	ivec2 to = min(from + 1 + ivec2(equal(pos, params.dest_size - 1)) * (params.source_size & 1), params.source_size - 1);
	vec2 range = vec2(1e30, -1e30);
	for (int y = from.y; y <= to.y; y++) {
		for (int x = from.x; x <= to.x; x++) {
			vec2 r = imageLoad(source, ivec2(x, y)).rg;
			range = vec2(min(range.x, r.x), max(range.y, r.y));
		}
	}
	imageStore(dest, pos, vec4(range, 0.0, 0.0));
#endif
}
