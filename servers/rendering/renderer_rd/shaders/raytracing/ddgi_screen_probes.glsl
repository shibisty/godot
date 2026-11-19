#[compute]

#version 460

#VERSION_DEFINES

// Mariner L1: screen space probes of the DDGI (Environment.ddgi_screen_probes_enabled), in the manner of the screen
// probes of AMD GI-1.0 / FidelityFX Brixelizer GI (MIT) and Lumen, over the hardware raytracing scene:
//  MODE_PLACE       one probe per tile of pixels, on a (per frame jittered) pixel of the visible surface;
//  (ddgi_trace.glsl MODE_SCREEN_PROBES traces 8x8 world octahedral directions per probe, the upper hemisphere)
//  MODE_INTEGRATE   one work group per probe: the probe of the previous frame at the same surface point (reprojected)
//                   gives the history of each texel; the blend is projected to L1 spherical harmonics (irradiance);
//  MODE_FILTER      the SH of each probe averaged with its neighbours on the same surface (plane distance, normal);
//  MODE_STORE_COLOR the lit color of the frame and its linear depth, for the hits of the next frame's rays.

#define MAX_VIEWS 2

#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"
#include "../ddgi_data_inc.glsl"

layout(set = 0, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(push_constant, std430) uniform Params {
	uint grid_x; // Probes across.
	uint grid_y; // Probes down.
	uint tile; // Pixels per probe.
	uint frame_index;
	uint width; // Internal size.
	uint height;
	float hysteresis;
	uint has_history; // Bit 0: there is a history. L1 only, bit 1: the probes are jittered in their tiles (L1b).
}
params;

// Linear view depth and view position of a pixel from the depth buffer (reverse Z: 0 is the far plane / sky).
vec3 view_position(vec2 uv, float depth) {
	vec4 p = scene_data_block.data.inv_projection_matrix * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return p.xyz / p.w;
}

uint hash(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

#ifdef MODE_PLACE

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1) uniform texture2D depth_buffer;
layout(set = 0, binding = 2) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 3) uniform sampler nearest_sampler;
layout(set = 0, binding = 4, std430) restrict writeonly buffer Probes {
	DDGIScreenProbe probes[];
}
probes;

void main() {
	uvec2 probe = gl_GlobalInvocationID.xy;
	if (probe.x >= params.grid_x || probe.y >= params.grid_y) {
		return;
	}
	DDGIScreenProbe result;
	result.position = vec4(0.0);
	result.normal = vec4(0.0, 1.0, 0.0, 0.0);
	ivec2 tile_origin = ivec2(probe * params.tile);
	ivec2 size = ivec2(params.width, params.height);
	// L1b: the center of the tile, the same surface point each frame of a still view (the history holds, the
	// probes a pixel reads stay the same); then pixels of the tile that are the same each frame, where the center
	// is sky. With the jitter (bit 1): a random pixel of the tile each frame (the probes cover the tile over the
	// frames, as the rays cover the texels), the center last. Up to four tries for a pixel that is not sky.
	bool jitter = (params.has_history & 2u) != 0u;
	uint h = hash(probe.x * 1973u + probe.y * 9277u + (jitter ? params.frame_index * 26699u : 0u));
	for (int i = 0; i < 4; i++) {
		h = hash(h + uint(i));
		ivec2 px = tile_origin + ivec2(h % params.tile, (h >> 8u) % params.tile);
		if (i == (jitter ? 3 : 0)) {
			px = tile_origin + ivec2(params.tile / 2u);
		}
		px = min(px, size - 1);
		float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), px, 0).r;
		if (depth <= 0.0) {
			continue;
		}
		vec3 n_raw = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), px, 0).xyz * 2.0 - 1.0;
		if (dot(n_raw, n_raw) < 0.25) {
			continue; // No normal written (unshaded, or not in the prepass).
		}
		vec3 view = view_position((vec2(px) + 0.5) / vec2(size), depth);
		mat4 inv_view = ddgi_affine_to_mat4(scene_data_block.data.inv_view_matrix);
		result.position = vec4((inv_view * vec4(view, 1.0)).xyz, -view.z);
		result.normal = vec4(normalize(mat3(inv_view) * normalize(n_raw)), 0.0);
		break;
	}
	probes.probes[probe.y * params.grid_x + probe.x] = result;
}

#endif

#ifdef MODE_INTEGRATE

#define TEXELS DDGI_SCREEN_PROBE_TEXELS
#define TEXEL_COUNT (TEXELS * TEXELS)

layout(local_size_x = TEXELS, local_size_y = TEXELS, local_size_z = 1) in;

layout(set = 0, binding = 1, rgba16f) uniform restrict readonly image2D trace_image;
layout(set = 0, binding = 2, rgba16f) uniform restrict readonly image2D history_image;
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image2D radiance_image;
layout(set = 0, binding = 4, std430) restrict readonly buffer Probes {
	DDGIScreenProbe probes[];
}
probes;
layout(set = 0, binding = 5, std430) restrict readonly buffer PreviousProbes {
	DDGIScreenProbe probes[];
}
previous_probes;
layout(set = 0, binding = 6, std430) restrict writeonly buffer SH {
	uvec4 header;
	vec4 sh[]; // 3 per probe (r, g, b): L00, L1-1, L10, L11.
}
sh_buffer;
// L2: the radiance with the texels below the horizon filled (mirror direction), for the filtered radiance.
layout(set = 0, binding = 7, rgba16f) uniform restrict writeonly image2D filled_image;

shared vec4 s_r[TEXEL_COUNT];
shared vec4 s_g[TEXEL_COUNT];
shared vec4 s_b[TEXEL_COUNT];
shared float s_w[TEXEL_COUNT];
shared vec4 s_e[TEXEL_COUNT]; // L1e: radiance x cosine to the probe's normal x solid angle (rgb), cosine x solid angle (a).

void main() {
	uvec2 probe = gl_WorkGroupID.xy;
	uvec2 local = gl_LocalInvocationID.xy;
	uint li = gl_LocalInvocationIndex;
	uint index = probe.y * params.grid_x + probe.x;
	ivec2 texel = ivec2(probe * uint(TEXELS) + local);
	DDGIScreenProbe sp = probes.probes[index];

	vec4 value = vec4(0.0);
	if (sp.position.w > 0.0) {
		value = imageLoad(trace_image, texel);
		// History: the probe of the previous frame closest to this probe's surface point, around where the point was
		// on screen then. Accepted on the same plane and with a similar normal (else the probe starts over).
		if ((params.has_history & 1u) != 0u) {
			vec4 clip = scene_data_block.prev_data.projection_matrix * (ddgi_affine_to_mat4(scene_data_block.prev_data.view_matrix) * vec4(sp.position.xyz, 1.0));
			if (clip.w > 1e-5) {
				vec2 px = ((clip.xy / clip.w) * 0.5 + 0.5) * vec2(params.width, params.height);
				ivec2 center = ivec2(floor(px / float(params.tile)));
				// About the size of a tile on the surface at this depth.
				float tile_size = sp.position.w * float(params.tile) * 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * float(params.height));
				float best = 1e20;
				ivec2 best_probe = ivec2(-1);
				for (int y = -1; y <= 1; y++) {
					for (int x = -1; x <= 1; x++) {
						ivec2 c = center + ivec2(x, y);
						if (any(lessThan(c, ivec2(0))) || c.x >= int(params.grid_x) || c.y >= int(params.grid_y)) {
							continue;
						}
						DDGIScreenProbe pp = previous_probes.probes[c.y * int(params.grid_x) + c.x];
						if (pp.position.w <= 0.0 || dot(pp.normal.xyz, sp.normal.xyz) < 0.8) {
							continue;
						}
						vec3 delta = pp.position.xyz - sp.position.xyz;
						float plane = abs(dot(delta, sp.normal.xyz));
						float dist = length(delta);
						if (plane > 0.02 * sp.position.w + 0.05 || dist > tile_size * 2.0 + 0.05) {
							continue;
						}
						if (dist < best) {
							best = dist;
							best_probe = c;
						}
					}
				}
				if (best_probe.x >= 0) {
					vec4 history = imageLoad(history_image, best_probe * TEXELS + ivec2(local));
					// Texels below the horizon of the probe have no ray (a = 0): only the traced ones blend, and
					// (L1b) only with a history that was traced - the previous probe's normal may differ, and a
					// texel below its horizon holds no light: blended, it darkened the texels near the horizon.
					value = value.a > 0.0 ? (history.a > 0.0 ? mix(value, history, params.hysteresis) : value) : vec4(0.0);
				}
			}
		}
	}
	imageStore(radiance_image, texel, value);

	// L1 SH projection, weighted by the solid angle of the octahedral texel (dw ~ 1 / |v|^3 for the point v of the
	// octahedron |x| + |y| + |z| = 1 under the texel).
	vec2 e = ((vec2(local) + 0.5) / float(TEXELS)) * 2.0 - 1.0;
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	float w = 1.0 / pow(length(v), 3.0);
	vec3 dir = oct_to_vec3(e);
	// L2: the texels below the probe's horizon (no rays) take the light of their mirror direction above it. The
	// irradiance along the probe's normal stays the same, but the radiance near the horizon is no longer halved by
	// the hard step to zero (L1 can't hold it): rough reflections at grazing angles read it.
	// L1e: the traced light of the texel, before the fill below (the irradiance is of this one).
	const vec4 traced = value;
	s_r[li] = vec4(value.rgb, value.a);
	barrier();
	if (sp.position.w > 0.0 && value.a <= 0.0) {
		vec3 mirror = reflect(dir, sp.normal.xyz);
		ivec2 m = clamp(ivec2(vec3_to_oct(mirror) * float(TEXELS)), ivec2(0), ivec2(TEXELS - 1));
		vec4 mv = s_r[m.y * TEXELS + m.x];
		value = vec4(mv.rgb, mv.a);
	}
	imageStore(filled_image, texel, value);
	barrier();
	// L1e: the SH of the traced light (zero below the probe's horizon), not of the filled one. With the mirror fill
	// (L2) the light was symmetric about the surface, its L1 part along the normal was zero and the irradiance
	// became the plain mean of the hemisphere: everything near the horizon (the slope across a hollow, a crater's
	// wall, a stone next to the probe) counted as much as the zenith, and the ground under the probes was lit by
	// about two thirds of the sky light the grids give (measured on the Terra stand, a slope inside a crater).
	vec4 y = vec4(0.282095, 0.488603 * dir.y, 0.488603 * dir.z, 0.488603 * dir.x) * w;
	s_r[li] = traced.r * y;
	s_g[li] = traced.g * y;
	s_b[li] = traced.b * y;
	s_w[li] = w;
	// The irradiance of the probe's own normal exactly (cosine weighted): L1 alone weighs the hemisphere half evenly
	// and half by the cosine. The SH is scaled to it below; the pixels a probe lights have nearly its normal.
	float cosine = traced.a > 0.0 ? max(dot(dir, sp.normal.xyz), 0.0) * w : 0.0;
	s_e[li] = vec4(traced.rgb * cosine, cosine);
	barrier();
	for (uint stride = TEXEL_COUNT / 2u; stride > 0u; stride >>= 1u) {
		if (li < stride) {
			s_r[li] += s_r[li + stride];
			s_g[li] += s_g[li + stride];
			s_b[li] += s_b[li + stride];
			s_w[li] += s_w[li + stride];
			s_e[li] += s_e[li + stride];
		}
		barrier();
	}
	if (li == 0u) {
		float norm = sp.position.w > 0.0 ? 4.0 * 3.14159265359 / max(s_w[0], 1e-6) : 0.0;
		vec4 sh_r = s_r[0] * norm;
		vec4 sh_g = s_g[0] * norm;
		vec4 sh_b = s_b[0] * norm;
		if (s_e[0].a > 1e-6) {
			vec3 exact = s_e[0].rgb / s_e[0].a;
			vec3 approx = vec3(ddgi_sh_irradiance(sh_r, sp.normal.xyz), ddgi_sh_irradiance(sh_g, sp.normal.xyz), ddgi_sh_irradiance(sh_b, sp.normal.xyz));
			vec3 scale = mix(vec3(1.0), clamp(exact / max(approx, vec3(1e-6)), vec3(0.25), vec3(4.0)), step(vec3(1e-6), approx));
			sh_r *= scale.r;
			sh_g *= scale.g;
			sh_b *= scale.b;
		}
		sh_buffer.sh[index * 3u + 0u] = sh_r;
		sh_buffer.sh[index * 3u + 1u] = sh_g;
		sh_buffer.sh[index * 3u + 2u] = sh_b;
	}
}

#endif

#ifdef MODE_FILTER

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, std430) restrict readonly buffer Probes {
	DDGIScreenProbe probes[];
}
probes;
layout(set = 0, binding = 2, std430) restrict readonly buffer SH {
	uvec4 header;
	vec4 sh[];
}
sh_in;
layout(set = 0, binding = 3, std430) restrict writeonly buffer SHFiltered {
	uvec4 header;
	vec4 sh[];
}
sh_out;

void main() {
	ivec2 probe = ivec2(gl_GlobalInvocationID.xy);
	if (probe.x >= int(params.grid_x) || probe.y >= int(params.grid_y)) {
		return;
	}
	uint index = uint(probe.y) * params.grid_x + uint(probe.x);
	DDGIScreenProbe sp = probes.probes[index];
	vec4 r = vec4(0.0);
	vec4 g = vec4(0.0);
	vec4 b = vec4(0.0);
	if (sp.position.w > 0.0) {
		float weight_sum = 0.0;
		float tolerance = 0.02 * sp.position.w + 0.05;
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 c = probe + ivec2(x, y);
				if (any(lessThan(c, ivec2(0))) || c.x >= int(params.grid_x) || c.y >= int(params.grid_y)) {
					continue;
				}
				uint ci = uint(c.y) * params.grid_x + uint(c.x);
				DDGIScreenProbe cp = probes.probes[ci];
				if (cp.position.w <= 0.0) {
					continue;
				}
				float plane = abs(dot(cp.position.xyz - sp.position.xyz, sp.normal.xyz));
				float wn = max(dot(cp.normal.xyz, sp.normal.xyz), 0.0);
				float w = (x == 0 && y == 0 ? 1.0 : (x == 0 || y == 0 ? 0.5 : 0.25)) * max(0.0, 1.0 - plane / tolerance) * wn * wn;
				if (w <= 0.0) {
					continue;
				}
				r += sh_in.sh[ci * 3u + 0u] * w;
				g += sh_in.sh[ci * 3u + 1u] * w;
				b += sh_in.sh[ci * 3u + 2u] * w;
				weight_sum += w;
			}
		}
		if (weight_sum > 0.0) {
			r /= weight_sum;
			g /= weight_sum;
			b /= weight_sum;
		}
	}
	sh_out.sh[index * 3u + 0u] = r;
	sh_out.sh[index * 3u + 1u] = g;
	sh_out.sh[index * 3u + 2u] = b;
}

#endif

#ifdef MODE_STORE_COLOR

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1) uniform texture2D color_buffer;
layout(set = 0, binding = 2) uniform texture2D depth_buffer;
layout(set = 0, binding = 3) uniform sampler nearest_sampler;
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D last_color_image;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= int(params.width) || px.y >= int(params.height)) {
		return;
	}
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), px, 0).r;
	vec4 result = vec4(0.0);
	if (depth > 0.0) {
		vec3 color = texelFetch(sampler2D(color_buffer, nearest_sampler), px, 0).rgb;
		if (any(isnan(color)) || any(isinf(color))) {
			color = vec3(0.0);
		}
		vec3 view = view_position((vec2(px) + 0.5) / vec2(params.width, params.height), depth);
		result = vec4(clamp(color, vec3(0.0), vec3(65000.0)), -view.z);
	}
	imageStore(last_color_image, px, result);
}

#endif

// Mariner L2: ray traced glossy reflections. Half resolution: one ray per 2x2 pixels (ddgi_trace.glsl
// MODE_REFLECTIONS); MODE_REFL_TEMPORAL accumulates them over the frames (the surface point reprojected), clamped to
// the neighbourhood of this frame's rays; MODE_REFL_RESOLVE filters them along the surface (depth, normal) into full
// resolution with a footprint growing with the roughness, fading out below max_roughness (params.hysteresis).

#if defined(MODE_REFL_TEMPORAL) || defined(MODE_REFL_RESOLVE)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct SurfacePixel {
	bool valid;
	float depth; // Linear.
	float roughness;
	vec3 normal; // View space.
	vec3 view_pos;
};

// View space point of a pixel (uv) at a linear depth, for a projection's inverse.
vec3 view_at_depth(vec2 p_uv, float p_linear_depth, mat4 p_inv_projection) {
	vec4 v = p_inv_projection * vec4(p_uv * 2.0 - 1.0, 0.5, 1.0);
	v.xyz /= v.w;
	return v.xyz * (p_linear_depth / max(-v.z, 1e-6));
}

SurfacePixel read_surface(texture2D p_depth, texture2D p_normal, sampler p_sampler, ivec2 p_px, ivec2 p_size) {
	SurfacePixel sp;
	float depth = texelFetch(sampler2D(p_depth, p_sampler), p_px, 0).r;
	vec4 nr = texelFetch(sampler2D(p_normal, p_sampler), p_px, 0);
	vec3 n = nr.xyz * 2.0 - 1.0;
	sp.valid = depth > 0.0 && dot(n, n) > 0.25;
	sp.normal = sp.valid ? normalize(n) : vec3(0.0, 0.0, 1.0);
	sp.roughness = ddgi_decode_roughness(nr.w);
	sp.view_pos = view_position((vec2(p_px) + 0.5) / vec2(p_size), depth);
	sp.depth = -sp.view_pos.z;
	sp.valid = sp.valid && sp.roughness <= params.hysteresis;
	return sp;
}

#endif

#ifdef MODE_REFL_TEMPORAL

layout(set = 0, binding = 1, rgba16f) uniform restrict readonly image2D trace_image;
layout(set = 0, binding = 2, rgba16f) uniform restrict readonly image2D history_image;
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image2D accum_image;
layout(set = 0, binding = 4) uniform texture2D depth_buffer;
layout(set = 0, binding = 5) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 6) uniform sampler nearest_sampler;

void main() {
	ivec2 h = ivec2(gl_GlobalInvocationID.xy);
	ivec2 half_size = ivec2(params.grid_x, params.grid_y);
	if (h.x >= half_size.x || h.y >= half_size.y) {
		return;
	}
	ivec2 full_size = ivec2(params.width, params.height);
	ivec2 px = ddgi_reflection_pixel(h, params.frame_index, full_size);
	SurfacePixel s = read_surface(depth_buffer, normal_roughness_buffer, nearest_sampler, px, full_size);
	vec4 value = imageLoad(trace_image, h);
	if (!s.valid || value.a <= 0.0) {
		imageStore(accum_image, h, vec4(0.0));
		return;
	}
	vec3 result = value.rgb;
	if (params.has_history != 0u) {
		vec3 world = (ddgi_affine_to_mat4(scene_data_block.data.inv_view_matrix) * vec4(s.view_pos, 1.0)).xyz;
		vec4 prev_view = ddgi_affine_to_mat4(scene_data_block.prev_data.view_matrix) * vec4(world, 1.0);
		vec4 clip = scene_data_block.prev_data.projection_matrix * prev_view;
		if (clip.w > 1e-5) {
			vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
			ivec2 hp = ivec2(floor(uv * vec2(full_size) * 0.5));
			if (all(greaterThanEqual(hp, ivec2(0))) && all(lessThan(hp, half_size))) {
				vec4 history = imageLoad(history_image, hp);
				// The point the history texel traced last frame, on this pixel's plane (a depth test alone fails at
				// grazing angles, where the depth changes fast from pixel to pixel) and not far along it.
				bool valid = false;
				if (history.a > 0.0) {
					ivec2 prev_px = ddgi_reflection_pixel(hp, params.frame_index - 1u, full_size);
					vec3 prev_point = view_at_depth((vec2(prev_px) + 0.5) / vec2(full_size), history.a, scene_data_block.prev_data.inv_projection_matrix);
					vec3 prev_world = (ddgi_affine_to_mat4(scene_data_block.prev_data.inv_view_matrix) * vec4(prev_point, 1.0)).xyz;
					vec3 n_world = normalize(mat3(ddgi_affine_to_mat4(scene_data_block.data.inv_view_matrix)) * s.normal);
					vec3 delta = prev_world - world;
					valid = abs(dot(delta, n_world)) < 0.01 * s.depth + 0.02 && length(delta) < 0.05 * s.depth + 0.1;
				}
				if (valid) {
					// Clamp the history to this frame's rays around (ghosting of moved reflections), widened for
					// the noise of rough surfaces.
					vec3 lo = value.rgb;
					vec3 hi = value.rgb;
					for (int y = -1; y <= 1; y++) {
						for (int x = -1; x <= 1; x++) {
							vec4 c = imageLoad(trace_image, clamp(h + ivec2(x, y), ivec2(0), half_size - 1));
							if (c.a > 0.0) {
								lo = min(lo, c.rgb);
								hi = max(hi, c.rgb);
							}
						}
					}
					vec3 extent = (hi - lo) * (0.25 + 2.0 * s.roughness);
					vec3 clamped = clamp(history.rgb, lo - extent, hi + extent);
					float keep = mix(0.6, 0.93, smoothstep(0.03, 0.35, s.roughness));
					result = mix(value.rgb, clamped, keep);
				}
			}
		}
	}
	imageStore(accum_image, h, vec4(result, s.depth));
}

#endif

#ifdef MODE_REFL_RESOLVE

layout(set = 0, binding = 1, rgba16f) uniform restrict readonly image2D accum_image;
layout(set = 0, binding = 2) uniform texture2D depth_buffer;
layout(set = 0, binding = 3) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 4) uniform sampler nearest_sampler;
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D reflection_image;

void main() {
	ivec2 q = ivec2(gl_GlobalInvocationID.xy);
	ivec2 full_size = ivec2(params.width, params.height);
	if (q.x >= full_size.x || q.y >= full_size.y) {
		return;
	}
	ivec2 half_size = ivec2(params.grid_x, params.grid_y);
	SurfacePixel s = read_surface(depth_buffer, normal_roughness_buffer, nearest_sampler, q, full_size);
	vec4 result = vec4(0.0);
	if (s.valid) {
		float sigma = 1.2 + 2.5 * smoothstep(0.08, 0.45, s.roughness); // Full resolution pixels.
		int radius = s.roughness < 0.1 ? 1 : 2;
		ivec2 center = q / 2;
		float tolerance = 0.01 * s.depth + 0.02; // Distance from this pixel's plane.
		vec3 sum = vec3(0.0);
		float weight_sum = 0.0;
		for (int y = -radius; y <= radius; y++) {
			for (int x = -radius; x <= radius; x++) {
				ivec2 c = center + ivec2(x, y);
				if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, half_size))) {
					continue;
				}
				vec4 a = imageLoad(accum_image, c);
				if (a.a <= 0.0) {
					continue;
				}
				ivec2 cp = ddgi_reflection_pixel(c, params.frame_index, full_size);
				vec3 cv = view_at_depth((vec2(cp) + 0.5) / vec2(full_size), a.a, scene_data_block.data.inv_projection_matrix);
				float wd = max(0.0, 1.0 - abs(dot(cv - s.view_pos, s.normal)) / tolerance);
				vec3 cn = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), cp, 0).xyz * 2.0 - 1.0;
				float wn = pow(max(dot(normalize(cn), s.normal), 0.0), 8.0);
				vec2 delta = vec2(cp - q);
				float ws = exp(-dot(delta, delta) / (2.0 * sigma * sigma));
				float w = wd * wn * ws;
				sum += a.rgb * w;
				weight_sum += w;
			}
		}
		if (weight_sum > 1e-4) {
			float fade = 1.0 - smoothstep(params.hysteresis - 0.1, params.hysteresis, s.roughness);
			result = vec4(sum / weight_sum, fade * smoothstep(1e-4, 2e-3, weight_sum)); // Normalized: a far sample is still this surface's.
		}
	}
	imageStore(reflection_image, q, result);
}

#endif

#ifdef MODE_FILTER_RADIANCE

// Mariner L2: the probes' radiance (below-horizon texels filled) averaged with the neighbours on the same surface
// (5x5 probes, tent weights; plane and normal weights as MODE_FILTER): the radiance rough reflections read in the scene shader. One work group per probe.

layout(local_size_x = DDGI_SCREEN_PROBE_TEXELS, local_size_y = DDGI_SCREEN_PROBE_TEXELS, local_size_z = 1) in;

layout(set = 0, binding = 1, std430) restrict readonly buffer Probes {
	DDGIScreenProbe probes[];
}
probes;
layout(set = 0, binding = 2, rgba16f) uniform restrict readonly image2D filled_image;
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image2D filtered_image;

void main() {
	ivec2 probe = ivec2(gl_WorkGroupID.xy);
	ivec2 local = ivec2(gl_LocalInvocationID.xy);
	const int texels = DDGI_SCREEN_PROBE_TEXELS;
	DDGIScreenProbe sp = probes.probes[uint(probe.y) * params.grid_x + uint(probe.x)];
	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	if (sp.position.w > 0.0) {
		float tolerance = 0.02 * sp.position.w + 0.05;
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				ivec2 c = probe + ivec2(x, y);
				if (any(lessThan(c, ivec2(0))) || c.x >= int(params.grid_x) || c.y >= int(params.grid_y)) {
					continue;
				}
				DDGIScreenProbe cp = probes.probes[uint(c.y) * params.grid_x + uint(c.x)];
				if (cp.position.w <= 0.0) {
					continue;
				}
				float plane = abs(dot(cp.position.xyz - sp.position.xyz, sp.normal.xyz));
				float wn = max(dot(cp.normal.xyz, sp.normal.xyz), 0.0);
				float tent = float(3 - abs(x)) * float(3 - abs(y)); // 5x5 tent.
				float w = tent * max(0.0, 1.0 - plane / tolerance) * wn * wn;
				if (w <= 0.0) {
					continue;
				}
				vec4 v = imageLoad(filled_image, c * texels + local);
				if (v.a <= 0.0) {
					continue;
				}
				sum += v.rgb * w;
				weight_sum += w;
			}
		}
	}
	imageStore(filtered_image, probe * texels + local, weight_sum > 0.0 ? vec4(sum / weight_sum, 1.0) : vec4(0.0));
}

#endif
