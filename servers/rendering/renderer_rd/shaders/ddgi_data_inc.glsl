// Mariner E12: dynamic diffuse global illumination probes (DDGI, after Majercik et al. 2019, "Dynamic Diffuse Global
// Illumination with Ray-Traced Irradiance Fields"), written for Godot's Forward+ renderer.
//
// A grid of probes snapped to world cells around the camera; a probe's data lives in the slot (cell mod count), so the
// grid scrolls toroidally: when the camera moves, only the probes of newly entered cells start over. Per probe:
// - irradiance: 8x8 octahedral texels (+1 texel border) of the cosine weighted average radiance around a direction (the
//   "ambient light" of the scene shader; irradiance / PI), alpha = probe active (not inside geometry);
// - distance: 16x16 octahedral texels (+1 texel border) of the mean and mean squared distance to the closest surface,
//   for the Chebyshev visibility test that stops light leaking through walls.
//
// This file: the parameter block layout and the grid / atlas helpers. ddgi_sample_inc.glsl: sampling the probes.
// Needs oct_inc.glsl.

// One probe grid ("volume"): the world grid around the camera, or the grid of a DDGIVolume3D in the frame of a ship
// (DG2). Probes, ray directions and the octahedral maps live in grid space (probe (i, j, k) at (i, j, k) * spacing);
// grid_to_world is rigid (the world grid: identity), so the data of a ship's grid stays valid when the ship moves.
struct DDGIData {
	ivec4 grid_min; // xyz: cell of the first probe of the grid; w: DDGI_FLAG_* (0: volume off).
	ivec4 probe_count; // xyz: probes per axis; w: rays per probe.
	vec4 spacing; // xyz: probe spacing per axis (m), w: energy.
	vec4 atlas_inv_size; // xy: 1 / irradiance atlas size, zw: 1 / distance atlas size.
	vec4 misc; // x: specular occlusion, y: max distance of the visibility moments, z: hysteresis, w: surface bias (m).
	mat4 ray_rotation; // Random rotation of the ray directions of this frame.
	mat4 grid_to_world;
	mat4 world_to_grid;
	vec4 channels; // x: multiplier of the sky channel, y: of the lights channel (DG3); zw: unused.
	vec4 sea; // DG4: rgb: color of the light scattered out of the sea body (linear), a: sea level (world y).
};

// DG3: two irradiance channels per probe, each with its own bounces: "sky" (light that came from the ambient light /
// sky) and "lights" (sun, lamps, emissive materials). The scene shader weights them with channels.xy at once, without
// waiting for the probes (lightning, clouds, ship lamps).

#define DDGI_VOLUME_WORLD 0
#define DDGI_VOLUME_LOCAL 1
#define DDGI_VOLUME_COUNT 2

#define DDGI_FLAG_ACTIVE 1
#define DDGI_FLAG_RELOCATION 2 // Probes move out of geometry (offset per probe, DG1b).
#define DDGI_FLAG_SEA 4 // DG4: probe rays see an endless sea plane at sea.a (the ocean mesh ends near the camera).

// Per probe slot state (ddgi_update.glsl, read by the ray pass).
struct DDGIProbeState {
	ivec4 cell_age; // xyz: world cell of the data in the slot, w: frames accumulated (0 = none).
	vec4 offset; // xyz: probe offset from the cell position, in spacings (|x| <= 0.45); w: unused.
};

#define DDGI_IRRADIANCE_TEXELS 8
#define DDGI_DISTANCE_TEXELS 16

// Ray i of n: spherical Fibonacci directions (rotated randomly every frame by ray_rotation).
vec3 ddgi_ray_direction(uint i, uint n) {
	const float golden_fraction = 0.6180339887498949;
	float phi = 6.283185307179586 * fract(float(i) * golden_fraction);
	float cos_theta = 1.0 - (2.0 * float(i) + 1.0) / float(n);
	float sin_theta = sqrt(clamp(1.0 - cos_theta * cos_theta, 0.0, 1.0));
	return vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

// Positive modulo (GLSL % is undefined for negative operands).
ivec3 ddgi_mod(ivec3 a, ivec3 b) {
	ivec3 r = a - b * ivec3(floor(vec3(a) / vec3(b)));
	return clamp(r, ivec3(0), b - 1);
}

ivec3 ddgi_cell_to_slot(ivec3 cell, ivec3 count) {
	return ddgi_mod(cell, count);
}

// World cell of the probe stored in a slot (the grid covers grid_min .. grid_min + count - 1).
ivec3 ddgi_slot_to_cell(ivec3 slot, ivec3 grid_min, ivec3 count) {
	return grid_min + ddgi_mod(slot - grid_min, count);
}

ivec3 ddgi_slot_from_index(uint index, ivec3 count) {
	return ivec3(int(index % uint(count.x)), int((index / uint(count.x)) % uint(count.y)), int(index / uint(count.x * count.y)));
}

// Texel of a slot in the probe data texture (offsets).
ivec2 ddgi_probe_texel(ivec3 slot, ivec3 count) {
	return ivec2(slot.x + slot.y * count.x, slot.z);
}

// Top left texel of the tile (with border) of a slot in an atlas with tiles of `texels` + 2.
ivec2 ddgi_tile_origin(ivec3 slot, ivec3 count, int texels) {
	return ivec2(slot.x + slot.y * count.x, slot.z) * (texels + 2);
}

vec2 ddgi_atlas_uv(ivec3 slot, ivec3 count, vec3 dir, int texels, vec2 inv_atlas_size) {
	vec2 oct = vec3_to_oct(dir); // [0, 1]
	vec2 px = vec2(ddgi_tile_origin(slot, count, texels) + 1) + oct * float(texels);
	return px * inv_atlas_size;
}
