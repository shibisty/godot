#[compute]

#version 460

#VERSION_DEFINES

// Mariner E12: DDGI probe rays. One invocation per ray of a probe (x: ray, y: probe slot), inline ray query on the
// raytracing scene (TLAS, patch E5a). A hit is shaded with the direct light of the scene lights (shadow rays) and the
// irradiance of the probes of the previous frame (multiple bounces); a miss sees the sky as the ambient light of the
// rasterizer does (ambient color / sky contribution / energy). Writes the radiance and the hit distance (negative for
// back faces: the probe sees the inside of a closed mesh) for ddgi_update.glsl.

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_control_flow_attributes : enable

#define GLSL 1
#define MAX_VIEWS 2
#define PT_SCENE_SET 0

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

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

layout(set = 0, binding = 10, std430) restrict writeonly buffer RayBuffer {
	uvec4 rays[]; // packHalf2x16: sky radiance rg, (sky b, signed distance), lights radiance rg, (lights b, 0).
}
ray_buffer;

// Both grids (previous frame) for the bounce light at hits.
layout(set = 0, binding = 11) uniform texture2D ddgi_irradiance_world;
layout(set = 0, binding = 12) uniform texture2D ddgi_distance_world;
layout(set = 0, binding = 13) uniform texture2D ddgi_probe_data_world;
layout(set = 0, binding = 16) uniform texture2D ddgi_irradiance_local;
layout(set = 0, binding = 17) uniform texture2D ddgi_distance_local;
layout(set = 0, binding = 18) uniform texture2D ddgi_probe_data_local;
layout(set = 0, binding = 19) uniform texture2D ddgi_irradiance_lights_world;
layout(set = 0, binding = 20) uniform texture2D ddgi_irradiance_lights_local;

layout(set = 0, binding = 14, std430) restrict readonly buffer ProbeStateBuffer {
	DDGIProbeState probe_state[];
}
probe_buffer;

layout(set = 0, binding = 15, std430) restrict readonly buffer SlotList {
	uint slots[]; // Probe slots updated this frame.
}
slot_list;

layout(set = 1, binding = 0) uniform texture2D rt_textures[];

#define DDGI_TRACE_FLAG_RADIANCE 1u // The radiance map is valid.
#define DDGI_TRACE_FLAG_MULTI_BOUNCE 2u // The probes of the previous frame are valid.

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
	if ((mat_flags & MAT_FLAG_POINT_FILTER) != 0u) {
		return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler_nearest), uv, lod);
	}
	return textureLod(sampler2D(rt_textures[nonuniformEXT(tex_idx)], material_sampler), uv, lod);
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
	if (mat.albedo_texture_idx != 0u) {
		vec2 uv = fetch_uv(geom, i0, i1, i2, bary) * mat.uv1_scale + mat.uv1_offset;
		alpha *= sample_material_texture(mat.albedo_texture_idx, uv, mat.flags, 0.0).a;
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
	vec3 sum = vec3(0.0);
	uint evaluated = 0u;
	uint rng = 0u;
	vec3 origin = offset_ray_origin(p, n);
	for (uint i = 0u; i < params.light_count && evaluated < 16u; i++) {
		RTLightData light = rt_lights[i];
		vec3 l;
		float atten = 1.0;
		float max_dist = params.ray_length;
		if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
			l = -normalize(light.position);
		} else {
			vec3 to_light = light.position - p;
			float d2 = dot(to_light, to_light);
			if (light.max_range_squared != 0.0 && d2 > light.max_range_squared) {
				continue;
			}
			float d = sqrt(d2);
			l = to_light / max(d, 1e-4);
			max_dist = d;
			LightSample ls;
			ls.distance_sq = d2;
			atten = lights_get_attenuation(ls, light.inv_max_range, light.attenuation);
			if (light.type == RT_LIGHT_TYPE_SPOT) {
				float scos = dot(-l, light.spot_direction);
				if (scos <= light.cos_spot_angle) {
					continue;
				}
				float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
				atten *= 1.0 - pow(spot_rim, light.inv_spot_attenuation);
			}
		}
		float ndl = dot(n, l);
		if (ndl <= 0.0 || atten <= 0.0) {
			continue;
		}
		evaluated++;
		vec3 visibility = lights_trace_shadow_ray(origin, l, max_dist, rng);
		// Light emission is energy * PI (rasterizer convention), Lambert is albedo / PI.
		sum += light.emission * light.indirect_energy * (ndl * atten / PI) * visibility;
	}
	return sum;
}

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
	vec3 radiance_lights = vec3(0.0); // Lights channel.
	float dist;
	pt_transmission_seed = pcg_hash(slot_index * 7919u + ray_index * 104729u + floatBitsToUint(d.ray_rotation[0][0]));
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
			dist = r.t;
		}
	}
	radiance = clamp(radiance, vec3(0.0), vec3(65000.0));
	radiance_lights = clamp(radiance_lights, vec3(0.0), vec3(65000.0));
	ray_buffer.rays[slot_index * ray_count + ray_index] = uvec4(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, clamp(dist, -65000.0, 65000.0))), packHalf2x16(radiance_lights.rg), packHalf2x16(vec2(radiance_lights.b, 0.0)));
}
