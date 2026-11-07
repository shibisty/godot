#[compute]

#version 450

#VERSION_DEFINES

// Mariner E11: screen space contact shadows of a directional light. For every pixel of the depth
// buffer a short ray is marched towards the light in view space; where it passes behind the depth of the frame (by
// less than a thickness), the pixel is in the shadow of something the shadow map is too coarse for.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2D contact_mask;

layout(set = 0, binding = 2, std140) uniform Params {
	mat4 projection; // As used to rasterize the depth buffer (with depth correction and jitter).
	mat4 inv_projection;
	vec4 light_length; // xyz: direction towards the light (view space), w: ray length (m).
	vec4 screen_steps; // xy: size, z: steps, w: thickness (fraction of the view distance).
	vec4 fade; // x: min thickness (m), y: fade start (m), z: fade end (m), w: frame (noise).
}
params;

float view_z(vec2 p_uv, float p_depth) {
	vec4 v = params.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return v.z / v.w;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_steps.xy);
	if (any(greaterThanEqual(pos, size))) {
		return;
	}

	float depth = texelFetch(depth_buffer, pos, 0).r;
	if (depth <= 0.0) { // Reverse Z: the far plane (sky).
		imageStore(contact_mask, pos, vec4(1.0));
		return;
	}

	vec2 uv = (vec2(pos) + 0.5) / vec2(size);
	vec4 v = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 origin = v.xyz / v.w;
	float dist = length(origin);
	if (dist > params.fade.z) {
		imageStore(contact_mask, pos, vec4(1.0));
		return;
	}

	// Interleaved gradient noise, changing every frame (TAA / upscalers average the steps).
	float noise = fract(52.9829189 * fract(dot(vec2(pos) + params.fade.w * 5.588238, vec2(0.06711056, 0.00583715))));

	int steps = int(params.screen_steps.z);
	float step_len = params.light_length.w / float(steps);
	vec3 dir = params.light_length.xyz;
	// Start a little away from the surface (avoid self shadowing of the pixel's own depth).
	float start = step_len * (0.5 + noise);

	float occlusion = 0.0;
	for (int i = 0; i < steps; i++) {
		vec3 p = origin + dir * (start + step_len * float(i));
		vec4 clip = params.projection * vec4(p, 1.0);
		if (clip.w <= 0.0) {
			break;
		}
		vec3 ndc = clip.xyz / clip.w;
		vec2 suv = ndc.xy * 0.5 + 0.5;
		if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
			break; // Left the screen: nothing known there.
		}
		float scene_depth = texelFetch(depth_buffer, ivec2(suv * vec2(size)), 0).r;
		if (scene_depth <= 0.0) {
			continue;
		}
		float scene_z = view_z(suv, scene_depth);
		float behind = scene_z - p.z; // > 0: the ray point is behind the surface seen there (view z is negative).
		float thickness = max(params.fade.x, -p.z * params.screen_steps.w);
		if (behind > 0.0 && behind < thickness) {
			// Closer hits are darker; hits near the end of the ray fade (soft end, no hard cut).
			occlusion = 1.0 - float(i) / float(steps);
			break;
		}
	}

	// Fade out with the view distance (the shadow map takes over) and at the screen border.
	float distance_fade = 1.0 - smoothstep(params.fade.y, params.fade.z, dist);
	vec2 border = min(uv, 1.0 - uv) * 20.0;
	float border_fade = clamp(min(border.x, border.y), 0.0, 1.0);
	occlusion *= distance_fade * border_fade;

	imageStore(contact_mask, pos, vec4(1.0 - occlusion));
}
