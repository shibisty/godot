// Mariner E12: sampling the DDGI probes (see ddgi_data_inc.glsl). ddgi_sample_volume() samples one grid; the including
// shader declares the parameter block (DDGIData volumes[DDGI_VOLUME_COUNT]) and the atlases of both grids and defines
// DDGI_VOLUMES, DDGI_IRRADIANCE_0 / _1 (sky channel), DDGI_IRRADIANCE_LIGHTS_0 / _1, DDGI_DISTANCE_0 / _1,
// DDGI_PROBE_DATA_0 / _1 (texture2D) and DDGI_SAMPLER (linear, clamp) before including this file, for ddgi_apply().

// Irradiance of the sky channel (rgb, as ambient light, with the energy applied) and coverage (a: 0 outside the grid,
// or no probe data) at a world point; r_lights: irradiance of the lights channel. r_inside: how far the point is inside
// the grid (0 outside, 1 more than a cell inside), with or without probe data.
vec4 ddgi_sample_volume(DDGIData d, texture2D irradiance_tex, texture2D irradiance_lights_tex, texture2D distance_tex, texture2D probe_tex, sampler smp, vec3 p_world, vec3 n_world, vec3 to_eye_world, out float r_inside, out vec3 r_lights) {
	r_inside = 0.0;
	r_lights = vec3(0.0);
	if ((d.grid_min.w & DDGI_FLAG_ACTIVE) == 0) {
		return vec4(0.0);
	}
	vec3 p = (d.world_to_grid * vec4(p_world, 1.0)).xyz;
	vec3 n = normalize(mat3(d.world_to_grid) * n_world);
	vec3 to_eye = normalize(mat3(d.world_to_grid) * to_eye_world);
	ivec3 count = d.probe_count.xyz;
	vec3 spacing = d.spacing.xyz;
	vec3 biased = p + (n * 0.2 + to_eye * 0.8) * d.misc.w;
	vec3 local = biased / spacing - vec3(d.grid_min.xyz);

	// Fade out over the outermost cell of the grid.
	vec3 edge = min(local, vec3(count - 1) - local);
	float coverage = clamp(min(edge.x, min(edge.y, edge.z)), 0.0, 1.0);
	r_inside = coverage;
	if (coverage <= 0.0) {
		return vec4(0.0);
	}

	ivec3 base = clamp(ivec3(floor(local)), ivec3(0), count - 2);
	vec3 alpha = clamp(local - vec3(base), vec3(0.0), vec3(1.0));
	vec2 irradiance_uv_n = vec3_to_oct(n); // [0, 1]

	vec3 sum = vec3(0.0);
	vec3 sum_lights = vec3(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 8; i++) {
		ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		ivec3 cell = d.grid_min.xyz + base + offset;
		ivec3 slot = ddgi_cell_to_slot(cell, count);
		vec3 probe_pos = (vec3(cell) + texelFetch(sampler2D(probe_tex, smp), ddgi_probe_texel(slot, count), 0).xyz) * spacing;

		// Smooth backface weight: probes behind the surface count less (but not zero, for thin geometry).
		vec3 to_probe = normalize(probe_pos - p);
		float wrap = (dot(to_probe, n) + 1.0) * 0.5;
		float weight = wrap * wrap + 0.2;

		// Chebyshev visibility from the distance moments of the probe in the direction of the (biased) point.
		vec3 probe_to_point = biased - probe_pos;
		float dist = length(probe_to_point);
		vec2 moments = textureLod(sampler2D(distance_tex, smp), ddgi_atlas_uv(slot, count, probe_to_point / max(dist, 1e-4), DDGI_DISTANCE_TEXELS, d.atlas_inv_size.zw), 0.0).rg;
		float visibility = 1.0;
		if (dist > moments.x) {
			float variance = abs(moments.x * moments.x - moments.y);
			float delta = dist - moments.x;
			visibility = variance / (variance + delta * delta);
			visibility = visibility * visibility * visibility;
		}
		weight *= max(0.05, visibility);
		weight = max(1e-6, weight);
		// Crush tiny weights (light leaks through thin walls).
		const float crush = 0.2;
		if (weight < crush) {
			weight *= weight * weight / (crush * crush);
		}
		vec3 tri = mix(vec3(1.0) - alpha, alpha, vec3(offset));
		weight *= tri.x * tri.y * tri.z;

		vec2 px = vec2(ddgi_tile_origin(slot, count, DDGI_IRRADIANCE_TEXELS) + 1) + irradiance_uv_n * float(DDGI_IRRADIANCE_TEXELS);
		vec4 irradiance = textureLod(sampler2D(irradiance_tex, smp), px * d.atlas_inv_size.xy, 0.0);
		weight *= irradiance.a; // Inactive probes (inside geometry) and probes without data yet.
		if (weight <= 0.0) {
			continue;
		}
		vec3 irradiance_lights = textureLod(sampler2D(irradiance_lights_tex, smp), px * d.atlas_inv_size.xy, 0.0).rgb;

		sum += irradiance.rgb * weight;
		sum_lights += irradiance_lights * weight;
		weight_sum += weight;
	}
	if (weight_sum < 1e-5) {
		return vec4(0.0);
	}
	r_lights = sum_lights / weight_sum * d.spacing.w;
	return vec4(sum / weight_sum * d.spacing.w, coverage);
}

// The ambient light with the DDGI grids applied, per channel: the world grid over the ambient light, the local (ship)
// grid over that. Returns the sky channel (the ambient light where no grid applies), r_lights the lights channel (0
// there). p_dark_start: inside a grid its probes count also before they have data (the ray pass: the light builds up
// bounce by bounce instead of starting at the sky light), else only where they have data (the scene shader).
// r_amount: how much of the result comes from the probes (0..1).
vec3 ddgi_apply(vec3 ambient, vec3 p, vec3 n, vec3 to_eye, bool p_dark_start, out float r_amount, out vec3 r_lights) {
	float inside;
	vec3 lights;
	vec4 gi = ddgi_sample_volume(DDGI_VOLUMES[DDGI_VOLUME_WORLD], DDGI_IRRADIANCE_0, DDGI_IRRADIANCE_LIGHTS_0, DDGI_DISTANCE_0, DDGI_PROBE_DATA_0, DDGI_SAMPLER, p, n, to_eye, inside, lights);
	float a = p_dark_start ? inside : gi.a;
	vec3 result = mix(ambient, gi.rgb, a);
	r_lights = lights * a;
	r_amount = a;
	gi = ddgi_sample_volume(DDGI_VOLUMES[DDGI_VOLUME_LOCAL], DDGI_IRRADIANCE_1, DDGI_IRRADIANCE_LIGHTS_1, DDGI_DISTANCE_1, DDGI_PROBE_DATA_1, DDGI_SAMPLER, p, n, to_eye, inside, lights);
	a = p_dark_start ? inside : gi.a;
	result = mix(result, gi.rgb, a);
	r_lights = mix(r_lights, lights, a);
	r_amount = max(r_amount, a);
	return result;
}
