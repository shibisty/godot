#[compute]

#version 460

#VERSION_DEFINES

// Mariner E5c: path tracer for Forward+ (photo mode / reference renders), ported from NVIDIA's Godot fork
// (NVIDIA-RTX/godot, nvidia-pt-dlss: scene_raytracing_raygen.glsl, raytracing_closest_hit_common_inc.glsl,
// raytracing_lights_inc.glsl, brdf_inc.glsl). Differences to the fork:
// - one compute shader with inline ray queries instead of a ray tracing pipeline (raygen + miss + hit groups), on the
//   TLAS of patch E5a; materials are the E5a material data (BaseMaterial3D parameters, ShaderMaterial parameters by
//   name) - custom spatial shader code is not compiled into hit groups;
// - progressive accumulation over frames (RGBA32F: radiance sum, primary hit coverage), composited over the sky that
//   the rasterizer drew before (primary rays that miss show the full resolution sky shader, secondary rays sample the
//   radiance map);
// - dielectric media (water): Fresnel reflection / refraction through a GGX interface, Beer-Lambert absorption,
//   single scattering of the sun and sky (analytic along each segment, sun visibility by one shadow ray), foam as a
//   rough white diffuse surface (vertex color alpha); optional per-pixel detail of the surface: wave slope maps sampled
//   at the undisplaced point (mesh UV) and a foam threshold pattern (RT_MAT_FLAG_WAVE_DETAIL / RT_MAT_FLAG_FOAM_PATTERN);
// - volumetric fog of the rasterizer (froxel volume of this frame) over the traced pixels, like the opaque pass;
// - no motion vectors, no DLSS Ray Reconstruction guide buffers, no procedural (AABB) geometry.

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_control_flow_attributes : enable

#define GLSL 1
#define MAX_VIEWS 2
#define PT_SCENE_SET 0

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// clang-format off
#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "pathtracer_scene_inc.glsl"
// clang-format on

layout(set = 0, binding = 3, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

#define PT_FLAG_TRACE (1u << 0) // Trace samples this frame (else only composite the accumulation).
#define PT_FLAG_FOG (1u << 1)
#define PT_FLAG_DEPTH_FOG (1u << 2)
#define PT_FLAG_DIELECTRICS (1u << 3) // The scene has dielectric (water) materials.
#define PT_FLAG_RADIANCE (1u << 4) // The radiance map is valid (sky background / reflections).
#define PT_FLAG_WRITE_DEPTH (1u << 5) // First frame of the accumulation: write the primary depth.
#define PT_FLAG_VOLUMETRIC_FOG (1u << 6) // Composite the volumetric fog of the frame over the traced pixels.

layout(set = 0, binding = 5, std140) uniform Params {
	uvec4 counts; // x: light count, y: frame index (RNG), z: samples accumulated before this frame, w: PT_FLAG_*.
	uvec4 config; // x: samples per pixel, y: max bounces, z: debug mode, w: index of the sun light (or 0xFFFFFFFF).
	vec4 config2; // x: firefly clamp (0 = off), y: scene ray length, z: 1 / volumetric fog length, w: its detail spread.
	vec4 ambient; // rgb: ambient light (no radiance map), a: unused.
	vec2 size;
	vec2 inv_size;
}
params;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 6) uniform texture2DArray radiance_octmap;
#else
layout(set = 0, binding = 6) uniform texture2D radiance_octmap;
#endif
layout(set = 0, binding = 7) uniform sampler radiance_sampler;
layout(set = 0, binding = 8) uniform sampler material_sampler; // Linear, mipmaps, repeat.
layout(set = 0, binding = 9) uniform sampler material_sampler_nearest;

layout(set = 0, binding = 10, rgba32f) uniform restrict image2D accum_image;
layout(set = 0, binding = 11, rgba16f) uniform restrict image2D color_image;
layout(set = 0, binding = 12, r32f) uniform restrict image2D depth_image;
layout(set = 0, binding = 13) uniform texture3D volumetric_fog_texture;

layout(set = 1, binding = 0) uniform texture2D rt_textures[];

// Medium state for shadow rays (read by raytracing_lights_inc.glsl).
bool pt_medium_inside = false;
vec3 pt_medium_extinction = vec3(0.0);
float pt_medium_level = 0.0;

// ============================================================================
// RNG and ray offsets (fork: raytracing_common_inc.glsl)
// ============================================================================

uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	uint seed = pixel.x + pixel.y * 65536u + frame * 1000000u + sample_idx * 100000000u;
	return pcg_hash(seed);
}

float rand(inout uint state) {
	state = pcg_hash(state);
	return float(state) / 4294967296.0;
}

vec2 rand2(inout uint state) {
	return vec2(rand(state), rand(state));
}

float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

// "A Fast and Robust Method for Avoiding Self-Intersection" (Ray Tracing Gems, ch. 6).
vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(_offset_component(p.x, n.x, of.x), _offset_component(p.y, n.y, of.y), _offset_component(p.z, n.z, of.z));
}

// ============================================================================
// Materials
// ============================================================================

vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags, float lod) {
	if ((mat_flags & MAT_FLAG_POINT_FILTER) != 0u) {
		return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler_nearest), uv, lod);
	}
	return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler), uv, lod);
}

bool pt_alpha_test(uint inst, uint prim, vec2 candidate_bary) {
	MaterialData mat = materials[inst];
	if (mat.alpha_scissor <= 0.0) {
		return true;
	}
	GeometryData geom = geometries[inst];
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, prim, i0, i1, i2);
	float alpha = mat.albedo_color.a;
	if (mat.albedo_texture_idx != 0u) {
		vec2 uv = fetch_uv(geom, i0, i1, i2, bary) * mat.uv1_scale + mat.uv1_offset;
		alpha *= sample_material_texture(mat.albedo_texture_idx, uv, mat.flags, 0.0).a;
	}
	if ((mat.flags & MAT_FLAG_USE_VERTEX_COLOR) != 0u) {
		alpha *= fetch_color(geom, i0, i1, i2, bary).a;
	}
	return alpha >= mat.alpha_scissor;
}

vec3 medium_absorption(MaterialData mat) {
	return max(mat.emission_color, vec3(0.0));
}

vec3 medium_scattering(MaterialData mat) {
	return max(vec3(mat.uv1_scale, mat.uv1_offset.x), vec3(0.0));
}

// B-spline bicubic of mip 0 (GPU Gems 2, ch. 20; as the sea shader of the game, W17): smooth slopes where a texel of a
// wave map covers many pixels, without the diamond pattern of bilinear filtering in reflections.
vec4 bspline_weights(float a) {
	float a2 = a * a;
	float a3 = a2 * a;
	return vec4(-a3 + 3.0 * a2 - 3.0 * a + 1.0, 3.0 * a3 - 6.0 * a2 + 4.0, -3.0 * a3 + 3.0 * a2 + 3.0 * a + 1.0, a3) / 6.0;
}

vec4 sample_material_bspline(uint tex_idx, vec2 uv) {
	vec2 dims = vec2(textureSize(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler), 0));
	vec2 inv = 1.0 / dims;
	vec2 st = uv * dims + 0.5;
	vec2 f = fract(st);
	vec4 wx = bspline_weights(f.x);
	vec4 wy = bspline_weights(f.y);
	vec4 g = vec4(wx.xz + wx.yw, wy.xz + wy.yw);
	vec4 h = (vec4(wx.yw, wy.yw) / g + vec2(-1.5, 0.5).xyxy + floor(st).xxyy) * inv.xxyy;
	vec2 w = g.xz / (g.xz + g.yw);
	return mix(mix(sample_material_texture(tex_idx, h.yw, 0u, 0.0), sample_material_texture(tex_idx, h.xw, 0u, 0.0), w.x),
			mix(sample_material_texture(tex_idx, h.yz, 0u, 0.0), sample_material_texture(tex_idx, h.xz, 0u, 0.0), w.x), w.y);
}

vec4 sample_wave_map(uint tex_idx, vec2 uv, bool smooth_filter) {
	return smooth_filter ? sample_material_bspline(tex_idx, uv) : sample_material_texture(tex_idx, uv, 0u, 0.0);
}

// Slope (dh/dx, dh/dz) of the wave maps of a dielectric at the undisplaced world point p (RT_MAT_FLAG_WAVE_DETAIL).
// Mip 0 everywhere: the accumulation over jittered samples filters the detail (supersampling).
vec2 wave_detail_slope(MaterialData mat, vec2 p, bool smooth_filter) {
	vec2 s = vec2(0.0);
	if (mat.metallic > 0.0) {
		s += sample_wave_map(mat.normal_texture_idx, p * mat.metallic, smooth_filter).xy;
	}
	if (mat.ao_strength > 0.0) {
		s += sample_wave_map(mat.normal_texture_idx, p * mat.ao_strength, smooth_filter).zw;
	}
	if (mat.orm_texture_idx != 0u) {
		if (mat.normal_map_depth > 0.0) {
			s += sample_wave_map(mat.orm_texture_idx, p * mat.normal_map_depth, smooth_filter).xy;
		}
		if (mat.emission_strength > 0.0) {
			s += sample_wave_map(mat.orm_texture_idx, p * mat.emission_strength, smooth_filter).zw;
		}
	}
	return s;
}

// Sum of two uniform threshold maps a * U1 + (1 - a) * U2 mapped back to a uniform value (trapezoid CDF), so that
// the covered fraction stays exact with two scales (no visible tiling of one map).
float uniform_sum2(float u1, float u2, float a) {
	float b = 1.0 - a;
	float x = a * u1 + b * u2;
	if (x < b) {
		return x * x / (2.0 * a * b);
	}
	if (x > a) {
		return 1.0 - (1.0 - x) * (1.0 - x) / (2.0 * a * b);
	}
	return (x - b * 0.5) / a;
}

// Foam density (0..1) of a dielectric with a foam pattern (RT_MAT_FLAG_FOAM_PATTERN) at the undisplaced world point p:
// the threshold map exceeds 1 - coverage on the covered fraction of the area; thin at the edge, dense at the core.
float foam_pattern_density(MaterialData mat, vec2 p, float coverage) {
	if (coverage < 0.002) {
		return 0.0;
	}
	vec2 sc = unpackHalf2x16(floatBitsToUint(mat.uv1_offset.y));
	float h = sample_material_texture(mat.albedo_texture_idx, p * sc.x, 0u, 0.0).r;
	if (sc.y > 0.0) {
		vec2 q = mat2(vec2(0.8, 0.6), vec2(-0.6, 0.8)) * p * sc.y;
		h = uniform_sum2(h, sample_material_texture(mat.albedo_texture_idx, q, 0u, 0.0).r, 0.68);
	}
	const float soft = 0.04;
	const float core = 0.15;
	float m = h - (1.0 - min(coverage, 1.0)) + soft * 0.5;
	return pow(clamp(m / (core + soft), 0.0, 1.0), 0.8);
}

#include "raytracing_lights_inc.glsl"

// ============================================================================
// Ray casting
// ============================================================================

RayHit trace_closest(vec3 origin, vec3 dir, float t_max) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsCullBackFacingTrianglesEXT, 0xFF, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			uint inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
			if ((materials[inst].flags & MAT_FLAG_DIELECTRIC) != 0u || pt_alpha_test(inst, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false))) {
				rayQueryConfirmIntersectionEXT(rq);
			}
		}
	}
	RayHit r;
	r.hit = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
	r.t = 0.0;
	r.instance = 0u;
	r.primitive = 0u;
	r.front_face = true;
	r.barycentrics = vec2(0.0);
	r.object_to_world = mat4x3(1.0);
	r.world_to_object = mat4x3(1.0);
	if (r.hit) {
		r.t = rayQueryGetIntersectionTEXT(rq, true);
		r.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
		r.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
		r.front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true);
		r.barycentrics = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		r.object_to_world = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
		r.world_to_object = rayQueryGetIntersectionWorldToObjectEXT(rq, true);
	}
	return r;
}

// Closest dielectric surface along a ray (other geometry ignored); used to find out if the camera is in the water.
RayHit trace_dielectric(vec3 origin, vec3 dir, float t_max) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			uint inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
			if ((materials[inst].flags & MAT_FLAG_DIELECTRIC) != 0u) {
				rayQueryConfirmIntersectionEXT(rq);
			}
		}
	}
	RayHit r;
	r.hit = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
	r.t = r.hit ? rayQueryGetIntersectionTEXT(rq, true) : 0.0;
	r.instance = r.hit ? uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) : 0u;
	r.primitive = r.hit ? uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)) : 0u;
	r.front_face = r.hit ? rayQueryGetIntersectionFrontFaceEXT(rq, true) : true;
	r.barycentrics = r.hit ? rayQueryGetIntersectionBarycentricsEXT(rq, true) : vec2(0.0);
	r.object_to_world = r.hit ? rayQueryGetIntersectionObjectToWorldEXT(rq, true) : mat4x3(1.0);
	r.world_to_object = r.hit ? rayQueryGetIntersectionWorldToObjectEXT(rq, true) : mat4x3(1.0);
	return r;
}

// ============================================================================
// Sky and fog
// ============================================================================

vec3 sky_radiance(vec3 dir, float roughness) {
	if ((params.counts.w & PT_FLAG_RADIANCE) == 0u) {
		return params.ambient.rgb;
	}
	// radiance_inverse_xform maps view space to sky space; mat3(inv_view_matrix) is world -> view (transposed rows).
	vec3 sky_dir = scene_data_block.data.radiance_inverse_xform * (mat3(scene_data_block.data.inv_view_matrix) * dir);
	vec2 uv = vec3_to_oct_with_border(sky_dir, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
	vec3 color;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float layer;
	float blend = modf(clamp(roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(uv, layer + 1.0), 0.0).rgb;
	color = mix(a, b, blend);
#else
	color = textureLod(sampler2D(radiance_octmap, radiance_sampler), uv, clamp(roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
#endif
	return color * scene_data_block.data.IBL_exposure_normalization;
}

// Environment fog of the primary segment (fork: fog_inc.glsl / fog_process), applied like the rasterizer.
vec4 primary_fog(vec3 hit_pos) {
	mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 vertex = (view_mat * vec4(hit_pos, 1.0)).xyz;
	vec3 fog_color = scene_data_block.data.fog_light_color;
	if (scene_data_block.data.fog_aerial_perspective > 0.0) {
		float mip_level = mix(1.0 / MAX_ROUGHNESS_LOD, 1.0, 1.0 - (abs(vertex.z) - scene_data_block.data.z_near) / (scene_data_block.data.z_far - scene_data_block.data.z_near));
		vec3 world_dir = normalize(hit_pos - transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)))[3].xyz);
		fog_color = mix(fog_color, sky_radiance(world_dir, mip_level), scene_data_block.data.fog_aerial_perspective);
	}
	if (scene_data_block.data.fog_sun_scatter > 0.001) {
		vec3 view = normalize(vertex);
		uint light_count = params.counts.x;
		for (uint i = 0u; i < light_count; i++) {
			if (rt_lights[i].type != RT_LIGHT_TYPE_DIRECTIONAL) {
				continue;
			}
			vec3 light_dir = mat3(view_mat) * -normalize(rt_lights[i].position);
			fog_color += rt_lights[i].emission * pow(max(dot(view, light_dir), 0.0), 8.0) * scene_data_block.data.fog_sun_scatter;
		}
	}
	float fog_amount;
	if ((params.counts.w & PT_FLAG_DEPTH_FOG) != 0u) {
		float fog_z = smoothstep(scene_data_block.data.fog_depth_begin, scene_data_block.data.fog_depth_end, length(vertex));
		fog_amount = pow(fog_z, scene_data_block.data.fog_depth_curve) * scene_data_block.data.fog_density;
	} else {
		fog_amount = 1.0 - exp(min(0.0, -length(vertex) * scene_data_block.data.fog_density));
	}
	if (abs(scene_data_block.data.fog_height_density) >= 0.0001) {
		float y_dist = hit_pos.y - scene_data_block.data.fog_height;
		fog_amount = max(1.0 - exp(min(0.0, y_dist * scene_data_block.data.fog_height_density)), fog_amount);
	}
	return vec4(fog_color, clamp(fog_amount, 0.0, 1.0));
}

// ============================================================================
// Water (dielectric medium)
// ============================================================================

float fresnel_dielectric(float cos_i, float eta) { // eta = n1 / n2
	float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
	if (sin2_t >= 1.0) {
		return 1.0;
	}
	float cos_t = sqrt(1.0 - sin2_t);
	float rs = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
	float rp = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
	return 0.5 * (rs * rs + rp * rp);
}

// Integral over s in [0, S] of exp(-k s) * exp(-c (d0 - dir_y s)): attenuation along the segment times the attenuation
// of light coming down from the surface to the depth at s (flat surface at the entry level, depth d0 at the start).
vec3 medium_integral(vec3 k, vec3 c, float d0, float dir_y, float S) {
	vec3 a = k - c * dir_y;
	vec3 start = exp(-c * max(d0, 0.0));
	vec3 r;
	for (int i = 0; i < 3; i++) {
		if (abs(a[i]) < 1e-4) {
			r[i] = S;
		} else if (S > 1e20) {
			r[i] = a[i] > 0.0 ? 1.0 / a[i] : 0.0;
		} else {
			r[i] = (1.0 - exp(-a[i] * S)) / a[i];
		}
	}
	return start * r;
}

// Single scattering of the sun and sky along a segment in the water and its transmittance (Beer-Lambert).
void medium_segment(vec3 origin, vec3 dir, float seg_len, uint mat_idx, float level, inout vec3 radiance, inout vec3 throughput, inout uint rng) {
	MaterialData mat = materials[mat_idx];
	vec3 sigma_s = medium_scattering(mat);
	vec3 sigma_t = medium_absorption(mat) + sigma_s;
	float ior = max(mat.specular, 1.0);
	float S = seg_len;
	if (S > 1e20 && dir.y > 0.0) {
		S = max(level - origin.y, 0.0) / max(dir.y, 1e-3); // Up to the surface level.
	}
	float d0 = level - origin.y;
	const float phase = 1.0 / (4.0 * PI); // Isotropic.
	vec3 scatter = vec3(0.0);

	uint sun = params.config.w;
	if (sun != 0xFFFFFFFFu) {
		RTLightData light = rt_lights[sun];
		vec3 l = -normalize(light.position); // To the sun.
		if (l.y > 0.0) {
			float mu_w = sqrt(max(1.0 - (1.0 - l.y * l.y) / (ior * ior), 0.01)); // Refracted sun angle cosine.
			float transmit = 1.0 - fresnel_dielectric(l.y, 1.0 / ior);
			vec3 sun_in = light.emission * transmit * phase * medium_integral(sigma_t, sigma_t / mu_w, d0, dir.y, S);
			// Visibility of the sun (hull, cargo) from one point of the segment.
			float s_max = min(S, 3.0 / max(min(sigma_t.r, min(sigma_t.g, sigma_t.b)), 1e-3));
			vec3 p = origin + dir * (rand(rng) * s_max);
			vec3 l_w = normalize(vec3(l.x / ior, mu_w, l.z / ior));
			// The shadow ray attenuates over its path to the surface; the integral already has that, keep occlusion only.
			pt_medium_inside = true;
			pt_medium_extinction = sigma_t;
			pt_medium_level = level;
			vec3 expected = exp(-sigma_t * max(level - p.y, 0.0) / mu_w);
			vec3 visibility = lights_trace_shadow_ray(p, l_w, params.config2.y, rng);
			sun_in *= min(visibility / max(expected, vec3(1e-6)), vec3(1.0));
			scatter += sun_in;
		}
	}
	// Diffuse sky light from above (average cosine ~0.83).
	vec3 sky_e = sky_radiance(vec3(0.0, 1.0, 0.0), 1.0) * PI * 0.92;
	scatter += sky_e * phase * medium_integral(sigma_t, sigma_t / 0.83, d0, dir.y, S);

	radiance += throughput * sigma_s * scatter;
	throughput *= S > 1e20 ? vec3(0.0) : exp(-sigma_t * S);
}

// ============================================================================
// Path tracing
// ============================================================================

#define MAX_DIFFUSE_BOUNCES 2u

float specular_to_f0(float specular) {
	return 0.16 * specular * specular;
}

void add_radiance(inout vec3 radiance, vec3 contribution, uint bounce) {
	float clamp_value = params.config2.x;
	if (clamp_value > 0.0 && bounce > 0u) {
		float lum = max(dot(contribution, vec3(0.2126, 0.7152, 0.0722)), 1e-6);
		contribution *= min(1.0, clamp_value / lum);
	}
	radiance += contribution;
}

struct CameraMedium {
	bool inside;
	uint mat;
	float level;
};

vec3 debug_color(uint mode, HitData h, MaterialData mat, vec3 albedo, vec3 normal, float roughness, float metallic, vec3 emissive, vec2 uv, float t) {
	if (mode == 2u) {
		return h.geometry_normal * 0.5 + 0.5;
	} else if (mode == 3u || mode == 12u) {
		return normal * 0.5 + 0.5;
	} else if (mode == 5u) {
		return h.tangent * 0.5 + 0.5;
	} else if (mode == 6u) {
		return h.bitangent * 0.5 + 0.5;
	} else if (mode == 7u) {
		return vec3(fract(uv), 0.0);
	} else if (mode == 9u) {
		return vec3(1.0, roughness, metallic);
	} else if (mode == 14u) {
		return vec3(metallic);
	} else if (mode == 15u) {
		return vec3(roughness);
	} else if (mode == 16u) {
		return normalize(mat3(scene_data_block.data.inv_view_matrix) * normal) * 0.5 + 0.5;
	} else if (mode == 18u) {
		return baseColorToSpecularF0(albedo, metallic, specular_to_f0(mat.specular));
	} else if (mode == 19u) {
		return h.is_front_face ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (mode == 20u) {
		return vec3(clamp(t / max(scene_data_block.data.z_far - scene_data_block.data.z_near, 1e-3), 0.0, 1.0));
	} else if (mode == 21u) {
		return emissive;
	}
	return albedo; // Albedo and the modes that need DLSS Ray Reconstruction data.
}

void primary_ray(vec2 pixel_pos, out vec3 origin, out vec3 dir) {
	vec2 ndc = pixel_pos * params.inv_size * 2.0 - 1.0;
	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
	// Reverse Z: 1 is the near plane, 0 the far plane.
	vec4 p_near = scene_data_block.data.inv_projection_matrix * vec4(ndc, 1.0, 1.0);
	vec4 p_far = scene_data_block.data.inv_projection_matrix * vec4(ndc, 0.0, 1.0);
	vec3 o = p_near.xyz / p_near.w;
	vec3 f = p_far.xyz / p_far.w;
	origin = (inv_view * vec4(o, 1.0)).xyz;
	dir = normalize(mat3(inv_view) * (f - o));
}

vec4 trace_path(uvec2 pixel, uint sample_idx, CameraMedium cam) {
	uint rng = init_rng(pixel, params.counts.y, sample_idx);
	vec3 origin, dir;
	primary_ray(vec2(pixel) + rand2(rng), origin, dir);

	const uint max_bounces = params.config.y;
	const uint debug_mode = params.config.z;
	const uint light_count = params.counts.x;
	const float ray_length = params.config2.y;

	vec3 radiance = vec3(0.0);
	vec3 throughput = vec3(1.0);
	float coverage = 0.0;
	uint total_bounces = 0u;
	uint diffuse_bounces = 0u;
	bool in_water = cam.inside;
	uint water_mat = cam.mat;
	float water_level = cam.level;

	[[dont_unroll]] for (uint event = 0u; event < max_bounces + 6u; event++) {
		RayHit r = trace_closest(origin, dir, ray_length);
		bool primary = event == 0u;

		if (in_water) {
			medium_segment(origin, dir, r.hit ? r.t : 1e30, water_mat, water_level, radiance, throughput, rng);
			if (primary) {
				coverage = 1.0;
			}
			if (!r.hit) {
				break;
			}
		}
		if (!r.hit) {
			if (!primary) {
				add_radiance(radiance, throughput * sky_radiance(dir, 0.0), total_bounces);
			}
			break; // Primary miss: the rasterized sky shows through (coverage 0).
		}
		if (primary) {
			coverage = 1.0;
		}

		HitData h = compute_hit_data(r, origin, dir);
		MaterialData mat = materials[h.geometry_idx];
		vec3 V = -dir;

		if (primary && !in_water && (params.counts.w & PT_FLAG_FOG) != 0u) {
			vec4 fog = primary_fog(h.hit_pos);
			radiance += throughput * fog.rgb * fog.a;
			throughput *= 1.0 - fog.a;
		}

		bool foam = false;
		if ((mat.flags & MAT_FLAG_DIELECTRIC) != 0u) {
			bool has_color = geometries[h.geometry_idx].color_byte_offset != OFFSET_NONE;
			float foam_amount = has_color ? h.color.a : 0.0;
			// Per-pixel detail: undisplaced world point of the hit (mesh UV = undisplaced object x, z).
			const bool detail = (mat.flags & (MAT_FLAG_WAVE_DETAIL | MAT_FLAG_FOAM_PATTERN)) != 0u;
			vec2 p_rest = detail ? (r.object_to_world * vec4(h.uv.x, 0.0, h.uv.y, 1.0)).xz : vec2(0.0);
			if (!in_water && (mat.flags & MAT_FLAG_FOAM_PATTERN) != 0u) {
				foam_amount = foam_pattern_density(mat, p_rest, foam_amount);
			}
			if (in_water || rand(rng) >= foam_amount) {
				// Interface: outward normal = vertex normal (up for the sea surface); it decides the side.
				vec3 n_out = h.vertex_normal;
				bool entering = dot(dir, n_out) < 0.0;
				vec3 n = entering ? n_out : -n_out;
				if ((mat.flags & MAT_FLAG_WAVE_DETAIL) != 0u) {
					// Per-pixel wave slopes (all maps, smooth on primary hits) + the extra slope of the vertex colors.
					vec2 s = wave_detail_slope(mat, p_rest, primary);
					if (has_color) {
						s = s * h.color.b + (h.color.rg * 2.0 - 1.0);
					}
					vec3 nd = normalize(vec3(-s.x, 1.0, -s.y) * mat3(r.world_to_object));
					nd = entering ? nd : -nd;
					// Keep it facing the ray (steep detail at grazing angles).
					float ndv = dot(nd, V);
					if (ndv < 0.02) {
						nd = normalize(nd + V * (0.02 - ndv));
					}
					n = nd;
				}
				if (primary && debug_mode != 0u) {
					return vec4(debug_color(debug_mode, h, mat, vec3(0.0), n, mat.roughness, 0.0, vec3(0.0), h.uv, r.t), 1.0);
				}
				float ior = max(mat.specular, 1.0);
				float eta = entering ? 1.0 / ior : ior;
				float roughness = clamp(mat.roughness, 0.0, 1.0);

				// Sun glints (reflection lobe), from above only.
				if (entering && light_count > 0u) {
					MaterialProperties glint;
					glint.baseColor = vec3(0.0);
					glint.metalness = 0.0;
					glint.roughness = max(roughness, 0.02);
					float f0 = (ior - 1.0) / (ior + 1.0);
					glint.dielectricF0 = f0 * f0;
					glint.emissive = vec3(0.0);
					glint.transmissivness = 0.0;
					glint.opacity = 1.0;
					pt_medium_inside = false;
					add_radiance(radiance, throughput * lights_evaluate_direct_lighting(offset_ray_origin(h.hit_pos, h.geometry_normal), n, V, glint, rng, diffuse_bounces > 0u, light_count), total_bounces);
				}

				vec3 m = n;
				if (roughness > 0.01) {
					float alpha = roughness * roughness;
					vec4 q = getRotationToZAxis(n);
					vec3 v_local = rotatePoint(q, V);
					if (v_local.z > 0.0) {
						m = normalize(rotatePoint(invertRotation(q), sampleGGXVNDF(v_local, vec2(alpha, alpha), rand2(rng))));
					}
				}
				float cos_i = clamp(dot(V, m), 0.0, 1.0);
				float F = fresnel_dielectric(cos_i, eta);
				vec3 new_dir;
				bool crossed = false;
				if (rand(rng) < F) {
					new_dir = reflect(dir, m);
					if (dot(new_dir, n) <= 0.0) {
						new_dir = reflect(dir, n);
					}
				} else {
					new_dir = refract(dir, m, eta);
					if (dot(new_dir, new_dir) < 0.5 || dot(new_dir, n) >= 0.0) {
						new_dir = reflect(dir, n); // Total internal reflection.
					} else {
						crossed = true;
					}
				}
				// Shading normals (vertex / wave detail) differ from the triangle: the medium changes only when the ray
				// really passes the triangle, a reflection stays on the side it came from.
				if (crossed && dot(new_dir, h.geometry_normal) >= 0.0) {
					crossed = false;
				} else if (!crossed && dot(new_dir, h.geometry_normal) <= 0.0) {
					new_dir = reflect(new_dir, h.geometry_normal);
				}
				if (crossed) {
					in_water = entering;
					if (entering) {
						water_mat = h.geometry_idx;
						water_level = h.hit_pos.y;
					}
				}
				total_bounces++;
				if (total_bounces > max_bounces + 4u) {
					break;
				}
				vec3 side = dot(new_dir, h.face_normal) > 0.0 ? h.face_normal : -h.face_normal;
				origin = offset_ray_origin(h.hit_pos, side);
				dir = new_dir;
				continue;
			}
			foam = true;
		}

		// Opaque surface (fork: HG0 StandardMaterial3D evaluation + shade_and_bounce).
		vec2 uv = h.uv * mat.uv1_scale + mat.uv1_offset;
		float lod = primary ? 0.0 : 2.0;
		vec3 albedo;
		float alpha;
		float roughness;
		float metallic;
		vec3 emissive = vec3(0.0);
		vec3 normal = h.shading_normal;
		vec3 ts_normal = vec3(0.0, 0.0, 1.0);
		float spec = mat.specular;
		if (foam) {
			albedo = mat.albedo_color.rgb;
			alpha = 1.0;
			roughness = 1.0;
			metallic = 0.0;
			spec = 0.5;
		} else {
			vec4 albedo_tex = mat.albedo_texture_idx != 0u ? sample_material_texture(mat.albedo_texture_idx, uv, mat.flags, lod) : vec4(1.0);
			albedo = albedo_tex.rgb * mat.albedo_color.rgb;
			alpha = albedo_tex.a * mat.albedo_color.a;
			if ((mat.flags & MAT_FLAG_USE_VERTEX_COLOR) != 0u) {
				albedo *= h.color.rgb;
			}
			vec3 orm = mat.orm_texture_idx != 0u ? sample_material_texture(mat.orm_texture_idx, uv, mat.flags, lod).rgb : vec3(1.0);
			roughness = clamp(orm.g * mat.roughness, 0.0, 1.0);
			metallic = clamp(orm.b * mat.metallic, 0.0, 1.0);
			emissive = mat.emission_color * mat.emission_strength;
			if ((mat.flags & MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
				emissive *= sample_material_texture(mat.emission_texture_idx, uv, mat.flags, lod).rgb;
			}
			emissive *= scene_data_block.data.emissive_exposure_normalization;
			if ((mat.flags & MAT_FLAG_HAS_NORMAL_MAP) != 0u) {
				vec3 ns = sample_material_texture(mat.normal_texture_idx, uv, mat.flags, lod).rgb;
				ts_normal.xy = ns.xy * 2.0 - 1.0;
				ts_normal.z = sqrt(max(0.0, 1.0 - dot(ts_normal.xy, ts_normal.xy)));
				normal = apply_normal_map(h, ts_normal, mat.normal_map_depth);
			}
		}

		if (primary && debug_mode != 0u) {
			if (debug_mode == 4u) {
				return vec4(ts_normal * 0.5 + 0.5, 1.0);
			}
			return vec4(debug_color(debug_mode, h, mat, albedo, normal, roughness, metallic, emissive, uv, r.t), 1.0);
		}

		if ((mat.flags & MAT_FLAG_UNSHADED) != 0u) {
			add_radiance(radiance, throughput * albedo, total_bounces);
			break;
		}

		vec3 N = clampShadingNormal(normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		add_radiance(radiance, throughput * emissive, total_bounces);

		if (total_bounces >= max_bounces || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
			break;
		}

		MaterialProperties brdf_mat;
		brdf_mat.baseColor = albedo;
		brdf_mat.metalness = metallic;
		brdf_mat.roughness = roughness;
		brdf_mat.dielectricF0 = specular_to_f0(spec);
		brdf_mat.emissive = emissive;
		brdf_mat.transmissivness = 0.0;
		brdf_mat.opacity = 1.0;

		// NEE (direct light sampling).
		if (light_count > 0u) {
			pt_medium_inside = in_water;
			if (in_water) {
				MaterialData wm = materials[water_mat];
				pt_medium_extinction = medium_absorption(wm) + medium_scattering(wm);
				pt_medium_level = water_level;
			}
			vec3 direct = lights_evaluate_direct_lighting(offset_ray_origin(h.hit_pos, h.geometry_normal), N, V, brdf_mat, rng, diffuse_bounces > 0u, light_count);
			add_radiance(radiance, throughput * direct, total_bounces);
		}

		// BRDF importance sampling of the next bounce.
		vec3 specular_f0 = baseColorToSpecularF0(brdf_mat.baseColor, brdf_mat.metalness, brdf_mat.dielectricF0);
		vec3 diffuse_reflectance = baseColorToDiffuseReflectance(brdf_mat.baseColor, brdf_mat.metalness);
		float specular_lum = luminance(specular_f0);
		float diffuse_lum = luminance(diffuse_reflectance);
		int brdf_type;
		if (diffuse_lum < 0.0001) {
			brdf_type = SPECULAR_TYPE;
		} else if (specular_lum < 0.0001) {
			brdf_type = DIFFUSE_TYPE;
		} else {
			float p = clamp(specular_lum / (specular_lum + diffuse_lum), 0.01, 0.99);
			if (rand(rng) < p) {
				brdf_type = SPECULAR_TYPE;
				throughput /= p;
			} else {
				brdf_type = DIFFUSE_TYPE;
				throughput /= (1.0 - p);
			}
		}
		vec3 next_dir;
		vec3 brdf_weight;
		if (!evalIndirectCombinedBRDF(rand2(rng), N, h.geometry_normal, V, brdf_mat, brdf_type, next_dir, brdf_weight, vec4(0.0))) {
			vec3 recovered_dir;
			if (luminance(brdf_weight) == 0.0 || !recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
				break;
			}
			next_dir = recovered_dir;
		}
		throughput *= brdf_weight;
		total_bounces++;
		if (brdf_type == DIFFUSE_TYPE) {
			diffuse_bounces++;
		}
		// Russian roulette after a few bounces.
		if (total_bounces >= 3u) {
			float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
			if (rand(rng) >= survive) {
				break;
			}
			throughput /= survive;
		}
		origin = offset_ray_origin(h.hit_pos, h.geometry_normal);
		dir = next_dir;
	}

	if (any(isnan(radiance)) || any(isinf(radiance))) {
		radiance = vec3(0.0);
	}
	return vec4(radiance, coverage);
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(vec2(pixel), params.size))) {
		return;
	}
	ivec2 ipixel = ivec2(pixel);
	const uint flags = params.counts.w;
	const uint base_samples = params.counts.z;
	const uint spp = (flags & PT_FLAG_TRACE) != 0u ? params.config.x : 0u;

	vec4 accum = base_samples > 0u ? imageLoad(accum_image, ipixel) : vec4(0.0);

	if (spp > 0u) {
		CameraMedium cam;
		cam.inside = false;
		cam.mat = 0u;
		cam.level = 0.0;
		if ((flags & PT_FLAG_DIELECTRICS) != 0u) {
			// Camera in the water: the closest water surface above it is seen from below.
			vec3 cam_pos = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)))[3].xyz;
			RayHit up = trace_dielectric(cam_pos, vec3(0.0, 1.0, 0.0), params.config2.y);
			if (up.hit) {
				HitData uh = compute_hit_data(up, cam_pos, vec3(0.0, 1.0, 0.0));
				if (uh.vertex_normal.y > 0.0) {
					cam.inside = true;
					cam.mat = uh.geometry_idx;
					cam.level = uh.hit_pos.y;
				}
			}
		}

		if ((flags & PT_FLAG_WRITE_DEPTH) != 0u) {
			vec3 o, d;
			primary_ray(vec2(pixel) + 0.5, o, d);
			RayHit r = trace_closest(o, d, params.config2.y);
			float depth = 0.0;
			if (r.hit) {
				mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
				vec4 clip = scene_data_block.data.projection_matrix * (view_mat * vec4(o + d * r.t, 1.0));
				depth = clamp(clip.z / clip.w, 0.0, 1.0);
			}
			imageStore(depth_image, ipixel, vec4(depth));
		}

		[[dont_unroll]] for (uint s = 0u; s < spp; s++) {
			accum += trace_path(pixel, base_samples + s, cam);
		}
		imageStore(accum_image, ipixel, accum);
	}

	float n = float(max(base_samples + spp, 1u));
	vec4 background = imageLoad(color_image, ipixel);
	vec3 traced = accum.rgb / n;
	float coverage = clamp(accum.a / n, 0.0, 1.0);
	if ((flags & PT_FLAG_VOLUMETRIC_FOG) != 0u && coverage > 0.0) {
		// Volumetric fog at the depth of the central primary ray, blended like the opaque pass of the rasterizer
		// (scene_forward_clustered.glsl, volumetric_fog_process: rgb in-scattered light, a transmittance).
		float depth = imageLoad(depth_image, ipixel).r;
		if (depth > 0.0) {
			vec2 uv = (vec2(pixel) + 0.5) * params.inv_size;
			vec4 vp = scene_data_block.data.inv_projection_matrix * vec4(uv * 2.0 - 1.0, depth, 1.0);
			vec3 fog_pos = vec3(uv, (-vp.z / vp.w) * params.config2.z);
			if (fog_pos.z >= 0.0) {
				if (fog_pos.z < 1.0) {
					fog_pos.z = pow(fog_pos.z, params.config2.w);
				}
				vec4 vf = textureLod(sampler3D(volumetric_fog_texture, radiance_sampler), fog_pos, 0.0);
				traced = traced * vf.a + vf.rgb * coverage;
			}
		}
	}
	vec3 color = traced + (1.0 - coverage) * background.rgb;
	imageStore(color_image, ipixel, vec4(color, background.a));
}
