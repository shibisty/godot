#[compute]

#version 450

#VERSION_DEFINES

// Mariner E15a: motion blur by the motion vectors of the frame (camera attributes motion_blur_*).
// After the idea of McGuire et al., "A Reconstruction Filter for Plausible Motion Blur" (I3D 2012): the largest motion
// of each tile of pixels and of its neighbor tiles bounds how far a pixel can be smeared; every pixel then gathers
// samples along that motion, weighted by whether the sampled pixel is in front (its blur reaches here) or behind
// (seen through this pixel's own blur). Four passes: velocity (pixels, camera motion weighted, depth), tile max,
// neighbor max, gather (from a copy of the color).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D velocity_buffer; // prev_uv - uv (internal size).
layout(set = 0, binding = 1) uniform sampler2D depth_buffer; // Internal size.
layout(set = 0, binding = 2) uniform sampler2D color_source; // Copy of the color (color size).
layout(rgba16f, set = 0, binding = 3) uniform restrict image2D prepared; // xy: velocity (px), z: view distance.
layout(rg16f, set = 0, binding = 4) uniform restrict image2D tile_max;
layout(rg16f, set = 0, binding = 5) uniform restrict image2D neighbor_max;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D color_dest;

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 reprojection; // Current clip (depth from the buffer) -> previous clip, static world.
	mat4 inv_projection; // Current clip -> view.
	vec2 color_size;
	vec2 tile_count;
	float intensity; // Fraction of one frame's motion.
	float camera_motion; // Weight of the camera's own motion.
	float max_length; // Pixels.
	float tile_size; // Pixels.
	uint mode;
	uint samples;
	uint frame;
	uint pad;
}
params;

#define MODE_VELOCITY 0
#define MODE_TILE_MAX 1
#define MODE_NEIGHBOR_MAX 2
#define MODE_GATHER 3
#define MODE_COPY 4 // color_source -> color_dest (the color as source of the gather).

vec2 camera_velocity(vec2 uv, float depth) {
	vec4 prev = params.reprojection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	return 0.5 + (prev.xy / prev.w) * 0.5 - uv;
}

// The longer of two velocities.
vec2 vmax(vec2 a, vec2 b) {
	return dot(a, a) >= dot(b, b) ? a : b;
}

// 1 where a is in front of (or at) b, fading out over a soft extent behind it.
float closer(float a, float b) {
	float extent = max(0.05, 0.01 * min(a, b));
	return clamp(1.0 - (a - b) / extent, 0.0, 1.0);
}

float cone(float dist, float len) {
	return clamp(1.0 - dist / max(len, 0.5), 0.0, 1.0);
}

float cylinder(float dist, float len) {
	return 1.0 - smoothstep(0.95 * len, 1.05 * len + 0.01, dist);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	if (params.mode == MODE_VELOCITY) {
		if (any(greaterThanEqual(pos, ivec2(params.color_size)))) {
			return;
		}
		vec2 uv = (vec2(pos) + 0.5) / params.color_size;
		vec2 v = textureLod(velocity_buffer, uv, 0.0).xy;
		float depth = textureLod(depth_buffer, uv, 0.0).r;
		vec2 v_camera = camera_velocity(uv, depth);
		if (all(lessThanEqual(v, vec2(-1.0)))) {
			v = v_camera; // No motion vector written (sky): the camera's motion at that depth.
		}
		// Objects' own motion, plus the camera's own motion weighted.
		v = (v - v_camera) + v_camera * params.camera_motion;
		vec2 v_px = v * params.color_size * params.intensity;
		float len = length(v_px);
		if (len > params.max_length) {
			v_px *= params.max_length / len;
		}
		vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
		float dist = depth <= 0.0 ? 1e6 : length(view.xyz / view.w);
		imageStore(prepared, pos, vec4(v_px, dist, 0.0));
		return;
	}

	if (params.mode == MODE_TILE_MAX) {
		if (any(greaterThanEqual(pos, ivec2(params.tile_count)))) {
			return;
		}
		int ts = int(params.tile_size);
		ivec2 from = pos * ts;
		ivec2 to = min(from + ts, ivec2(params.color_size));
		vec2 m = vec2(0.0);
		for (int y = from.y; y < to.y; y++) {
			for (int x = from.x; x < to.x; x++) {
				m = vmax(m, imageLoad(prepared, ivec2(x, y)).xy);
			}
		}
		imageStore(tile_max, pos, vec4(m, 0.0, 0.0));
		return;
	}

	if (params.mode == MODE_NEIGHBOR_MAX) {
		if (any(greaterThanEqual(pos, ivec2(params.tile_count)))) {
			return;
		}
		vec2 m = vec2(0.0);
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 t = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.tile_count) - 1);
				m = vmax(m, imageLoad(tile_max, t).xy);
			}
		}
		imageStore(neighbor_max, pos, vec4(m, 0.0, 0.0));
		return;
	}

	if (any(greaterThanEqual(pos, ivec2(params.color_size)))) {
		return;
	}
	if (params.mode == MODE_COPY) {
		imageStore(color_dest, pos, texelFetch(color_source, pos, 0));
		return;
	}

	// MODE_GATHER.
	vec3 center_color = texelFetch(color_source, pos, 0).rgb;
	ivec2 tile = min(pos / int(params.tile_size), ivec2(params.tile_count) - 1);
	vec2 vn = imageLoad(neighbor_max, tile).xy;
	float vn_len = length(vn);
	if (vn_len < 0.5) {
		imageStore(color_dest, pos, vec4(center_color, 1.0));
		return;
	}
	vec4 c = imageLoad(prepared, pos);
	vec2 vc = c.xy;
	float vc_len = length(vc);
	float zc = c.z;

	// Interleaved gradient noise, changing every frame (TAA / upscalers average the steps).
	float noise = fract(52.9829189 * fract(dot(vec2(pos) + float(params.frame % 64u) * 5.588238, vec2(0.06711056, 0.00583715))));

	float weight = 1.0 / max(vc_len, 1.0);
	vec3 sum = center_color * weight;
	float weight_sum = weight;
	int samples = int(params.samples);
	for (int i = 0; i < samples; i++) {
		// Symmetric steps over [-1, 1], jittered; every other along the pixel's own motion where it is long enough
		// (keeps the blur of thin moving objects inside a fast moving neighborhood).
		float t = mix(-1.0, 1.0, (float(i) + noise) / float(samples));
		vec2 dir = ((i & 1) == 1 && vc_len > 0.5) ? vc : vn;
		vec2 offset = dir * t;
		ivec2 p = clamp(ivec2(vec2(pos) + 0.5 + offset), ivec2(0), ivec2(params.color_size) - 1);
		vec4 s = imageLoad(prepared, p);
		float vs_len = length(s.xy);
		float dist = length(offset);
		float front = closer(s.z, zc); // The sample is in front: its blur reaches here.
		float back = closer(zc, s.z); // The sample is behind: seen through this pixel's blur.
		float w = front * cone(dist, vs_len) + back * cone(dist, vc_len) + cylinder(dist, vs_len) * cylinder(dist, vc_len) * 2.0;
		sum += texelFetch(color_source, p, 0).rgb * w;
		weight_sum += w;
	}
	imageStore(color_dest, pos, vec4(sum / weight_sum, 1.0));
}
