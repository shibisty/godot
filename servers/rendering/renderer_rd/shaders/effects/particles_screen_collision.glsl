#[compute]

#version 450

#VERSION_DEFINES

// Mariner E22: depth and normal of a view at half resolution for the screen space collision of GPU particles. Each
// texel keeps the nearest of its 2x2 source pixels (a thin rail is not lost); the normal comes from the depth
// differences towards the neighbours, the smaller side on each axis (no normals across silhouette edges).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer; // Reverse Z.
layout(rgba32f, set = 0, binding = 1) uniform restrict writeonly image2D dest; // xyz: world normal, w: view distance (0: none).

layout(set = 0, binding = 2, std140) uniform Params {
	mat4 inv_projection;
	mat4 view_to_world;
	vec2 source_size;
	vec2 dest_size;
}
params;

vec3 view_position(ivec2 p, float depth) {
	vec2 uv = (vec2(p) + 0.5) / params.source_size;
	vec4 v = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return v.xyz / v.w;
}

float fetch_depth(ivec2 p) {
	return texelFetch(depth_buffer, clamp(p, ivec2(0), ivec2(params.source_size) - 1), 0).r;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.dest_size)))) {
		return;
	}
	// The nearest of the 2x2 source pixels (reverse Z: the largest depth).
	ivec2 best = pos * 2;
	float best_depth = fetch_depth(best);
	for (int i = 1; i < 4; i++) {
		ivec2 p = pos * 2 + ivec2(i & 1, i >> 1);
		float d = fetch_depth(p);
		if (d > best_depth) {
			best_depth = d;
			best = p;
		}
	}
	if (best_depth <= 0.0) {
		imageStore(dest, pos, vec4(0.0));
		return;
	}
	vec3 c = view_position(best, best_depth);
	vec3 r = view_position(best + ivec2(1, 0), fetch_depth(best + ivec2(1, 0)));
	vec3 l = view_position(best - ivec2(1, 0), fetch_depth(best - ivec2(1, 0)));
	vec3 d = view_position(best + ivec2(0, 1), fetch_depth(best + ivec2(0, 1)));
	vec3 u = view_position(best - ivec2(0, 1), fetch_depth(best - ivec2(0, 1)));
	vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
	vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
	vec3 n = cross(dy, dx);
	if (dot(n, n) < 1e-12) {
		imageStore(dest, pos, vec4(0.0, 0.0, 0.0, -c.z));
		return;
	}
	n = normalize(n);
	if (dot(n, -c) < 0.0) {
		n = -n; // Towards the camera.
	}
	imageStore(dest, pos, vec4(normalize(mat3(params.view_to_world) * n), -c.z));
}
