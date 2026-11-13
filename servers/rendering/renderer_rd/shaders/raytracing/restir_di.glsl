#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RT_NONUNIFORM_WATERFALL
#extension GL_KHR_shader_subgroup_ballot : require
#endif

// Mariner E20: ReSTIR DI - direct light of many omni / spot lights (Light3D.light_restir) with ray traced shadows, for
// the opaque pass of Forward+. Algorithm after Bitterli et al. 2020 ("Spatiotemporal reservoir resampling for
// real-time ray tracing with dynamic direct lighting"); own implementation for Godot.
//
// Per pixel of the depth prepass (position from the depth, normal and roughness from the normal-roughness buffer):
//  PASS_INITIAL  - resampled importance sampling of `candidates` light samples (light + a point on its disk) from the
//                  light grid cell of the pixel, target = unshadowed luminance of the light (diffuse + specular);
//                  the chosen sample is tested with a shadow ray (occluded: weight 0); then temporal reuse of the
//                  reservoir of the reprojected pixel of the previous frame (same surface, M capped).
//  PASS_SPATIAL  - reuse of the reservoirs of random neighbours on the same surface.
//  PASS_SHADE    - the light of the final sample with a shadow ray: demodulated diffuse (the scene shader multiplies
//                  the albedo) and specular without Fresnel (rgb) with the Fresnel weight in alpha: the scene shader
//                  adds rgb * (f0 + (f90 - f0) * alpha), as its own lights do.
//  PASS_SHADOW   - Mariner E20b: ray traced shadows of up to two directional lights (Light3D.light_restir): one ray
//                  per light towards a random point of the light disk (angular size), visibility and the distance to
//                  the occluder for the denoiser (rt_shadow_denoise.glsl), which sizes its filter to the penumbra.
//                  Mariner E20c: run again after the depth pre-pass of the late opaque surfaces (water) for the pixels
//                  they cover (`late`), with its own surfaces and history.
// Same light model as the rasterizer (scene_forward_lights_inc.glsl): Burley diffuse, GGX specular, windowed distance
// attenuation, spot cone.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PASS_INITIAL 0
#define PASS_SPATIAL 1
#define PASS_SHADE 2
#define PASS_SHADOW 3

#define FLAG_TEMPORAL 1u
#define FLAG_HISTORY_VALID 2u

#define LIGHT_OMNI 0u
#define LIGHT_SPOT 1u

#define INVALID_LIGHT 0xFFFFFFFFu
#define M_CAP 20.0

#define M_PI 3.14159265359

struct RestirLight { // 64 bytes, must match RestirDI::Light.
	vec3 position;
	float inv_range;
	vec3 color; // Linear, energy (× π / exposure as the rasterizer).
	float attenuation;
	vec3 direction; // Spot: direction of the cone.
	float cos_spot_angle;
	float inv_spot_attenuation;
	float size; // Radius of the light (soft shadows).
	float specular;
	uint type;
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict readonly buffer Lights {
	RestirLight lights[];
};

layout(set = 0, binding = 2, std430) restrict readonly buffer GridCells {
	uvec2 grid_cells[]; // Offset in grid_indices, count.
};

layout(set = 0, binding = 3, std430) restrict readonly buffer GridIndices {
	uint grid_indices[];
};

layout(set = 0, binding = 4) uniform sampler2D depth_buffer;
layout(set = 0, binding = 5) uniform sampler2D normal_roughness_buffer;

layout(rgba32ui, set = 0, binding = 6) uniform restrict readonly uimage2D reservoir_in;
layout(rgba32ui, set = 0, binding = 7) uniform restrict writeonly uimage2D reservoir_out;
layout(rgba32f, set = 0, binding = 8) uniform restrict readonly image2D surface_in;
layout(rgba32f, set = 0, binding = 9) uniform restrict writeonly image2D surface_out;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D diffuse_out;
layout(rgba16f, set = 0, binding = 11) uniform restrict writeonly image2D specular_out;

layout(set = 0, binding = 12, std140) uniform Params {
	mat4 inv_projection; // Clip (as rasterized) -> view.
	mat4 view_to_world;
	mat4 prev_world_to_clip; // World -> clip of the previous frame (as rasterized).
	vec4 grid_origin; // xyz: world position of the corner of cell 0, w: cell size.
	ivec4 grid_size; // xyz: cells, w: light count.
	ivec2 screen_size;
	uint frame;
	uint flags;
	uint candidates;
	uint spatial_samples;
	float spatial_radius; // Pixels.
	uint prev_light_count; // Size of prev_to_current.
}
params;

layout(set = 0, binding = 13, std430) restrict readonly buffer Remap {
	uint prev_to_current[]; // Light index of the previous frame -> index of this frame (INVALID_LIGHT: gone).
};

// Mariner E20b: ray traced shadows of directional lights (PASS_SHADOW).
layout(rgba16f, set = 0, binding = 14) uniform restrict writeonly image2D shadow_out; // Visibility, occluder distance per light (rg, ba).

layout(set = 0, binding = 15, std140) uniform ShadowParams { // Must match RestirDI::ShadowParams.
	vec4 lights[2]; // xyz: towards the light (world), w: tangent of the angular radius of its disk.
	uvec4 info; // x: count, y: history valid (rt_shadow_denoise.glsl).
	vec4 range; // x: length of the shadow rays.
}
shadow_params;

// Mariner E20c: depth before the late opaque surfaces (water, render_mode opaque_late): PASS_SHADOW for the late pass
// traces only the pixels the late surfaces cover.
layout(set = 0, binding = 16) uniform sampler2D opaque_depth;

// Mariner E20e: materials of the ray tracing scene for the shadow rays (alpha scissor, raytracing_opacity).
#include "../oct_inc.glsl"
#define PT_SCENE_SET 2
#define PT_SCENE_NO_TLAS
#ifndef PI
#define PI M_PI
#endif
#include "pathtracer_scene_inc.glsl"
layout(set = 2, binding = 3) uniform sampler material_sampler; // Linear, mipmaps, repeat.
layout(set = 2, binding = 4) uniform sampler material_sampler_nearest;
layout(set = 1, binding = 0) uniform texture2D rt_textures[];

layout(push_constant, std430) uniform PushConstant {
	uint pass;
	uint late; // E20c: PASS_SHADOW for the late opaque surfaces (also writes their surfaces to surface_out).
	uint pad1;
	uint pad2;
}
push;

// ---------------------------------------------------------------------------------------------------------------------
// Random numbers.

uint rng_state;

uint pcg(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float rand() {
	rng_state = pcg(rng_state);
	return float(rng_state >> 8) * (1.0 / 16777216.0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Surface of a pixel.

struct Surface {
	vec3 position; // World.
	vec3 normal; // World.
	vec3 view; // Towards the camera.
	float roughness;
	bool valid;
};

vec2 oct_encode(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 o = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return o * 0.5 + 0.5;
}

vec3 oct_decode(vec2 o) {
	o = o * 2.0 - 1.0;
	vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
	float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

// Normal in two 12 bit halves of a float (exact in 24 bits of mantissa); -1: no surface (sky).
float pack_normal(vec3 n) {
	uvec2 q = uvec2(clamp(oct_encode(n), 0.0, 1.0) * 4095.0 + 0.5);
	return float(q.x * 4096u + q.y);
}

vec3 unpack_normal(float p) {
	uint v = uint(p);
	return oct_decode(vec2(float(v >> 12u), float(v & 4095u)) / 4095.0);
}

Surface surface_from_buffers(ivec2 pixel) {
	Surface s;
	s.valid = false;
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		return s; // Sky (reverse Z).
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	view.xyz /= view.w;
	vec4 nr = texelFetch(normal_roughness_buffer, pixel, 0);
	vec3 view_normal = normalize(nr.xyz * 2.0 - 1.0);
	float r = nr.w > 0.5 ? 1.0 - nr.w : nr.w; // Dynamic objects are stored mirrored above 0.5.
	s.roughness = clamp(r * (255.0 / 127.0), 0.0, 1.0);
	s.position = (params.view_to_world * vec4(view.xyz, 1.0)).xyz;
	s.normal = normalize(mat3(params.view_to_world) * view_normal);
	s.view = normalize(params.view_to_world[3].xyz - s.position);
	s.valid = true;
	return s;
}

// Position and normal of a pixel from a surface image (the previous frame's, or this frame's in the spatial pass).
bool surface_from_image(ivec2 pixel, out vec3 position, out vec3 normal) {
	vec4 v = imageLoad(surface_in, pixel);
	if (v.w < 0.0) {
		return false;
	}
	position = v.xyz;
	normal = unpack_normal(v.w);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Light model (as light_compute() of the rasterizer, without the user light() code, rim, clearcoat, anisotropy).

float schlick_fresnel(float u) {
	float m = 1.0 - u;
	float m2 = m * m;
	return m2 * m2 * m;
}

float d_ggx(float NdotH, float alpha) {
	float a = NdotH * alpha;
	float k = alpha / (1.0 - NdotH * NdotH + a * a);
	return k * k * (1.0 / M_PI);
}

float v_ggx(float NdotL, float NdotV, float alpha) {
	return clamp(0.5 / mix(2.0 * NdotL * NdotV, NdotL + NdotV, alpha), 0.0, 1.0);
}

// Point of the light sample: a point on the disk of the light facing the surface.
vec3 light_point(RestirLight l, vec2 u, vec3 p) {
	if (l.size <= 0.0) {
		return l.position;
	}
	vec3 axis = normalize(p - l.position);
	vec3 t = normalize(cross(abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), axis));
	vec3 b = cross(axis, t);
	float r = sqrt(u.x) * l.size;
	float phi = 2.0 * M_PI * u.y;
	return l.position + (t * cos(phi) + b * sin(phi)) * r;
}

// Light of a sample at a surface: diffuse (demodulated), specular without Fresnel, specular × Schlick weight.
// Returns false if the sample gives no light.
bool evaluate(Surface s, uint index, vec2 u, out vec3 diffuse, out vec3 specular, out vec3 specular_fc, out vec3 to_point, out float point_distance) {
	diffuse = vec3(0.0);
	specular = vec3(0.0);
	specular_fc = vec3(0.0);
	to_point = vec3(0.0);
	point_distance = 0.0;
	if (index >= uint(params.grid_size.w)) {
		return false;
	}
	RestirLight l = lights[index];

	vec3 rel = l.position - s.position;
	float center_distance = length(rel);
	if (center_distance * l.inv_range >= 1.0) {
		return false;
	}
	float nd = center_distance * l.inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	float attenuation = nd * pow(max(center_distance, 0.0001), -l.attenuation);
	if (l.type == LIGHT_SPOT) {
		float scos = max(dot(-rel / center_distance, l.direction), l.cos_spot_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - l.cos_spot_angle));
		attenuation *= 1.0 - pow(spot_rim, l.inv_spot_attenuation);
	}
	if (attenuation <= 0.0) {
		return false;
	}

	vec3 point = light_point(l, u, s.position);
	vec3 d = point - s.position;
	point_distance = length(d);
	vec3 L = d / max(point_distance, 1e-6);
	to_point = L;
	float NdotL = dot(s.normal, L);
	if (NdotL <= 0.0) {
		return false;
	}
	vec3 N = s.normal;
	vec3 V = s.view;
	float cNdotL = min(NdotL, 1.0);
	float cNdotV = max(dot(N, V), 1e-4);
	vec3 H = normalize(V + L);
	float cLdotH = clamp(dot(L, H), 0.0, 1.0);
	float roughness = s.roughness;

	// Burley.
	float FD90_minus_1 = 2.0 * cLdotH * cLdotH * roughness - 0.5;
	float FdV = 1.0 + FD90_minus_1 * schlick_fresnel(cNdotV);
	float FdL = 1.0 + FD90_minus_1 * schlick_fresnel(cNdotL);
	diffuse = l.color * ((1.0 / M_PI) * FdV * FdL * cNdotL * attenuation);

	if (roughness > 0.0) {
		float cNdotH = clamp(dot(N, H), 0.0, 1.0);
		float alpha = roughness * roughness;
		float D = d_ggx(cNdotH, alpha);
		float G = v_ggx(cNdotL, cNdotV, alpha);
		specular = l.color * (cNdotL * D * G * attenuation * l.specular);
		specular_fc = specular * schlick_fresnel(cLdotH);
	}
	return true;
}

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Target function of the resampling: unshadowed luminance of the light (specular at a mid reflectance).
float target(Surface s, uint index, vec2 u) {
	vec3 diffuse, specular, specular_fc, L;
	float dist;
	if (!evaluate(s, index, u, diffuse, specular, specular_fc, L, dist)) {
		return 0.0;
	}
	return luminance(diffuse) + luminance(specular) * 0.25;
}

// Mariner E20e: does a candidate triangle of a shadow ray stop the light? Alpha scissor of its material (albedo alpha
// from the texture and the vertex colors, as the path tracer and DDGI), and the share of rays a partly transparent
// instance lets through (GeometryInstance3D.raytracing_opacity), decided per ray and triangle. Dielectric media
// (water) stay opaque for shadows.
uint shadow_ray_seed = 0u;

bool shadow_occludes(uint inst, uint prim, vec2 candidate_bary) {
	GeometryData g = geometries[inst];
	uint transmission = g.layer_mask >> GEOM_TRANSMISSION_SHIFT;
	if (transmission != 0u && (pcg(shadow_ray_seed ^ (inst * 0x9E3779B9u) ^ (prim * 0x85EBCA6Bu)) >> 24u) < transmission) {
		return false;
	}
	MaterialData mat = materials[inst];
	if (mat.alpha_scissor <= 0.0 || (mat.flags & MAT_FLAG_DIELECTRIC) != 0u) {
		return true;
	}
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);
	uint i0, i1, i2;
	get_triangle_indices_ex(g, prim, i0, i1, i2);
	float alpha = mat.albedo_color.a;
	if (mat.albedo_texture_idx != 0u) {
		vec2 uv = fetch_uv(g, i0, i1, i2, bary) * mat.uv1_scale + mat.uv1_offset;
#ifdef RT_NONUNIFORM_WATERFALL
		// Mariner E5d: one texture index of the wave at a time (rt_load(), pathtracer_scene_inc.glsl).
		uint tex_idx = mat.albedo_texture_idx;
		for (;;) {
			uint idx = subgroupBroadcastFirst(tex_idx);
			if (idx == tex_idx) {
				if ((mat.flags & MAT_FLAG_POINT_FILTER) != 0u) {
					alpha *= textureLod(sampler2D(rt_textures[idx], material_sampler_nearest), uv, 0.0).a;
				} else {
					alpha *= textureLod(sampler2D(rt_textures[idx], material_sampler), uv, 0.0).a;
				}
				break;
			}
		}
#else
		if ((mat.flags & MAT_FLAG_POINT_FILTER) != 0u) {
			alpha *= textureLod(sampler2D(rt_textures[nonuniformEXT(mat.albedo_texture_idx)], material_sampler_nearest), uv, 0.0).a;
		} else {
			alpha *= textureLod(sampler2D(rt_textures[nonuniformEXT(mat.albedo_texture_idx)], material_sampler), uv, 0.0).a;
		}
#endif
	}
	if ((mat.flags & MAT_FLAG_USE_VERTEX_COLOR) != 0u) {
		alpha *= fetch_color(g, i0, i1, i2, bary).a;
	}
	return alpha >= mat.alpha_scissor;
}

// Runs a shadow ray query to its end, confirming only the candidates that stop the light.
// (A macro: GLSL does not allow ray query objects as inout parameters.)
#define SHADOW_RAY_PROCEED(q) \
	shadow_ray_seed = pcg(rng_state ^ 0x68E31DA4u); \
	rng_state = shadow_ray_seed; \
	while (rayQueryProceedEXT(q)) { \
		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && \
				shadow_occludes(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), rayQueryGetIntersectionBarycentricsEXT(q, false))) { \
			rayQueryConfirmIntersectionEXT(q); \
		} \
	}

// Shadow ray from the surface to the light point (the light itself is not in the scene: lights have no geometry).
bool visible(Surface s, vec3 L, float dist) {
	float scale = max(length(s.position - params.view_to_world[3].xyz), 1.0);
	vec3 origin = s.position + s.normal * (0.002 * scale) + L * (0.001 * scale);
	float t_max = dist - 0.002 * scale;
	if (t_max <= 0.0) {
		return true;
	}
	rayQueryEXT q;
	rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFF, origin, 0.0, L, t_max);
	SHADOW_RAY_PROCEED(q);
	return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reservoirs.

struct Reservoir {
	uint light; // INVALID_LIGHT: empty.
	vec2 u; // Point on the light.
	float w_sum; // While building; the unbiased contribution weight W once finished.
	float M;
	float target; // Target of the chosen sample at the pixel being built.
};

Reservoir reservoir_empty() {
	Reservoir r;
	r.light = INVALID_LIGHT;
	r.u = vec2(0.0);
	r.w_sum = 0.0;
	r.M = 0.0;
	r.target = 0.0;
	return r;
}

uvec4 reservoir_pack(Reservoir r, float W) {
	return uvec4(r.light, packUnorm2x16(r.u), floatBitsToUint(W), floatBitsToUint(r.M));
}

void reservoir_unpack(uvec4 v, out uint light, out vec2 u, out float W, out float M) {
	light = v.x;
	u = unpackUnorm2x16(v.y);
	W = uintBitsToFloat(v.z);
	M = uintBitsToFloat(v.w);
	if (!(W >= 0.0) || isinf(W)) {
		W = 0.0;
	}
	if (!(M >= 0.0) || isinf(M)) {
		M = 0.0;
	}
}

bool reservoir_update(inout Reservoir r, uint light, vec2 u, float weight, float M, float target_value) {
	r.w_sum += weight;
	r.M += M;
	if (weight > 0.0 && rand() * r.w_sum <= weight) {
		r.light = light;
		r.u = u;
		r.target = target_value;
		return true;
	}
	return false;
}

float reservoir_W(Reservoir r) {
	return (r.light != INVALID_LIGHT && r.target > 0.0 && r.M > 0.0) ? r.w_sum / (r.M * r.target) : 0.0;
}

// Merges a finished reservoir of another pixel (light, u, W, M) into r, with the target at this surface.
void reservoir_merge(inout Reservoir r, Surface s, uint light, vec2 u, float W, float M) {
	float t = (light != INVALID_LIGHT && W > 0.0) ? target(s, light, u) : 0.0;
	reservoir_update(r, light, u, t * W * M, M, t);
}

// ---------------------------------------------------------------------------------------------------------------------

// Light candidates of a world position: the list of its grid cell (outside the grid: none); all lights if there is no
// grid.
void grid_list(vec3 p, out uint offset, out uint count, out bool global) {
	offset = 0u;
	global = params.grid_size.x == 0;
	if (global) {
		count = uint(params.grid_size.w);
		return;
	}
	vec3 c = floor((p - params.grid_origin.xyz) / params.grid_origin.w);
	ivec3 ci = ivec3(c);
	if (any(lessThan(ci, ivec3(0))) || any(greaterThanEqual(ci, params.grid_size.xyz))) {
		count = 0u;
		return;
	}
	uvec2 cell = grid_cells[(ci.z * params.grid_size.y + ci.y) * params.grid_size.x + ci.x];
	offset = cell.x;
	count = cell.y;
	global = false;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}
	rng_state = pcg(uint(pixel.x) + pcg(uint(pixel.y) + pcg(params.frame * 3u + push.pass)));

	Surface s = surface_from_buffers(pixel);

	if (push.pass == PASS_INITIAL) {
		if (!s.valid) {
			imageStore(surface_out, pixel, vec4(0.0, 0.0, 0.0, -1.0));
			imageStore(reservoir_out, pixel, uvec4(INVALID_LIGHT, 0u, 0u, 0u));
			return;
		}
		imageStore(surface_out, pixel, vec4(s.position, pack_normal(s.normal)));

		// Resampled importance sampling of the candidates.
		Reservoir r = reservoir_empty();
		uint offset, count;
		bool global;
		grid_list(s.position, offset, count, global);
		if (count > 0u) {
			float inv_pdf = float(count); // Uniform over the list, uniform u.
			for (uint i = 0u; i < params.candidates; i++) {
				uint k = min(uint(rand() * float(count)), count - 1u);
				uint light = global ? k : grid_indices[offset + k];
				vec2 u = vec2(rand(), rand());
				float t = target(s, light, u);
				reservoir_update(r, light, u, t * inv_pdf, 0.0, t);
			}
		}
		r.M = 1.0;
		float W = r.w_sum > 0.0 ? r.w_sum / (float(params.candidates) * r.target) : 0.0;
		r.w_sum = W * r.target; // Normalized: a reservoir of M = 1 with weight W.

		// Visibility of the chosen sample: an occluded sample is not passed on.
		if (r.light != INVALID_LIGHT && W > 0.0) {
			vec3 d, sp, sf, L;
			float dist;
			if (evaluate(s, r.light, r.u, d, sp, sf, L, dist) && !visible(s, L, dist)) {
				r.w_sum = 0.0;
			}
		}

		// Temporal reuse: the reservoir of this surface point in the previous frame.
		if (bool(params.flags & FLAG_TEMPORAL) && bool(params.flags & FLAG_HISTORY_VALID)) {
			vec4 clip = params.prev_world_to_clip * vec4(s.position, 1.0);
			if (clip.w > 0.0) {
				vec2 prev_uv = clip.xy / clip.w * 0.5 + 0.5;
				ivec2 prev_pixel = ivec2(floor(prev_uv * vec2(params.screen_size)));
				vec3 prev_position, prev_normal;
				if (all(greaterThanEqual(prev_pixel, ivec2(0))) && all(lessThan(prev_pixel, params.screen_size)) && surface_from_image(prev_pixel, prev_position, prev_normal)) {
					float scale = max(length(s.position - params.view_to_world[3].xyz), 1.0);
					if (abs(dot(prev_position - s.position, s.normal)) < 0.02 * scale && dot(prev_normal, s.normal) > 0.9) {
						uint light;
						vec2 u;
						float W_prev, M_prev;
						reservoir_unpack(imageLoad(reservoir_in, prev_pixel), light, u, W_prev, M_prev);
						light = light < params.prev_light_count ? prev_to_current[light] : INVALID_LIGHT;
						if (light != INVALID_LIGHT) {
							reservoir_merge(r, s, light, u, W_prev, min(M_prev, M_CAP * r.M));
						}
					}
				}
			}
		}
		imageStore(reservoir_out, pixel, reservoir_pack(r, reservoir_W(r)));
		return;
	}

	if (push.pass == PASS_SPATIAL) {
		uint light;
		vec2 u;
		float W, M;
		reservoir_unpack(imageLoad(reservoir_in, pixel), light, u, W, M);
		if (!s.valid || params.spatial_samples == 0u) {
			imageStore(reservoir_out, pixel, uvec4(light, packUnorm2x16(u), floatBitsToUint(W), floatBitsToUint(M)));
			return;
		}
		Reservoir r = reservoir_empty();
		reservoir_merge(r, s, light, u, W, M);
		float scale = max(length(s.position - params.view_to_world[3].xyz), 1.0);
		// Neighbours that took part (surface, M) for the normalization below.
		vec3 n_position[8];
		vec3 n_normal[8];
		float n_M[8];
		uint n_count = 0u;
		for (uint i = 0u; i < params.spatial_samples; i++) {
			float radius = params.spatial_radius * sqrt(rand());
			float phi = 2.0 * M_PI * rand();
			ivec2 q = pixel + ivec2(round(vec2(cos(phi), sin(phi)) * radius));
			if (q == pixel || any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.screen_size))) {
				continue;
			}
			vec3 qp, qn;
			if (!surface_from_image(q, qp, qn) || abs(dot(qp - s.position, s.normal)) > 0.02 * scale || dot(qn, s.normal) < 0.9) {
				continue;
			}
			uint ql;
			vec2 qu;
			float qW, qM;
			reservoir_unpack(imageLoad(reservoir_in, q), ql, qu, qW, qM);
			reservoir_merge(r, s, ql, qu, qW, qM);
			n_position[n_count] = qp;
			n_normal[n_count] = qn;
			n_M[n_count] = qM;
			n_count++;
		}
		// Normalization over the reservoirs that could have produced the chosen sample (Bitterli et al. 2020, Z): a
		// neighbour whose surface the sample does not reach (out of the light's range, behind the surface) does not
		// count. Plain 1/M darkens many short-range lights.
		float Z = r.light != INVALID_LIGHT ? M : 0.0;
		if (r.light != INVALID_LIGHT) {
			for (uint i = 0u; i < n_count; i++) {
				Surface ns;
				ns.position = n_position[i];
				ns.normal = n_normal[i];
				ns.view = normalize(params.view_to_world[3].xyz - ns.position);
				ns.roughness = s.roughness;
				ns.valid = true;
				if (target(ns, r.light, r.u) > 0.0) {
					Z += n_M[i];
				}
			}
		}
		float W_out = (r.light != INVALID_LIGHT && r.target > 0.0 && Z > 0.0) ? r.w_sum / (Z * r.target) : 0.0;
		imageStore(reservoir_out, pixel, reservoir_pack(r, W_out));
		return;
	}

	if (push.pass == PASS_SHADOW) {
		// Per light: visibility (1 lit, 0 occluded), occluder distance (0 lit, -1 facing away: no penumbra estimate).
		vec4 result = vec4(1.0, 0.0, 1.0, 0.0);
		if (push.late != 0u) {
			// Late opaque surfaces only: where the depth changed in the late depth pre-pass.
			if (s.valid && texelFetch(opaque_depth, pixel, 0).r == texelFetch(depth_buffer, pixel, 0).r) {
				s.valid = false;
			}
			imageStore(surface_out, pixel, s.valid ? vec4(s.position, pack_normal(s.normal)) : vec4(0.0, 0.0, 0.0, -1.0));
		}
		if (s.valid) {
			float scale = max(length(s.position - params.view_to_world[3].xyz), 1.0);
			for (uint i = 0u; i < min(shadow_params.info.x, 2u); i++) {
				vec4 sl = shadow_params.lights[i];
				vec3 axis = sl.xyz;
				vec3 t = normalize(cross(abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), axis));
				vec3 b = cross(axis, t);
				float r = sqrt(rand()) * sl.w;
				float phi = 2.0 * M_PI * rand();
				vec3 L = normalize(axis + (t * cos(phi) + b * sin(phi)) * r);
				float vis = 0.0;
				float hit = -1.0;
				if (dot(s.normal, L) > 0.0) {
					vec3 origin = s.position + s.normal * (0.002 * scale) + L * (0.001 * scale);
					rayQueryEXT q;
					// Not the first hit: the nearest occluder sizes the penumbra.
					rayQueryInitializeEXT(q, tlas, gl_RayFlagsSkipClosestHitShaderEXT, 0xFF, origin, 0.0, L, shadow_params.range.x);
					SHADOW_RAY_PROCEED(q);
					if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
						vis = 1.0;
						hit = 0.0;
					} else {
						hit = max(rayQueryGetIntersectionTEXT(q, true), 1e-3);
					}
				}
				result[i * 2u] = vis;
				result[i * 2u + 1u] = hit;
			}
		}
		imageStore(shadow_out, pixel, result);
		return;
	}

	// PASS_SHADE.
	vec4 diffuse_result = vec4(0.0);
	vec4 specular_result = vec4(0.0);
	if (s.valid) {
		uint light;
		vec2 u;
		float W, M;
		reservoir_unpack(imageLoad(reservoir_in, pixel), light, u, W, M);
		vec3 diffuse, specular, specular_fc, L;
		float dist;
		if (light != INVALID_LIGHT && W > 0.0 && evaluate(s, light, u, diffuse, specular, specular_fc, L, dist) && visible(s, L, dist)) {
			diffuse_result.rgb = diffuse * W;
			specular_result.rgb = specular * W;
			float ls = luminance(specular);
			specular_result.a = ls > 0.0 ? luminance(specular_fc) / ls : 0.0;
		}
	}
	imageStore(diffuse_out, pixel, diffuse_result);
	imageStore(specular_out, pixel, specular_result);
}
