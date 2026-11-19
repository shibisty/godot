#define M_PI 3.14159265359
#define M_TAU 6.28318530718
#define ROUGHNESS_MAX_LOD 5

#define MAX_VOXEL_GI_INSTANCES 8
#define MAX_VIEWS 2

#extension GL_KHR_shader_subgroup_ballot : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable

#include "../cluster_data_inc.glsl"
#include "../decal_data_inc.glsl"
#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"

#if !defined(MODE_RENDER_DEPTH) || defined(MODE_RENDER_MATERIAL) || defined(MODE_RENDER_SDF) || defined(MODE_RENDER_NORMAL_ROUGHNESS) || defined(MODE_RENDER_VOXEL_GI) || defined(TANGENT_USED) || defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED)
#ifndef NORMAL_USED
#define NORMAL_USED
#endif
#endif

#if !defined(TANGENT_USED) && (defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED))
#define TANGENT_USED
#endif

layout(push_constant, std430) uniform DrawCall {
	uint instance_index;
	uint uv_offset;
	uint multimesh_motion_vectors_current_offset;
	uint multimesh_motion_vectors_previous_offset;
#ifdef UBERSHADER
	uint sc_packed_0;
	uint sc_packed_1;
	uint sc_packed_2;
	uint uc_packed_0;
#endif
}
draw_call;

/* Specialization Constants */

#ifdef UBERSHADER

#define POLYGON_CULL_DISABLED 0
#define POLYGON_CULL_FRONT 1
#define POLYGON_CULL_BACK 2

// Pull the constants from the draw call's push constants.
uint sc_packed_0() {
	return draw_call.sc_packed_0;
}

uint sc_packed_1() {
	return draw_call.sc_packed_1;
}

uint uc_cull_mode() {
	return (draw_call.uc_packed_0 >> 0) & 3U;
}

#else

// Pull the constants from the pipeline's specialization constants.
layout(constant_id = 0) const uint pso_sc_packed_0 = 0;
layout(constant_id = 1) const uint pso_sc_packed_1 = 0;

uint sc_packed_0() {
	return pso_sc_packed_0;
}

uint sc_packed_1() {
	return pso_sc_packed_1;
}

#endif

bool sc_use_forward_gi() {
	return ((sc_packed_0() >> 0) & 1U) != 0;
}

bool sc_use_light_projector() {
	return ((sc_packed_0() >> 1) & 1U) != 0;
}

bool sc_use_light_soft_shadows() {
	return ((sc_packed_0() >> 2) & 1U) != 0;
}

bool sc_use_directional_soft_shadows() {
	return ((sc_packed_0() >> 3) & 1U) != 0;
}

bool sc_decal_use_mipmaps() {
	return ((sc_packed_0() >> 4) & 1U) != 0;
}

bool sc_projector_use_mipmaps() {
	return ((sc_packed_0() >> 5) & 1U) != 0;
}

bool sc_use_depth_fog() {
	return ((sc_packed_0() >> 6) & 1U) != 0;
}

bool sc_use_lightmap_bicubic_filter() {
	return ((sc_packed_0() >> 7) & 1U) != 0;
}

uint sc_soft_shadow_samples() {
	return (sc_packed_0() >> 8) & 63U;
}

uint sc_penumbra_shadow_samples() {
	return (sc_packed_0() >> 14) & 63U;
}

uint sc_directional_soft_shadow_samples() {
	return (sc_packed_0() >> 20) & 63U;
}

uint sc_directional_penumbra_shadow_samples() {
	return (sc_packed_0() >> 26) & 63U;
}

bool sc_multimesh() {
	return ((sc_packed_1() >> 0) & 1U) != 0;
}

bool sc_multimesh_format_2d() {
	return ((sc_packed_1() >> 1) & 1U) != 0;
}

bool sc_multimesh_has_color() {
	return ((sc_packed_1() >> 2) & 1U) != 0;
}

// Mariner E14c.
bool sc_multimesh_has_custom_data2() {
	return ((sc_packed_1() >> 6) & 1U) != 0;
}

bool sc_multimesh_has_custom_data() {
	return ((sc_packed_1() >> 3) & 1U) != 0;
}

bool sc_fog_use_legacy_blending() {
	return ((sc_packed_1() >> 4) & 1U) != 0;
}

bool sc_cluster_has_area_light() {
	return ((sc_packed_1() >> 5) & 1U) != 0;
}

// Mariner AA: render_mode alpha_fringe, drawing the edge (transparent pass).
bool sc_alpha_fringe_pass() {
	return ((sc_packed_1() >> 7) & 1U) != 0;
}

float sc_luminance_multiplier() {
	// Not used in clustered renderer but we share some code with the mobile renderer that requires this.
	return 1.0;
}

layout(constant_id = 2) const bool sc_emulate_point_size = false;

#ifdef POINT_SIZE_USED

#define VERTEX_INDEX (sc_emulate_point_size ? gl_InstanceIndex : gl_VertexIndex)
#define INSTANCE_INDEX (sc_emulate_point_size ? (gl_VertexIndex / 6) : gl_InstanceIndex)

#else

#define VERTEX_INDEX gl_VertexIndex
#define INSTANCE_INDEX gl_InstanceIndex

#endif

#define REFLECTION_MULTIPLIER 1.0

#define SDFGI_MAX_CASCADES 8

/* Set 0: Base Pass (never changes) */

#include "../light_data_inc.glsl"

layout(set = 0, binding = 2) uniform sampler shadow_sampler;

// Mariner V5-T2: the instance is drawn as the visibility resolve of the software raster (one full screen triangle),
// its object id in the payload in bits 17-23 (a multimesh: no particle trails).
#define INSTANCE_FLAGS_VISIBILITY_RESOLVE (1 << 0)
#define INSTANCE_FLAGS_VISIBILITY_RESOLVE_OBJECT_SHIFT 17
#define INSTANCE_FLAGS_DYNAMIC (1 << 3)
#define INSTANCE_FLAGS_NON_UNIFORM_SCALE (1 << 4)
#define INSTANCE_FLAGS_USE_GI_BUFFERS (1 << 5)
#define INSTANCE_FLAGS_USE_SDFGI (1 << 6)
#define INSTANCE_FLAGS_USE_LIGHTMAP_CAPTURE (1 << 7)
#define INSTANCE_FLAGS_USE_LIGHTMAP (1 << 8)
#define INSTANCE_FLAGS_USE_SH_LIGHTMAP (1 << 9)
#define INSTANCE_FLAGS_USE_VOXEL_GI (1 << 10)
#define INSTANCE_FLAGS_PARTICLES (1 << 11)
#define INSTANCE_FLAGS_PARTICLE_TRAIL_SHIFT 16
#define INSTANCE_FLAGS_FADE_SHIFT 24
//3 bits of stride
#define INSTANCE_FLAGS_PARTICLE_TRAIL_MASK 0xFF

#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSAO (1 << 0)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSIL (1 << 1)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSR (1 << 2)
#define SCREEN_SPACE_EFFECTS_FLAGS_RESOLVE_SSR (1 << 3)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_GTAO (1 << 4) // Mariner E18: ao_buffer holds the world bent normal (rgb) and the visibility (a).
#define SCREEN_SPACE_EFFECTS_FLAGS_GTAO_BENT_NORMALS (1 << 5)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_RESTIR (1 << 6) // Mariner E20: Light3D.light_restir lights come from restir_diffuse / restir_specular.
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_RT_SHADOW (1 << 7) // Mariner E20b/c: shadows of Light3D.light_restir directional lights from restir_shadow_mask.

layout(set = 0, binding = 3, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 4, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 5, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;

layout(set = 0, binding = 6, std430) restrict readonly buffer ReflectionProbeData {
	ReflectionData data[];
}
reflections;

layout(set = 0, binding = 7, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

#define LIGHTMAP_FLAG_USE_DIRECTION 1
#define LIGHTMAP_FLAG_USE_SPECULAR_DIRECTION 2

#define LIGHTMAP_SHADOWMASK_MODE_NONE 0
#define LIGHTMAP_SHADOWMASK_MODE_REPLACE 1
#define LIGHTMAP_SHADOWMASK_MODE_OVERLAY 2
#define LIGHTMAP_SHADOWMASK_MODE_ONLY 3

struct Lightmap {
	mat3 normal_xform;
	vec2 light_texture_size;
	float exposure_normalization;
	uint flags;
};

layout(set = 0, binding = 8, std140) restrict readonly buffer Lightmaps {
	Lightmap data[];
}
lightmaps;

struct LightmapCapture {
	vec4 sh[9];
};

layout(set = 0, binding = 9, std140) restrict readonly buffer LightmapCaptures {
	LightmapCapture data[];
}
lightmap_captures;

layout(set = 0, binding = 10) uniform texture2D decal_atlas;
layout(set = 0, binding = 11) uniform texture2D decal_atlas_srgb;

layout(set = 0, binding = 12, std430) restrict readonly buffer Decals {
	DecalData data[];
}
decals;

layout(set = 0, binding = 13, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

struct SDFVoxelGICascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell; // 1/bounds * grid_size
	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 14, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	SDFVoxelGICascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

layout(set = 0, binding = 15) uniform sampler DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;

layout(set = 0, binding = 16) uniform texture2D best_fit_normal_texture;

layout(set = 0, binding = 17) uniform texture2D dfg;

layout(set = 0, binding = 18) uniform sampler2D ltc_lut1;

layout(set = 0, binding = 19) uniform sampler2D ltc_lut2;

layout(set = 0, binding = 20) uniform texture2D area_light_atlas;
/* Set 1: Render Pass (changes per render pass) */

layout(set = 1, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

struct ImplementationData {
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;

	uint ss_effects_flags;
	float ssao_light_affect;
	float ssao_ao_affect;
	uint pad1;

	mat4 sdf_to_bounds;

	ivec3 sdf_offset;
	uint pad2;

	ivec3 sdf_size;
	bool gi_upscale_for_msaa;

	bool volumetric_fog_enabled;
	float volumetric_fog_inv_length;
	float volumetric_fog_detail_spread;
	uint volumetric_fog_pad;
};

layout(set = 1, binding = 1, std140) uniform ImplementationDataBlock {
	ImplementationData data;
}
implementation_data_block;

#define implementation_data implementation_data_block.data

struct InstanceData {
	mat3x4 transform;
	vec4 compressed_aabb_position_pad; // Only .xyz is used. .w is padding.
	vec4 compressed_aabb_size_pad; // Only .xyz is used. .w is padding.
	vec4 uv_scale;
	uint flags;
	uint instance_uniforms_ofs; //base offset in global buffer for instance variables
	uint gi_offset; //GI information when using lightmapping (VCT or lightmap index)
	uint layer_mask;
	mat3x4 prev_transform;
	vec4 lightmap_uv_scale;
#ifdef USE_DOUBLE_PRECISION
	vec4 model_precision;
	vec4 prev_model_precision;
#endif
};

layout(set = 1, binding = 2, std430) buffer restrict readonly InstanceDataBuffer {
	InstanceData data[];
}
instances;

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 1, binding = 3) uniform texture2DArray radiance_octmap;

#else

layout(set = 1, binding = 3) uniform texture2D radiance_octmap;

#endif

layout(set = 1, binding = 4) uniform texture2DArray reflection_atlas;

layout(set = 1, binding = 5) uniform texture2D shadow_atlas;

layout(set = 1, binding = 6) uniform texture2D directional_shadow_atlas;

layout(set = 1, binding = 7) uniform texture2DArray lightmap_textures[MAX_LIGHTMAP_TEXTURES * 2];

layout(set = 1, binding = 8) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];

layout(set = 1, binding = 9, std430) buffer restrict readonly ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 1, binding = 10) uniform sampler decal_sampler;

layout(set = 1, binding = 11) uniform sampler light_projector_sampler;

layout(set = 1, binding = 12 + 0) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 1, binding = 12 + 1) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 1, binding = 12 + 2) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 12 + 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 12 + 4) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 12 + 5) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 12 + 6) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 1, binding = 12 + 7) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 1, binding = 12 + 8) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 12 + 9) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 12 + 10) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 1, binding = 12 + 11) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

#ifdef MODE_RENDER_SDF

layout(r16ui, set = 1, binding = 24) uniform restrict writeonly uimage3D albedo_volume_grid;
layout(r32ui, set = 1, binding = 25) uniform restrict writeonly uimage3D emission_grid;
layout(r32ui, set = 1, binding = 26) uniform restrict writeonly uimage3D emission_aniso_grid;
layout(r32ui, set = 1, binding = 27) uniform restrict uimage3D geom_facing_grid;

//still need to be present for shaders that use it, so remap them to something
#define depth_buffer shadow_atlas
#define color_buffer shadow_atlas
#define normal_roughness_buffer shadow_atlas

#define multiviewSampler sampler2D
#else

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 24) uniform texture2DArray depth_buffer;
layout(set = 1, binding = 25) uniform texture2DArray color_buffer;
layout(set = 1, binding = 26) uniform texture2DArray normal_roughness_buffer;
layout(set = 1, binding = 27) uniform texture2DArray ao_buffer;
layout(set = 1, binding = 28) uniform texture2DArray ambient_buffer;
layout(set = 1, binding = 29) uniform texture2DArray reflection_buffer;
#define multiviewSampler sampler2DArray
#else // USE_MULTIVIEW
layout(set = 1, binding = 24) uniform texture2D depth_buffer;
layout(set = 1, binding = 25) uniform texture2D color_buffer;
layout(set = 1, binding = 26) uniform texture2D normal_roughness_buffer;
layout(set = 1, binding = 27) uniform texture2D ao_buffer;
layout(set = 1, binding = 28) uniform texture2D ambient_buffer;
layout(set = 1, binding = 29) uniform texture2D reflection_buffer;
#define multiviewSampler sampler2D
#endif
layout(set = 1, binding = 30) uniform texture2DArray sdfgi_lightprobe_texture;
layout(set = 1, binding = 31) uniform texture3D sdfgi_occlusion_cascades;

struct VoxelGIData {
	mat4 xform; // 64 - 64

	vec3 bounds; // 12 - 76
	float dynamic_range; // 4 - 80

	float bias; // 4 - 84
	float normal_bias; // 4 - 88
	bool blend_ambient; // 4 - 92
	uint mipmaps; // 4 - 96

	vec3 pad; // 12 - 108
	float exposure_normalization; // 4 - 112
};

layout(set = 1, binding = 32, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 1, binding = 33) uniform texture3D volumetric_fog_texture;

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 34) uniform texture2DArray ssil_buffer;
layout(set = 1, binding = 35) uniform texture2DArray ssr_buffer;
layout(set = 1, binding = 36) uniform texture2DArray ssr_mip_level_buffer;
#else
layout(set = 1, binding = 34) uniform texture2D ssil_buffer;
layout(set = 1, binding = 35) uniform texture2D ssr_buffer;
layout(set = 1, binding = 36) uniform texture2D ssr_mip_level_buffer;
#endif // USE_MULTIVIEW

layout(set = 1, binding = 37) uniform texture2D directional_object_shadow; // Mariner E10b.
layout(set = 1, binding = 38) uniform texture2D contact_shadow_mask; // Mariner E11 (white outside the opaque pass).

// Mariner E12: DDGI probes (parameters with DDGI_FLAG_ACTIVE clear when the environment has none this frame).
#include "../ddgi_data_inc.glsl"
layout(set = 1, binding = 39, std140) uniform DDGIBlock {
	DDGIData volumes[DDGI_VOLUME_COUNT];
}
ddgi_block;
layout(set = 1, binding = 40) uniform texture2D ddgi_irradiance_world;
layout(set = 1, binding = 41) uniform texture2D ddgi_distance_world;
layout(set = 1, binding = 42) uniform texture2D ddgi_probe_data_world;
layout(set = 1, binding = 43) uniform texture2D ddgi_irradiance_lights_world;
layout(set = 1, binding = 44) uniform texture2D ddgi_irradiance_local;
layout(set = 1, binding = 45) uniform texture2D ddgi_distance_local;
layout(set = 1, binding = 46) uniform texture2D ddgi_probe_data_local;
layout(set = 1, binding = 47) uniform texture2D ddgi_irradiance_lights_local;
// Mariner L1: screen space probes (header.w = 0: none this frame).
layout(set = 1, binding = 58, std430) restrict readonly buffer DDGIScreenProbeSH {
	uvec4 header; // Probes across, down, pixels per probe, flags (1: probes active, 2: L2 reflections on).
	vec4 sh[]; // 3 per probe (r, g, b): L1 SH of the radiance.
}
ddgi_screen_sh;
layout(set = 1, binding = 59, std430) restrict readonly buffer DDGIScreenProbes {
	DDGIScreenProbe probes[];
}
ddgi_screen_probes;
layout(set = 1, binding = 60) uniform texture2D ddgi_screen_radiance; // Mariner L2: the probes' filtered radiance, 8x8 octahedral each.
layout(set = 1, binding = 61) uniform texture2D ddgi_reflection_buffer; // Mariner L2: traced reflections (rgb), weight (a).

// Mariner V5-T2: the visibility buffer of the software raster (render_mode visibility_resolve; see render_sw_raster.h).
layout(set = 1, binding = 62) uniform utexture2D sw_raster_depth;
layout(set = 1, binding = 63) uniform utexture2D sw_raster_payload;
layout(set = 1, binding = 64) uniform utexture2D sw_raster_attributes;
layout(set = 1, binding = 65, std430) restrict readonly buffer SwRasterBounds {
	uint data[]; // Per object id: ~min x, ~min y, max x + 1, max y + 1 of its pixels (0: none).
}
sw_raster_bounds;

// Mariner L2: a texel of a probe's 8x8 octahedral map, wrapped across the map's edges (a texel past an edge is the
// interior texel it mirrors, as the octahedral border of the DDGI tiles), then bilinear between four of them.
vec3 ddgi_screen_radiance_texel(ivec2 p_probe, ivec2 p_t) {
	const int n = DDGI_SCREEN_PROBE_TEXELS;
	ivec2 t = p_t;
	if (t.x < 0 || t.x >= n) {
		t = ivec2(clamp(t.x, 0, n - 1), n - 1 - t.y);
	}
	if (t.y < 0 || t.y >= n) {
		t = ivec2(n - 1 - t.x, clamp(t.y, 0, n - 1));
	}
	return texelFetch(sampler2D(ddgi_screen_radiance, SAMPLER_NEAREST_CLAMP), p_probe * n + clamp(t, ivec2(0), ivec2(n - 1)), 0).rgb;
}

vec3 ddgi_screen_radiance_sample(ivec2 p_probe, vec2 p_oct) {
	vec2 f = p_oct * float(DDGI_SCREEN_PROBE_TEXELS) - 0.5;
	ivec2 b = ivec2(floor(f));
	vec2 a = f - vec2(b);
	vec3 c00 = ddgi_screen_radiance_texel(p_probe, b);
	vec3 c10 = ddgi_screen_radiance_texel(p_probe, b + ivec2(1, 0));
	vec3 c01 = ddgi_screen_radiance_texel(p_probe, b + ivec2(0, 1));
	vec3 c11 = ddgi_screen_radiance_texel(p_probe, b + ivec2(1, 1));
	return mix(mix(c00, c10, a.x), mix(c01, c11, a.x), a.y);
}

// Mariner L1: irradiance / PI (rgb) of the screen probes at a pixel and how much it applies (a): the four probes
// around the pixel, bilinear, each only as far as it lies on the pixel's surface (plane distance) with a similar
// normal. a = 0 where none fits (edges, newly visible surfaces, transparent surfaces in front of the probed one).
// L2: r_specular - the probes' radiance towards p_reflect (rgb; their filtered radiance maps, towards the SH
// reconstruction for the roughest surfaces) and how much it applies (a).
vec4 ddgi_screen_probe_irradiance(vec2 p_frag, vec3 p_world, vec3 n_world, float p_depth, vec3 p_reflect, float p_roughness, out vec4 r_specular) {
	uvec4 header = ddgi_screen_sh.header;
	r_specular = vec4(0.0);
	if ((header.w & 1u) == 0u) {
		return vec4(0.0);
	}
	// Mariner L1c: the probes are for the light of near things (between stones, in hollows). Where a tile of pixels
	// covers more than about a meter of the surface (far ground; nearer at a lower 3D scale) a probe fits only the
	// pixels of its own plane - at grazing angles a line one pixel thick and one tile long - and the rest of the tile
	// reads the grids: light dashes over the far slopes. Fade the probes out by the size of a tile on the surface.
	float footprint = p_depth * float(header.z) * 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
	float near_fade = 1.0 - smoothstep(0.75, 1.5, footprint);
	if (near_fade <= 0.0) {
		return vec4(0.0);
	}
	vec2 oct = vec3_to_oct(p_reflect);
	vec3 map_sum = vec3(0.0);
	ivec2 grid = ivec2(header.xy);
	vec2 f = p_frag / float(header.z) - 0.5;
	ivec2 base = ivec2(floor(f));
	vec2 a = f - vec2(base);
	float tolerance = 0.02 * p_depth + 0.05;
	vec4 r = vec4(0.0);
	vec4 g = vec4(0.0);
	vec4 b = vec4(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 4; i++) {
		ivec2 o = ivec2(i & 1, i >> 1);
		ivec2 c = clamp(base + o, ivec2(0), grid - 1);
		uint index = uint(c.y * grid.x + c.x);
		DDGIScreenProbe sp = ddgi_screen_probes.probes[index];
		if (sp.position.w <= 0.0) {
			continue;
		}
		vec2 bw = mix(vec2(1.0) - a, a, vec2(o));
		float plane = abs(dot(sp.position.xyz - p_world, n_world));
		float wn = max(dot(sp.normal.xyz, n_world), 0.0);
		float w = bw.x * bw.y * max(0.0, 1.0 - plane / tolerance) * wn * wn;
		if (w <= 1e-5) {
			continue;
		}
		r += ddgi_screen_sh.sh[index * 3u + 0u] * w;
		g += ddgi_screen_sh.sh[index * 3u + 1u] * w;
		b += ddgi_screen_sh.sh[index * 3u + 2u] * w;
		weight_sum += w;
		if ((header.w & 2u) != 0u) {
			map_sum += ddgi_screen_radiance_sample(c, oct) * w;
		}
	}
	if (weight_sum < 1e-4) {
		return vec4(0.0);
	}
	r /= weight_sum;
	g /= weight_sum;
	b /= weight_sum;
	vec3 irradiance = vec3(ddgi_sh_irradiance(r, n_world), ddgi_sh_irradiance(g, n_world), ddgi_sh_irradiance(b, n_world));
	float amount = smoothstep(0.02, 0.25, weight_sum) * near_fade;
	float lobe = mix(1.0, 0.6, clamp((p_roughness - 0.2) / 0.6, 0.0, 1.0));
	vec3 sh_spec = vec3(ddgi_sh_radiance(r, p_reflect, lobe), ddgi_sh_radiance(g, p_reflect, lobe), ddgi_sh_radiance(b, p_reflect, lobe));
	r_specular = vec4(mix(map_sum / weight_sum, sh_spec, smoothstep(0.6, 1.0, p_roughness)), amount);
	return vec4(irradiance, amount);
}
layout(set = 1, binding = 48) uniform texture2D translucent_shadow_color; // Mariner E21: transmitted colour of the nearest translucent caster.
layout(set = 1, binding = 49) uniform texture2D translucent_shadow_distance; // Mariner E21: its distance along the light.
layout(set = 1, binding = 50) uniform texture2D restir_diffuse; // Mariner E20: demodulated diffuse light of the ReSTIR lights.
layout(set = 1, binding = 51) uniform texture2D restir_specular; // Mariner E20: specular without Fresnel (rgb), Fresnel weight (a).
layout(set = 1, binding = 52) uniform texture2D restir_shadow_mask; // Mariner E20b: ray traced shadows of directional lights (r, g).
// Mariner G1a: the overhead map (inactive: OverheadMapData.params.w = 0).
#include "../overhead_map_data_inc.glsl"
layout(set = 1, binding = 53, std140) uniform OverheadMapBlock {
	OverheadMapData data;
}
overhead_map_block;
layout(set = 1, binding = 54) uniform texture2D overhead_map_height_texture;
#define OVERHEAD_MAP overhead_map_block.data
#define OVERHEAD_HEIGHT overhead_map_height_texture
#include "../overhead_map_inc.glsl"
#define DDGI_VOLUMES ddgi_block.volumes
#define DDGI_IRRADIANCE_0 ddgi_irradiance_world
#define DDGI_DISTANCE_0 ddgi_distance_world
#define DDGI_PROBE_DATA_0 ddgi_probe_data_world
#define DDGI_IRRADIANCE_1 ddgi_irradiance_local
#define DDGI_DISTANCE_1 ddgi_distance_local
#define DDGI_PROBE_DATA_1 ddgi_probe_data_local
#define DDGI_IRRADIANCE_LIGHTS_0 ddgi_irradiance_lights_world
#define DDGI_IRRADIANCE_LIGHTS_1 ddgi_irradiance_lights_local
#define DDGI_SAMPLER SAMPLER_LINEAR_CLAMP
#include "../ddgi_sample_inc.glsl"

// Mariner GB1 / GB2: the thin G-buffer (rgb sqrt(albedo), a class / 255; read by surface decals) and the decal buffer
// (0: height, wetness, roughness offset; 1: albedo over black, coverage), read by the opaque pass. Black when unused.
#ifdef USE_MULTIVIEW
layout(set = 1, binding = 55) uniform texture2DArray gbuffer_buffer;
layout(set = 1, binding = 56) uniform texture2DArray surface_decal_buffer0;
layout(set = 1, binding = 57) uniform texture2DArray surface_decal_buffer1;
#else
layout(set = 1, binding = 55) uniform texture2D gbuffer_buffer;
layout(set = 1, binding = 56) uniform texture2D surface_decal_buffer0;
layout(set = 1, binding = 57) uniform texture2D surface_decal_buffer1;
#endif

#endif

vec4 normal_roughness_compatibility(vec4 p_normal_roughness) {
	float roughness = p_normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);
	return vec4(normalize(p_normal_roughness.xyz * 2.0 - 1.0) * 0.5 + 0.5, roughness);
}

// https://google.github.io/filament/Filament.html#toc5.3.4.7
// Note: The roughness value is inverted
vec3 prefiltered_dfg(float lod, float NoV) {
	return textureLod(sampler2D(dfg, SAMPLER_LINEAR_CLAMP), vec2(NoV, 1.0 - lod), 0.0).rgb;
}

// Compute multiscatter compensation
// https://google.github.io/filament/Filament.html#listing_energycompensationimpl
vec3 get_energy_compensation(vec3 f0, float env) {
	return 1.0 + f0 * (1.0 / env - 1.0);
}

/* Set 2 Skeleton & Instancing (can change per item) */

layout(set = 2, binding = 0, std430) restrict readonly buffer Transforms {
	vec4 data[];
}
transforms;

/* Set 3 User Material */
