#[compute]

#version 450

#VERSION_DEFINES

// Mariner F2: depth of field by gathering (scatter-as-gather), two layers, tiles of the circle of confusion.
// Own implementation after the published methods: Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014); Abadie, "A Life of a Bokeh" (SIGGRAPH 2018); Sousa, "Graphics Gems from CryENGINE 3" (2013).
// The circle of confusion is Godot's (bokeh_dof.glsl): the same distances, transitions, amount and physical camera.
// Passes (one variant each):
//   SETUP     — half resolution: color and signed CoC in half pixels (near < 0); near wins over the 2x2 block.
//   TILES     — 8x8 half pixel tiles: largest near and far CoC.
//   DILATE    — near CoC spreads to the tiles it reaches (the foreground blurs over what is behind it); far — 1 tile.
//   GATHER    — rings of samples in the aperture shape around each half pixel, radius = the tile's largest CoC:
//               background (far and in focus) counts a sample by min(its CoC, own CoC) — the blurred background does not
//               bleed over sharp nearer things; foreground (near) — premultiplied color and coverage alpha.
//   POSTFILTER — 3x3 median of both layers (removes the ring pattern and lone samples).
//   COMPOSITE — full resolution: sharp color or, under the foreground, the gathered background; the background by own
//               far CoC; the foreground over it.

#define BLOCK_SIZE 8
#define TILE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 full_size;
	ivec2 half_size;

	ivec2 tile_count;
	float z_near;
	float z_far;

	uint flags;
	float near_begin;
	float near_end;
	float far_begin;

	float far_end;
	float blur_size;
	float blur_size_near;
	float blur_size_far;

	float max_radius; // half pixels
	uint rings;
	uint sides; // 0 — circle
	float jitter;
}
params;

#define FLAG_ORTHOGONAL 1u
#define FLAG_NEAR 2u
#define FLAG_FAR 4u
#define FLAG_PHYSICAL_NEAR 8u
#define FLAG_PHYSICAL_FAR 16u

#if defined(MODE_SETUP) || defined(MODE_COMPOSITE)
layout(set = 0, binding = 0) uniform sampler2D depth_texture;

float linear_depth(vec2 uv) {
	float depth = textureLod(depth_texture, uv, 0.0).x * 2.0 - 1.0;
	if ((params.flags & FLAG_ORTHOGONAL) != 0u) {
		return -(depth * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
	}
	return 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
}

// Circle of confusion in full resolution pixels: near < 0, far > 0 (as bokeh_dof.glsl get_blur_size()).
float get_coc(float depth) {
	float coc = 0.0;
	if ((params.flags & FLAG_NEAR) != 0u && depth < params.near_begin) {
		if ((params.flags & FLAG_PHYSICAL_NEAR) != 0u) {
			float d = abs(params.near_begin - depth);
			coc = -(d / max(params.near_begin - d, 1e-4)) * params.blur_size_near;
		} else {
			coc = -(1.0 - smoothstep(params.near_end, params.near_begin, depth)) * params.blur_size;
		}
	} else if ((params.flags & FLAG_FAR) != 0u && depth > params.far_begin) {
		if ((params.flags & FLAG_PHYSICAL_FAR) != 0u) {
			float d = abs(params.far_begin - depth);
			coc = (d / (params.far_begin + d)) * params.blur_size_far;
		} else {
			coc = smoothstep(params.far_begin, params.far_end, depth) * params.blur_size;
		}
	}
	float lim = params.max_radius * 2.0;
	return clamp(coc, -lim, lim);
}
#endif

#ifdef MODE_SETUP
layout(set = 0, binding = 1) uniform sampler2D color_texture;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D half_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.half_size))) {
		return;
	}
	// Weighted 2x2 (as the downsample in the papers): a block on the edge of something sharp takes the color of its
	// blurred pixels only, otherwise the sharp pixels' color spreads in the gather (a halo around things in focus).
	vec3 near_color = vec3(0.0);
	float near_w = 0.0;
	float near_coc = 0.0;
	vec3 far_color = vec3(0.0);
	float far_w = 0.0;
	float far_coc = 0.0;
	for (int i = 0; i < 4; i++) {
		ivec2 p = min(pos * 2 + ivec2(i & 1, i >> 1), params.full_size - 1);
		vec2 uv = (vec2(p) + 0.5) / vec2(params.full_size);
		vec3 c = textureLod(color_texture, uv, 0.0).rgb;
		float coc = get_coc(linear_depth(uv)) * 0.5;
		if (coc < -0.25) {
			near_color += c;
			near_w += 1.0;
			near_coc = min(near_coc, coc);
		} else {
			float w = 0.001 + clamp(coc, 0.0, 1.0);
			far_color += c * w;
			far_w += w;
			far_coc = max(far_coc, coc);
		}
	}
	// near wins: the foreground edge widens, never shrinks
	vec4 v = near_w > 0.0 ? vec4(near_color / near_w, near_coc) : vec4(far_color / far_w, far_coc);
	imageStore(half_image, pos, v);
}
#endif

#ifdef MODE_TILES
layout(set = 0, binding = 0) uniform sampler2D half_texture;
layout(rg16f, set = 0, binding = 1) uniform restrict writeonly image2D tile_image;

void main() {
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(t, params.tile_count))) {
		return;
	}
	float near_max = 0.0;
	float far_max = 0.0;
	for (int y = 0; y < TILE; y++) {
		for (int x = 0; x < TILE; x++) {
			ivec2 p = min(t * TILE + ivec2(x, y), params.half_size - 1);
			float coc = texelFetch(half_texture, p, 0).a;
			near_max = max(near_max, -coc);
			far_max = max(far_max, coc);
		}
	}
	imageStore(tile_image, t, vec4(near_max, far_max, 0.0, 0.0));
}
#endif

#ifdef MODE_DILATE
layout(set = 0, binding = 0) uniform sampler2D tile_texture;
layout(rg16f, set = 0, binding = 1) uniform restrict writeonly image2D dilated_image;

void main() {
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(t, params.tile_count))) {
		return;
	}
	int reach = int(ceil(params.max_radius / float(TILE)));
	float near_max = 0.0;
	float far_max = 0.0;
	for (int y = -reach; y <= reach; y++) {
		for (int x = -reach; x <= reach; x++) {
			ivec2 q = t + ivec2(x, y);
			if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.tile_count))) {
				continue;
			}
			vec2 v = texelFetch(tile_texture, q, 0).xy;
			// nearest distance between the two tiles, half pixels
			float gap = float(max(max(abs(x), abs(y)) - 1, 0) * TILE);
			if (v.x > gap) {
				near_max = max(near_max, v.x);
			}
			if (abs(x) <= 1 && abs(y) <= 1) {
				far_max = max(far_max, v.y);
			}
		}
	}
	imageStore(dilated_image, t, vec4(near_max, far_max, 0.0, 0.0));
}
#endif

#ifdef MODE_GATHER
layout(set = 0, binding = 0) uniform sampler2D half_texture;
layout(set = 0, binding = 1) uniform sampler2D dilated_texture;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D background_image;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D foreground_image;

const float TAU = 6.28318530718;

// Aperture: a regular polygon inscribed in the circle (vertices at radius 1), or the circle.
float aperture(float a) {
	if (params.sides < 3u) {
		return 1.0;
	}
	float seg = TAU / float(params.sides);
	// as the classic bokeh: the box with edges along the axes, the hexagon with vertices up and down
	float rot = params.sides == 4u ? TAU / 8.0 : (params.sides == 6u ? TAU / 12.0 : 0.0);
	float b = mod(a + rot, seg) - seg * 0.5;
	return cos(seg * 0.5) / cos(b);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.half_size))) {
		return;
	}
	vec4 center = texelFetch(half_texture, pos, 0);
	vec2 tile = texelFetch(dilated_texture, pos / TILE, 0).xy;
	float radius = min(max(tile.x, tile.y), params.max_radius);
	if (radius < 0.5) {
		imageStore(background_image, pos, vec4(center.rgb, 1.0));
		imageStore(foreground_image, pos, vec4(0.0));
		return;
	}
	// A near pixel gathers the background as if fully blurred: the estimate of what is behind the foreground
	// (shows through where the blurred foreground is partly transparent, at its silhouette).
	float own_far = center.a < -0.5 ? radius : max(center.a, 0.0);
	vec3 bg_sum = vec3(0.0);
	float bg_w = 0.0;
	vec3 fg_sum = vec3(0.0);
	float fg_w = 0.0;
	float count = 1.0;
	// the center
	if (center.a < -0.5) {
		fg_sum += center.rgb;
		fg_w += 1.0;
	} else {
		bg_sum += center.rgb;
		bg_w += 1.0;
	}
	uint rings = max(params.rings, 1u);
	for (uint k = 1u; k <= rings; k++) {
		uint n = 8u * k;
		float r = radius * float(k) / float(rings);
		for (uint j = 0u; j < n; j++) {
			float a = (float(j) + 0.5 * float(k & 1u) + params.jitter) * TAU / float(n);
			vec2 off = vec2(cos(a), sin(a)) * r * aperture(a);
			// distance in the aperture's shape (the ring radius): a sample reaches here if its CoC covers it, so a
			// highlight becomes the aperture's polygon, not a circle
			float d = r;
			ivec2 p = clamp(pos + ivec2(round(off)), ivec2(0), params.half_size - 1);
			vec4 s = texelFetch(half_texture, p, 0);
			count += 1.0;
			if (s.a < -0.5) {
				// foreground: reaches here if its CoC covers the distance
				float cov = clamp(-s.a - d + 1.0, 0.0, 1.0);
				fg_sum += s.rgb * cov;
				fg_w += cov;
			} else {
				// background and in focus: the smaller of the two CoC — no bleeding over sharper nearer things
				float cov = clamp(min(max(s.a, 0.0), own_far) - d + 1.0, 0.0, 1.0);
				bg_sum += s.rgb * cov;
				bg_w += cov;
			}
		}
	}
	vec3 bg = bg_w > 1e-4 ? bg_sum / bg_w : center.rgb;
	// coverage of the blurred foreground: 1/2 at its silhouette, 1 inside a CoC from it, 0 outside a CoC
	float alpha = clamp(fg_w / count, 0.0, 1.0);
	vec3 fg = fg_w > 1e-4 ? fg_sum / fg_w : vec3(0.0);
	imageStore(background_image, pos, vec4(bg, 1.0));
	imageStore(foreground_image, pos, vec4(fg * alpha, alpha)); // premultiplied
}
#endif

#ifdef MODE_POSTFILTER
layout(set = 0, binding = 0) uniform sampler2D background_texture;
layout(set = 0, binding = 1) uniform sampler2D foreground_texture;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D background_image;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D foreground_image;

// median of 9 per channel (exchange network)
#define S2(a, b) \
	{ \
		vec4 t = min(v[a], v[b]); \
		v[b] = max(v[a], v[b]); \
		v[a] = t; \
	}

vec4 median9(sampler2D tex, ivec2 pos) {
	vec4 v[9];
	for (int i = 0; i < 9; i++) {
		v[i] = texelFetch(tex, clamp(pos + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), params.half_size - 1), 0);
	}
	S2(1, 2);
	S2(4, 5);
	S2(7, 8);
	S2(0, 1);
	S2(3, 4);
	S2(6, 7);
	S2(1, 2);
	S2(4, 5);
	S2(7, 8);
	S2(0, 3);
	S2(5, 8);
	S2(4, 7);
	S2(3, 6);
	S2(1, 4);
	S2(2, 5);
	S2(4, 7);
	S2(4, 2);
	S2(6, 4);
	S2(4, 2);
	return v[4];
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.half_size))) {
		return;
	}
	imageStore(background_image, pos, median9(background_texture, pos));
	imageStore(foreground_image, pos, median9(foreground_texture, pos));
}
#endif

#ifdef MODE_COMPOSITE
layout(set = 0, binding = 1) uniform sampler2D background_texture;
layout(set = 0, binding = 2) uniform sampler2D foreground_texture;
layout(rgba16f, set = 0, binding = 3) uniform restrict image2D color_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.full_size))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.full_size);
	vec4 color = imageLoad(color_image, pos);
	float coc = get_coc(linear_depth(uv));
	vec3 c = color.rgb;
	vec3 bg = textureLod(background_texture, uv, 0.0).rgb;
	if ((params.flags & FLAG_FAR) != 0u) {
		c = mix(c, bg, smoothstep(1.0, 3.0, coc));
	}
	if ((params.flags & FLAG_NEAR) != 0u) {
		vec4 fg = textureLod(foreground_texture, uv, 0.0);
		// under the blurred foreground: what is behind it (the gathered background), not the foreground itself
		c = mix(c, bg, smoothstep(-1.0, -3.0, coc));
		c = c * (1.0 - fg.a) + fg.rgb;
	}
	imageStore(color_image, pos, vec4(c, color.a));
}
#endif
