#[compute]

#version 450

#VERSION_DEFINES

// Mariner E20b: denoiser of the ray traced shadows of directional lights (PASS_SHADOW of restir_di.glsl: one ray per
// pixel and light towards a random point of the light disk). Own implementation, after the idea of penumbra driven
// shadow denoising (filter as wide as the penumbra, as PCSS sizes its filter - Fernando 2005):
//  PASS_TEMPORAL - the visibility of this frame blended into the history of the same surface point (reprojected with
//                  the previous camera, rejected on another surface); the occluder distance is averaged only over the
//                  occluded samples.
//  PASS_SPATIAL  - the occluder distance of the neighbourhood gives the width of the penumbra on screen
//                  (distance × tangent of the angular radius / size of a pixel there); the visibility is filtered with
//                  a disk of that radius on the same surface. Contact shadows (thin ropes, rails on a deck) keep sharp
//                  edges, shadows of far occluders get soft.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PASS_TEMPORAL 0
#define PASS_SPATIAL 1

#define MAX_HISTORY 16.0
#define MAX_RADIUS 32.0
#define M_PI 3.14159265359

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D raw; // Visibility, occluder distance (rg, ba).
layout(rgba16f, set = 0, binding = 1) uniform restrict readonly image2D history_in; // As raw, accumulated.
layout(rgba16f, set = 0, binding = 2) uniform restrict readonly image2D history_length_in;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D history_out;
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D history_length_out;
layout(rgba16f, set = 0, binding = 5) uniform restrict readonly image2D accumulated; // This frame's history_out.
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D mask_out; // Visibility per light (r, g).
layout(rgba32f, set = 0, binding = 7) uniform restrict readonly image2D surface_current;
layout(rgba32f, set = 0, binding = 8) uniform restrict readonly image2D surface_previous;

layout(set = 0, binding = 9, std140) uniform Params { // Shared with restir_di.glsl.
	mat4 inv_projection;
	mat4 view_to_world;
	mat4 prev_world_to_clip;
	vec4 grid_origin;
	ivec4 grid_size;
	ivec2 screen_size;
	uint frame;
	uint flags;
	uint candidates;
	uint spatial_samples;
	float spatial_radius;
	uint prev_light_count;
}
params;

layout(set = 0, binding = 10, std140) uniform ShadowParams { // Shared with restir_di.glsl.
	vec4 lights[2]; // xyz: towards the light (world), w: tangent of the angular radius of its disk.
	uvec4 info; // x: count, y: 1 if the history images hold the last frame.
	vec4 range; // x: length of the shadow rays, y: size of a pixel per unit of distance (z = 1: orthogonal, per pixel).
}
shadow_params;

layout(push_constant, std430) uniform PushConstant {
	uint pass;
	uint pad0;
	uint pad1;
	uint pad2;
}
push;

vec3 oct_decode(vec2 o) {
	o = o * 2.0 - 1.0;
	vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
	float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

vec3 unpack_normal(float p) {
	uint v = uint(p);
	return oct_decode(vec2(float(v >> 12u), float(v & 4095u)) / 4095.0);
}

uint pcg(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// Blends one light's (visibility, occluder distance) into its history. Occluder distance: only occluded samples count
// (0: lit, no occluder; -1: facing away from the light, no information).
vec2 accumulate(vec2 history, vec2 current, float alpha, bool has_history) {
	if (!has_history) {
		return current;
	}
	float v = mix(history.x, current.x, alpha);
	float t = history.y;
	if (current.y > 0.0) {
		t = history.y > 0.0 ? mix(history.y, current.y, max(alpha, 0.2)) : current.y;
	}
	return vec2(v, t);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}
	vec4 surface = imageLoad(surface_current, pixel);

	if (push.pass == PASS_TEMPORAL) {
		vec4 current = imageLoad(raw, pixel);
		if (surface.w < 0.0) {
			imageStore(history_out, pixel, vec4(1.0, 0.0, 1.0, 0.0));
			imageStore(history_length_out, pixel, vec4(0.0));
			return;
		}
		vec3 position = surface.xyz;
		vec3 normal = unpack_normal(surface.w);
		float scale = max(length(position - params.view_to_world[3].xyz), 1.0);
		vec4 h = vec4(0.0);
		float history_length = 0.0;
		if (shadow_params.info.y != 0u) {
			vec4 clip = params.prev_world_to_clip * vec4(position, 1.0);
			if (clip.w > 0.0) {
				// Bilinear footprint of the reprojected point, only taps on the same surface.
				vec2 prev = (clip.xy / clip.w * 0.5 + 0.5) * vec2(params.screen_size) - 0.5;
				ivec2 base = ivec2(floor(prev));
				vec2 f = prev - vec2(base);
				float wsum = 0.0;
				float lsum = 0.0;
				for (int i = 0; i < 4; i++) {
					ivec2 o = ivec2(i & 1, i >> 1);
					ivec2 q = base + o;
					if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.screen_size))) {
						continue;
					}
					vec4 ps = imageLoad(surface_previous, q);
					if (ps.w < 0.0 || abs(dot(ps.xyz - position, normal)) > 0.02 * scale || dot(unpack_normal(ps.w), normal) < 0.9) {
						continue;
					}
					float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
					h += imageLoad(history_in, q) * w;
					lsum += imageLoad(history_length_in, q).x * w;
					wsum += w;
				}
				if (wsum > 0.01) {
					h /= wsum;
					history_length = lsum / wsum;
				}
			}
		}
		bool has_history = history_length > 0.0;
		if (has_history) {
			// Clamp the history to the visibility range of this frame's samples around (5x5, same surface): fully
			// lit or fully occluded neighbourhoods (the shadow of a moving occluder or a moving sun) take the new
			// value at once, penumbrae (mixed samples) keep their history.
			vec2 lo = current.xz;
			vec2 hi = current.xz;
			for (int y = -2; y <= 2; y++) {
				for (int x = -2; x <= 2; x++) {
					ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
					vec4 qs = imageLoad(surface_current, q);
					if (qs.w < 0.0 || abs(dot(qs.xyz - position, normal)) > 0.02 * scale) {
						continue;
					}
					vec4 qr = imageLoad(raw, q);
					lo = min(lo, qr.xz);
					hi = max(hi, qr.xz);
				}
			}
			h.x = clamp(h.x, lo.x, hi.x);
			h.z = clamp(h.z, lo.y, hi.y);
		}
		history_length = min(history_length + 1.0, MAX_HISTORY);
		float alpha = 1.0 / history_length;
		vec2 l0 = accumulate(h.xy, current.xy, alpha, has_history);
		vec2 l1 = accumulate(h.zw, current.zw, alpha, has_history);
		imageStore(history_out, pixel, vec4(l0, l1));
		imageStore(history_length_out, pixel, vec4(history_length, 0.0, 0.0, 0.0));
		return;
	}

	// PASS_SPATIAL.
	if (surface.w < 0.0) {
		imageStore(mask_out, pixel, vec4(1.0));
		return;
	}
	vec3 position = surface.xyz;
	vec3 normal = unpack_normal(surface.w);
	vec3 camera = params.view_to_world[3].xyz;
	float distance = length(position - camera);
	float scale = max(distance, 1.0);
	// World size of a pixel at this distance.
	float pixel_size = max(shadow_params.range.y * (shadow_params.range.z > 0.5 ? 1.0 : distance), 1e-5);
	vec4 center = imageLoad(accumulated, pixel);
	float history_length = imageLoad(history_length_in, pixel).x; // This frame's (bound as the history input).
	// Young histories are noisy everywhere: a small minimum radius until a few frames are accumulated.
	float min_radius = 1.5 * clamp(1.0 - history_length / 6.0, 0.0, 1.0);

	vec2 result = vec2(1.0);
	uint count = min(shadow_params.info.x, 2u);
	for (uint li = 0u; li < count; li++) {
		vec2 c = li == 0u ? center.xy : center.zw;
		// Occluder distance of the neighbourhood (5x5, step 3): only occluded samples on the same surface.
		float t_sum = 0.0;
		float t_count = 0.0;
		float v_min = c.x;
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				ivec2 q = clamp(pixel + ivec2(x, y) * 3, ivec2(0), params.screen_size - 1);
				vec4 qs = imageLoad(surface_current, q);
				if (qs.w < 0.0 || abs(dot(qs.xyz - position, normal)) > 0.05 * scale) {
					continue;
				}
				vec4 qa = imageLoad(accumulated, q);
				vec2 qv = li == 0u ? qa.xy : qa.zw;
				v_min = min(v_min, qv.x);
				if (qv.y > 0.0) {
					t_sum += qv.y;
					t_count += 1.0;
				}
			}
		}
		if (t_count == 0.0 || v_min > 0.999) {
			result[li] = c.x; // No occluder around: lit (or facing away), nothing to filter.
			continue;
		}
		float occluder = t_sum / t_count;
		float radius = clamp(occluder * shadow_params.lights[li].w / pixel_size, min_radius, MAX_RADIUS);
		if (radius < 0.5) {
			result[li] = c.x;
			continue;
		}
		// Disk of 16 taps on a golden angle spiral, rotated per pixel (fixed over frames: no flicker), gaussian weights,
		// stopped at other planes and normals.
		float rotation = float(pcg(uint(pixel.x) + pcg(uint(pixel.y))) & 0xFFFFu) * (2.0 * M_PI / 65536.0);
		float sigma = max(radius * 0.5, 0.5);
		float sum = c.x;
		float wsum = 1.0;
		for (int k = 0; k < 16; k++) {
			float rr = radius * sqrt((float(k) + 0.5) / 16.0);
			float a = rotation + float(k) * 2.39996323;
			ivec2 q = pixel + ivec2(round(vec2(cos(a), sin(a)) * rr));
			if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.screen_size))) {
				continue;
			}
			vec4 qs = imageLoad(surface_current, q);
			if (qs.w < 0.0) {
				continue;
			}
			float w = exp(-rr * rr / (2.0 * sigma * sigma));
			w *= exp(-abs(dot(qs.xyz - position, normal)) / (0.01 * scale));
			w *= pow(max(dot(unpack_normal(qs.w), normal), 0.0), 32.0);
			vec4 qa = imageLoad(accumulated, q);
			sum += (li == 0u ? qa.x : qa.z) * w;
			wsum += w;
		}
		result[li] = sum / wsum;
	}
	imageStore(mask_out, pixel, vec4(result, 0.0, 1.0));
}
