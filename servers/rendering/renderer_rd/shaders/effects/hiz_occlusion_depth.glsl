#[compute]

#version 450

#VERSION_DEFINES

// Mariner E13: the depth buffer reduced to a small grid of distances for occlusion culling on the CPU
// (RendererSceneHiZOcclusion). Each cell holds the distance from the camera of the farthest surface seen in it, or
// "no surface" where any of its pixels sees the sky: an object behind that distance is hidden in the whole cell.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Cells {
	float data[];
}
cells;

layout(push_constant, std430) uniform Params {
	mat4 inv_projection; // Of the depth buffer (depth correction and jitter included).
	ivec2 depth_size;
	ivec2 cell_count;
	uint flip_y; // Row 0 of the depth buffer is the top of the view: write it to the last row (row 0 = bottom).
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

const float NO_SURFACE = 1e30;

void main() {
	ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(cell, params.cell_count))) {
		return;
	}
	// The pixels the cell overlaps (all of them, also those it shares with its neighbors).
	ivec2 from = (cell * params.depth_size) / params.cell_count;
	ivec2 to = min(((cell + 1) * params.depth_size + params.cell_count - 1) / params.cell_count, params.depth_size);

	float farthest = 0.0;
	for (int y = from.y; y < to.y && farthest < NO_SURFACE; y++) {
		for (int x = from.x; x < to.x; x++) {
			float depth = texelFetch(depth_buffer, ivec2(x, y), 0).r;
			if (depth <= 0.0) { // Reverse Z: the far plane (sky).
				farthest = NO_SURFACE;
				break;
			}
			vec2 uv = (vec2(x, y) + 0.5) / vec2(params.depth_size);
			vec4 v = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
			farthest = max(farthest, length(v.xyz / v.w));
		}
	}
	int row = params.flip_y != 0u ? params.cell_count.y - 1 - cell.y : cell.y;
	cells.data[row * params.cell_count.x + cell.x] = farthest;
}
