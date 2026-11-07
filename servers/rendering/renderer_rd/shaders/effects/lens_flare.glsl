#[compute]

#version 450

#VERSION_DEFINES

// Mariner E15b: lens flare and lens dirt (CameraAttributes.lens_*).
// Light above a threshold (bright pass at a quarter of the resolution) is mirrored through the center of the image into
// a row of ghosts and a ring (halo), as the reflections between the elements of a lens do, with a little color
// fringing; blurred, it is added to the image, more where the lens dirt texture is. The sun (the brightest directional
// light) adds a starburst and a glare, scaled by how much of its disc is seen (the depth buffer around it).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D color_source; // HDR color (linear filter).
layout(set = 0, binding = 1) uniform sampler2D depth_buffer; // Nearest.
layout(set = 0, binding = 2) uniform sampler2D bright_source; // Quarter, linear.
layout(set = 0, binding = 3) uniform sampler2D flare_source; // Quarter, linear (blur input / composite).
layout(set = 0, binding = 4) uniform sampler2D dirt_texture;
layout(rgba16f, set = 0, binding = 5) uniform restrict image2D dest;
layout(set = 0, binding = 6, std430) restrict buffer SunVisibility {
	float data[];
}
sun_visibility;

layout(set = 0, binding = 7, std140) uniform Params {
	vec2 dest_size;
	vec2 source_size;
	vec2 sun_uv;
	float sun_in_front;
	float sun_radius; // In UV (of the height).
	vec4 sun_color; // Linear, energy and streak intensity included.
	float intensity;
	float threshold;
	float ghost_spacing;
	float halo_width;
	float chromatic;
	float dirt_intensity;
	float aspect;
	float streak_count;
	uint mode;
	uint ghosts;
	uint has_dirt;
	uint pad;
}
params;

#define MODE_BRIGHT 0
#define MODE_FEATURES 1
#define MODE_BLUR_H 2
#define MODE_BLUR_V 3
#define MODE_SUN 4
#define MODE_COMPOSITE 5

const float PI = 3.14159265359;

vec3 fringe(vec2 uv, vec2 dir) {
	vec2 o = dir * params.chromatic;
	return vec3(textureLod(bright_source, uv + o, 0.0).r, textureLod(bright_source, uv, 0.0).g, textureLod(bright_source, uv - o, 0.0).b);
}

// 1 at the center of the image, 0 at its corners: ghosts and halo fade towards the rim of the lens.
float center_weight(vec2 uv, float p) {
	return pow(1.0 - clamp(length(uv - 0.5) / 0.7071, 0.0, 1.0), p);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	if (params.mode == MODE_SUN) {
		// One invocation: the visible fraction of the sun's disc (sky in the depth buffer), faded at the image edges.
		if (pos != ivec2(0)) {
			return;
		}
		float visible = 0.0;
		if (params.sun_in_front > 0.5) {
			float seen = 0.0;
			float total = 0.0;
			for (int i = 0; i < 32; i++) {
				float a = float(i) * 2.39996; // Golden angle spiral.
				float r = sqrt((float(i) + 0.5) / 32.0) * params.sun_radius;
				vec2 uv = params.sun_uv + vec2(cos(a) * r / params.aspect, sin(a) * r);
				if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
					continue;
				}
				total += 1.0;
				if (textureLod(depth_buffer, uv, 0.0).r <= 0.0) { // Reverse Z: the far plane (sky).
					seen += 1.0;
				}
			}
			vec2 edge = min(params.sun_uv, 1.0 - params.sun_uv);
			visible = total > 0.0 ? seen / total * smoothstep(-0.05, 0.05, min(edge.x, edge.y)) : 0.0;
		}
		sun_visibility.data[0] = visible;
		return;
	}

	if (any(greaterThanEqual(pos, ivec2(params.dest_size)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / params.dest_size;

	if (params.mode == MODE_BRIGHT) {
		// Average of the 4x4 source pixels under this quarter pixel (four bilinear taps), above the threshold.
		vec2 o = 1.0 / params.source_size;
		vec3 c = textureLod(color_source, uv + vec2(-o.x, -o.y), 0.0).rgb + textureLod(color_source, uv + vec2(o.x, -o.y), 0.0).rgb + textureLod(color_source, uv + vec2(-o.x, o.y), 0.0).rgb + textureLod(color_source, uv + vec2(o.x, o.y), 0.0).rgb;
		c *= 0.25;
		float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
		float f = max(lum - params.threshold, 0.0) / max(lum, 1e-4);
		imageStore(dest, pos, vec4(min(c * f, vec3(1e4)), 1.0));
		return;
	}

	if (params.mode == MODE_FEATURES) {
		vec2 flip = vec2(1.0) - uv;
		vec2 ghost_vec = (vec2(0.5) - flip) * params.ghost_spacing;
		vec2 dir = length(ghost_vec) > 1e-5 ? normalize(ghost_vec) : vec2(0.0);
		vec3 result = vec3(0.0);
		for (uint i = 0u; i < params.ghosts; i++) {
			vec2 s = fract(flip + ghost_vec * float(i));
			// Warmer near the center, colder towards the rim (coatings).
			vec3 tint = mix(vec3(1.0, 0.8, 0.6), vec3(0.6, 0.8, 1.0), clamp(length(s - 0.5) * 2.0, 0.0, 1.0));
			result += fringe(s, dir) * tint * center_weight(s, 10.0);
		}
		if (params.halo_width > 0.0) {
			// The ring: at a fixed distance from the center (round in the image, not stretched by the aspect).
			vec2 d = (flip - 0.5) * vec2(params.aspect, 1.0);
			vec2 hd = length(d) > 1e-5 ? normalize(d) * params.halo_width : vec2(0.0);
			vec2 h = flip + vec2(hd.x / params.aspect, hd.y);
			result += fringe(h, dir) * center_weight(h, 5.0) * 0.5;
		}
		imageStore(dest, pos, vec4(result, 1.0));
		return;
	}

	if (params.mode == MODE_BLUR_H || params.mode == MODE_BLUR_V) {
		vec2 step = params.mode == MODE_BLUR_H ? vec2(1.0 / params.dest_size.x, 0.0) : vec2(0.0, 1.0 / params.dest_size.y);
		const float w[5] = float[](0.2270, 0.1945, 0.1216, 0.0540, 0.0162);
		vec3 sum = textureLod(flare_source, uv, 0.0).rgb * w[0];
		for (int i = 1; i < 5; i++) {
			sum += (textureLod(flare_source, uv + step * float(i) * 1.5, 0.0).rgb + textureLod(flare_source, uv - step * float(i) * 1.5, 0.0).rgb) * w[i];
		}
		imageStore(dest, pos, vec4(sum, 1.0));
		return;
	}

	// MODE_COMPOSITE (full resolution, in place).
	vec4 color = imageLoad(dest, pos);
	vec3 dirt = params.has_dirt != 0u ? textureLod(dirt_texture, uv, 0.0).rgb * params.dirt_intensity : vec3(0.0);
	vec3 lens = textureLod(flare_source, uv, 0.0).rgb * params.intensity * (1.0 + dirt);

	float visible = sun_visibility.data[0];
	if (visible > 0.0) {
		vec2 d = (uv - params.sun_uv) * vec2(params.aspect, 1.0);
		float r = length(d);
		// Starburst: rays from the edges of the aperture blades, fixed in the image (the aperture turns with the camera).
		float star = 0.0;
		float lines = max(1.0, floor(params.streak_count * 0.5));
		for (float k = 0.0; k < lines; k += 1.0) {
			float a = PI * k / lines + 0.35;
			vec2 axis = vec2(cos(a), sin(a));
			float along = abs(dot(d, axis));
			float across = abs(d.x * axis.y - d.y * axis.x);
			star += exp(-across * 400.0) * exp(-along * 12.0);
		}
		float glare = exp(-r * 40.0) * 0.5 + exp(-r * 6.0) * 0.04;
		vec3 sun = params.sun_color.rgb * visible;
		// The dirt on the lens lights up around the sun.
		lens += sun * (star * 0.5 + glare + dirt * exp(-r * 3.0) * 0.05);
	}
	imageStore(dest, pos, vec4(color.rgb + lens, color.a));
}
