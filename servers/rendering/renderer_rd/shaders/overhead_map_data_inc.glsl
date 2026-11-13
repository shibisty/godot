// Mariner G1a: the uniform data of the overhead map (OverheadMap::UBO in effects/overhead_map.h, std140).

struct OverheadMapData {
	vec4 box_min; // xyz: the box's minimum (world), w: texel size (m).
	vec4 box_size; // x: size (m), y: height (m), z: 1 / size, w: 1 / height.
	vec4 params; // x: sky occlusion amount, y: radius (m), z: bias (m), w: active (1 / 0).
	vec4 params2; // x: mipmap count.
};
