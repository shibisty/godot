#[compute]

#version 460

#VERSION_DEFINES

// Mariner E12: DDGI probe update. One work group per probe slot; each invocation writes one texel of the probe's
// distance tile (18x18 with border) and the first 10x10 also one texel of its irradiance tile. Border texels compute
// the value of the interior texel they mirror (octahedral wrap), so the tiles filter bilinearly without seams and no
// separate border pass is needed. New values blend into the old ones with the hysteresis; a slot whose world cell
// changed (the grid scrolled) or that is new starts over, and the hysteresis ramps up with the age of the probe.
// Relocation (DG1b): a probe inside geometry (many back face hits) moves through the closest back face, a probe too
// close to a surface (for example exactly in the plane of a wall or a deck) moves away from it; at most 0.45 spacings
// from its cell, so the grid stays a grid for the trilinear weights.

#define DDGI_MAX_RAYS 256
#define DDGI_TILE 18 // Distance tile with border.

layout(local_size_x = DDGI_TILE, local_size_y = DDGI_TILE, local_size_z = 1) in;

// clang-format off
#include "../oct_inc.glsl"
#include "../ddgi_data_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std140) uniform DDGIBlock {
	DDGIData volumes[DDGI_VOLUME_COUNT];
}
ddgi_block;

layout(push_constant, std430) uniform Params {
	uint volume; // DDGI_VOLUME_* updated by this dispatch.
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(set = 0, binding = 1, std430) restrict readonly buffer RayBuffer {
	uvec4 rays[]; // Sky rgb + signed distance, lights rgb (half floats, ddgi_trace.glsl).
}
ray_buffer;

layout(set = 0, binding = 2, std430) restrict buffer ProbeState {
	DDGIProbeState probe_state[];
}
probe_buffer;

layout(set = 0, binding = 3, rgba16f) uniform restrict image2D irradiance_image;
layout(set = 0, binding = 4, rg16f) uniform restrict image2D distance_image;
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D probe_data_image;
layout(set = 0, binding = 7, rgba16f) uniform restrict image2D irradiance_lights_image; // DG3: lights channel.

layout(set = 0, binding = 6, std430) restrict readonly buffer SlotList {
	uint slots[]; // Probe slots updated this frame (one work group each).
}
slot_list;

shared vec3 s_dir[DDGI_MAX_RAYS];
shared vec4 s_ray[DDGI_MAX_RAYS]; // rgb sky radiance, a signed distance.
shared vec3 s_ray_lights[DDGI_MAX_RAYS]; // Lights radiance.
shared uint s_backfaces;
shared int s_age;
shared vec3 s_offset; // Offset the rays of this frame were traced from (in spacings).

// Interior texel (0 .. texels - 1) that a texel of the bordered tile (0 .. texels + 1) shows.
ivec2 ddgi_border_source(ivec2 t, int texels) {
	ivec2 i = t - 1;
	bool out_x = i.x < 0 || i.x >= texels;
	bool out_y = i.y < 0 || i.y >= texels;
	if (out_x && out_y) {
		return ivec2(i.x < 0 ? texels - 1 : 0, i.y < 0 ? texels - 1 : 0); // Corners: the opposite corner.
	}
	if (out_y) {
		return ivec2(texels - 1 - i.x, i.y < 0 ? 0 : texels - 1); // Top / bottom row: mirrored horizontally.
	}
	if (out_x) {
		return ivec2(i.x < 0 ? 0 : texels - 1, texels - 1 - i.y); // Left / right column: mirrored vertically.
	}
	return i;
}

// Blend of a new irradiance into the old one; large changes (a light switched, a door opened) blend in faster.
vec3 ddgi_blend_irradiance(vec3 value, vec3 old, float h) {
	vec3 delta = abs(value - old);
	float rel = max(delta.r, max(delta.g, delta.b)) / max(max(old.r, max(old.g, old.b)), 0.05);
	if (rel > 0.8) {
		h = max(0.0, h - 0.15);
	}
	return mix(value, old, h);
}

vec3 ddgi_texel_direction(ivec2 interior, int texels) {
	vec2 oct = (vec2(interior) + 0.5) / float(texels) * 2.0 - 1.0;
	return oct_to_vec3(oct);
}

void main() {
	const DDGIData d = ddgi_block.volumes[params.volume];
	const ivec3 count = d.probe_count.xyz;
	const uint ray_count = min(uint(d.probe_count.w), uint(DDGI_MAX_RAYS));
	const uint slot_index = slot_list.slots[gl_WorkGroupID.x];
	const uint local_index = gl_LocalInvocationIndex;
	const ivec3 slot = ddgi_slot_from_index(slot_index, count);
	const ivec3 cell = ddgi_slot_to_cell(slot, d.grid_min.xyz, count);

	if (local_index == 0u) {
		DDGIProbeState state = probe_buffer.probe_state[slot_index];
		bool same_cell = state.cell_age.xyz == cell;
		s_age = same_cell ? state.cell_age.w : 0;
		s_offset = same_cell && (d.grid_min.w & DDGI_FLAG_RELOCATION) != 0 ? state.offset.xyz : vec3(0.0);
		s_backfaces = 0u;
	}
	barrier();

	for (uint i = local_index; i < ray_count; i += uint(DDGI_TILE * DDGI_TILE)) {
		uvec4 packed = ray_buffer.rays[slot_index * uint(d.probe_count.w) + i];
		vec2 rg = unpackHalf2x16(packed.x);
		vec2 bd = unpackHalf2x16(packed.y);
		s_ray[i] = vec4(rg, bd.x, bd.y);
		s_ray_lights[i] = vec3(unpackHalf2x16(packed.z), unpackHalf2x16(packed.w).x);
		s_dir[i] = normalize(mat3(d.ray_rotation) * ddgi_ray_direction(i, uint(d.probe_count.w)));
		if (bd.y < 0.0) {
			atomicAdd(s_backfaces, 1u);
		}
	}
	barrier();

	const int age = s_age;
	const bool probe_active = float(s_backfaces) <= 0.25 * float(ray_count);
	// New probes converge fast (running average), then blend with the hysteresis.
	const float hysteresis = age == 0 ? 0.0 : min(d.misc.z, float(age) / float(age + 1));
	const ivec2 t = ivec2(gl_LocalInvocationID.xy);

	// Distance moments.
	{
		ivec2 interior = ddgi_border_source(t, DDGI_DISTANCE_TEXELS);
		vec3 dir = ddgi_texel_direction(interior, DDGI_DISTANCE_TEXELS);
		const float max_distance = d.misc.y;
		vec2 sum = vec2(0.0);
		float weight_sum = 0.0;
		for (uint i = 0u; i < ray_count; i++) {
			float w = max(0.0, dot(dir, s_dir[i]));
			w = pow(w, 50.0);
			if (w <= 0.0) {
				continue;
			}
			float dist = min(abs(s_ray[i].a), max_distance);
			sum += vec2(dist, dist * dist) * w;
			weight_sum += w;
		}
		ivec2 px = ddgi_tile_origin(slot, count, DDGI_DISTANCE_TEXELS) + t;
		vec2 value = weight_sum > 1e-6 ? sum / weight_sum : vec2(max_distance, max_distance * max_distance);
		if (hysteresis > 0.0) {
			value = mix(value, imageLoad(distance_image, px).rg, hysteresis);
		}
		imageStore(distance_image, px, vec4(value, 0.0, 0.0));
	}

	// Irradiance (cosine weighted average radiance).
	if (t.x < DDGI_IRRADIANCE_TEXELS + 2 && t.y < DDGI_IRRADIANCE_TEXELS + 2) {
		ivec2 interior = ddgi_border_source(t, DDGI_IRRADIANCE_TEXELS);
		vec3 dir = ddgi_texel_direction(interior, DDGI_IRRADIANCE_TEXELS);
		vec3 sum = vec3(0.0);
		vec3 sum_lights = vec3(0.0);
		float weight_sum = 0.0;
		for (uint i = 0u; i < ray_count; i++) {
			float w = max(0.0, dot(dir, s_dir[i]));
			if (w <= 0.0 || s_ray[i].a < 0.0) {
				continue; // Back faces carry no light.
			}
			sum += s_ray[i].rgb * w;
			sum_lights += s_ray_lights[i] * w;
			weight_sum += w;
		}
		ivec2 px = ddgi_tile_origin(slot, count, DDGI_IRRADIANCE_TEXELS) + t;
		vec3 value = weight_sum > 1e-6 ? sum / weight_sum : vec3(0.0);
		vec3 value_lights = weight_sum > 1e-6 ? sum_lights / weight_sum : vec3(0.0);
		if (hysteresis > 0.0) {
			value = ddgi_blend_irradiance(value, imageLoad(irradiance_image, px).rgb, hysteresis);
			value_lights = ddgi_blend_irradiance(value_lights, imageLoad(irradiance_lights_image, px).rgb, hysteresis);
		}
		imageStore(irradiance_image, px, vec4(value, probe_active ? 1.0 : 0.0));
		imageStore(irradiance_lights_image, px, vec4(value_lights, 1.0));
	}

	barrier();
	if (local_index == 0u) {
		vec3 offset = s_offset;
		if ((d.grid_min.w & DDGI_FLAG_RELOCATION) != 0) {
			const vec3 spacing = d.spacing.xyz;
			const float min_frontface = 0.25 * min(spacing.x, min(spacing.y, spacing.z)); // Closest surface a probe may have.
			float closest_back = 1e27;
			vec3 back_dir = vec3(0.0);
			float closest_front = 1e27;
			vec3 front_dir = vec3(0.0);
			for (uint i = 0u; i < ray_count; i++) {
				float dist = s_ray[i].a;
				if (dist < 0.0) {
					float t = -dist * 5.0; // Back face distances are stored at 0.2x (ddgi_trace.glsl).
					if (t < closest_back) {
						closest_back = t;
						back_dir = s_dir[i];
					}
				} else if (dist < closest_front) {
					closest_front = dist;
					front_dir = s_dir[i];
				}
			}
			vec3 offset_m = offset * spacing;
			if (float(s_backfaces) > 0.25 * float(ray_count) && closest_back < 1e26) {
				// Inside geometry: through the closest back face, a little past it.
				offset_m += back_dir * (closest_back + min_frontface * 0.5);
			} else if (closest_front < min_frontface) {
				// Too close to a surface (in the plane of a wall, deck): away from it.
				offset_m -= front_dir * (min_frontface - closest_front);
			}
			offset = clamp(offset_m / spacing, vec3(-0.45), vec3(0.45));
		}
		probe_buffer.probe_state[slot_index].cell_age = ivec4(cell, min(age + 1, 1000));
		probe_buffer.probe_state[slot_index].offset = vec4(offset, 0.0);
		imageStore(probe_data_image, ddgi_probe_texel(slot, count), vec4(offset, 0.0));
	}
}
