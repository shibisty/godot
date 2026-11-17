#[compute]

#version 460

#VERSION_DEFINES

// Mariner E12: DDGI probe rays. One invocation per ray of a probe (x: ray, y: probe slot), inline ray query on the
// raytracing scene (TLAS, patch E5a). A hit is shaded with the direct light of the scene lights (shadow rays) and the
// irradiance of the probes of the previous frame (multiple bounces); a miss sees the sky as the ambient light of the
// rasterizer does (ambient color / sky contribution / energy). Writes the radiance and the hit distance (negative for
// back faces: the probe sees the inside of a closed mesh) for ddgi_update.glsl.

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RT_NONUNIFORM_WATERFALL
#extension GL_KHR_shader_subgroup_ballot : require
#endif
#extension GL_EXT_control_flow_attributes : enable

#define GLSL 1
#define MAX_VIEWS 2
#define PT_SCENE_SET 0

#if defined(MODE_SCREEN_PROBES) || defined(MODE_REFLECTIONS)
// Mariner L1: the rays of the screen space probes (x, y: texel of the probe radiance map, 8x8 per probe).
// Mariner L2: the reflection rays (x, y: half resolution texel).
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#else
layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
#endif

// clang-format off
#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"
#include "../ddgi_data_inc.glsl"
#include "brdf_inc.glsl"
#include "pathtracer_scene_inc.glsl"
// clang-format on

layout(set = 0, binding = 3, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

// (binding 4: lights, raytracing_lights_inc.glsl)

layout(set = 0, binding = 5, std140) uniform DDGIBlock {
	DDGIData volumes[DDGI_VOLUME_COUNT];
}
ddgi_block;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 6) uniform texture2DArray radiance_octmap;
#else
layout(set = 0, binding = 6) uniform texture2D radiance_octmap;
#endif
layout(set = 0, binding = 7) uniform sampler linear_clamp_sampler;
layout(set = 0, binding = 8) uniform sampler material_sampler; // Linear, mipmaps, repeat.
layout(set = 0, binding = 9) uniform sampler material_sampler_nearest;

#if defined(MODE_SCREEN_PROBES) || defined(MODE_REFLECTIONS)
layout(set = 0, binding = 10, rgba16f) uniform restrict writeonly image2D trace_image; // Radiance (rgb), traced (a).
#else
layout(set = 0, binding = 10, std430) restrict writeonly buffer RayBuffer {
	uvec4 rays[]; // packHalf2x16: sky radiance rg, (sky b, signed distance), lights radiance rg, (lights b, 0).
}
ray_buffer;
#endif

// Both grids (previous frame) for the bounce light at hits.
layout(set = 0, binding = 11) uniform texture2D ddgi_irradiance_world;
layout(set = 0, binding = 12) uniform texture2D ddgi_distance_world;
layout(set = 0, binding = 13) uniform texture2D ddgi_probe_data_world;
layout(set = 0, binding = 16) uniform texture2D ddgi_irradiance_local;
layout(set = 0, binding = 17) uniform texture2D ddgi_distance_local;
layout(set = 0, binding = 18) uniform texture2D ddgi_probe_data_local;
layout(set = 0, binding = 19) uniform texture2D ddgi_irradiance_lights_world;
layout(set = 0, binding = 20) uniform texture2D ddgi_irradiance_lights_local;

#if defined(MODE_SCREEN_PROBES) || defined(MODE_REFLECTIONS)
#ifdef MODE_REFLECTIONS
layout(set = 0, binding = 14) uniform texture2D depth_buffer;
layout(set = 0, binding = 21) uniform texture2D normal_roughness_buffer;
#else
layout(set = 0, binding = 14, std430) restrict readonly buffer ScreenProbes {
	DDGIScreenProbe probes[];
}
screen_probes;
#endif

// The lit color of the previous frame (rgb) and its linear depth (a, 0: none), for hits that were on screen.
layout(set = 0, binding = 15) uniform texture2D last_color;
#else
layout(set = 0, binding = 14, std430) restrict readonly buffer ProbeStateBuffer {
	DDGIProbeState probe_state[];
}
probe_buffer;

layout(set = 0, binding = 15, std430) restrict readonly buffer SlotList {
	uint slots[]; // Probe slots updated this frame.
}
slot_list;
#endif

layout(set = 1, binding = 0) uniform texture2D rt_textures[];

#define DDGI_TRACE_FLAG_RADIANCE 1u // The radiance map is valid.
#define DDGI_TRACE_FLAG_MULTI_BOUNCE 2u // The probes of the previous frame are valid.
#define DDGI_TRACE_FLAG_LAST_COLOR 4u // Screen probes: last_color holds the previous frame.

#ifdef MODE_REFLECTIONS
layout(push_constant, std430) uniform Params {
	uint light_count;
	uint flags;
	float ray_length;
	uint grid_x; // Half resolution width.
	uint grid_y; // Half resolution height.
	uint frame_index;
	float max_roughness;
	uint pad0;
}
params;
#elif defined(MODE_SCREEN_PROBES)
layout(push_constant, std430) uniform Params {
	uint light_count;
	uint flags;
	float ray_length;
	uint grid_x; // Probes across.
	uint grid_y; // Probes down.
	uint frame_index;
	uint pad0;
	uint pad1;
}
params;
#else
layout(push_constant, std430) uniform Params {
	uint light_count;
	uint flags;
	float ray_length;
	uint slot_count;
	uint volume; // DDGI_VOLUME_* traced by this dispatch.
	uint pad0;
	uint pad1;
	uint pad2;
}
params;
#endif

#define DDGI_VOLUMES ddgi_block.volumes
#define DDGI_IRRADIANCE_0 ddgi_irradiance_world
#define DDGI_DISTANCE_0 ddgi_distance_world
#define DDGI_PROBE_DATA_0 ddgi_probe_data_world
#define DDGI_IRRADIANCE_1 ddgi_irradiance_local
#define DDGI_DISTANCE_1 ddgi_distance_local
#define DDGI_PROBE_DATA_1 ddgi_probe_data_local
#define DDGI_IRRADIANCE_LIGHTS_0 ddgi_irradiance_lights_world
#define DDGI_IRRADIANCE_LIGHTS_1 ddgi_irradiance_lights_local
#define DDGI_SAMPLER linear_clamp_sampler
#include "../ddgi_sample_inc.glsl"

// Read by raytracing_lights_inc.glsl (no medium for probe rays).
bool pt_medium_inside = false;
vec3 pt_medium_extinction = vec3(0.0);
float pt_medium_level = 0.0;

uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
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

vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags, float lod) {
#ifdef RT_NONUNIFORM_WATERFALL
	// Mariner E5d: one texture index of the wave at a time (rt_load(), pathtracer_scene_inc.glsl).
	vec4 value = vec4(0.0);
	for (;;) {
		uint idx = subgroupBroadcastFirst(tex_idx);
		if (idx == tex_idx) {
			if ((mat_flags & MAT_FLAG_POINT_FILTER) != 0u) {
				value = textureLod(sampler2D(rt_textures[idx], material_sampler_nearest), uv, lod);
			} else {
				value = textureLod(sampler2D(rt_textures[idx], material_sampler), uv, lod);
			}
			break;
		}
	}
	return value;
#else
	if ((mat_flags & MAT_FLAG_POINT_FILTER) != 0u) {
		return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler_nearest), uv, lod);
	}
	return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler), uv, lod);
#endif
}

// Per ray state for pt_alpha_test() (DG-F): seed of the stochastic transmission, camera ray (stand-ins skipped).
uint pt_transmission_seed = 0u;
bool pt_camera_ray = false;

bool pt_alpha_test(uint inst, uint prim, vec2 candidate_bary) {
	// Mariner E12 DG-F: stand-ins only for rays are invisible to camera rays; partly transparent instances
	// (canopies, nets) let a share of the rays through, decided per ray and triangle.
	GeometryData g = geometries[inst];
	if (pt_camera_ray && (g.flags & GEOM_FLAG_RAYTRACING_ONLY) != 0u) {
		return false;
	}
	uint transmission = g.layer_mask >> GEOM_TRANSMISSION_SHIFT;
	if (transmission != 0u && (pcg_hash(pt_transmission_seed ^ (inst * 0x9E3779B9u) ^ (prim * 0x85EBCA6Bu)) >> 24u) < transmission) {
		return false;
	}
	MaterialData mat = materials[inst];
	if (mat.alpha_scissor <= 0.0) {
		return true;
	}
	GeometryData geom = g;
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, prim, i0, i1, i2);
	float alpha = mat.albedo_color.a;
	uint alpha_tex = rt_alpha_texture(mat); // Mariner AA2: or a distance-field mask.
	if (alpha_tex != 0u) {
		vec2 uv = fetch_uv(geom, i0, i1, i2, bary) * mat.uv1_scale + mat.uv1_offset;
		alpha *= rt_alpha_value(mat, sample_material_texture(alpha_tex, uv, mat.flags, 0.0));
	}
	if ((mat.flags & MAT_FLAG_USE_VERTEX_COLOR) != 0u) {
		alpha *= fetch_color(geom, i0, i1, i2, bary).a;
	}
	return alpha >= mat.alpha_scissor;
}

#include "raytracing_lights_inc.glsl"

// Closest hit, back faces included (they mark probes inside geometry).
RayHit trace_closest_two_sided(vec3 origin, vec3 dir, float t_max) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, dir, t_max);
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

vec3 sky_radiance(vec3 dir, float roughness) {
	if ((params.flags & DDGI_TRACE_FLAG_RADIANCE) == 0u) {
		return vec3(0.0);
	}
	// radiance_inverse_xform maps view space to sky space; mat3(inv_view_matrix) is world -> view (transposed rows).
	vec3 sky_dir = scene_data_block.data.radiance_inverse_xform * (mat3(scene_data_block.data.inv_view_matrix) * dir);
	vec2 uv = vec3_to_oct_with_border(sky_dir, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
	vec3 color;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	float layer;
	float blend = modf(clamp(roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, linear_clamp_sampler), vec3(uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, linear_clamp_sampler), vec3(uv, min(layer + 1.0, MAX_ROUGHNESS_LOD)), 0.0).rgb;
	color = mix(a, b, blend);
#else
	color = textureLod(sampler2D(radiance_octmap, linear_clamp_sampler), uv, clamp(roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
#endif
	return color * scene_data_block.data.IBL_exposure_normalization;
}

// Light the rasterizer's ambient would give from a direction (ambient source, color, sky contribution, energy).
vec3 ambient_radiance(vec3 dir, float roughness) {
	const uint flags = scene_data_block.data.flags;
	if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) == 0u) {
		return vec3(0.0);
	}
	vec3 ambient = scene_data_block.data.ambient_light_color_energy.rgb;
	if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP) != 0u) {
		ambient = mix(ambient, sky_radiance(dir, roughness) * scene_data_block.data.ambient_light_color_energy.a, scene_data_block.data.ambient_color_sky_mix);
	}
	return ambient;
}

float schlick_fresnel(float cos_i, float f0) {
	float m = clamp(1.0 - cos_i, 0.0, 1.0);
	float m2 = m * m;
	return f0 + (1.0 - f0) * m2 * m2 * m;
}

// Lambert direct light of the scene lights at a surface point (all directional lights and the positional lights in
// range, one shadow ray each; at most 16 lights per point).
vec3 direct_diffuse(vec3 p, vec3 n) {
	// No continue in this loop: vkd3d-proton's DXIL -> SPIR-V made invalid structured control flow of it (D3D12 on
	// the lavapipe stand). Each light is evaluated under one condition instead.
	vec3 sum = vec3(0.0);
	uint evaluated = 0u;
	uint rng = 0u;
	vec3 origin = offset_ray_origin(p, n);
	for (uint i = 0u; i < params.light_count && evaluated < 16u; i++) {
		RTLightData light = rt_lights[i];
		vec3 l;
		float atten = 1.0;
		float max_dist = params.ray_length;
		bool in_range = true;
		if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
			l = -normalize(light.position);
		} else {
			vec3 to_light = light.position - p;
			float d2 = dot(to_light, to_light);
			in_range = light.max_range_squared == 0.0 || d2 <= light.max_range_squared;
			float d = sqrt(d2);
			l = to_light / max(d, 1e-4);
			max_dist = d;
			LightSample ls;
			ls.distance_sq = d2;
			atten = in_range ? lights_get_attenuation(ls, light.inv_max_range, light.attenuation) : 0.0;
			if (light.type == RT_LIGHT_TYPE_SPOT) {
				float scos = dot(-l, light.spot_direction);
				float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
				atten = scos <= light.cos_spot_angle ? 0.0 : atten * (1.0 - pow(spot_rim, light.inv_spot_attenuation));
			}
		}
		float ndl = dot(n, l);
		if (in_range && ndl > 0.0 && atten > 0.0) {
			evaluated++;
			vec3 visibility = lights_trace_shadow_ray(origin, l, max_dist, rng);
			// Light emission is energy * PI (rasterizer convention), Lambert is albedo / PI.
			sum += light.emission * light.indirect_energy * (ndl * atten / PI) * visibility;
		}
	}
	return sum;
}

// Light a surface hit sends back along the ray: the sky channel (bounced ambient / probes) and the lights channel
// (direct light of the scene lights, emission, bounced light of the lights channel). r_albedo: the surface colour.
void shade_surface(HitData h, MaterialData mat, vec3 dir, out vec3 radiance, out vec3 radiance_lights, out vec3 r_albedo) {
	vec2 uv = h.uv * mat.uv1_scale + mat.uv1_offset;
	const float lod = 3.0;
	vec3 albedo = mat.albedo_color.rgb;
	if (mat.albedo_texture_idx != 0u) {
		albedo *= sample_material_texture(mat.albedo_texture_idx, uv, mat.flags, lod).rgb;
	}
	if ((mat.flags & MAT_FLAG_USE_VERTEX_COLOR) != 0u) {
		albedo *= h.color.rgb;
	}
	float metallic = mat.metallic;
	if (mat.orm_texture_idx != 0u) {
		metallic *= sample_material_texture(mat.orm_texture_idx, uv, mat.flags, lod).b;
	}
	vec3 emissive = mat.emission_color * mat.emission_strength;
	if ((mat.flags & MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
		emissive *= sample_material_texture(mat.emission_texture_idx, uv, mat.flags, lod).rgb;
	}
	emissive *= scene_data_block.data.emissive_exposure_normalization;
	vec3 diffuse_albedo = albedo * (1.0 - clamp(metallic, 0.0, 1.0));
	vec3 n = h.shading_normal;

	radiance = vec3(0.0);
	radiance_lights = emissive;
	if ((mat.flags & MAT_FLAG_UNSHADED) != 0u) {
		radiance_lights = albedo;
	} else {
		vec3 light = direct_diffuse(h.hit_pos, n);
		vec3 indirect = ambient_radiance(n, 1.0);
		vec3 indirect_lights = vec3(0.0);
		if ((params.flags & DDGI_TRACE_FLAG_MULTI_BOUNCE) != 0u) {
			// Inside the grid the probes give the bounce light, also while they have none yet (they start dark
			// and gather the light bounce by bounce, instead of starting at the sky light and taking many
			// frames to lose it indoors); outside it, the sky / ambient light. Each channel bounces in itself.
			float amount;
			indirect = ddgi_apply(indirect, h.hit_pos, n, -dir, true, amount, indirect_lights);
		}
		radiance = diffuse_albedo * indirect;
		radiance_lights += diffuse_albedo * (light + indirect_lights);
	}
	r_albedo = albedo;
}

// Mariner DG-M: a direction scattered by leaves - any direction, more often onward than back.
vec3 scatter_direction(vec3 dir, inout uint rng) {
	vec2 u = rand2(rng);
	float z = 1.0 - 2.0 * u.x;
	float r = sqrt(max(1.0 - z * z, 0.0));
	float phi = 6.28318530718 * u.y;
	vec3 v = vec3(r * cos(phi), r * sin(phi), z);
	if (dot(v, dir) < 0.0 && rand(rng) < 0.5) {
		v = -v;
	}
	return v;
}

#if defined(MODE_SCREEN_PROBES) || defined(MODE_REFLECTIONS)
// Mariner L1: a hit that was on screen in the previous frame takes the lit color there (all lights, bounces, the sky
// and emission as the rasterizer drew them; one more bounce each frame through the probes): the hit point projected
// into the previous frame, accepted where the depth there is the hit's own (not hidden then, not a different surface).
bool sp_has_reused = false;
vec3 sp_reused = vec3(0.0);

bool screen_reuse(vec3 p, vec3 n, vec3 dir) {
	if ((params.flags & DDGI_TRACE_FLAG_LAST_COLOR) == 0u || dot(n, -dir) <= 0.0) {
		return false;
	}
	vec4 view = ddgi_affine_to_mat4(scene_data_block.prev_data.view_matrix) * vec4(p, 1.0);
	vec4 clip = scene_data_block.prev_data.projection_matrix * view;
	if (clip.w <= 1e-5) {
		return false;
	}
	vec2 ndc = clip.xy / clip.w;
	if (any(greaterThan(abs(ndc), vec2(0.999)))) {
		return false;
	}
	ivec2 size = textureSize(sampler2D(last_color, material_sampler_nearest), 0);
	ivec2 px = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1);
	vec4 last = texelFetch(sampler2D(last_color, material_sampler_nearest), px, 0);
	float depth = -view.z;
	if (last.a <= 0.0 || abs(last.a - depth) > 0.02 * depth + 0.05) {
		return false;
	}
	sp_reused = last.rgb;
	sp_has_reused = true;
	return true;
}
#endif

// The light along a ray (the DDGI probe rays and the screen probe rays): sky channel (r_radiance), lights channel
// (r_radiance_lights) and the signed hit distance (negative: the inside of closed geometry).
void trace_radiance(DDGIData d, vec3 origin, vec3 dir, out vec3 radiance, out vec3 radiance_lights, out float dist) {
	radiance_lights = vec3(0.0);
	RayHit r = trace_closest_two_sided(origin, dir, params.ray_length);

	// DG4: the sea as an endless plane, where it is closer than the scene's geometry (the ocean mesh, if it is in the
	// raytracing scene at all, ends near the camera; beyond it rays would see the sky below the horizon).
	float sea_t = -1.0;
	bool under_sea = false;
	if ((d.grid_min.w & DDGI_FLAG_SEA) != 0 && origin.y < d.sea.a && !r.hit && dir.y <= 0.0) {
		// A probe under the sea level (outside closed hulls), looking down into the water: like the inside of geometry,
		// so that the probe counts as inactive (no sky light from below the sea) or moves up out of the water.
		under_sea = true;
	} else if ((d.grid_min.w & DDGI_FLAG_SEA) != 0 && abs(dir.y) > 1e-4) {
		float t = (d.sea.a - origin.y) / dir.y;
		if (t > 0.0 && t < params.ray_length && (!r.hit || t < r.t)) {
			sea_t = t;
		}
	}
	if (under_sea) {
		radiance = vec3(0.0);
		dist = -params.ray_length * 0.2;
	} else if (sea_t > 0.0) {
		if (dir.y > 0.0) {
			// From below the sea level (outside closed geometry): the probe is under water, like inside geometry.
			radiance = vec3(0.0);
			dist = -sea_t * 0.2;
		} else {
			// Fresnel reflection of the ambient light over the light scattered out of the water body (sea color x the
			// light from above, the direct part in the lights channel with ray traced shadows of hulls and coasts).
			const vec3 n = vec3(0.0, 1.0, 0.0);
			const float f0 = 0.02; // Water, IOR 1.33.
			float fresnel = schlick_fresnel(max(-dir.y, 0.0), f0);
			vec3 p = origin + dir * sea_t;
			radiance = ambient_radiance(reflect(dir, n), 0.2) * fresnel + (1.0 - fresnel) * d.sea.rgb * ambient_radiance(n, 1.0);
			radiance_lights = (1.0 - fresnel) * d.sea.rgb * direct_diffuse(p, n);
			dist = sea_t;
		}
	} else if (!r.hit) {
		radiance = ambient_radiance(dir, 0.5); // Prefiltered a little: a sun disk in the sky would flicker.
		dist = 1e4;
	} else {
		HitData h = compute_hit_data(r, origin, dir);
		MaterialData mat = materials[h.geometry_idx];
		if (!r.front_face && mat.cull_mode != 0u && (mat.flags & MAT_FLAG_DIELECTRIC) == 0u) {
			// Back face of a one-sided surface: the probe is inside geometry (counted by the update pass).
			radiance = vec3(0.0);
			dist = -r.t * 0.2;
		} else if ((mat.flags & MAT_FLAG_DIELECTRIC) != 0u) {
			// Water: the reflected ambient (Fresnel) over the light scattered out of the water body.
			vec3 n = h.geometry_normal;
			float ior = max(mat.specular, 1.0);
			float f0 = (ior - 1.0) / (ior + 1.0);
			float fresnel = schlick_fresnel(max(dot(-dir, n), 0.0), f0 * f0);
			vec3 sigma_s = max(vec3(mat.uv1_scale, mat.uv1_offset.x), vec3(0.0));
			vec3 sigma_t = max(mat.emission_color, vec3(0.0)) + sigma_s;
			vec3 body = sigma_s / max(sigma_t, vec3(1e-4)) * 0.25; // Single scattering albedo of the medium, rough.
			radiance = ambient_radiance(reflect(dir, n), 0.2) * fresnel + (1.0 - fresnel) * body * ambient_radiance(vec3(0.0, 1.0, 0.0), 1.0);
			radiance_lights = (1.0 - fresnel) * body * direct_diffuse(h.hit_pos, vec3(0.0, 1.0, 0.0));
			dist = r.t;
		} else {
			dist = r.t;
#if defined(MODE_SCREEN_PROBES) || defined(MODE_REFLECTIONS)
			if (screen_reuse(h.hit_pos, h.geometry_normal, dir)) {
				radiance = vec3(0.0);
				return;
			}
#endif
			vec3 albedo;
			shade_surface(h, mat, dir, radiance, radiance_lights, albedo);
			// Mariner DG-M: a scattering medium (GeometryInstance3D.raytracing_scattering: a canopy stand-in). The
			// leaves that stopped the ray send a share of its light on, tinted by their hue: the ray goes on from the
			// hit in a scattered direction (up to three events) and adds what it finds - the sky, the next surface.
			uint scattering = (geometries[r.instance].layer_mask >> GEOM_SCATTERING_SHIFT) & 0xFu;
			vec3 throughput = vec3(1.0);
			vec3 at = h.hit_pos;
			vec3 from_dir = dir;
			uint rng = pt_transmission_seed ^ 0x5bd1e995u;
			for (int event = 0; event < 3 && scattering != 0u; event++) {
				throughput *= (float(scattering) / 15.0) * albedo / max(max(albedo.r, albedo.g), max(albedo.b, 1e-3));
				vec3 v = scatter_direction(from_dir, rng);
				pt_transmission_seed = pcg_hash(pt_transmission_seed + 0x9E3779B9u);
				vec3 o = at + v * 0.05;
				RayHit rs = trace_closest_two_sided(o, v, params.ray_length);
				if (!rs.hit) {
					radiance += throughput * ambient_radiance(v, 0.5);
					break;
				}
				HitData hs = compute_hit_data(rs, o, v);
				MaterialData ms = materials[hs.geometry_idx];
				if ((ms.flags & MAT_FLAG_DIELECTRIC) != 0u || (!rs.front_face && ms.cull_mode != 0u)) {
					break; // Water, the inside of closed geometry: nothing more (as dark as before).
				}
				vec3 sky_s;
				vec3 lights_s;
				shade_surface(hs, ms, v, sky_s, lights_s, albedo);
				radiance += throughput * sky_s;
				radiance_lights += throughput * lights_s;
				scattering = (geometries[rs.instance].layer_mask >> GEOM_SCATTERING_SHIFT) & 0xFu;
				at = hs.hit_pos;
				from_dir = v;
			}
		}
	}
}

#if !defined(MODE_SCREEN_PROBES) && !defined(MODE_REFLECTIONS)
void main() {
	const uint ray_index = gl_GlobalInvocationID.x;
	if (gl_GlobalInvocationID.y >= params.slot_count) {
		return;
	}
	const uint slot_index = slot_list.slots[gl_GlobalInvocationID.y];
	const DDGIData d = ddgi_block.volumes[params.volume];
	const ivec3 count = d.probe_count.xyz;
	const uint ray_count = uint(d.probe_count.w);
	if (ray_index >= ray_count || slot_index >= uint(count.x * count.y * count.z)) {
		return;
	}

	ivec3 slot = ddgi_slot_from_index(slot_index, count);
	ivec3 cell = ddgi_slot_to_cell(slot, d.grid_min.xyz, count);
	// The offset of this slot's probe, if the slot still holds this cell (else the probe starts over at the cell).
	DDGIProbeState state = probe_buffer.probe_state[slot_index];
	vec3 offset = state.cell_age.xyz == cell && (d.grid_min.w & DDGI_FLAG_RELOCATION) != 0 ? state.offset.xyz : vec3(0.0);
	// Grid space -> world (the ship's grid moves with the ship; the ray directions of the maps stay in grid space).
	vec3 origin = (d.grid_to_world * vec4((vec3(cell) + offset) * d.spacing.xyz, 1.0)).xyz;
	vec3 dir = normalize(mat3(d.grid_to_world) * (mat3(d.ray_rotation) * ddgi_ray_direction(ray_index, ray_count)));

	vec3 radiance; // Sky channel.
	vec3 radiance_lights; // Lights channel.
	float dist;
	pt_transmission_seed = pcg_hash(slot_index * 7919u + ray_index * 104729u + floatBitsToUint(d.ray_rotation[0][0]));
	trace_radiance(d, origin, dir, radiance, radiance_lights, dist);
	radiance = clamp(radiance, vec3(0.0), vec3(65000.0));
	radiance_lights = clamp(radiance_lights, vec3(0.0), vec3(65000.0));
	ray_buffer.rays[slot_index * ray_count + ray_index] = uvec4(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, clamp(dist, -65000.0, 65000.0))), packHalf2x16(radiance_lights.rg), packHalf2x16(vec2(radiance_lights.b, 0.0)));
}
#elif defined(MODE_REFLECTIONS)
// GGX visible normal sampling (Heitz 2018, "Sampling the GGX Distribution of Visible Normals"), tangent space (z up).
vec3 sample_ggx_vndf(vec3 v, float alpha, vec2 u) {
	vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));
	float lensq = vh.x * vh.x + vh.y * vh.y;
	vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(vh, t1);
	float r = sqrt(u.x);
	float phi = 6.28318530718 * u.y;
	float p1 = r * cos(phi);
	float p2 = r * sin(phi);
	float s = 0.5 * (1.0 + vh.z);
	p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
	vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
	return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

void main() {
	const ivec2 half_px = ivec2(gl_GlobalInvocationID.xy);
	if (half_px.x >= int(params.grid_x) || half_px.y >= int(params.grid_y)) {
		return;
	}
	ivec2 full_size = textureSize(sampler2D(depth_buffer, material_sampler_nearest), 0);
	ivec2 px = ddgi_reflection_pixel(half_px, params.frame_index, full_size);
	float depth = texelFetch(sampler2D(depth_buffer, material_sampler_nearest), px, 0).r;
	vec4 nr = texelFetch(sampler2D(normal_roughness_buffer, material_sampler_nearest), px, 0);
	vec3 n_view = nr.xyz * 2.0 - 1.0;
	float roughness = ddgi_decode_roughness(nr.w);
	vec4 result = vec4(0.0);
	if (depth > 0.0 && dot(n_view, n_view) > 0.25 && roughness <= params.max_roughness) {
		vec4 vp = scene_data_block.data.inv_projection_matrix * vec4((vec2(px) + 0.5) / vec2(full_size) * 2.0 - 1.0, depth, 1.0);
		vec3 view_pos = vp.xyz / vp.w;
		mat4 inv_view = ddgi_affine_to_mat4(scene_data_block.data.inv_view_matrix);
		vec3 p = (inv_view * vec4(view_pos, 1.0)).xyz;
		vec3 n = normalize(mat3(inv_view) * normalize(n_view));
		vec3 to_eye = normalize(inv_view[3].xyz - p);
		if (dot(n, to_eye) < 0.0) {
			n = normalize(n - dot(n, to_eye) * to_eye); // Back facing normal (normal maps): into the plane facing the eye.
		}
		// Tangent frame around the normal; the view vector in it; a microfacet normal; the reflection.
		vec3 t = normalize(abs(n.y) < 0.999 ? cross(vec3(0.0, 1.0, 0.0), n) : cross(vec3(1.0, 0.0, 0.0), n));
		vec3 b = cross(n, t);
		vec3 v_t = vec3(dot(to_eye, t), dot(to_eye, b), max(dot(to_eye, n), 1e-4));
		uint rng = pcg_hash(uint(half_px.x) * 73856093u ^ uint(half_px.y) * 19349663u ^ params.frame_index * 83492791u);
		float alpha = max(roughness * roughness, 0.002);
		// The long tail of GGX cut (Stachowiak 2015, the bias of the Frostbite SSR): with one ray per 2x2 pixels its
		// rare far samples stay visible as dots of another colour.
		vec2 u = rand2(rng);
		u.x *= 0.7;
		vec3 m_t = sample_ggx_vndf(v_t, alpha, u);
		vec3 m = normalize(t * m_t.x + b * m_t.y + n * m_t.z);
		vec3 dir = reflect(-to_eye, m);
		if (dot(dir, n) <= 0.0) {
			dir = reflect(-to_eye, n); // Below the surface: the mirror direction.
		}
		vec3 origin = p + n * (0.01 + view_pos.z * -0.001);
		const DDGIData d = ddgi_block.volumes[DDGI_VOLUME_WORLD];
		pt_transmission_seed = rng;
		vec3 radiance;
		vec3 radiance_lights;
		float dist;
		trace_radiance(d, origin, dir, radiance, radiance_lights, dist);
		vec3 total;
		if (sp_has_reused) {
			total = sp_reused;
		} else if (dist >= 1e4) {
			// The sky as the scene shader reflects it: the radiance map at sqrt(roughness), the background energy, the
			// sky channel multiplier (no DDGI energy); the ambient light where there is no radiance map.
			vec3 sky = (params.flags & DDGI_TRACE_FLAG_RADIANCE) != 0u ? sky_radiance(dir, sqrt(roughness)) * scene_data_block.data.ambient_light_color_energy.a : radiance;
			total = sky * d.channels.x;
		} else {
			total = (radiance * d.channels.x + radiance_lights * d.channels.y) * d.spacing.w;
		}
		result = vec4(clamp(total, vec3(0.0), vec3(65000.0)), 1.0);
	}
	imageStore(trace_image, half_px, result);
}
#else
void main() {
	const uvec2 texel = gl_GlobalInvocationID.xy;
	const uvec2 probe = texel / uint(DDGI_SCREEN_PROBE_TEXELS);
	if (probe.x >= params.grid_x || probe.y >= params.grid_y) {
		return;
	}
	const uvec2 local = texel % uint(DDGI_SCREEN_PROBE_TEXELS);
	DDGIScreenProbe sp = screen_probes.probes[probe.y * params.grid_x + probe.x];
	vec4 result = vec4(0.0);
	if (sp.position.w > 0.0) {
		// A direction in the texel (jittered each frame: the temporal accumulation covers the texel).
		uint rng = pcg_hash(texel.x * 73856093u ^ texel.y * 19349663u ^ params.frame_index * 83492791u);
		vec2 uv = (vec2(local) + rand2(rng)) / float(DDGI_SCREEN_PROBE_TEXELS);
		vec3 dir = oct_to_vec3(uv * 2.0 - 1.0);
		vec3 n = sp.normal.xyz;
		if (dot(dir, n) > 0.0) {
			// Off the surface by a little more far away (depth precision of the reconstructed position).
			vec3 origin = sp.position.xyz + n * (0.02 + sp.position.w * 0.002);
			const DDGIData d = ddgi_block.volumes[DDGI_VOLUME_WORLD];
			pt_transmission_seed = rng;
			vec3 radiance;
			vec3 radiance_lights;
			float dist;
			trace_radiance(d, origin, dir, radiance, radiance_lights, dist);
			vec3 total;
			if (sp_has_reused) {
				total = sp_reused;
			} else {
				// As the scene shader applies the probes: energy x channel multipliers.
				total = (radiance * d.channels.x + radiance_lights * d.channels.y) * d.spacing.w;
			}
			result = vec4(clamp(total, vec3(0.0), vec3(65000.0)), 1.0);
		}
	}
	imageStore(trace_image, ivec2(texel), result);
}
#endif
