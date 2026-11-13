#[compute]

#version 450

#VERSION_DEFINES

// Mariner G1a: heights of the overhead map (Environment.overhead_map_*). The depth pass from above writes the
// normalized height (0 at the bottom of the box - also where nothing was drawn -, 1 at the top); COPY puts it into
// mipmap 0 of an RG16 UNORM texture, DOWNSAMPLE makes the next mipmap from 2x2: r the average (how much of the
// footprint is covered, for roofs and decks), g the maximum (walls and posts do not thin out). The maximum is dilated by
// a texel at every level (3x3 in COPY, the 2x2 block plus its ring in DOWNSAMPLE): a wall one texel thin otherwise
// lost half its height to the bilinear filter between it and the empty texels next to it. The sky occlusion samples
// far rings from the coarse mipmaps.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 size; // Of the destination.
	ivec2 pad;
}
params;

#ifdef MODE_COPY
layout(set = 0, binding = 0) uniform sampler2D source_depth;
#else
layout(rg16, set = 0, binding = 0) uniform restrict readonly image2D source_height;
#endif
layout(rg16, set = 0, binding = 1) uniform restrict writeonly image2D dest_height;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
#ifdef MODE_COPY
	ivec2 src_size = textureSize(source_depth, 0);
	float d = clamp(texelFetch(source_depth, pos, 0).r, 0.0, 1.0);
	float m = d;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			m = max(m, texelFetch(source_depth, clamp(pos + ivec2(x, y), ivec2(0), src_size - 1), 0).r);
		}
	}
	vec2 h = vec2(d, clamp(m, 0.0, 1.0));
#else
	ivec2 src_size = imageSize(source_height);
	ivec2 p = pos * 2;
	ivec2 q = min(p + 1, src_size - 1);
	float avg = 0.25 * (imageLoad(source_height, p).r + imageLoad(source_height, ivec2(q.x, p.y)).r + imageLoad(source_height, ivec2(p.x, q.y)).r + imageLoad(source_height, q).r);
	float m = 0.0;
	for (int y = -1; y <= 2; y++) {
		for (int x = -1; x <= 2; x++) {
			m = max(m, imageLoad(source_height, clamp(p + ivec2(x, y), ivec2(0), src_size - 1)).g);
		}
	}
	vec2 h = vec2(avg, m);
#endif
	imageStore(dest_height, pos, vec4(h, 0.0, 0.0));
}
