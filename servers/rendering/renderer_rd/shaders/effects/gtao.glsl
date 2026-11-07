#[compute]

#version 450

#VERSION_DEFINES

/*
Mariner E18: ground truth ambient occlusion (GTAO) with bent normals.

The horizon search, the analytic slice visibility, the bent normal integral and the temporal accumulation (mode 0)
are ported from Spartan Engine, data/shaders/ssao.hlsl at commit cb258a150 (the last version released under the MIT
license), translated from HLSL to GLSL and adapted to Godot (right-handed view space, reverse depth, Godot's motion
vectors and normal-roughness buffer, history validity without previous camera matrices). The edge-aware filter
(mode 1) is Godot's own. The algorithm is from Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect
Occlusion" (2016), as in Intel's XeGTAO.

Original notice:

Copyright(c) 2015-2026 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer; // Reverse Z, as rasterized.
layout(set = 0, binding = 1) uniform sampler2D normal_buffer; // Normal (view space, encoded) and roughness.
layout(set = 0, binding = 2) uniform sampler2D velocity_buffer; // prev_uv - uv; unused without motion vectors.
layout(set = 0, binding = 3) uniform sampler2D history; // Mode 0: previous frame (xy: octahedral bent normal, z: visibility, w: frames). Mode 1: this frame.
layout(set = 0, binding = 4) uniform sampler2D history_depth; // View depth of the previous frame.
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D history_out;
layout(r32f, set = 0, binding = 6) uniform restrict writeonly image2D history_depth_out;
layout(rgba8, set = 0, binding = 7) uniform restrict writeonly image2D final_out; // rgb: world bent normal * 0.5 + 0.5, a: visibility.

layout(set = 0, binding = 8, std140) uniform Params {
	mat4 inv_projection;
	mat4 view_to_world; // Rotation only.
	vec2 size;
	float radius;
	float intensity; // Exponent of the visibility.
	uint directions;
	uint steps;
	uint frame;
	uint mode;
	uint history_valid; // 0: the camera cut or the history is new.
	uint use_velocity;
	float sharpness; // Depth weight of the filter.
	float thickness; // Mariner F1: assumed thickness of the depth buffer [m]; 0: horizon-based GTAO.
}
params;

#define PI 3.14159265359
#define PI_HALF 1.57079632679

// Constants of the temporal accumulation (Spartan): short history, rejecting is preferred over ghosting. Spartan
// rejects a history differing by more than 0.04 in visibility or 0.98 in the bent normal's cosine, leaving the noise to
// its TAA; Godot often runs without TAA, and those limits threw away nearly every history, so here they are 0.2 / 0.9
// (a changed occlusion still takes over within about three frames, the history weight being at most 0.65).
const float g_r2_a1 = 0.7548776662466927; // R2 (Roberts 2018), a low discrepancy 2D sequence.
const float g_r2_a2 = 0.5698402909980532;
const float g_max_accum_frames = 8.0;
const float g_disoccl_rel = 0.01;
const float g_motion_reject_px = 0.75;
const float g_vis_reject = 0.2;
const float g_bent_reject = 0.9;
const float g_wake_motion_px = 1.5;
const float g_wake_delta_px = 1.0;

float fast_acos(float in_x) {
	float x = abs(in_x);
	float res = -0.156583 * x + PI_HALF;
	res *= sqrt(1.0 - x);
	return (in_x >= 0.0) ? res : PI - res;
}

vec2 temporal_r2(uint frame) {
	float f = float(frame) + 0.5;
	return vec2(fract(f * g_r2_a1), fract(f * g_r2_a2));
}

float noise_interleaved_gradient(vec2 screen_pos) {
	const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
	return fract(magic.z * fract(dot(screen_pos, magic.xy)));
}

float hash(vec2 p) {
	uvec2 u = floatBitsToUint(p * vec2(141421356.0, 2718281828.0));
	return float((u.x ^ u.y) * 3141592653u) / 4294967295.0;
}

vec2 octahedral_encode(vec3 n) {
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	if (n.z < 0.0) {
		vec2 sign_not_zero = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * sign_not_zero;
	}
	return n.xy;
}

vec3 octahedral_decode(vec2 e) {
	vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0) {
		vec2 sign_not_zero = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * sign_not_zero;
	}
	return normalize(n);
}

// Rotation taking p_from onto p_to (Jimenez / XeGTAO "rotFromToMatrix").
mat3 rotation_from_to(vec3 p_from, vec3 p_to) {
	vec3 from = normalize(p_from);
	vec3 to = normalize(p_to);
	float cos_theta = dot(from, to);
	if (cos_theta > 0.9999) {
		return mat3(1.0);
	}
	if (cos_theta < -0.9999) {
		return mat3(vec3(1, 0, 0), vec3(0, -1, 0), vec3(0, 0, -1));
	}
	vec3 axis = normalize(cross(from, to));
	float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
	float omc = 1.0 - cos_theta;
	float x = axis.x;
	float y = axis.y;
	float z = axis.z;
	// Columns (GLSL); the HLSL original fills rows.
	return mat3(
			vec3(cos_theta + x * x * omc, y * x * omc + z * sin_theta, z * x * omc - y * sin_theta),
			vec3(x * y * omc - z * sin_theta, cos_theta + y * y * omc, z * y * omc + x * sin_theta),
			vec3(x * z * omc + y * sin_theta, y * z * omc - x * sin_theta, cos_theta + z * z * omc));
}

// Cosine-weighted visibility of a slice and its horizon angles.
float compute_slice_visibility(float horizon_cos0, float horizon_cos1, float cos_norm, float n, out float h0, out float h1) {
	h0 = -fast_acos(horizon_cos1);
	h1 = fast_acos(horizon_cos0);
	h0 = n + clamp(h0 - n, -PI_HALF, PI_HALF);
	h1 = n + clamp(h1 - n, -PI_HALF, PI_HALF);
	float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
	float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
	return iarc0 + iarc1;
}

// Jimenez 2016, equations 20-21: t0 along the slice direction, t1 along the local axis towards the viewer.
vec2 compute_slice_bent_t(float h0, float h1, float n) {
	float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
	float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) - 3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
	return vec2(t0, t1);
}

// Mariner F1: GTAO with a visibility bitmask (Therrien, Levesque, Gilet, "Screen Space Indirect Lighting with
// Visibility Bitmask", 2023). The slice's hemisphere is split into 32 sectors; a sample occludes the sectors between
// its front face and its back face (the front pushed away from the viewer by the assumed thickness), so the background
// behind a trunk or a branch is no longer shadowed by it. The sectors are weighted by the projected solid angle, the
// same integrand as the horizon-based slice visibility, so a fully open slice still integrates to 1.
#define SECTOR_COUNT 32u

// Cosine-weighted visibility integrated from the view vector to the angle h (signed, in the slice).
float slice_integral(float h, float n) {
	float a = -0.25 * cos(2.0 * h - n) + 0.5 * h * sin(n) + 0.25 * cos(n);
	return h >= 0.0 ? a : -a;
}

uint occlusion_bits(float h_front, float h_back, float n) {
	vec2 t = clamp((vec2(h_front, h_back) - n + PI_HALF) / PI, 0.0, 1.0);
	uint a = uint(floor(min(t.x, t.y) * float(SECTOR_COUNT)));
	uint b = uint(ceil(max(t.x, t.y) * float(SECTOR_COUNT)));
	a = min(a, SECTOR_COUNT);
	b = min(b, SECTOR_COUNT);
	if (b <= a) {
		return 0u;
	}
	uint count = b - a;
	uint mask = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
	return mask << a;
}

float raw_depth(vec2 uv) {
	return textureLod(depth_buffer, uv, 0.0).r;
}

vec3 view_position(vec2 uv, float depth) {
	vec4 v = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return v.xyz / v.w;
}

vec3 view_position(vec2 uv) {
	return view_position(uv, raw_depth(uv));
}

vec3 view_normal(ivec2 pos) {
	return normalize(texelFetch(normal_buffer, pos, 0).xyz * 2.0 - 1.0);
}

vec2 velocity_px(ivec2 pos) {
	return texelFetch(velocity_buffer, pos, 0).xy * params.size;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.size)))) {
		return;
	}
	const vec2 texel_size = 1.0 / params.size;
	const vec2 origin_uv = (vec2(pos) + 0.5) * texel_size;

	if (params.mode == 1) {
		// Edge-aware filter of this frame's result (4x4 around the pixel, weights by view depth and normal).
		float center_depth = texelFetch(depth_buffer, pos, 0).r;
		if (center_depth <= 0.0) {
			imageStore(final_out, pos, vec4(0.5, 1.0, 0.5, 1.0));
			return;
		}
		float center_z = -view_position(origin_uv, center_depth).z;
		vec3 center_normal = view_normal(pos);
		float vis_sum = 0.0;
		vec3 bent_sum = vec3(0.0);
		float weight_sum = 0.0;
		for (int y = -2; y <= 1; y++) {
			for (int x = -2; x <= 1; x++) {
				ivec2 p = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.size) - 1);
				float d = texelFetch(depth_buffer, p, 0).r;
				if (d <= 0.0) {
					continue;
				}
				float z = -view_position((vec2(p) + 0.5) * texel_size, d).z;
				float depth_w = exp(-abs(z - center_z) / max(center_z, 1e-3) * params.sharpness * 100.0);
				float normal_w = pow(max(dot(view_normal(p), center_normal), 0.0), 8.0);
				float w = depth_w * normal_w + 1e-4 * float(x == 0 && y == 0);
				vec4 h = texelFetch(history, p, 0);
				vis_sum += h.z * w;
				bent_sum += octahedral_decode(h.xy) * w;
				weight_sum += w;
			}
		}
		float vis = vis_sum / max(weight_sum, 1e-6);
		vec3 bent = length(bent_sum) > 1e-6 ? normalize(bent_sum) : octahedral_decode(texelFetch(history, pos, 0).xy);
		imageStore(final_out, pos, vec4(bent * 0.5 + 0.5, vis));
		return;
	}

	const float depth_raw = texelFetch(depth_buffer, pos, 0).r;
	if (depth_raw <= 0.0) { // Sky (reverse Z).
		vec3 sky_bent = vec3(0.0, 1.0, 0.0);
		imageStore(history_out, pos, vec4(octahedral_encode(sky_bent), 1.0, 1.0));
		imageStore(history_depth_out, pos, vec4(0.0));
		return;
	}

	const vec3 origin_position = view_position(origin_uv, depth_raw);
	const vec3 origin_normal = view_normal(pos);
	const vec3 geo_normal_world = normalize(mat3(params.view_to_world) * origin_normal);

	// R2 temporal offsets; the spatial gradient noise stays fixed so frames do not beat.
	const vec2 r2 = temporal_r2(params.frame);
	const float noise_gradient_spatial = noise_interleaved_gradient(vec2(pos));
	const float offset_spatial = 0.25 * float((pos.y - pos.x) & 3);
	const float ray_offset = fract(offset_spatial + r2.x) + (hash(origin_uv) * 2.0 - 1.0) * 0.25;
	const vec3 view_vec = normalize(-origin_position);

	const float falloff_range = 0.6 * params.radius;
	const float falloff_from = params.radius - falloff_range;
	const float falloff_mul = -1.0 / falloff_range;
	const float falloff_add = falloff_from / falloff_range + 1.0;
	const vec3 pos_right = view_position(origin_uv + vec2(texel_size.x, 0.0));
	const float pixel_size_at_center = max(length(pos_right - origin_position), 1e-6);
	const float screenspace_radius = params.radius / pixel_size_at_center;
	const float pixel_too_close_threshold = 1.3;
	const float min_s = pixel_too_close_threshold / screenspace_radius;
	const float noise_slice = noise_gradient_spatial + r2.y;
	const float noise_sample = ray_offset;

	// The same matrix for every slice. Godot's view space is right-handed (the viewer is towards +z), so the local
	// +z axis maps onto the view vector (Spartan maps -z in its left-handed space).
	const mat3 local_to_view = rotation_from_to(vec3(0.0, 0.0, 1.0), view_vec);

	float visibility = 0.0;
	vec3 bent_normal = vec3(0.0);
	const uint directions = max(params.directions, 1u);
	const uint steps = max(params.steps, 1u);

	for (uint slice = 0; slice < directions; slice++) {
		float slice_k = (float(slice) + noise_slice) / float(directions);
		float phi = slice_k * PI;
		float cos_phi = cos(phi);
		float sin_phi = sin(phi);
		// View y is up, texel y is down.
		vec2 omega = vec2(cos_phi, -sin_phi) * screenspace_radius;
		const vec3 direction_vec = vec3(cos_phi, sin_phi, 0.0);
		const vec3 ortho_direction_vec = direction_vec - dot(direction_vec, view_vec) * view_vec;
		const vec3 axis_vec = normalize(cross(ortho_direction_vec, view_vec));

		vec3 projected_normal_vec = origin_normal - axis_vec * dot(origin_normal, axis_vec);
		float projected_normal_vec_length = length(projected_normal_vec);
		float cos_norm = clamp(dot(projected_normal_vec, view_vec) / max(projected_normal_vec_length, 1e-6), 0.0, 1.0);
		float sign_norm = sign(dot(ortho_direction_vec, projected_normal_vec));
		float n = sign_norm * fast_acos(cos_norm);

		if (params.thickness > 0.0) {
			// Mariner F1: visibility bitmask.
			uint occluded = 0u;
			for (uint step = 0; step < steps; step++) {
				float step_base_noise = float(slice + step * steps) * 0.6180339887498948482;
				float step_noise = fract(noise_sample + step_base_noise);
				float s = (float(step) + step_noise) / float(steps);
				s += min_s;
				vec2 sample_offset = round(s * omega) * texel_size;
				for (uint side = 0; side < 2; side++) {
					vec2 sample_uv = side == 0 ? origin_uv + sample_offset : origin_uv - sample_offset;
					float sample_raw = raw_depth(sample_uv);
					if (sample_raw <= 0.0) {
						continue; // The sky does not occlude.
					}
					vec3 front = view_position(sample_uv, sample_raw);
					vec3 delta_front = front - origin_position;
					float dist = length(delta_front);
					float weight = clamp(dist * falloff_mul + falloff_add, 0.0, 1.0);
					if (weight <= 0.0 || dist < 1e-6) {
						continue;
					}
					// The falloff thins the sample: towards the radius it occludes less and less of the slice.
					vec3 back = front + normalize(front) * (params.thickness * weight);
					vec3 delta_back = back - origin_position;
					float side_sign = side == 0 ? 1.0 : -1.0;
					float h_front = side_sign * fast_acos(clamp(dot(delta_front / dist, view_vec), -1.0, 1.0));
					float h_back = side_sign * fast_acos(clamp(dot(normalize(delta_back), view_vec), -1.0, 1.0));
					occluded |= occlusion_bits(h_front, h_back, n);
				}
			}
			float slice_vis = 0.0;
			vec2 t = vec2(0.0);
			const float sector = PI / float(SECTOR_COUNT);
			float h_a = n - PI_HALF;
			float p_a = slice_integral(h_a, n);
			for (uint i = 0; i < SECTOR_COUNT; i++) {
				float h_b = h_a + sector;
				float p_b = slice_integral(h_b, n);
				if ((occluded & (1u << i)) == 0u) {
					float w = p_b - p_a;
					float h_c = h_a + 0.5 * sector;
					slice_vis += w;
					t += vec2(sin(h_c), cos(h_c)) * w;
				}
				h_a = h_b;
				p_a = p_b;
			}
			visibility += slice_vis * projected_normal_vec_length;
			vec3 local_bent = vec3(direction_vec.x * t.x, direction_vec.y * t.x, t.y);
			bent_normal += local_to_view * local_bent * projected_normal_vec_length;
			continue;
		}

		float low_horizon_cos0 = cos(n + PI_HALF);
		float low_horizon_cos1 = cos(n - PI_HALF);
		float horizon_cos0 = low_horizon_cos0;
		float horizon_cos1 = low_horizon_cos1;

		for (uint step = 0; step < steps; step++) {
			float step_base_noise = float(slice + step * steps) * 0.6180339887498948482;
			float step_noise = fract(noise_sample + step_base_noise);
			float s = (float(step) + step_noise) / float(steps);
			s += min_s;

			vec2 sample_offset = round(s * omega) * texel_size;
			vec2 sample_screen_pos0 = origin_uv + sample_offset;
			vec3 sample_delta0 = view_position(sample_screen_pos0) - origin_position;
			float sample_dist0 = length(sample_delta0);
			vec3 sample_horizon_vec0 = sample_delta0 / max(sample_dist0, 1e-6);

			vec2 sample_screen_pos1 = origin_uv - sample_offset;
			vec3 sample_delta1 = view_position(sample_screen_pos1) - origin_position;
			float sample_dist1 = length(sample_delta1);
			vec3 sample_horizon_vec1 = sample_delta1 / max(sample_dist1, 1e-6);

			float weight0 = clamp(sample_dist0 * falloff_mul + falloff_add, 0.0, 1.0);
			float weight1 = clamp(sample_dist1 * falloff_mul + falloff_add, 0.0, 1.0);

			float shc0 = dot(sample_horizon_vec0, view_vec);
			float shc1 = dot(sample_horizon_vec1, view_vec);
			// The sky (reverse Z 0, a point at infinity) does not occlude.
			shc0 = raw_depth(sample_screen_pos0) <= 0.0 ? low_horizon_cos0 : mix(low_horizon_cos0, shc0, weight0);
			shc1 = raw_depth(sample_screen_pos1) <= 0.0 ? low_horizon_cos1 : mix(low_horizon_cos1, shc1, weight1);

			horizon_cos0 = max(horizon_cos0, shc0);
			horizon_cos1 = max(horizon_cos1, shc1);
		}

		// Analytic GTAO visibility and the bent normal share the same horizons.
		float h0, h1;
		float slice_vis = compute_slice_visibility(horizon_cos0, horizon_cos1, cos_norm, n, h0, h1);
		// Weighted by the projected normal length, so grazing slices fade out.
		visibility += slice_vis * projected_normal_vec_length;

		vec2 t = compute_slice_bent_t(h0, h1, n);
		vec3 local_bent = vec3(direction_vec.x * t.x, direction_vec.y * t.x, t.y);
		bent_normal += local_to_view * local_bent * projected_normal_vec_length;
	}

	visibility /= float(directions);
	visibility = pow(clamp(visibility, 0.0, 1.0), params.intensity);
	// As XeGTAO: a fully occluded visible pixel is still lit a little.
	visibility = max(visibility, 0.03);

	// World space bent normal; the geometric normal when the integral collapses.
	if (dot(bent_normal, bent_normal) > 1e-8 && !any(isnan(bent_normal))) {
		bent_normal = normalize(mat3(params.view_to_world) * normalize(bent_normal));
	} else {
		bent_normal = geo_normal_world;
	}
	if (isnan(visibility)) {
		visibility = 1.0;
	}

	// Temporal accumulation: fails closed on any risk of ghosting.
	vec3 out_bent = bent_normal;
	float out_vis = visibility;
	float accum = 1.0;
	const float view_depth = -origin_position.z;

	vec2 uv_prev = origin_uv;
	bool history_valid = params.history_valid != 0;
	if (params.use_velocity != 0) {
		vec2 v = velocity_px(pos);
		float motion_px = length(v);
		uv_prev = origin_uv + v * texel_size;
		// A plus of radius 2: pixels in the wake of a moving neighbor lose their history.
		const ivec2 wake_taps[8] = ivec2[](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2));
		float neighbor_motion_px = 0.0;
		for (int i = 0; i < 8; i++) {
			ivec2 p = pos + wake_taps[i];
			if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, ivec2(params.size)))) {
				continue;
			}
			neighbor_motion_px = max(neighbor_motion_px, length(velocity_px(p)));
		}
		bool in_wake = neighbor_motion_px > g_wake_motion_px && (neighbor_motion_px - motion_px) > g_wake_delta_px;
		history_valid = history_valid && !in_wake && motion_px < g_motion_reject_px;
	}
	const vec2 inset = 2.0 * texel_size;
	history_valid = history_valid && all(greaterThan(uv_prev, inset)) && all(lessThan(uv_prev, 1.0 - inset));

	if (history_valid) {
		vec4 hist = textureLod(history, uv_prev, 0.0);
		// Any change of depth at this point: an occluder arrived or left (also catches moving objects without motion vectors).
		float prev_depth = textureLod(history_depth, uv_prev, 0.0).r;
		float depth_rel = abs(view_depth - prev_depth) / max(max(view_depth, prev_depth), 1e-3);
		vec3 hist_bent = octahedral_decode(hist.xy);
		float bent_ok = clamp(dot(hist_bent, bent_normal), 0.0, 1.0);
		float vis_delta = abs(hist.z - visibility);

		history_valid = prev_depth > 0.0 && depth_rel < g_disoccl_rel && bent_ok > g_bent_reject && vis_delta < g_vis_reject && hist.w >= 1.0 && !any(isnan(hist)) && !any(isnan(hist_bent));

		if (history_valid) {
			accum = min(hist.w + 1.0, g_max_accum_frames);
			// Floor towards the current frame, so an accepted history cannot trail.
			float w = max(1.0 / accum, 0.35);
			out_vis = mix(hist.z, visibility, w);
			out_bent = normalize(mix(hist_bent, bent_normal, w));
		}
	}

	imageStore(history_out, pos, vec4(octahedral_encode(out_bent), out_vis, accum));
	imageStore(history_depth_out, pos, vec4(view_depth));
}
