#[compute]

#version 450

#VERSION_DEFINES

// Mariner V5a: clears a part of an R32UI storage image (the shadow scratch, the shadow stats) by compute. A texture
// clear of a texture used earlier in the same command list is a barrier from the undefined layout, which Direct3D 12
// rejects there (enhanced barriers: SyncBefore NONE after an access); the shadow passes clear these several times a
// frame.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, r32ui) uniform restrict writeonly uimage2D target;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, params.size))) {
		return;
	}
	imageStore(target, p, uvec4(0u));
}
