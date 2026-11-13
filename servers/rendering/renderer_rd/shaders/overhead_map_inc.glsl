// Mariner G1a: the overhead map (Environment.overhead_map_*) - the scene from above around the camera as normalized
// heights with averaged mipmaps (effects/overhead_map.cpp). Needs, before the include: OVERHEAD_MAP - the
// OverheadMapData (overhead_map_data_inc.glsl), OVERHEAD_HEIGHT - the heights (texture2D), and
// SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP.

// Heights of the map (world, meters) at p_xz over a footprint of about 2^p_lod texels: x the average (the covered part of
// the footprint), y the maximum.
vec2 overhead_map_heights(vec2 p_xz, float p_lod) {
	vec2 uv = (p_xz - OVERHEAD_MAP.box_min.xz) * OVERHEAD_MAP.box_size.z;
	return OVERHEAD_MAP.box_min.y + textureLod(sampler2D(OVERHEAD_HEIGHT, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, p_lod).rg * OVERHEAD_MAP.box_size.y;
}

bool overhead_map_inside(vec3 p_world) {
	vec3 rel = p_world - OVERHEAD_MAP.box_min.xyz;
	return OVERHEAD_MAP.params.w > 0.5 && all(greaterThanEqual(rel.xz, vec2(0.0))) && all(lessThan(rel.xz, vec2(OVERHEAD_MAP.box_size.x))) && rel.y >= 0.0 && rel.y < OVERHEAD_MAP.box_size.y;
}

// Sky visibility of a point, cosine weighted about the up axis (1 = the open sky; outside the map 1). The point is moved
// off its surface along p_normal (world) by the bias plus 1.5 texels: walls do not sample the roof above their own edge.
// Per direction (8), rings at radii from 2 texels to the radius (geometric, footprint a quarter of the radius). The map
// is a height field, so whether the point itself is covered tells an overhang from a wall; both cases are computed and
// blended by how much of a 4 texel footprint around the point the cover takes (a roof - all of it, the edge of a roof -
// half, the top of a thin wall next to the point - little: no seams at edges and wall feet):
// * open: walls from the ground up to the largest elevation of the maximum heights (the horizon);
// * covered (under a roof, a deck, a bridge): the cover reaches as far as its average over the footprint is above half
//   its height over the point (the edge interpolated between rings); the occluder spans from the elevation of its edge
//   up to the zenith; walls beyond it as above, from the rings whose footprint is past the edge.
// The sky is seen between the horizon and the edge: cos^2(horizon) - cos^2(edge) (cosine weighted, elevations).
float overhead_map_sky_visibility(vec3 p_world, vec3 p_normal) {
	float texel = OVERHEAD_MAP.box_min.w;
	vec3 p = p_world + p_normal * (OVERHEAD_MAP.params.z + texel * 1.5);
	if (!overhead_map_inside(p)) {
		return 1.0;
	}
	const int DIRECTIONS = 8;
	const int RINGS = 8;
	float radius = max(OVERHEAD_MAP.params.y, texel * 4.0);
	float r0 = texel * 2.0;
	float ratio = pow(radius / r0, 1.0 / float(RINGS - 1));
	float max_lod = OVERHEAD_MAP.params2.x - 1.0;
	float receiver = p.y;

	// The cover over the point: its height (the maximum) and the part of the footprint it takes (the average). A cover
	// is higher than the bias over the point: the foot of a wall moves sideways off its surface at the ground's height,
	// where the ground itself (a millimeter higher in the 16 bit map) would cover it as far as the radius.
	float cover = overhead_map_heights(p.xz, 0.0).y - receiver;
	float covered_part = cover > max(OVERHEAD_MAP.params.z, 0.05) ? clamp((overhead_map_heights(p.xz, min(2.0, max_lod)).x - receiver) / cover, 0.0, 1.0) : 0.0;
	float covered = smoothstep(0.3, 0.7, covered_part);

	float visible = 0.0;
	for (int d = 0; d < DIRECTIONS; d++) {
		float a = (float(d) + 0.5) * (6.28318530718 / float(DIRECTIONS));
		vec2 dir = vec2(cos(a), sin(a));
		float sin2_open = 0.0; // Horizon of the open case.
		float sin2_horizon = 0.0; // Horizon past the edge of the cover.
		float cos2_edge = 0.0;
		float edge = 0.0;
		bool in_cover = covered > 0.0;
		float f_prev = 1.0; // Covered part at the point.
		float r_prev = r0 / ratio;
		float r = r0;
		for (int k = 0; k < RINGS; k++) {
			float lod = clamp(log2(max(r * 0.25 / texel, 1.0)), 0.0, max_lod);
			vec2 h = overhead_map_heights(p.xz + dir * r, lod) - receiver;
			float sin2 = h.y > 0.0 ? (h.y * h.y) / (h.y * h.y + r * r) : 0.0;
			sin2_open = max(sin2_open, sin2);
			if (in_cover) {
				float f = h.x / cover;
				if (f <= 0.5) {
					// The edge where the cover drops to half, between the previous ring and this one (log radius).
					float t = clamp((f_prev - 0.5) / max(f_prev - f, 1e-4), 0.0, 1.0);
					edge = r_prev * pow(ratio, t);
					cos2_edge = (edge * edge) / (edge * edge + cover * cover);
					in_cover = false;
				}
				f_prev = f;
			} else if (r * 0.75 - texel * 2.0 > edge) {
				// Walls: not the cover itself, whose maximum (dilated over the footprint) reaches past its edge.
				sin2_horizon = max(sin2_horizon, sin2);
			}
			r_prev = r;
			r *= ratio;
		}
		float visible_open = 1.0 - sin2_open;
		// Covered as far as the radius: no sky; else cos^2(horizon) - cos^2(edge), with cos^2 = 1 - sin^2.
		float visible_covered = in_cover ? 0.0 : max((1.0 - sin2_horizon) - cos2_edge, 0.0);
		visible += mix(visible_open, visible_covered, covered);
	}
	visible /= float(DIRECTIONS);
	return mix(1.0, visible, OVERHEAD_MAP.params.x);
}

// Cover from above (0 open, 1 under a roof, a deck, a bridge: no rain, no snow), soft over the edge of the cover (a
// footprint of 2 texels). The point is moved off its surface as for the sky visibility; a cover is higher than the bias
// over it.
float overhead_map_cover(vec3 p_world, vec3 p_normal) {
	float texel = OVERHEAD_MAP.box_min.w;
	vec3 p = p_world + p_normal * (OVERHEAD_MAP.params.z + texel * 1.5);
	if (!overhead_map_inside(p)) {
		return 0.0;
	}
	float cover = overhead_map_heights(p.xz, 0.0).y - p.y;
	if (cover <= max(OVERHEAD_MAP.params.z, 0.05)) {
		return 0.0;
	}
	float covered_part = clamp((overhead_map_heights(p.xz, min(1.0, OVERHEAD_MAP.params2.x - 1.0)).x - p.y) / cover, 0.0, 1.0);
	return smoothstep(0.3, 0.7, covered_part);
}
