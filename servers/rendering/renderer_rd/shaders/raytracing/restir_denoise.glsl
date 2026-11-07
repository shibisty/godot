#[compute]

#version 450

#VERSION_DEFINES

// Mariner E20: denoiser of the ReSTIR DI light (diffuse and specular, one sample per pixel). After the idea of SVGF
// (Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering"); own implementation for Godot:
//  PASS_TEMPORAL - the light of this frame blended into the history of the same surface point (reprojected with the
//                  previous camera, rejected on another surface), with the first two moments of the luminance; the
//                  variance comes from the moments (short histories: boosted).
//  PASS_ATROUS   - 5x5 B3 spline à-trous filter (step 1, 2, 4, 8 pixels), stopped at other planes, normals and at
//                  luminance differences large for the variance; the variance is filtered with the squared weights.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PASS_TEMPORAL 0
#define PASS_ATROUS 1

#define FLAG_HISTORY_VALID 2u

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D in_diffuse;
layout(rgba16f, set = 0, binding = 1) uniform restrict readonly image2D in_specular;
layout(rgba16f, set = 0, binding = 2) uniform restrict readonly image2D history_diffuse;
layout(rgba16f, set = 0, binding = 3) uniform restrict readonly image2D history_specular;
layout(rgba16f, set = 0, binding = 4) uniform restrict readonly image2D history_moments;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D out_diffuse;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D out_specular;
layout(rgba16f, set = 0, binding = 7) uniform restrict writeonly image2D out_moments;
layout(rgba16f, set = 0, binding = 8) uniform restrict readonly image2D in_variance;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D out_variance;
layout(rgba32f, set = 0, binding = 10) uniform restrict readonly image2D surface_current;
layout(rgba32f, set = 0, binding = 11) uniform restrict readonly image2D surface_previous;

layout(set = 0, binding = 12, std140) uniform Params { // Shared with restir_di.glsl.
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

layout(push_constant, std430) uniform PushConstant {
	uint pass;
	int step;
	uint pad0;
	uint pad1;
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

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}
	vec4 surface = imageLoad(surface_current, pixel);
	vec4 diffuse = imageLoad(in_diffuse, pixel);
	vec4 specular = imageLoad(in_specular, pixel);
	if (surface.w < 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_variance, pixel, vec4(0.0));
		if (push.pass == PASS_TEMPORAL) {
			imageStore(out_moments, pixel, vec4(0.0));
		}
		return;
	}
	vec3 position = surface.xyz;
	vec3 normal = unpack_normal(surface.w);
	float scale = max(length(position - params.view_to_world[3].xyz), 1.0);

	if (push.pass == PASS_TEMPORAL) {
		float ld = luminance(diffuse.rgb);
		float ls = luminance(specular.rgb);
		vec4 moments = vec4(ld, ld * ld, ls, ls * ls);
		float history_length = 0.0;
		vec4 hd = vec4(0.0);
		vec4 hs = vec4(0.0);
		vec4 hm = vec4(0.0);

		if (bool(params.flags & FLAG_HISTORY_VALID)) {
			vec4 clip = params.prev_world_to_clip * vec4(position, 1.0);
			if (clip.w > 0.0) {
				// Bilinear footprint of the reprojected point, only taps on the same surface.
				vec2 prev = (clip.xy / clip.w * 0.5 + 0.5) * vec2(params.screen_size) - 0.5;
				ivec2 base = ivec2(floor(prev));
				vec2 f = prev - vec2(base);
				float wsum = 0.0;
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
					hd += imageLoad(history_diffuse, q) * w;
					hs += imageLoad(history_specular, q) * w;
					hm += imageLoad(history_moments, q) * w;
					wsum += w;
				}
				if (wsum > 0.01) {
					hd /= wsum;
					hs /= wsum;
					hm /= wsum;
					history_length = hd.a;
				}
			}
		}

		history_length = min(history_length + 1.0, 64.0);
		float alpha = max(1.0 / history_length, 0.08);
		float alpha_moments = max(1.0 / history_length, 0.2);
		vec3 d = mix(hd.rgb, diffuse.rgb, history_length > 1.0 ? alpha : 1.0);
		vec4 s = mix(hs, specular, history_length > 1.0 ? alpha : 1.0);
		vec4 m = mix(hm, moments, history_length > 1.0 ? alpha_moments : 1.0);
		vec2 variance = max(vec2(m.y - m.x * m.x, m.w - m.z * m.z), vec2(0.0));
		// Young histories: the moments say little, assume more variance so the filter works harder.
		variance *= history_length < 4.0 ? 4.0 / history_length : 1.0;

		imageStore(out_diffuse, pixel, vec4(d, history_length));
		imageStore(out_specular, pixel, s);
		imageStore(out_moments, pixel, m);
		imageStore(out_variance, pixel, vec4(variance, 0.0, 0.0));
		return;
	}

	// PASS_ATROUS.
	const float kernel[3] = float[](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
	vec2 variance = imageLoad(in_variance, pixel).xy;
	// Variance at the center, slightly blurred (3x3 gaussian) for a steadier edge stopping.
	vec2 var_blur = vec2(0.0);
	{
		float wsum = 0.0;
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
				float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
				var_blur += imageLoad(in_variance, q).xy * w;
				wsum += w;
			}
		}
		var_blur /= wsum;
	}
	float ld = luminance(diffuse.rgb);
	float ls = luminance(specular.rgb);
	const float sigma_l = 4.0;
	float denom_d = sigma_l * sqrt(max(var_blur.x, 0.0)) + 1e-4;
	float denom_s = sigma_l * sqrt(max(var_blur.y, 0.0)) + 1e-4;

	vec4 sum_d = vec4(0.0);
	vec4 sum_s = vec4(0.0);
	vec2 sum_var = vec2(0.0);
	vec2 wsum = vec2(0.0);
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 q = pixel + ivec2(x, y) * push.step;
			if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.screen_size))) {
				continue;
			}
			vec4 qs = imageLoad(surface_current, q);
			if (qs.w < 0.0) {
				continue;
			}
			float k = kernel[abs(x)] * kernel[abs(y)];
			float w_plane = exp(-abs(dot(qs.xyz - position, normal)) / (0.01 * scale));
			float w_normal = pow(max(dot(unpack_normal(qs.w), normal), 0.0), 64.0);
			vec4 qd = imageLoad(in_diffuse, q);
			vec4 qsp = imageLoad(in_specular, q);
			vec2 qvar = imageLoad(in_variance, q).xy;
			float wd = k * w_plane * w_normal * exp(-abs(luminance(qd.rgb) - ld) / denom_d);
			float ws = k * w_plane * w_normal * exp(-abs(luminance(qsp.rgb) - ls) / denom_s);
			sum_d += qd * wd;
			sum_s += qsp * ws;
			sum_var += vec2(wd * wd, ws * ws) * qvar;
			wsum += vec2(wd, ws);
		}
	}
	vec4 out_d = wsum.x > 0.0 ? sum_d / wsum.x : diffuse;
	vec4 out_s = wsum.y > 0.0 ? sum_s / wsum.y : specular;
	out_d.a = diffuse.a; // History length passes through.
	vec2 out_var = vec2(wsum.x > 0.0 ? sum_var.x / (wsum.x * wsum.x) : variance.x, wsum.y > 0.0 ? sum_var.y / (wsum.y * wsum.y) : variance.y);
	imageStore(out_diffuse, pixel, out_d);
	imageStore(out_specular, pixel, out_s);
	imageStore(out_variance, pixel, vec4(out_var, 0.0, 0.0));
}
