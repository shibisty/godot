#[compute]

#version 450

#VERSION_DEFINES

// Mariner E14b: the instances of an indirect MultiMesh that a shadow pass needs. The generator (a compute shader of
// the game) writes its instances and their count (instance count of the first surface's command) as for the camera;
// for each shadow pass (a cascade, a spot light, a face of an omni light) the instances whose bounding sphere touches
// the pass are copied into a section of their own, with draw commands of their own.
// Mariner E25a: the same for the camera of each scene render.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Source {
	vec4 data[];
}
source;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Dest {
	vec4 data[];
}
dest;

layout(set = 0, binding = 2, std430) restrict readonly buffer SourceCommands {
	uint data[];
}
source_commands;

layout(set = 0, binding = 3, std430) restrict coherent buffer Counters {
	uint data[];
}
counters;

layout(set = 0, binding = 4, std430) restrict writeonly buffer DestCommands {
	uint data[];
}
dest_commands;

// Mariner E25b: occlusion culling of the camera sections by the depth pyramid the CPU culls instances with
// (RendererSceneOcclusionCull::HZBuffer: the occluder raster or the reprojected depth of earlier frames), the same
// test as HZBuffer::_is_occluded() with each instance's box.
layout(set = 0, binding = 5, std430) restrict readonly buffer OcclusionDepth {
	float data[]; // All mips, rows from the bottom of the view.
}
occlusion_depth;

#define SECTION_OCCLUSION 1
#define SECTION_RANGE 2
#define SECTION_LOD 4
#define SECTION_LOD_ORTHOGONAL 8

#define MAX_LOD_BUCKETS 8
#define MAX_LOD_SURFACES 8

// Per section (pass), optional: what else than the side planes culls its instances.
struct SectionData {
	vec4 view_rows[3]; // View from the multimesh's space (rows of the affine transform), for occlusion.
	mat4 projection; // For occlusion.
	vec4 aabb_min; // Mesh box.
	vec4 aabb_max;
	vec4 world_rows[3]; // World from the multimesh's space, for the visibility range and LOD distances.
	vec4 camera; // xyz: the camera's position (world); w: LOD distance factor (lod_distance_multiplier / lod_bias).
	vec4 range; // Mariner E25c: visibility range begin, end, begin margin, end margin (0: no limit).
	vec4 lod_limits[2]; // Mariner E25c: upper end of each LOD bucket but the last (LOD distance / model scale).
	uvec4 info; // Buckets, SECTION_* flags.
	uvec4 lod_counts[MAX_LOD_BUCKETS * MAX_LOD_SURFACES / 4]; // Index count per bucket and surface.
};

layout(set = 0, binding = 6, std430) restrict readonly buffer Sections {
	SectionData data[];
}
sections;

layout(set = 0, binding = 7, std430) restrict buffer CullState {
	uint data[]; // Per source instance: occlusion timeout frame, visibility range state (1: in range).
}
cull_state;

layout(set = 0, binding = 8, std430) restrict readonly buffer Occlusion {
	uvec4 mips[24]; // Width, height, offset in occlusion_depth.
	uvec4 info; // Mip count, frame, occlusion timers (jitter), orthogonal.
	vec4 misc; // Z near.
}
occlusion;

layout(push_constant, std430) uniform Params {
	vec4 planes[4]; // Side planes of the pass in the multimesh's space (normalized), inside: dot >= 0.
	vec4 sphere; // Bounding sphere of the mesh: center, radius.
	uint stride; // Floats per instance.
	uint source_offset; // Instances.
	uint section;
	uint capacity; // Instances per section (and LOD bucket).
	uint mode; // MODE_*, LOD buckets per section << 8.
	uint surface_count;
	uint sections; // Sections times buckets.
	uint section_data; // 0: none, otherwise the index of the first section's SectionData + 1.
}
params;

#define MODE_CULL 0
#define MODE_COMMANDS 1
#define COMMAND_STRIDE 5

#define OCCLUSION_MAX_SAMPLES 512

bool is_occluded(SectionData p_section, mat4 p_instance) {
	mat4 view = transpose(mat4(p_section.view_rows[0], p_section.view_rows[1], p_section.view_rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 view_from_instance = view * p_instance;

	float z_near = occlusion.misc.x;
	bool orthogonal = occlusion.info.w != 0;
	vec3 view_min = vec3(1e30);
	vec3 view_max = vec3(-1e30);
	vec2 rect_min = vec2(1e30);
	vec2 rect_max = vec2(-1e30);
	for (uint j = 0; j < 8; j++) {
		vec3 corner = vec3((j & 4) != 0 ? p_section.aabb_min.x : p_section.aabb_max.x, (j & 2) != 0 ? p_section.aabb_min.y : p_section.aabb_max.y, (j & 1) != 0 ? p_section.aabb_min.z : p_section.aabb_max.z);
		vec3 v = (view_from_instance * vec4(corner, 1.0)).xyz;
		if (-v.z < z_near) {
			return false; // Reaches the near plane (or behind the camera): visible, as the CPU test treats the camera's surroundings.
		}
		view_min = min(view_min, v);
		view_max = max(view_max, v);
		vec4 clip = p_section.projection * vec4(v, 1.0);
		vec2 normalized = (clip.xy / clip.w) * 0.5 + 0.5;
		rect_min = min(rect_min, normalized);
		rect_max = max(rect_max, normalized);
	}

	// Nearest distance of the box: the closest point of the view space box of the instance's box (conservative).
	float min_depth;
	if (orthogonal) {
		min_depth = -view_max.z;
	} else {
		min_depth = length(clamp(vec3(0.0), view_min, view_max));
	}

	rect_max = min(rect_max, vec2(1.0));
	rect_min = max(rect_min, vec2(0.0));

	int mip_count = int(occlusion.info.x);
	vec2 screen_diagonal = (rect_max - rect_min) * vec2(occlusion.mips[0].xy);
	float size = max(screen_diagonal.x, screen_diagonal.y);
	int lod = clamp(int(ceil(log2(max(size, 1e-6)))), 0, mip_count - 1);

	int sample_count = 0;
	for (; lod >= 0; lod--) {
		int w = int(occlusion.mips[lod].x);
		int h = int(occlusion.mips[lod].y);
		uint offset = occlusion.mips[lod].z;

		int minx = clamp(int(rect_min.x * float(w) - 1.0), 0, w - 1);
		int maxx = clamp(int(rect_max.x * float(w) + 1.0), 0, w - 1);
		int miny = clamp(int(rect_min.y * float(h) - 1.0), 0, h - 1);
		int maxy = clamp(int(rect_max.y * float(h) + 1.0), 0, h - 1);

		sample_count += (maxx - minx + 1) * (maxy - miny + 1);
		if (sample_count > OCCLUSION_MAX_SAMPLES) {
			return false;
		}

		bool visible = false;
		for (int y = miny; y <= maxy && !visible; y++) {
			for (int x = minx; x <= maxx; x++) {
				if (occlusion_depth.data[offset + uint(y * w + x)] > min_depth) {
					visible = true;
					break;
				}
			}
		}
		if (!visible) {
			return true;
		}
	}
	return false;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint buckets = max(params.mode >> 8, 1u);

	if ((params.mode & 0xFF) == MODE_COMMANDS) {
		// One thread per section (and bucket) and surface: the source command with the section's instance count and, per
		// LOD bucket, the index count of the bucket's LOD.
		if (index >= params.sections * params.surface_count) {
			return;
		}
		uint section = index / params.surface_count;
		uint surface = index % params.surface_count;
		for (uint k = 0; k < COMMAND_STRIDE; k++) {
			dest_commands.data[index * COMMAND_STRIDE + k] = source_commands.data[surface * COMMAND_STRIDE + k];
		}
		dest_commands.data[index * COMMAND_STRIDE + 1] = counters.data[section];
		if (buckets > 1 && params.section_data != 0 && surface < MAX_LOD_SURFACES) {
			uint bucket = section % buckets;
			uint entry = bucket * MAX_LOD_SURFACES + surface;
			uint count = sections.data[params.section_data - 1 + section / buckets].lod_counts[entry / 4][entry % 4];
			if (count != 0) {
				dest_commands.data[index * COMMAND_STRIDE + 0] = count;
			}
		}
		return;
	}

	uint count = min(source_commands.data[1], params.capacity);
	if (index >= count) {
		return;
	}
	uint vec4s = params.stride / 4;
	uint base = (params.source_offset + index) * vec4s;
	// 3D transform: three rows.
	vec4 r0 = source.data[base + 0];
	vec4 r1 = source.data[base + 1];
	vec4 r2 = source.data[base + 2];

	uint bucket = 0;
	if (params.section_data != 0) {
		SectionData section_data = sections.data[params.section_data - 1 + params.section];
		uint flags = section_data.info.y;
		mat4 instance = transpose(mat4(r0, r1, r2, vec4(0.0, 0.0, 0.0, 1.0)));
		mat4 world = transpose(mat4(section_data.world_rows[0], section_data.world_rows[1], section_data.world_rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
		mat4 world_from_instance = world * instance;

		if ((flags & SECTION_RANGE) != 0) {
			// Mariner E25c: visibility range (fade disabled), as RendererSceneCull::_visibility_range_check(): the distance
			// from the camera to the center of the instance's box, the range widened by the margins while in range
			// (hysteresis), narrowed while out of it. Camera and shadow passes evaluate the same state.
			vec3 center = (world_from_instance * vec4((section_data.aabb_min.xyz + section_data.aabb_max.xyz) * 0.5, 1.0)).xyz;
			float dist = distance(section_data.camera.xyz, center);
			bool was_in_range = cull_state.data[index * 2 + 1] != 0;
			float begin_offset = was_in_range ? -section_data.range.z : section_data.range.z;
			float end_offset = was_in_range ? section_data.range.w : -section_data.range.w;
			bool in_range = !(section_data.range.y > 0.0 && dist > section_data.range.y + end_offset) && !(section_data.range.x > 0.0 && dist < section_data.range.x + begin_offset);
			if (in_range != was_in_range) {
				cull_state.data[index * 2 + 1] = in_range ? 1 : 0;
			}
			if (!in_range) {
				return;
			}
		}

		vec4 c = vec4(params.sphere.xyz, 1.0);
		vec3 center = vec3(dot(r0, c), dot(r1, c), dot(r2, c));
		float scale = max(length(vec3(r0.x, r1.x, r2.x)), max(length(vec3(r0.y, r1.y, r2.y)), length(vec3(r0.z, r1.z, r2.z))));
		float radius = params.sphere.w * scale;
		for (uint p = 0; p < 4; p++) {
			if (dot(params.planes[p].xyz, center) + params.planes[p].w < -radius) {
				return;
			}
		}

		if ((flags & SECTION_OCCLUSION) != 0) {
			bool occluded = is_occluded(section_data, instance);
			if (occlusion.info.z != 0) {
				// Occlusion timers (as HZBuffer::is_occluded()): an instance seen stays visible for some frames, so that a
				// jittered occluder raster doesn't make it flicker.
				uint frame = occlusion.info.y;
				uint timeout = cull_state.data[index * 2];
				if (!occluded) {
					cull_state.data[index * 2] = frame + 9;
				} else if (timeout != 0 && frame >= timeout) {
					cull_state.data[index * 2] = 0;
					timeout = 0;
				}
				occluded = occluded && timeout == 0;
			}
			if (occluded) {
				return;
			}
		}

		if ((flags & SECTION_LOD) != 0 && buckets > 1) {
			// Mariner E25c: mesh LOD per instance, as the renderer picks it per instance (distance from the camera to the
			// instance's world box, over its largest scale); the bucket's LODs are picked on the CPU.
			float lod_distance = 1.0;
			if ((flags & SECTION_LOD_ORTHOGONAL) == 0) {
				vec3 box_min = vec3(1e30);
				vec3 box_max = vec3(-1e30);
				for (uint j = 0; j < 8; j++) {
					vec3 corner = vec3((j & 4) != 0 ? section_data.aabb_min.x : section_data.aabb_max.x, (j & 2) != 0 ? section_data.aabb_min.y : section_data.aabb_max.y, (j & 1) != 0 ? section_data.aabb_min.z : section_data.aabb_max.z);
					vec3 w = (world_from_instance * vec4(corner, 1.0)).xyz;
					box_min = min(box_min, w);
					box_max = max(box_max, w);
				}
				vec3 camera = section_data.camera.xyz;
				lod_distance = length(max(vec3(0.0), max(box_min - camera, camera - box_max)));
			}
			mat3 basis = mat3(world_from_instance);
			float model_scale = max(length(basis[0]), max(length(basis[1]), length(basis[2])));
			float normalized = lod_distance * section_data.camera.w / max(model_scale, 1e-6);
			for (uint b = 0; b < buckets - 1; b++) {
				if (normalized >= section_data.lod_limits[b / 4][b % 4]) {
					bucket = b + 1;
				}
			}
		}
	} else {
		vec4 c = vec4(params.sphere.xyz, 1.0);
		vec3 center = vec3(dot(r0, c), dot(r1, c), dot(r2, c));
		float scale = max(length(vec3(r0.x, r1.x, r2.x)), max(length(vec3(r0.y, r1.y, r2.y)), length(vec3(r0.z, r1.z, r2.z))));
		float radius = params.sphere.w * scale;
		for (uint p = 0; p < 4; p++) {
			if (dot(params.planes[p].xyz, center) + params.planes[p].w < -radius) {
				return;
			}
		}
	}

	uint region = params.section * buckets + bucket;
	uint slot = atomicAdd(counters.data[region], 1);
	uint dst = (region * params.capacity + slot) * vec4s;
	for (uint k = 0; k < vec4s; k++) {
		dest.data[dst + k] = source.data[base + k];
	}
}
