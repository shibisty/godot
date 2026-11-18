#[compute]

#version 450

#VERSION_DEFINES

// Mariner V2: the cut of a surface's cluster DAG (ArrayMesh.build_cluster_lod(), V1) for the camera of a scene render.
// One invocation per cluster: the cluster is in the cut when its own error, seen from the camera at its LOD sphere, is
// at most the threshold (screen_mesh_lod_threshold, pixels) and its parent group's error, seen at the parent's sphere,
// is above it (the DAG's errors and spheres grow monotonically: the cut is watertight). Its indices are appended to
// the shadow list (the whole cut) and, inside the camera's side planes, to the camera list; the indirect draw
// commands count them. Everything in the mesh's space (the camera's position brought into it).
//
// V3, shadow passes: each (surface, shadow pass) request picks its own cut - the error below a texel of the pass's
// shadow map (orthogonal: in world units, independent of the camera, so cached cascades stay valid; perspective: from
// the light) - with the pass's side planes, into one shared index buffer:
//   MODE_SHADOW_COUNT  per cluster: its indices counted for its request;
//   MODE_SHADOW_PREFIX one invocation: each request's range in the shared buffer and its indirect command (requests that
//                      no longer fit draw nothing and are counted as overflow);
//   MODE_SHADOW_WRITE  per cluster: its indices into its request's range.
//
// V4, streaming (params.stream bit 31): the source indices are the streaming pool; a cluster counts only when its page
// is resident, and a cluster too coarse for the threshold still counts when the page refining it is not resident -
// that page is requested. Every page drawn from is marked used (ClusterStreaming reads both marks back).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Cluster {
	vec4 cull_sphere; // Bounding sphere of the cluster's triangles.
	vec4 self_sphere; // LOD sphere of its level.
	vec4 parent_sphere; // LOD sphere of the group replacing it.
	float self_error;
	float parent_error; // 3.4e38: a root.
	uint index_offset;
	uint index_count;
};

#if !defined(MODE_SHADOW_PREFIX)
layout(set = 0, binding = 0, std430) restrict readonly buffer Clusters {
	Cluster data[];
}
clusters;

layout(set = 0, binding = 1, std430) restrict readonly buffer SourceIndices {
	uint data[];
}
source_indices;

#endif

#if defined(MODE_SHADOW_COUNT) || defined(MODE_SHADOW_PREFIX) || defined(MODE_SHADOW_WRITE)
#define MODE_SHADOW
#endif

#if !defined(MODE_SHADOW)
layout(set = 0, binding = 2, std430) restrict writeonly buffer CameraIndices {
	uint data[];
}
camera_indices;

layout(set = 0, binding = 3, std430) restrict writeonly buffer ShadowIndices {
	uint data[];
}
shadow_indices;
#elif defined(MODE_SHADOW_WRITE)
layout(set = 0, binding = 2, std430) restrict writeonly buffer SharedIndices {
	uint data[];
}
shared_indices;
#endif

#ifdef MODE_SHADOW
// Per request: index count, first index in the shared buffer, write cursor.
layout(set = 0, binding = 3, std430) restrict buffer Requests {
	uint data[];
}
requests;
#endif

// Indexed indirect draws (index count, instance count, first index, vertex offset, first instance): camera, shadow;
// shadow modes: one per request.
layout(set = 0, binding = 4, std430) restrict buffer Commands {
	uint data[];
}
commands;

#if !defined(MODE_SHADOW_PREFIX)
// V4: per cluster (own page, refining page, offset in the own page, 0); per page its pool offset; request bits then used
// bits of the pages (feedback words each).
layout(set = 0, binding = 6, std430) restrict readonly buffer Stream {
	uvec4 data[];
}
stream;

layout(set = 0, binding = 7, std430) restrict readonly buffer Residency {
	uint data[];
}
residency;

layout(set = 0, binding = 8, std430) restrict buffer Feedback {
	uint data[];
}
feedback;
#endif

// Totals of the render (all surfaces): camera triangles, shadow triangles, camera clusters, shadow clusters (the camera's
// cut); shadow pass triangles, shadow pass clusters, requests over the shared buffer, shadow pass requests (V3).
layout(set = 0, binding = 5, r32ui) uniform restrict uimage2D stats_image;

layout(push_constant, std430) uniform Params {
	vec4 planes[4]; // The camera's side planes in the mesh's space: inside where dot(xyz, p) + w >= -radius.
	vec4 camera; // xyz: the camera's position in the mesh's space; w: pixels per unit of error / distance over the threshold.
	vec4 misc; // x: mesh space units per world unit (distance floor), y: 1 - orthogonal (distance 1).
	uint cluster_count;
	uint request; // Shadow modes: this request; MODE_SHADOW_PREFIX: the request count.
	uint capacity; // Shadow modes: indices in the shared buffer.
	uint stream; // V4: bit 31 streamed; the feedback words.
}
params;

float projected_error(vec4 p_sphere, float p_error) {
	if (p_error >= 3.0e38) {
		return 3.0e38; // A root: nothing coarser.
	}
	float distance_to_sphere = params.misc.y > 0.5 ? max(distance(p_sphere.xyz, params.camera.xyz) - p_sphere.w, 1e-4 * params.misc.x) : 1.0;
	return p_error / distance_to_sphere * params.camera.w;
}

#if !defined(MODE_SHADOW_PREFIX)
const uint NO_PAGE = 0xFFFFFFFFu;

// The first source index of a cluster: its own range, or (streamed) its page's place in the pool plus its offset.
uint source_offset(Cluster c, uint p_index) {
	if ((params.stream & 0x80000000u) == 0u) {
		return c.index_offset;
	}
	uvec4 s = stream.data[p_index];
	return residency.data[s.x] + s.z;
}
#endif

bool selected(Cluster c, uint p_index, bool p_side_planes) {
	if (!(projected_error(c.parent_sphere, c.parent_error) > 1.0)) {
		return false; // The coarser clusters replace it (always resident when this one is).
	}
	bool fine_enough = projected_error(c.self_sphere, c.self_error) <= 1.0;
#if !defined(MODE_SHADOW_PREFIX)
	if ((params.stream & 0x80000000u) != 0u) {
		uvec4 s = stream.data[p_index];
		if (residency.data[s.x] == NO_PAGE) {
			return false; // Not resident: its coarser ancestor stands in.
		}
		if (!fine_enough) {
			if (s.y == NO_PAGE || residency.data[s.y] != NO_PAGE) {
				return false; // The finer clusters are drawn (or this is the finest).
			}
			// Too coarse, but the finer page is not resident: draw this one and ask for it.
			atomicOr(feedback.data[s.y >> 5u], 1u << (s.y & 31u));
		}
		uint words = params.stream & 0x7FFFFFFFu;
		atomicOr(feedback.data[words + (s.x >> 5u)], 1u << (s.x & 31u));
	} else if (!fine_enough) {
		return false;
	}
#else
	if (!fine_enough) {
		return false;
	}
#endif
	if (p_side_planes) {
		for (int p = 0; p < 4; p++) {
			if (dot(params.planes[p].xyz, c.cull_sphere.xyz) + params.planes[p].w < -c.cull_sphere.w) {
				return false;
			}
		}
	}
	return true;
}

void main() {
#if defined(MODE_SHADOW_PREFIX)
	if (gl_GlobalInvocationID.x != 0) {
		return;
	}
	uint offset = 0;
	uint overflow = 0;
	uint triangles = 0;
	for (uint r = 0; r < params.request; r++) {
		uint count = requests.data[r * 3u];
		if (offset + count > params.capacity) {
			count = 0; // No room: this request draws nothing this frame.
			overflow++;
		}
		requests.data[r * 3u] = count;
		requests.data[r * 3u + 1u] = offset;
		requests.data[r * 3u + 2u] = 0;
		commands.data[r * 5u + 0u] = count;
		commands.data[r * 5u + 1u] = 1u;
		commands.data[r * 5u + 2u] = offset;
		commands.data[r * 5u + 3u] = 0u;
		commands.data[r * 5u + 4u] = 0u;
		offset += count;
		triangles += count / 3u;
	}
	imageAtomicAdd(stats_image, ivec2(4, 0), triangles);
	imageAtomicAdd(stats_image, ivec2(6, 0), overflow);
	imageAtomicAdd(stats_image, ivec2(7, 0), params.request);
#else
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.cluster_count) {
		return;
	}
	Cluster c = clusters.data[index];
#if defined(MODE_SHADOW_COUNT)
	if (selected(c, index, true)) {
		atomicAdd(requests.data[params.request * 3u], c.index_count);
		imageAtomicAdd(stats_image, ivec2(5, 0), 1u);
	}
#elif defined(MODE_SHADOW_WRITE)
	uint count = requests.data[params.request * 3u];
	if (count == 0u || !selected(c, index, true)) {
		return;
	}
	uint at = requests.data[params.request * 3u + 1u] + atomicAdd(requests.data[params.request * 3u + 2u], c.index_count);
	uint from = source_offset(c, index);
	for (uint k = 0; k < c.index_count; k++) {
		shared_indices.data[at + k] = source_indices.data[from + k];
	}
#else
	if (!selected(c, index, false)) {
		return;
	}
	uint count = c.index_count;
	uint from = source_offset(c, index);
	uint shadow_offset = atomicAdd(commands.data[5], count);
	for (uint k = 0; k < count; k++) {
		shadow_indices.data[shadow_offset + k] = source_indices.data[from + k];
	}
	imageAtomicAdd(stats_image, ivec2(1, 0), count / 3u);
	imageAtomicAdd(stats_image, ivec2(3, 0), 1u);

	vec3 center = c.cull_sphere.xyz;
	float radius = c.cull_sphere.w;
	for (int p = 0; p < 4; p++) {
		if (dot(params.planes[p].xyz, center) + params.planes[p].w < -radius) {
			return; // Outside the camera's view: only in the shadow list.
		}
	}
	uint camera_offset = atomicAdd(commands.data[0], count);
	for (uint k = 0; k < count; k++) {
		camera_indices.data[camera_offset + k] = source_indices.data[from + k];
	}
	imageAtomicAdd(stats_image, ivec2(0, 0), count / 3u);
	imageAtomicAdd(stats_image, ivec2(2, 0), 1u);
#endif
#endif
}
