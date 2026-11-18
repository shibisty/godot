#[compute]

#version 450

#VERSION_DEFINES

// Mariner V5: software rasterizer of small triangles (MultiMesh.software_raster), after the hierarchical scheme of
// Laine & Karras 2011 / Nanite 2021 (bins -> tiles -> pixels by triangle size; written for Godot). One invocation per
// triangle of an instance: the triangle is transformed, culled (behind the camera, outside the screen, too large for
// this path) and its pixels walked with edge functions. Two passes over the same triangles:
//   MODE_DEPTH    atomicMax of the depth (reverse Z: larger is nearer) into the visibility depth;
//   MODE_PAYLOAD  where the triangle's depth is the stored one, its object, instance and triangle into the payload
//                 and the mesh attributes there (perspective correct) into the attributes, for render_mode
//                 visibility_resolve (V5-T2): normal and tangent of the mesh (object space), UV, vertex colour.
// 32-bit atomics only (Vulkan and Direct3D 12 alike).
// Mariner V5a: MODE_SHADOW (with MODE_DEPTH) - the depth of a shadow pass (orthogonal: cascades, the object map) into
// a scratch R32UI of the pass's rectangle; RenderSoftwareRaster merges it into the shadow map with the depth test.

#define MAX_PIXELS_ACROSS 32 // Larger triangles are not drawn by this path.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Vertices {
	uint data[];
}
vertices;

layout(set = 0, binding = 1, std430) restrict readonly buffer Indices {
	uint data[];
}
indices;

layout(set = 0, binding = 2, std430) restrict readonly buffer Instances {
	float data[];
}
instances;

layout(set = 0, binding = 3, std430) restrict readonly buffer Commands {
	uint data[]; // Indirect multimesh: [word + 1] is the instance count of the first surface (word: flags >> 8).
}
commands;

layout(set = 0, binding = 4, r32ui) uniform restrict uimage2D depth_image;
#ifndef MODE_SHADOW
// x: object << 24 | instance, y: triangle, z: vertex colour (RGBA8), w: bit 0 - the binormal is negative.
layout(set = 0, binding = 5, rgba32ui) uniform restrict uimage2D payload_image;
#endif
layout(set = 0, binding = 6, r32ui) uniform restrict uimage2D stats_image; // Triangles drawn, pixels, too large, near.
#ifndef MODE_SHADOW
// x: normal, y: tangent (octahedral, snorm 16 x 2, object space), zw: UV (float bits, as stored: compressed 0-1).
layout(set = 0, binding = 7, rgba32ui) uniform restrict uimage2D attribute_image;
#endif

layout(set = 0, binding = 8, std430) restrict readonly buffer Attributes {
	uint data[];
}
attributes;

#ifndef MODE_SHADOW
// Per object (4 words at object id * 4), the screen rectangle of its triangles (MODE_DEPTH): ~min x, ~min y, max x + 1,
// max y + 1 (all by atomicMax from 0: 0 - nothing), for the resolve draw.
layout(set = 0, binding = 9, std430) restrict buffer Bounds {
	uint data[];
}
bounds;
#endif

layout(push_constant, std430) uniform Params {
	mat4 view_projection; // Camera projection * view * the multimesh node's transform.
	vec4 position_scale; // Compressed positions: AABB size (xyz); w: the normal stream's first word (uint bits).
	vec4 position_offset; // AABB position (xyz); w: attribute stride << 16 | colour word << 8 | UV word (uint bits).
	uint instance_count; // 0xFFFFFFFF: from the indirect command.
	uint stride; // Floats per instance.
	uint base_offset; // Floats to the first instance of this frame.
	uint object_id; // Upper 8 bits of the payload's instance word.
	uint width;
	uint height;
	uint triangle_count;
	uint flags;
}
params;

#define FLAG_INDEX_16 1u
#define FLAG_COMPRESSED 2u
#define FLAG_NORMAL 4u
#define FLAG_TANGENT 8u
#define FLAG_COLOR 16u
#define FLAG_UV 32u
#define FLAG_PANCAKE 64u // Mariner V5a: casters in front of the near plane flattened onto it (as the shadow pass).

#define M_PI 3.14159265359

vec3 oct_to_vec3(vec2 e) {
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

vec2 vec3_to_oct_snorm(vec3 n) {
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	if (n.z < 0.0) {
		n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	}
	return n.xy;
}

// The vertex shader's _unpack_vertex_attributes() for one vertex of the mesh: normal, tangent, binormal sign.
void vertex_tbn(uint i, out vec3 r_normal, out vec3 r_tangent, out float r_sign) {
	r_normal = vec3(0.0, 0.0, 1.0);
	r_tangent = vec3(1.0, 0.0, 0.0);
	r_sign = 1.0;
	if ((params.flags & FLAG_NORMAL) == 0u) {
		return;
	}
	uint normal_base = floatBitsToUint(params.position_scale.w);
	if ((params.flags & FLAG_COMPRESSED) != 0u) {
		// Axis (octahedral) in the normal stream, the tangent's angle around it in the position's w.
		vec3 axis = oct_to_vec3(unpackUnorm2x16(vertices.data[normal_base + i]) * 2.0 - 1.0);
		float angle = float(vertices.data[i * 2u + 1u] >> 16u) / 65535.0;
		r_sign = angle > 0.5 ? 1.0 : -1.0;
		angle = abs(angle * 2.0 - 1.0) * M_PI;
		float c = cos(angle);
		float s = sin(angle);
		vec3 omc_axis = (1.0 - c) * axis;
		vec3 s_axis = s * axis;
		r_tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
		r_normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
		return;
	}
	uint words = (params.flags & FLAG_TANGENT) != 0u ? 2u : 1u;
	r_normal = oct_to_vec3(unpackUnorm2x16(vertices.data[normal_base + i * words]) * 2.0 - 1.0);
	if ((params.flags & FLAG_TANGENT) != 0u) {
		vec2 st = unpackUnorm2x16(vertices.data[normal_base + i * 2u + 1u]) * 2.0 - 1.0;
		r_tangent = oct_to_vec3(vec2(st.x, abs(st.y) * 2.0 - 1.0));
		r_sign = st.y < 0.0 ? -1.0 : 1.0;
	} else {
		vec3 t = abs(r_normal.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
		r_tangent = normalize(t - r_normal * dot(t, r_normal));
	}
}

vec2 vertex_uv(uint i) {
	if ((params.flags & FLAG_UV) == 0u) {
		return vec2(0.0);
	}
	uint packed = floatBitsToUint(params.position_offset.w);
	uint base = i * (packed >> 16u) + (packed & 0xFFu);
	if ((params.flags & FLAG_COMPRESSED) != 0u) {
		return unpackUnorm2x16(attributes.data[base]);
	}
	return vec2(uintBitsToFloat(attributes.data[base]), uintBitsToFloat(attributes.data[base + 1u]));
}

vec4 vertex_color(uint i) {
	if ((params.flags & FLAG_COLOR) == 0u) {
		return vec4(1.0);
	}
	uint packed = floatBitsToUint(params.position_offset.w);
	return unpackUnorm4x8(attributes.data[i * (packed >> 16u) + ((packed >> 8u) & 0xFFu)]);
}

vec3 vertex_position(uint i) {
	if ((params.flags & FLAG_COMPRESSED) != 0u) {
		uint a = vertices.data[i * 2u];
		uint b = vertices.data[i * 2u + 1u];
		vec3 n = vec3(float(a & 0xFFFFu), float(a >> 16u), float(b & 0xFFFFu)) / 65535.0;
		return n * params.position_scale.xyz + params.position_offset.xyz;
	}
	return vec3(uintBitsToFloat(vertices.data[i * 3u]), uintBitsToFloat(vertices.data[i * 3u + 1u]), uintBitsToFloat(vertices.data[i * 3u + 2u]));
}

uint vertex_index(uint i) {
	if ((params.flags & FLAG_INDEX_16) != 0u) {
		uint word = indices.data[i >> 1u];
		return (i & 1u) != 0u ? word >> 16u : word & 0xFFFFu;
	}
	return indices.data[i];
}

float edge(vec2 a, vec2 b, vec2 p) {
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void rasterize(uint id, inout uvec4 r_bounds) {
	uint instance = id / params.triangle_count;
	uint triangle = id % params.triangle_count;
	uint count = params.instance_count == 0xFFFFFFFFu ? commands.data[(params.flags >> 8u) + 1u] : params.instance_count;
	if (instance >= count) {
		return;
	}
	uint base = params.base_offset + instance * params.stride;
	// Godot's 3D multimesh layout: the three rows of the 3x4 transform.
	vec4 r0 = vec4(instances.data[base + 0u], instances.data[base + 1u], instances.data[base + 2u], instances.data[base + 3u]);
	vec4 r1 = vec4(instances.data[base + 4u], instances.data[base + 5u], instances.data[base + 6u], instances.data[base + 7u]);
	vec4 r2 = vec4(instances.data[base + 8u], instances.data[base + 9u], instances.data[base + 10u], instances.data[base + 11u]);

	vec4 clip[3];
	uint vi[3];
	for (uint k = 0u; k < 3u; k++) {
		vi[k] = vertex_index(triangle * 3u + k);
		vec4 p = vec4(vertex_position(vi[k]), 1.0);
		vec4 w = vec4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
		clip[k] = params.view_projection * w;
#ifdef MODE_SHADOW
		if ((params.flags & FLAG_PANCAKE) != 0u) {
			clip[k].z = min(clip[k].z, 0.9999 * clip[k].w); // The scene shader's pancaking (reverse Z).
		}
#endif
	}
	if (min(clip[0].w, min(clip[1].w, clip[2].w)) <= 1e-4) {
#ifdef MODE_DEPTH
		imageAtomicAdd(stats_image, ivec2(3, 0), 1u);
#endif
		return; // Crosses the near plane: the hardware path draws such objects (V5-T3).
	}
	vec2 size = vec2(params.width, params.height);
	vec2 s0 = (clip[0].xy / clip[0].w * 0.5 + 0.5) * size;
	vec2 s1 = (clip[1].xy / clip[1].w * 0.5 + 0.5) * size;
	vec2 s2 = (clip[2].xy / clip[2].w * 0.5 + 0.5) * size;
	vec3 z = vec3(clip[0].z / clip[0].w, clip[1].z / clip[1].w, clip[2].z / clip[2].w);
	vec3 inv_w = 1.0 / vec3(clip[0].w, clip[1].w, clip[2].w); // Perspective correct attributes.
	uvec3 order = uvec3(0u, 1u, 2u);
	float area = edge(s0, s1, s2);
	if (abs(area) < 1e-8) {
		return;
	}
	if (area < 0.0) { // Either winding (no back face culling): the depth test keeps the front.
		vec2 t = s1;
		s1 = s2;
		s2 = t;
		z = z.xzy;
		inv_w = inv_w.xzy;
		order = order.xzy;
		area = -area;
	}
	vec2 lo = floor(min(s0, min(s1, s2)) - 0.5);
	vec2 hi = ceil(max(s0, max(s1, s2)) - 0.5);
	lo = max(lo, vec2(0.0));
	hi = min(hi, size - 1.0);
	if (any(greaterThan(lo, hi))) {
		return; // Off screen.
	}
	if (hi.x - lo.x >= float(MAX_PIXELS_ACROSS) || hi.y - lo.y >= float(MAX_PIXELS_ACROSS)) {
#ifdef MODE_DEPTH
		imageAtomicAdd(stats_image, ivec2(2, 0), 1u);
#endif
		return;
	}
	if (max(z.x, max(z.y, z.z)) <= 0.0) {
		return; // Beyond the far plane.
	}
	r_bounds = max(r_bounds, uvec4(~uint(lo.x), ~uint(lo.y), uint(hi.x) + 1u, uint(hi.y) + 1u));
	float inv_area = 1.0 / area;
	uint pixels = 0u;
#ifdef MODE_PAYLOAD
	vec3 n[3];
	vec3 t[3];
	float binormal_sign = 1.0;
	vec2 uv[3];
	vec4 color[3];
	for (uint k = 0u; k < 3u; k++) {
		uint v = vi[order[k]];
		float s;
		vertex_tbn(v, n[k], t[k], s);
		binormal_sign = k == 0u ? s : binormal_sign; // Per triangle (MikkTSpace: constant over it).
		uv[k] = vertex_uv(v);
		color[k] = vertex_color(v);
	}
#endif
	for (float y = lo.y; y <= hi.y; y += 1.0) {
		for (float x = lo.x; x <= hi.x; x += 1.0) {
			vec2 p = vec2(x, y) + 0.5;
			float w0 = edge(s1, s2, p);
			float w1 = edge(s2, s0, p);
			float w2 = edge(s0, s1, p);
			if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
				continue;
			}
			float d = (w0 * z.x + w1 * z.y + w2 * z.z) * inv_area;
			if (d <= 0.0 || d > 1.0) {
				continue;
			}
			ivec2 px = ivec2(x, y);
			uint du = floatBitsToUint(d);
#ifdef MODE_DEPTH
			imageAtomicMax(depth_image, px, du);
			pixels++;
#else
			if (imageLoad(depth_image, px).r == du) {
				vec3 b = vec3(w0, w1, w2) * inv_w;
				b /= b.x + b.y + b.z;
				vec3 normal = normalize(n[0] * b.x + n[1] * b.y + n[2] * b.z);
				vec3 tangent = t[0] * b.x + t[1] * b.y + t[2] * b.z;
				tangent = normalize(tangent - normal * dot(normal, tangent) + vec3(1e-7, 0.0, 0.0));
				vec2 tex = uv[0] * b.x + uv[1] * b.y + uv[2] * b.z;
				vec4 col = color[0] * b.x + color[1] * b.y + color[2] * b.z;
				imageStore(payload_image, px, uvec4((params.object_id << 24u) | (instance & 0xFFFFFFu), triangle, packUnorm4x8(col), binormal_sign < 0.0 ? 1u : 0u));
				imageStore(attribute_image, px, uvec4(packSnorm2x16(vec3_to_oct_snorm(normal)), packSnorm2x16(vec3_to_oct_snorm(tangent)), floatBitsToUint(tex.x), floatBitsToUint(tex.y)));
			}
#endif
		}
	}
#ifdef MODE_DEPTH
	imageAtomicAdd(stats_image, ivec2(0, 0), 1u);
	imageAtomicAdd(stats_image, ivec2(1, 0), pixels);
#endif
}

shared uint group_bounds[4];

void main() {
	if (gl_LocalInvocationIndex < 4u) {
		group_bounds[gl_LocalInvocationIndex] = 0u;
	}
	memoryBarrierShared();
	barrier();
	uvec4 b = uvec4(0u);
	rasterize((gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x, b);
#if defined(MODE_DEPTH) && !defined(MODE_SHADOW)
	// The object's screen rectangle: reduced in the work group, then 4 atomics per group.
	if (b.z != 0u) {
		atomicMax(group_bounds[0], b.x);
		atomicMax(group_bounds[1], b.y);
		atomicMax(group_bounds[2], b.z);
		atomicMax(group_bounds[3], b.w);
	}
	memoryBarrierShared();
	barrier();
	if (gl_LocalInvocationIndex < 4u && group_bounds[gl_LocalInvocationIndex] != 0u) {
		atomicMax(bounds.data[params.object_id * 4u + gl_LocalInvocationIndex], group_bounds[gl_LocalInvocationIndex]);
	}
#endif
}
