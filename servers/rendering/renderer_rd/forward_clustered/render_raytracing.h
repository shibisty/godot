/**************************************************************************/
/*  render_raytracing.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// Raytracing scene for compositor effects: per-viewport TLAS of the visible scene (meshes, MultiMeshes, skinned /
// blend shape meshes) with per-instance geometry and material data. Ported from NVIDIA's Godot fork
// (NVIDIA-RTX/godot, branch nvidia-pt-dlss, RenderRaytracing) without the path tracer: no hit group compilation of
// custom shaders, lights, motion data or procedural (AABB) geometry. The result is stored in RenderSceneBuffersRD
// (RenderSceneBuffersRD::RaytracingScene) and read by compute shaders with GL_EXT_ray_query.

#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;

// Per TLAS instance geometry data. Must match the GLSL `RTInstanceData` (std430, 128 bytes) documented in
// RenderSceneBuffersRD.get_raytracing_instance_buffer().
struct alignas(16) RT_InstanceData {
	uint64_t vertex_address; // Positions (float3, or unorm16x4 when compressed), then normal/tangent block.
	uint64_t attribute_address; // Interleaved color / uv / uv2 / custom attributes.
	uint64_t index_address; // 0 when not indexed.
	uint64_t multimesh_address; // Per-instance data of a MultiMesh (transform 12 floats [+ color 4] [+ custom 4]), 0 if none.
	uint32_t vertex_count;
	uint32_t position_stride;
	uint32_t normal_byte_offset; // Offset of the normal/tangent block in the vertex buffer, RT_OFFSET_NONE if none.
	uint32_t normal_stride;
	uint32_t tangent_byte_offset;
	uint32_t tangent_stride;
	uint32_t attribute_stride;
	uint32_t uv_byte_offset; // RT_OFFSET_NONE if none.
	uint32_t uv_scale_packed; // Compressed meshes: half2 UV scale.
	uint32_t index_format; // RT_INDEX_FORMAT_*.
	uint32_t primitive_count;
	uint32_t flags; // RT_GEOM_FLAG_*.
	float aabb_size[3]; // Compressed meshes: position decode (the TLAS transform already includes it).
	uint32_t color_byte_offset; // RT_OFFSET_NONE if no vertex colors.
	float aabb_pos[3];
	uint32_t layer_mask; // VisualInstance3D layers of the instance.
	uint32_t mm_vertices_per_instance; // Merged MultiMesh: vertex_index / this = MultiMesh instance. 0: single instance.
	uint32_t mm_stride; // MultiMesh instance stride, in floats.
	uint32_t mm_color_offset; // In floats, RT_OFFSET_NONE if the MultiMesh has no colors.
	uint32_t mm_custom_offset; // In floats, RT_OFFSET_NONE if the MultiMesh has no custom data.
};
static_assert(sizeof(RT_InstanceData) == 128, "RT_InstanceData must be 128 bytes for std430.");

// Per TLAS instance material data. Must match the GLSL `RTMaterialData` (std430, 96 bytes).
struct alignas(16) RT_MaterialData {
	uint32_t albedo_texture_idx; // Index into the bindless texture array (0 = white).
	uint32_t normal_texture_idx;
	uint32_t orm_texture_idx; // ORM or roughness texture.
	uint32_t emission_texture_idx;
	float albedo_color[4]; // Linear.
	float emission_color[3]; // Linear.
	float emission_strength;
	float metallic;
	float roughness;
	float ao_strength;
	uint32_t flags; // RT_MAT_FLAG_*.
	float uv1_scale[2];
	float uv1_offset[2];
	float normal_map_depth;
	float specular;
	float alpha_scissor; // 0 if not alpha tested.
	uint32_t cull_mode; // RSE::CullMode of the shader.
};
static_assert(sizeof(RT_MaterialData) == 96, "RT_MaterialData must be 96 bytes for std430.");

enum {
	RT_OFFSET_NONE = 0xFFFFFFFFu,
	RT_CACHE_CHUNK_SIZE = 256,
	RT_CACHE_CHUNK_SHIFT = 8,
	RT_CACHE_CHUNK_MASK = 255,
	RT_MAX_TEXTURES = 16384, // Vulkan driver MAX_UNBOUNDED_DESCRIPTOR_COUNT.
};

enum {
	RT_MAT_FLAG_HAS_NORMAL_MAP = 1u,
	RT_MAT_FLAG_HAS_EMISSION_TEX = 2u,
	RT_MAT_FLAG_POINT_FILTER = 4u,
	RT_MAT_FLAG_USE_VERTEX_COLOR = 8u, // Albedo multiplied by vertex color (and MultiMesh instance color).
	RT_MAT_FLAG_CUSTOM_SHADER = 16u, // ShaderMaterial: parameters taken by name (albedo, roughness, ...), approximate.
	RT_MAT_FLAG_UNSHADED = 32u,
	// Mariner E5c: dielectric medium (path tracer water), ShaderMaterial with a float `rt_ior` > 1. The fields are
	// reused: emission_color = absorption (1/m, `rt_absorption`), emission_strength = 0, uv1_scale.xy and uv1_offset.x =
	// scattering (1/m, `rt_scattering`), specular = IOR, roughness = interface roughness, albedo = foam color (foam
	// coverage = vertex color alpha). Such instances are not forced opaque in the TLAS (ray queries see candidates).
	RT_MAT_FLAG_DIELECTRIC = 64u,
	// Mariner E5c: per-pixel detail of a dielectric surface (sea of the path tracer), see
	// RenderRaytracing::apply_dielectric_detail(). Wave slopes: up to four periodic slope maps sampled at the
	// undisplaced object space point (mesh UV = x, z) shifted by the instance origin, scaled by 1 / tile:
	// normal_texture_idx = `rt_wave_tex0` (RGBA: slope x, z of map 0, slope x, z of map 1), orm_texture_idx =
	// `rt_wave_tex1` (maps 2 and 3), scales (`rt_wave_scale`, 0 = map off) in metallic, ao_strength, normal_map_depth,
	// emission_strength. The interface normal is then (-s.x, 1, -s.y) with s = vertex color B x sum of the maps +
	// vertex color RG x 2 - 1 (extra slope, e.g. a local wave grid); the vertex normal is ignored.
	RT_MAT_FLAG_WAVE_DETAIL = 128u,
	// Foam pattern: albedo_texture_idx = `rt_foam_tex` (R: threshold map, uniform values), sampled at the undisplaced
	// world point x `rt_foam_scale` (two scales, the second sample rotated; half2 in the bits of uv1_offset.y); foam is
	// where the threshold exceeds 1 - coverage (vertex color alpha), denser towards its core.
	RT_MAT_FLAG_FOAM_PATTERN = 256u,
};

enum {
	RT_INDEX_FORMAT_UINT16 = 0,
	RT_INDEX_FORMAT_UINT32 = 1,
	RT_INDEX_FORMAT_NONE = 2,
};

enum {
	RT_GEOM_FLAG_COMPRESSED = 1u,
	RT_GEOM_FLAG_DEFORMED = 4u, // Skinned / blend shape mesh (vertices copied each frame).
	RT_GEOM_FLAG_MULTIMESH = 8u, // Merged MultiMesh (all instances in one BLAS, MultiMesh-space positions).
	RT_GEOM_FLAG_MULTIMESH_INSTANCE = 16u, // One MultiMesh instance per TLAS instance (large MultiMeshes).
};

class RenderRaytracing {
	struct SurfaceData {
		RID blas;
		RT_InstanceData geometry = {};
		Transform3D aabb_transform;
		bool is_compressed = false;
	};

	struct SurfaceCacheEntry {
		SurfaceData *ptr = nullptr;
		uint32_t cached_counter = 0;
		uint32_t cached_rid_version = 0;
	};

	struct MaterialData {
		RT_MaterialData data = {};
	};

	struct MaterialCacheEntry {
		MaterialData *ptr = nullptr;
		uint16_t cached_counter = 0;
		uint32_t cached_rid_version = 0;
	};

	// Skinned / blend shape surface: the deformed vertex buffer is owned by the mesh instance and may be reallocated,
	// so it is copied into our own buffer each frame it changes and the BLAS is refit against that copy.
	struct DeformedCacheEntry {
		SurfaceData *ptr = nullptr;
		uint64_t key = 0; // Key in deformed_handles.
		uint64_t last_used_frame = 0;
		uint64_t cached_change_stamp = 0;
		uint32_t cached_key_version = 0;
		uint32_t cached_surface_counter = 0;
		uint64_t cached_buffer_id = 0;
		bool blas_built_once = false;
		RID owned_vb;
		uint32_t owned_vb_capacity = 0;
		uint32_t cached_vertex_count = 0;
		uint32_t cached_full_size = 0;
	};

	// Per (MultiMesh, surface) merged BLAS: all instances baked into one vertex buffer (MultiMesh space) by a compute
	// pass, rebaked and refit when the MultiMesh data changes.
	struct MergedMMEntry {
		RID merged_vtx_buffer;
		uint32_t vtx_capacity_bytes = 0;
		RID merged_attr_buffer;
		uint32_t attr_capacity_bytes = 0;
		RID replicated_idx_buffer;
		uint32_t idx_capacity = 0;
		RID blas;
		uint32_t last_mm_count = 0;
		uint32_t last_surface_counter = 0;
		uint64_t last_used_frame = 0;
		uint64_t cached_mm_change = UINT64_MAX;
		bool blas_built_once = false;
	};

	struct MergeShader {
		enum Mode {
			MODE_NON_INDEXED = 0,
			MODE_INDEXED = 1,
			MODE_MAX,
		};
		MultimeshMergeShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} mm_merge_shader;

	struct MMSurfaceHandles {
		LocalVector<RID> per_surface;
		uint32_t mm_validator = 0;

		RID &surface_handle(uint32_t p_surface_index) {
			if (p_surface_index >= per_surface.size()) {
				per_surface.resize(p_surface_index + 1);
			}
			return per_surface[p_surface_index];
		}
	};

	// Bindless material textures (shared by all viewports). Index 0 is the default white texture.
	struct TextureRegistry {
		LocalVector<RID> textures;
		HashMap<RID, uint32_t> texture_to_index;
		LocalVector<uint32_t> free_indices;
		uint64_t version = 1;

		void initialize();
		void scrub_invalid();
		uint32_t add(RID p_texture);
	} texture_registry;

	bool initialized = false;

	Vector<SurfaceCacheEntry *> surface_chunks;
	Vector<MaterialCacheEntry *> material_chunks;
	LocalVector<MMSurfaceHandles> mm_handles;
	HashMap<uint64_t, RID> deformed_handles; // (mesh instance, surface) -> DeformedCacheEntry.

	RID_Owner<DeformedCacheEntry> deformed_pool;
	RID_Owner<MergedMMEntry> merged_mm_pool;

	// Per-frame scratch arrays (one entry per TLAS instance).
	LocalVector<RT_InstanceData> instance_data;
	LocalVector<RT_MaterialData> material_data;
	LocalVector<RID> blases;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks;

	static uint32_t _rid_index(RID p_rid) { return uint32_t(p_rid.get_id() & 0xFFFFFFFFULL); }
	static uint32_t _rid_version(RID p_rid) { return uint32_t(p_rid.get_id() >> 32); }
	SurfaceCacheEntry *_get_surface_cache_entry(uint32_t p_index);
	MaterialCacheEntry *_get_material_cache_entry(uint32_t p_index);

	void _free_deformed_entry(DeformedCacheEntry *p_entry);
	void _free_merged_mm_entry(MergedMMEntry *p_entry);

	void _prepare_frame();
	static void _fill_surface_geometry_data(void *p_mesh_surface, bool p_force_uncompressed, SurfaceData *r_surf_data, RID *r_vertex_buffer, RID *r_attribute_buffer, RID *r_index_buffer);
	void _populate_surface_blas(void *p_mesh_surface, RID p_vertex_buffer_override, bool p_force_uncompressed, bool p_prefer_fast_build, bool p_allow_update, SurfaceData *r_surf_data, LocalVector<RID> &r_dirty_blas_list);
	SurfaceData *_process_surface(RID p_mesh, uint32_t p_surface_index, void *p_mesh_surface, uint32_t p_surface_counter, LocalVector<RID> &r_dirty_blas_list);
	SurfaceData *_process_deformed_surface(RID p_mesh_instance, uint32_t p_surface_index, void *p_mesh_surface, RID p_current_vb, uint32_t p_surface_counter, LocalVector<RID> &r_dirty_blas_list, LocalVector<RID> &r_dirty_blas_update_list);
	bool _build_merged_mm_blas(RID p_mm_rid, RID p_mm_gpu_buffer, void *p_mesh_surface, uint32_t p_mm_count, uint32_t p_surface_index, uint32_t p_surface_counter, RD::ComputeListID &r_compute_list, LocalVector<RID> &r_dirty_blas_list, LocalVector<RID> &r_dirty_blas_update_list, SurfaceData *r_surf_data);
	const RT_MaterialData &_process_material(RID p_material_rid, uint32_t p_cull_mode, bool p_uses_alpha_scissor);

public:
	static bool is_supported();

	// Mariner E5c: per-pixel detail of a dielectric material (RT_MAT_FLAG_WAVE_DETAIL / RT_MAT_FLAG_FOAM_PATTERN):
	// packs the texture indices (0 = none) and scales into the fields a dielectric does not use. Wave detail needs
	// p_wave_tex0 and a positive scale, the foam pattern p_foam_tex and a positive scale.
	static void apply_dielectric_detail(RT_MaterialData &r_mat, uint32_t p_wave_tex0, uint32_t p_wave_tex1, const Vector4 &p_wave_scale, uint32_t p_foam_tex, const Vector2 &p_foam_scale);

	void initialize();
	void cleanup_caches();

	// Builds BLASes as needed and the TLAS of this frame for the viewport, stores it in the render buffers.
	void build_scene(const RenderDataRD *p_render_data);

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
