/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "render_raytracing.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererSceneRenderImplementation;

// ---------------------------------------------------------------------------
// Texture registry (bindless material textures)
// ---------------------------------------------------------------------------

void RenderRaytracing::TextureRegistry::initialize() {
	RID white = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	textures.clear();
	texture_to_index.clear();
	free_indices.clear();
	textures.push_back(white);
	texture_to_index[white] = 0;
	version++;
}

void RenderRaytracing::TextureRegistry::scrub_invalid() {
	RD *rd = RD::get_singleton();
	bool changed = false;
	for (uint32_t i = 1; i < textures.size(); i++) {
		if (textures[i] != textures[0] && !rd->texture_is_valid(textures[i])) {
			texture_to_index.erase(textures[i]);
			textures[i] = textures[0];
			free_indices.push_back(i);
			changed = true;
		}
	}
	if (changed) {
		version++;
	}
}

uint32_t RenderRaytracing::TextureRegistry::add(RID p_texture) {
	if (p_texture.is_null() || !RD::get_singleton()->texture_is_valid(p_texture)) {
		return 0;
	}
	HashMap<RID, uint32_t>::Iterator it = texture_to_index.find(p_texture);
	if (it != texture_to_index.end()) {
		return it->value;
	}
	uint32_t index;
	if (!free_indices.is_empty()) {
		index = free_indices[free_indices.size() - 1];
		free_indices.resize(free_indices.size() - 1);
		textures[index] = p_texture;
	} else {
		if (textures.size() >= RT_MAX_TEXTURES) {
			ERR_PRINT_ONCE("Raytracing scene: too many material textures, using white for the rest.");
			return 0;
		}
		index = textures.size();
		textures.push_back(p_texture);
	}
	texture_to_index[p_texture] = index;
	version++;
	return index;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool RenderRaytracing::is_supported() {
	return RendererRD::MeshStorage::get_singleton()->has_raytracing_buffers();
}

void RenderRaytracing::initialize() {
	if (initialized) {
		return;
	}
	initialized = true;

	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
	}

	texture_registry.initialize();
}

RenderRaytracing::~RenderRaytracing() {
	cleanup_caches();
	if (initialized) {
		mm_merge_shader.shader.version_free(mm_merge_shader.version);
	}
}

void RenderRaytracing::_free_deformed_entry(DeformedCacheEntry *p_entry) {
	RD *rd = RD::get_singleton();
	if (p_entry->ptr) {
		if (p_entry->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(p_entry->ptr->blas)) {
			rd->free_rid(p_entry->ptr->blas);
		}
		memdelete(p_entry->ptr);
		p_entry->ptr = nullptr;
	}
	if (p_entry->owned_vb.is_valid()) {
		rd->free_rid(p_entry->owned_vb);
		p_entry->owned_vb = RID();
	}
}

void RenderRaytracing::_free_merged_mm_entry(MergedMMEntry *p_entry) {
	RD *rd = RD::get_singleton();
	if (p_entry->blas.is_valid() && rd->acceleration_structure_is_valid(p_entry->blas)) {
		rd->free_rid(p_entry->blas);
	}
	p_entry->blas = RID();
	if (p_entry->merged_vtx_buffer.is_valid()) {
		rd->free_rid(p_entry->merged_vtx_buffer);
		p_entry->merged_vtx_buffer = RID();
	}
	if (p_entry->merged_attr_buffer.is_valid()) {
		rd->free_rid(p_entry->merged_attr_buffer);
		p_entry->merged_attr_buffer = RID();
	}
	if (p_entry->replicated_idx_buffer.is_valid()) {
		rd->free_rid(p_entry->replicated_idx_buffer);
		p_entry->replicated_idx_buffer = RID();
	}
}

void RenderRaytracing::cleanup_caches() {
	RD *rd = RD::get_singleton();

	for (int i = 0; i < surface_chunks.size(); i++) {
		if (!surface_chunks[i]) {
			continue;
		}
		for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
			SurfaceCacheEntry *entry = &surface_chunks[i][j];
			if (entry->ptr) {
				if (entry->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(entry->ptr->blas)) {
					rd->free_rid(entry->ptr->blas);
				}
				memdelete(entry->ptr);
				entry->ptr = nullptr;
			}
		}
		memdelete_arr(surface_chunks[i]);
	}
	surface_chunks.clear();

	for (int i = 0; i < material_chunks.size(); i++) {
		if (!material_chunks[i]) {
			continue;
		}
		for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
			if (material_chunks[i][j].ptr) {
				memdelete(material_chunks[i][j].ptr);
				material_chunks[i][j].ptr = nullptr;
			}
		}
		memdelete_arr(material_chunks[i]);
	}
	material_chunks.clear();

	{
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (const RID &rid : live) {
			DeformedCacheEntry *e = deformed_pool.get_or_null(rid);
			if (e) {
				_free_deformed_entry(e);
			}
			deformed_pool.free(rid);
		}
	}
	deformed_handles.clear();

	{
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (const RID &rid : live) {
			MergedMMEntry *e = merged_mm_pool.get_or_null(rid);
			if (e) {
				_free_merged_mm_entry(e);
			}
			merged_mm_pool.free(rid);
		}
	}
	mm_handles.clear();
}

RenderRaytracing::SurfaceCacheEntry *RenderRaytracing::_get_surface_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;
	while ((uint32_t)surface_chunks.size() <= chunk_idx) {
		surface_chunks.push_back(nullptr);
	}
	if (!surface_chunks[chunk_idx]) {
		surface_chunks.write[chunk_idx] = memnew_arr(SurfaceCacheEntry, RT_CACHE_CHUNK_SIZE);
	}
	return &surface_chunks[chunk_idx][entry_idx];
}

RenderRaytracing::MaterialCacheEntry *RenderRaytracing::_get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;
	while ((uint32_t)material_chunks.size() <= chunk_idx) {
		material_chunks.push_back(nullptr);
	}
	if (!material_chunks[chunk_idx]) {
		material_chunks.write[chunk_idx] = memnew_arr(MaterialCacheEntry, RT_CACHE_CHUNK_SIZE);
	}
	return &material_chunks[chunk_idx][entry_idx];
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::_prepare_frame() {
	instance_data.clear();
	material_data.clear();
	blases.clear();
	blas_transforms.clear();
	instance_flags.clear();
	instance_masks.clear();

	const uint64_t current_frame = RSG::rasterizer->get_frame_number();

	// Evict caches of skinned meshes / multimeshes not seen for a while (several viewports share them).
	{
		static const uint64_t DEFORMED_CACHE_TTL = (uint64_t)(int)GLOBAL_GET("rendering/raytracing/deformed_mesh_cache_ttl_frames");
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (const RID &rid : live) {
			DeformedCacheEntry *e = deformed_pool.get_or_null(rid);
			if (!e || e->last_used_frame == 0 || current_frame - e->last_used_frame <= DEFORMED_CACHE_TTL) {
				continue;
			}
			deformed_handles.erase(e->key);
			_free_deformed_entry(e);
			deformed_pool.free(rid);
		}
	}
	{
		static const uint64_t MM_BLAS_CACHE_TTL = (uint64_t)(int)GLOBAL_GET("rendering/raytracing/multimesh_blas_cache_ttl_frames");
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (const RID &rid : live) {
			MergedMMEntry *e = merged_mm_pool.get_or_null(rid);
			if (!e || e->last_used_frame == 0 || current_frame - e->last_used_frame <= MM_BLAS_CACHE_TTL) {
				continue;
			}
			_free_merged_mm_entry(e);
			merged_mm_pool.free(rid);
		}
	}

	texture_registry.scrub_invalid();
}

// ---------------------------------------------------------------------------
// Surface geometry
// ---------------------------------------------------------------------------

void RenderRaytracing::_fill_surface_geometry_data(void *p_mesh_surface, bool p_force_uncompressed, SurfaceData *r_surf_data, RID *r_vertex_buffer, RID *r_attribute_buffer, RID *r_index_buffer) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	bool compressed = (surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) && !p_force_uncompressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	r_surf_data->is_compressed = compressed;
	AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
	if (compressed) {
		r_surf_data->aabb_transform.basis = Basis::from_scale(surface_aabb.size);
		r_surf_data->aabb_transform.origin = surface_aabb.position;
	} else {
		r_surf_data->aabb_transform = Transform3D();
	}

	RT_InstanceData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));

	if (r_vertex_buffer) {
		*r_vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	}
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface);
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface);
	geom.vertex_count = vertex_count;

	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;

	// Normal / tangent block follows the positions (octahedral unorm16 pairs).
	uint32_t normal_stride = 0;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;
	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	if (compressed) {
		if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
		}
	} else if (has_normal && has_tangent) {
		normal_stride = sizeof(uint16_t) * 4;
		tangent_stride = sizeof(uint16_t) * 4;
		geom.normal_byte_offset = current_offset;
		geom.tangent_byte_offset = current_offset;
	} else if (has_normal) {
		normal_stride = sizeof(uint16_t) * 2;
		geom.normal_byte_offset = current_offset;
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = compressed ? RT_GEOM_FLAG_COMPRESSED : 0;

	if (compressed) {
		geom.aabb_size[0] = surface_aabb.size.x;
		geom.aabb_size[1] = surface_aabb.size.y;
		geom.aabb_size[2] = surface_aabb.size.z;
		geom.aabb_pos[0] = surface_aabb.position.x;
		geom.aabb_pos[1] = surface_aabb.position.y;
		geom.aabb_pos[2] = surface_aabb.position.z;
	} else {
		geom.aabb_size[0] = geom.aabb_size[1] = geom.aabb_size[2] = 1.0f;
	}

	// Attribute buffer layout: color, uv, uv2, custom0-3.
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;
	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	static const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
	static const uint32_t fmt_size[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			attrib_offset += fmt_size[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;

	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);

	if (index_buffer.is_valid() && index_count > 0) {
		geom.index_format = (vertex_count <= 65536 && vertex_count > 0) ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}

	geom.mm_color_offset = RT_OFFSET_NONE;
	geom.mm_custom_offset = RT_OFFSET_NONE;
}

void RenderRaytracing::_populate_surface_blas(void *p_mesh_surface, RID p_vertex_buffer_override, bool p_force_uncompressed, bool p_prefer_fast_build, bool p_allow_update, SurfaceData *r_surf_data, LocalVector<RID> &r_dirty_blas_list) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_force_uncompressed, r_surf_data, &vertex_buffer, &attribute_buffer, &index_buffer);
	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	RSE::PrimitiveType primitive = mesh_storage->mesh_surface_get_primitive(p_mesh_surface);
	if (primitive != RSE::PRIMITIVE_TRIANGLES || (surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES) || vertex_buffer.is_null()) {
		return; // Only 3D triangle lists go into the TLAS.
	}

	RD *rd = RD::get_singleton();
	RT_InstanceData &geom = r_surf_data->geometry;
	if (geom.primitive_count == 0) {
		return;
	}
	geom.vertex_address = rd->buffer_get_device_address(vertex_buffer);
	if (attribute_buffer.is_valid()) {
		geom.attribute_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_address = rd->buffer_get_device_address(index_buffer);
	}

	RD::AccelerationStructureGeometry as_geom;
	as_geom.vertex_buffer = vertex_buffer;
	as_geom.vertex_stride = geom.position_stride;
	as_geom.vertex_count = geom.vertex_count;
	as_geom.vertex_format = r_surf_data->is_compressed ? RD::DATA_FORMAT_R16G16B16A16_UNORM : RD::DATA_FORMAT_R32G32B32_SFLOAT;
	if (geom.index_format != RT_INDEX_FORMAT_NONE) {
		as_geom.index_buffer = index_buffer;
		as_geom.index_count = geom.primitive_count * 3;
	} else {
		as_geom.vertex_count = geom.primitive_count * 3;
	}

	BitField<RD::AccelerationStructureFlagBits> as_flags = p_prefer_fast_build ? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT : RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
	if (p_allow_update) {
		as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	}

	r_surf_data->blas = rd->blas_create({ &as_geom, 1 }, as_flags);
	if (r_surf_data->blas.is_valid()) {
		r_dirty_blas_list.push_back(r_surf_data->blas);
	}
}

RenderRaytracing::SurfaceData *RenderRaytracing::_process_surface(RID p_mesh, uint32_t p_surface_index, void *p_mesh_surface, uint32_t p_surface_counter, LocalVector<RID> &r_dirty_blas_list) {
	uint32_t cache_key = (p_mesh.get_local_index() << 8) | (p_surface_index & 0xFF);
	uint32_t mesh_version = _rid_version(p_mesh);
	SurfaceCacheEntry *entry = _get_surface_cache_entry(cache_key);

	RD *rd = RD::get_singleton();
	bool valid = entry->ptr && entry->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(entry->ptr->blas);
	if (valid && entry->cached_rid_version == mesh_version && entry->cached_counter == p_surface_counter) {
		return entry->ptr;
	}

	if (!entry->ptr) {
		entry->ptr = memnew(SurfaceData);
	} else if (valid) {
		rd->free_rid(entry->ptr->blas);
	}
	entry->ptr->blas = RID();

	_populate_surface_blas(p_mesh_surface, RID(), false, false, false, entry->ptr, r_dirty_blas_list);
	entry->cached_counter = p_surface_counter;
	entry->cached_rid_version = mesh_version;
	return entry->ptr;
}

RenderRaytracing::SurfaceData *RenderRaytracing::_process_deformed_surface(RID p_mesh_instance, uint32_t p_surface_index, void *p_mesh_surface, RID p_current_vb, uint32_t p_surface_counter, LocalVector<RID> &r_dirty_blas_list, LocalVector<RID> &r_dirty_blas_update_list) {
	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// The deformed vertex buffer is always uncompressed: float3 positions, then the normal/tangent block.
	uint64_t fmt = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	bool has_n = fmt & RSE::ARRAY_FORMAT_NORMAL;
	bool has_t = fmt & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t per_vertex_bytes = sizeof(float) * 3 + (has_n ? (has_t ? 8u : 4u) : 0u);
	uint32_t full_size = per_vertex_bytes * vertex_count;
	if (vertex_count == 0 || (fmt & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
		return nullptr;
	}

	const uint64_t current_frame = RSG::rasterizer->get_frame_number();
	const uint64_t key = (uint64_t(p_mesh_instance.get_local_index()) << 16) | (p_surface_index & 0xFFFF);
	const uint32_t key_version = _rid_version(p_mesh_instance);

	DeformedCacheEntry *entry = nullptr;
	{
		HashMap<uint64_t, RID>::Iterator it = deformed_handles.find(key);
		if (it != deformed_handles.end()) {
			entry = deformed_pool.get_or_null(it->value);
		}
		if (!entry) {
			RID handle = deformed_pool.make_rid();
			entry = deformed_pool.get_or_null(handle);
			entry->key = key;
			deformed_handles[key] = handle;
		}
	}

	const uint64_t change_stamp = mesh_storage->mesh_instance_get_last_change(p_mesh_instance, p_surface_index);
	const uint64_t buffer_id = p_current_vb.get_id();
	bool layout_changed = entry->cached_vertex_count != vertex_count || entry->cached_full_size != full_size;
	bool data_changed = layout_changed || entry->cached_change_stamp != change_stamp || entry->cached_buffer_id != buffer_id;
	bool blas_valid = entry->ptr && entry->ptr->blas.is_valid() && rd->acceleration_structure_is_valid(entry->ptr->blas);
	bool needs_full_rebuild = !blas_valid || !entry->blas_built_once || layout_changed || entry->cached_key_version != key_version || entry->cached_surface_counter != p_surface_counter;

	if (entry->owned_vb_capacity < full_size) {
		if (blas_valid) {
			rd->free_rid(entry->ptr->blas);
			entry->ptr->blas = RID();
			blas_valid = false;
		}
		if (entry->owned_vb.is_valid()) {
			rd->free_rid(entry->owned_vb);
		}
		BitField<RD::BufferCreationBits> flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
		entry->owned_vb = rd->vertex_buffer_create(full_size, Span<uint8_t>(), flags);
		ERR_FAIL_COND_V(entry->owned_vb.is_null(), nullptr);
		entry->owned_vb_capacity = full_size;
		data_changed = true;
		needs_full_rebuild = true;
	}

	// Copy at most once per frame even when several viewports see the surface.
	if (entry->last_used_frame != current_frame && data_changed) {
		rd->buffer_copy(p_current_vb, entry->owned_vb, 0, 0, full_size);
	}

	if (needs_full_rebuild) {
		if (!entry->ptr) {
			entry->ptr = memnew(SurfaceData);
		}
		if (blas_valid) {
			rd->free_rid(entry->ptr->blas);
		}
		entry->ptr->blas = RID();
		_populate_surface_blas(p_mesh_surface, entry->owned_vb, true, true, true, entry->ptr, r_dirty_blas_list);
		entry->blas_built_once = entry->ptr->blas.is_valid();
	} else if (data_changed && entry->last_used_frame != current_frame) {
		r_dirty_blas_update_list.push_back(entry->ptr->blas);
	}

	if (entry->ptr && entry->ptr->blas.is_valid()) {
		entry->ptr->geometry.flags |= RT_GEOM_FLAG_DEFORMED;
	}

	entry->cached_change_stamp = change_stamp;
	entry->cached_key_version = key_version;
	entry->cached_surface_counter = p_surface_counter;
	entry->cached_buffer_id = buffer_id;
	entry->cached_vertex_count = vertex_count;
	entry->cached_full_size = full_size;
	entry->last_used_frame = current_frame;
	return entry->ptr;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

const RT_MaterialData &RenderRaytracing::_process_material(RID p_material_rid, uint32_t p_cull_mode, bool p_uses_alpha_scissor) {
	static RT_MaterialData s_default_mat;
	static bool s_default_mat_initialized = false;
	if (!s_default_mat_initialized) {
		s_default_mat.albedo_color[0] = s_default_mat.albedo_color[1] = s_default_mat.albedo_color[2] = s_default_mat.albedo_color[3] = 1.0f;
		s_default_mat.roughness = 1.0f;
		s_default_mat.specular = 0.5f;
		s_default_mat.ao_strength = 1.0f;
		s_default_mat.uv1_scale[0] = s_default_mat.uv1_scale[1] = 1.0f;
		s_default_mat.normal_map_depth = 1.0f;
		s_default_mat.cull_mode = RSE::CULL_MODE_BACK;
		s_default_mat_initialized = true;
	}
	if (p_material_rid.is_null()) {
		return s_default_mat;
	}

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	uint16_t counter = material_storage->material_get_rt_invalidation_counter(p_material_rid);
	MaterialCacheEntry *entry = _get_material_cache_entry(_rid_index(p_material_rid));
	if (entry->ptr && entry->cached_rid_version == _rid_version(p_material_rid) && entry->cached_counter == counter) {
		return entry->ptr->data;
	}
	if (!entry->ptr) {
		entry->ptr = memnew(MaterialData);
	}
	entry->cached_rid_version = _rid_version(p_material_rid);
	entry->cached_counter = counter;

	RT_MaterialData &mat = entry->ptr->data;
	mat = s_default_mat;
	mat.metallic = 0.0f;
	mat.cull_mode = p_cull_mode;

	auto param = [&](const char *p_name) -> Variant {
		return material_storage->material_get_param(p_material_rid, StringName(p_name));
	};
	auto texture = [&](const char *p_name, bool p_srgb) -> uint32_t {
		Variant v = param(p_name);
		if (v.get_type() == Variant::OBJECT || v.get_type() == Variant::RID) {
			RID tex = v;
			if (tex.is_valid()) {
				return texture_registry.add(texture_storage->texture_get_rd_texture(tex, p_srgb));
			}
		}
		return 0;
	};
	auto first_texture = [&](std::initializer_list<const char *> p_names, bool p_srgb) -> uint32_t {
		for (const char *name : p_names) {
			uint32_t idx = texture(name, p_srgb);
			if (idx) {
				return idx;
			}
		}
		return 0;
	};
	auto first_float = [&](std::initializer_list<const char *> p_names, float &r_value) {
		for (const char *name : p_names) {
			Variant v = param(name);
			if (v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT) {
				r_value = v;
				return;
			}
		}
	};

	// BaseMaterial3D parameter names; for ShaderMaterials the same names (and a few common aliases) are used if
	// the shader declares them, which is an approximation of the material (no custom shader code runs on hits).
	String code = material_storage->material_get_shader_code(p_material_rid);
	bool is_base_material = code.begins_with("// NOTE: Shader automatically converted from") || code.contains("uniform vec4 albedo : source_color;\nuniform sampler2D texture_albedo");
	if (!is_base_material) {
		mat.flags |= RT_MAT_FLAG_CUSTOM_SHADER;
	}

	Variant albedo_var = param("albedo");
	if (albedo_var.get_type() != Variant::COLOR) {
		albedo_var = param("albedo_color");
	}
	if (albedo_var.get_type() != Variant::COLOR) {
		albedo_var = param("base_color");
	}
	if (albedo_var.get_type() == Variant::COLOR) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
	}
	mat.albedo_texture_idx = first_texture({ "texture_albedo", "albedo_texture", "albedo_tex" }, true);

	mat.normal_texture_idx = texture("texture_normal", false);
	if (mat.normal_texture_idx) {
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;
		first_float({ "normal_scale" }, mat.normal_map_depth);
	}
	mat.orm_texture_idx = first_texture({ "texture_orm", "texture_roughness" }, false);

	mat.emission_texture_idx = texture("texture_emission", true);
	Variant emission_var = param("emission");
	if (emission_var.get_type() == Variant::COLOR) {
		Color c = ((Color)emission_var).srgb_to_linear();
		mat.emission_color[0] = c.r;
		mat.emission_color[1] = c.g;
		mat.emission_color[2] = c.b;
	} else if (mat.emission_texture_idx) {
		mat.emission_color[0] = mat.emission_color[1] = mat.emission_color[2] = 1.0f;
	}
	if (mat.emission_texture_idx) {
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
	}
	// Emission is only enabled in BaseMaterial3D shaders when the feature is on.
	if (is_base_material && !code.contains("EMISSION")) {
		mat.emission_color[0] = mat.emission_color[1] = mat.emission_color[2] = 0.0f;
	} else {
		mat.emission_strength = 1.0f;
		first_float({ "emission_energy", "emission_strength" }, mat.emission_strength);
	}

	first_float({ "metallic" }, mat.metallic);
	first_float({ "roughness" }, mat.roughness);
	first_float({ "specular" }, mat.specular);
	first_float({ "ao_light_affect" }, mat.ao_strength);

	Variant uv1_scale_var = param("uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 sc = uv1_scale_var;
		mat.uv1_scale[0] = sc.x;
		mat.uv1_scale[1] = sc.y;
	} else if (param("uv_scale").get_type() == Variant::FLOAT) {
		mat.uv1_scale[0] = mat.uv1_scale[1] = (float)param("uv_scale");
	}
	Variant uv1_offset_var = param("uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
	}

	if (p_uses_alpha_scissor) {
		mat.alpha_scissor = 0.5f;
		first_float({ "alpha_scissor_threshold" }, mat.alpha_scissor);
	}

	// Vertex color as albedo: BaseMaterial3D flag (in the generated code) or a `use_vertex_color` parameter.
	Variant use_vc = param("use_vertex_color");
	if ((use_vc.get_type() == Variant::BOOL && (bool)use_vc) || code.contains("albedo_tex *= COLOR;")) {
		mat.flags |= RT_MAT_FLAG_USE_VERTEX_COLOR;
	}
	if (code.contains("unshaded")) {
		mat.flags |= RT_MAT_FLAG_UNSHADED;
	}
	// Mariner E5c: dielectric medium (water of the path tracer).
	Variant ior_var = param("rt_ior");
	if ((ior_var.get_type() == Variant::FLOAT || ior_var.get_type() == Variant::INT) && float(ior_var) > 1.0f) {
		mat.flags |= RT_MAT_FLAG_DIELECTRIC;
		mat.specular = float(ior_var);
		Vector3 absorption;
		Variant absorption_var = param("rt_absorption");
		if (absorption_var.get_type() == Variant::VECTOR3) {
			absorption = absorption_var;
		}
		Vector3 scattering;
		Variant scattering_var = param("rt_scattering");
		if (scattering_var.get_type() == Variant::VECTOR3) {
			scattering = scattering_var;
		}
		mat.emission_color[0] = absorption.x;
		mat.emission_color[1] = absorption.y;
		mat.emission_color[2] = absorption.z;
		mat.emission_strength = 0.0f;
		mat.emission_texture_idx = 0;
		mat.flags &= ~uint32_t(RT_MAT_FLAG_HAS_EMISSION_TEX);
		mat.uv1_scale[0] = scattering.x;
		mat.uv1_scale[1] = scattering.y;
		mat.uv1_offset[0] = scattering.z;
		mat.uv1_offset[1] = 0.0f;
		mat.albedo_texture_idx = 0;
		mat.alpha_scissor = 0.0f;
		// Per-pixel detail (waves, foam pattern) of the surface; without it the vertex normal and foam coverage.
		Vector4 wave_scale;
		Variant wave_scale_var = param("rt_wave_scale");
		if (wave_scale_var.get_type() == Variant::VECTOR4) {
			wave_scale = wave_scale_var;
		}
		Vector2 foam_scale;
		Variant foam_scale_var = param("rt_foam_scale");
		if (foam_scale_var.get_type() == Variant::VECTOR2) {
			foam_scale = foam_scale_var;
		}
		apply_dielectric_detail(mat, texture("rt_wave_tex0", false), texture("rt_wave_tex1", false), wave_scale, texture("rt_foam_tex", false), foam_scale);
	}

	Variant filter_var = param("texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}
	return mat;
}

void RenderRaytracing::apply_dielectric_detail(RT_MaterialData &r_mat, uint32_t p_wave_tex0, uint32_t p_wave_tex1, const Vector4 &p_wave_scale, uint32_t p_foam_tex, const Vector2 &p_foam_scale) {
	r_mat.flags &= ~uint32_t(RT_MAT_FLAG_WAVE_DETAIL | RT_MAT_FLAG_FOAM_PATTERN | RT_MAT_FLAG_HAS_NORMAL_MAP);
	r_mat.normal_texture_idx = 0;
	r_mat.orm_texture_idx = 0;
	r_mat.metallic = 0.0f;
	r_mat.ao_strength = 0.0f;
	r_mat.normal_map_depth = 0.0f;
	r_mat.emission_strength = 0.0f;
	r_mat.uv1_offset[1] = 0.0f;
	const float s0 = MAX(float(p_wave_scale.x), 0.0f);
	const float s1 = MAX(float(p_wave_scale.y), 0.0f);
	const float s2 = p_wave_tex1 ? MAX(float(p_wave_scale.z), 0.0f) : 0.0f;
	const float s3 = p_wave_tex1 ? MAX(float(p_wave_scale.w), 0.0f) : 0.0f;
	if (p_wave_tex0 && (s0 > 0.0f || s1 > 0.0f || s2 > 0.0f || s3 > 0.0f)) {
		r_mat.flags |= RT_MAT_FLAG_WAVE_DETAIL;
		r_mat.normal_texture_idx = p_wave_tex0;
		r_mat.orm_texture_idx = p_wave_tex1;
		r_mat.metallic = s0;
		r_mat.ao_strength = s1;
		r_mat.normal_map_depth = s2;
		r_mat.emission_strength = s3;
	}
	if (p_foam_tex && p_foam_scale.x > 0.0f) {
		r_mat.flags |= RT_MAT_FLAG_FOAM_PATTERN;
		r_mat.albedo_texture_idx = p_foam_tex;
		const uint32_t packed = uint32_t(Math::make_half_float(p_foam_scale.x)) | (uint32_t(Math::make_half_float(MAX(float(p_foam_scale.y), 0.0f))) << 16);
		memcpy(&r_mat.uv1_offset[1], &packed, sizeof(uint32_t));
	}
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS
// ---------------------------------------------------------------------------

bool RenderRaytracing::_build_merged_mm_blas(RID p_mm_rid, RID p_mm_gpu_buffer, void *p_mesh_surface, uint32_t p_mm_count, uint32_t p_surface_index, uint32_t p_surface_counter, RD::ComputeListID &r_compute_list, LocalVector<RID> &r_dirty_blas_list, LocalVector<RID> &r_dirty_blas_update_list, SurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RD *rd = RD::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	if ((surface_format & (RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES | RSE::ARRAY_FLAG_USE_2D_VERTICES)) || mesh_storage->mesh_surface_get_primitive(p_mesh_surface) != RSE::PRIMITIVE_TRIANGLES) {
		return false; // Merging needs float3 positions; compressed meshes use one TLAS instance per MultiMesh instance.
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface);
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);
	if (prim_count == 0 || vertex_count == 0) {
		return false;
	}
	static const uint64_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint64_t)(int)GLOBAL_GET("rendering/raytracing/multimesh_merged_blas_max_triangles");
	if ((uint64_t)p_mm_count * prim_count > MM_MERGED_BLAS_MAX_TRIANGLES) {
		return false;
	}

	const uint32_t li = p_mm_rid.get_local_index();
	const uint32_t mm_validator = _rid_version(p_mm_rid);
	if (li >= mm_handles.size()) {
		mm_handles.resize(li + 1);
	}
	MMSurfaceHandles &mm_entry = mm_handles[li];
	if (mm_entry.mm_validator != mm_validator) {
		// Local index recycled by another MultiMesh: release what the previous one left.
		for (RID &h : mm_entry.per_surface) {
			MergedMMEntry *old = merged_mm_pool.get_or_null(h);
			if (old) {
				_free_merged_mm_entry(old);
				merged_mm_pool.free(h);
			}
			h = RID();
		}
		mm_entry.per_surface.clear();
		mm_entry.mm_validator = mm_validator;
	}

	RID &handle = mm_entry.surface_handle(p_surface_index);
	MergedMMEntry *entry = merged_mm_pool.get_or_null(handle);
	if (!entry) {
		handle = merged_mm_pool.make_rid();
		entry = merged_mm_pool.get_or_null(handle);
	}

	const uint64_t current_frame = RSG::rasterizer->get_frame_number();
	const bool first_touch_this_frame = entry->last_used_frame != current_frame;
	entry->last_used_frame = current_frame;

	const uint64_t mm_change = mesh_storage->multimesh_get_rt_change_counter(p_mm_rid);
	bool blas_valid = entry->blas.is_valid() && rd->acceleration_structure_is_valid(entry->blas);
	if (entry->last_mm_count != p_mm_count || entry->last_surface_counter != p_surface_counter || !blas_valid) {
		if (blas_valid) {
			rd->free_rid(entry->blas);
		}
		entry->blas = RID();
		entry->blas_built_once = false;
		entry->last_mm_count = p_mm_count;
		entry->last_surface_counter = p_surface_counter;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t tbn_stride = has_normal ? (has_tangent ? 8 : 4) : 0;
	uint32_t src_tbn_byte_offset = vertex_count * 12;
	uint32_t merged_vtx_bytes = p_mm_count * vertex_count * (12 + tbn_stride);

	SurfaceData meta_sd;
	_fill_surface_geometry_data(p_mesh_surface, false, &meta_sd, nullptr, nullptr, nullptr);
	uint32_t attrib_stride = meta_sd.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	uint32_t merged_attr_bytes = has_attr ? (p_mm_count * vertex_count * attrib_stride) : 16;

	BitField<RD::BufferCreationBits> gpu_buf_flags = RD::BUFFER_CREATION_AS_STORAGE_BIT | RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	if (entry->merged_vtx_buffer.is_null() || entry->vtx_capacity_bytes < merged_vtx_bytes) {
		if (entry->merged_vtx_buffer.is_valid()) {
			rd->free_rid(entry->merged_vtx_buffer); // Also frees the BLAS built from it (dependency).
		}
		entry->blas = RID();
		entry->blas_built_once = false;
		entry->vtx_capacity_bytes = merged_vtx_bytes;
		entry->merged_vtx_buffer = rd->vertex_buffer_create(merged_vtx_bytes, {}, gpu_buf_flags);
		ERR_FAIL_COND_V(entry->merged_vtx_buffer.is_null(), false);
	}
	if (entry->merged_attr_buffer.is_null() || entry->attr_capacity_bytes < merged_attr_bytes) {
		if (entry->merged_attr_buffer.is_valid()) {
			rd->free_rid(entry->merged_attr_buffer);
		}
		entry->attr_capacity_bytes = merged_attr_bytes;
		entry->merged_attr_buffer = rd->storage_buffer_create(merged_attr_bytes, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(entry->merged_attr_buffer.is_null(), false);
		entry->blas_built_once = false; // Force a re-bake.
	}
	if (indexed) {
		uint32_t needed_idx = p_mm_count * index_count;
		if (entry->replicated_idx_buffer.is_null() || entry->idx_capacity < needed_idx) {
			if (entry->replicated_idx_buffer.is_valid()) {
				rd->free_rid(entry->replicated_idx_buffer);
			}
			entry->blas = RID();
			entry->blas_built_once = false;
			entry->idx_capacity = needed_idx;
			entry->replicated_idx_buffer = rd->index_buffer_create(needed_idx, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(entry->replicated_idx_buffer.is_null(), false);
		}
	}

	RID src_attr_buf = has_attr ? mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface) : entry->merged_attr_buffer;
	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(vtx_buf.is_null() || src_attr_buf.is_null(), false);

	const uint32_t mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
	const uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);

	const bool needs_rebake = first_touch_this_frame && (mm_change != entry->cached_mm_change || !entry->blas_built_once);
	if (needs_rebake) {
		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;

		Vector<RD::Uniform> uniforms;
		auto push_buf = [&](uint32_t p_binding, RID p_buf) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = p_binding;
			u.append_id(p_buf);
			uniforms.push_back(u);
		};
		push_buf(0, entry->merged_vtx_buffer);
		push_buf(1, p_mm_gpu_buffer);
		push_buf(2, entry->merged_attr_buffer);
		push_buf(3, src_attr_buf);
		if (indexed) {
			push_buf(4, entry->replicated_idx_buffer);
		}
		RID merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[mode], 0, true);
		ERR_FAIL_COND_V(merge_uniform_set.is_null(), false);

		uint64_t vtx_bda = rd->buffer_get_device_address(vtx_buf);
		uint32_t tbn_stride_words = tbn_stride / 4;
		uint32_t dst_tbn_base_words = p_mm_count * vertex_count * 3;

		if (r_compute_list == RD::ComputeListID(-1)) {
			r_compute_list = rd->compute_list_begin();
		}
		rd->compute_list_bind_compute_pipeline(r_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(r_compute_list, merge_uniform_set, 0);

		// Push constant layout of multimesh_merge.glsl (the index fields only exist in MODE_INDEXED).
		LocalVector<uint32_t> pc;
		pc.push_back(uint32_t(vtx_bda));
		pc.push_back(uint32_t(vtx_bda >> 32));
		if (indexed) {
			uint64_t src_idx_bda = rd->buffer_get_device_address(index_buffer);
			pc.push_back(uint32_t(src_idx_bda));
			pc.push_back(uint32_t(src_idx_bda >> 32));
			pc.push_back(index_count);
			pc.push_back(vertex_count <= 65536 ? 1u : 0u);
		}
		pc.push_back(vertex_count);
		pc.push_back(p_mm_count);
		pc.push_back(3); // Position stride in floats (uncompressed).
		pc.push_back(src_tbn_byte_offset / 4);
		pc.push_back(tbn_stride_words);
		pc.push_back(dst_tbn_base_words);
		pc.push_back(mm_stride);
		pc.push_back(mm_cur_offset);
		pc.push_back(has_normal ? 1u : 0u);
		pc.push_back(has_attr ? (attrib_stride / 4) : 0u);
		while (pc.size() % 4) {
			pc.push_back(0); // Push constants are padded to 16 bytes.
		}
		rd->compute_list_set_push_constant(r_compute_list, pc.ptr(), pc.size() * sizeof(uint32_t));

		uint32_t thread_count = p_mm_count * vertex_count;
		if (indexed) {
			thread_count = MAX(thread_count, p_mm_count * index_count);
		}
		rd->compute_list_dispatch_threads(r_compute_list, thread_count, 1, 1);
		rd->free_rid(merge_uniform_set);
	}

	if (entry->blas.is_null()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.vertex_buffer = entry->merged_vtx_buffer;
		as_geom.vertex_stride = 12;
		as_geom.vertex_count = p_mm_count * vertex_count;
		as_geom.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		if (indexed) {
			as_geom.index_buffer = entry->replicated_idx_buffer;
			as_geom.index_count = p_mm_count * index_count;
		} else {
			as_geom.vertex_count = p_mm_count * prim_count * 3;
		}
		entry->blas = rd->blas_create({ &as_geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT | RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		ERR_FAIL_COND_V(entry->blas.is_null(), false);
		entry->blas_built_once = false;
	}

	if (needs_rebake) {
		if (!entry->blas_built_once) {
			r_dirty_blas_list.push_back(entry->blas);
			entry->blas_built_once = true;
		} else {
			r_dirty_blas_update_list.push_back(entry->blas);
		}
		entry->cached_mm_change = mm_change;
	} else if (!entry->blas_built_once) {
		return false; // Not baked yet (should not happen on the first touch).
	}

	*r_surf_data = meta_sd;
	r_surf_data->blas = entry->blas;
	r_surf_data->is_compressed = false;
	r_surf_data->aabb_transform = Transform3D();

	RT_InstanceData &geom = r_surf_data->geometry;
	geom.vertex_address = rd->buffer_get_device_address(entry->merged_vtx_buffer);
	geom.vertex_count = p_mm_count * vertex_count;
	geom.position_stride = 12;
	geom.flags = (geom.flags & ~RT_GEOM_FLAG_COMPRESSED) | RT_GEOM_FLAG_MULTIMESH;
	if (has_normal) {
		uint32_t tbn_base = p_mm_count * vertex_count * 12;
		geom.normal_byte_offset = tbn_base;
		geom.normal_stride = tbn_stride;
		geom.tangent_byte_offset = has_tangent ? tbn_base : RT_OFFSET_NONE;
		geom.tangent_stride = has_tangent ? tbn_stride : 0;
	}
	if (has_attr) {
		geom.attribute_address = rd->buffer_get_device_address(entry->merged_attr_buffer);
	}
	if (indexed) {
		geom.index_address = rd->buffer_get_device_address(entry->replicated_idx_buffer);
		geom.index_format = RT_INDEX_FORMAT_UINT32;
	} else {
		geom.index_address = 0;
		geom.index_format = RT_INDEX_FORMAT_NONE;
	}
	geom.primitive_count = p_mm_count * prim_count;
	geom.mm_vertices_per_instance = vertex_count;
	return true;
}

// ---------------------------------------------------------------------------
// Scene (TLAS) build
// ---------------------------------------------------------------------------

static uint8_t _rt_instance_mask(uint32_t p_layer_mask) {
	// Bits 0-6: render layers 1-7; bit 7: any of layers 8-20. Ray queries select layers with their cull mask.
	return uint8_t((p_layer_mask & 0x7Fu) | ((p_layer_mask & ~0x7Fu) ? 0x80u : 0u));
}

static uint32_t _rt_instance_flags(const SceneShaderForwardClustered::ShaderData *p_shader, bool p_alpha_tested) {
	uint32_t flags = 0;
	RSE::CullMode cull = p_shader ? p_shader->cull_mode : RSE::CULL_MODE_BACK;
	switch (cull) {
		case RSE::CULL_MODE_DISABLED:
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT | RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			break;
		case RSE::CULL_MODE_FRONT:
			break;
		case RSE::CULL_MODE_BACK:
		default:
			// Godot's front faces are clockwise in raster terms: flip so ray queries report them as front facing.
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			break;
	}
	// Alpha-tested surfaces are non-opaque (ray queries see them as candidates and may alpha test), the rest opaque.
	flags |= p_alpha_tested ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
	return flags;
}

void RenderRaytracing::build_scene(const RenderDataRD *p_render_data) {
	// Private Forward+ types (RenderRaytracing is a friend of RenderForwardClustered).
	typedef RenderForwardClustered::GeometryInstanceForwardClustered RTGeometryInstance;
	typedef RenderForwardClustered::GeometryInstanceSurfaceDataCache RTSurfaceCache;

	ERR_FAIL_NULL(p_render_data);
	ERR_FAIL_NULL(p_render_data->rt_instances);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	initialize();
	_prepare_frame();

	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	RD::get_singleton()->draw_command_begin_label("Raytracing Scene");

	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;

	struct PendingMMSurface {
		RID mm_rid;
		RID mm_gpu_buffer;
		void *mesh_surface;
		const RTSurfaceCache *surf;
		uint32_t mm_count;
		uint32_t surface_counter;
		Transform3D transform;
		RT_MaterialData material;
		uint32_t inst_flags;
		uint8_t mask;
		uint32_t layer_mask;
	};
	LocalVector<PendingMMSurface> pending_mm_surfaces;

	// Material: override > instance surface material > mesh surface material.
	auto resolve_material = [mesh_storage](const RTSurfaceCache *p_surf, RID p_mesh) -> RID {
		const RTGeometryInstance *owner = p_surf->owner;
		if (owner->data->material_override.is_valid()) {
			return owner->data->material_override;
		}
		if (p_surf->surface_index < (uint32_t)owner->data->surface_materials.size() && owner->data->surface_materials[p_surf->surface_index].is_valid()) {
			return owner->data->surface_materials[p_surf->surface_index];
		}
		if (p_mesh.is_valid() && mesh_storage->owns_mesh(p_mesh)) {
			return mesh_storage->mesh_surface_get_material(p_mesh, p_surf->surface_index);
		}
		return RID();
	};

	auto push_instance = [&](RID p_blas, const Transform3D &p_transform, const RT_InstanceData &p_geom, const RT_MaterialData &p_mat, uint32_t p_flags, uint8_t p_mask) {
		blases.push_back(p_blas);
		blas_transforms.push_back(p_transform);
		instance_data.push_back(p_geom);
		material_data.push_back(p_mat);
		instance_flags.push_back(p_flags);
		instance_masks.push_back(p_mask);
	};

	// Phase 1: static and deformed meshes, collect MultiMesh surfaces.
	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RTGeometryInstance *inst = static_cast<const RTGeometryInstance *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		const uint8_t mask = _rt_instance_mask(inst->layer_mask);

		if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mm_rid = inst->data->base;
			if (mesh_storage->multimesh_get_transform_format(mm_rid) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}
			uint32_t mm_count = mesh_storage->multimesh_get_instances_to_draw(mm_rid);
			RID mm_gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(mm_rid);
			if (mm_count == 0 || mm_gpu_buffer.is_null()) {
				continue;
			}
			RID mesh = mesh_storage->multimesh_get_mesh(mm_rid);
			for (const RTSurfaceCache *surf = inst->surface_caches; surf; surf = surf->next) {
				if (surf->flags & RTSurfaceCache::FLAG_PASS_ALPHA) {
					continue; // Transparent surfaces are not in the TLAS.
				}
				bool alpha_tested = surf->shader && surf->shader->uses_alpha_clip;
				PendingMMSurface pending;
				pending.mm_rid = mm_rid;
				pending.mm_gpu_buffer = mm_gpu_buffer;
				pending.mesh_surface = surf->surface;
				pending.surf = surf;
				pending.mm_count = mm_count;
				pending.surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(surf->surface);
				pending.transform = inst->transform;
				pending.material = _process_material(resolve_material(surf, mesh), surf->shader ? surf->shader->cull_mode : RSE::CULL_MODE_BACK, alpha_tested);
				pending.inst_flags = _rt_instance_flags(surf->shader, alpha_tested || (pending.material.flags & RT_MAT_FLAG_DIELECTRIC));
				pending.mask = mask;
				pending.layer_mask = inst->layer_mask;
				pending_mm_surfaces.push_back(pending);
			}
			continue;
		}

		if (inst->data->base_type != RSE::INSTANCE_MESH) {
			continue;
		}
		RID mesh = inst->data->base;
		for (const RTSurfaceCache *surf = inst->surface_caches; surf; surf = surf->next) {
			if (surf->flags & RTSurfaceCache::FLAG_PASS_ALPHA) {
				continue;
			}
			void *mesh_surface = surf->surface;
			uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

			SurfaceData *surf_data = nullptr;
			if (inst->mesh_instance.is_valid()) {
				RID current_vb = mesh_storage->mesh_instance_get_vertex_buffer(inst->mesh_instance, surf->surface_index);
				if (current_vb.is_valid()) {
					surf_data = _process_deformed_surface(inst->mesh_instance, surf->surface_index, mesh_surface, current_vb, surface_counter, dirty_blas_list, dirty_blas_update_list);
				}
			}
			if (!surf_data) {
				surf_data = _process_surface(mesh, surf->surface_index, mesh_surface, surface_counter, dirty_blas_list);
			}
			if (!surf_data || surf_data->blas.is_null()) {
				continue;
			}

			bool alpha_tested = surf->shader && surf->shader->uses_alpha_clip;
			const RT_MaterialData &mat = _process_material(resolve_material(surf, mesh), surf->shader ? surf->shader->cull_mode : RSE::CULL_MODE_BACK, alpha_tested);
			RT_InstanceData geom = surf_data->geometry;
			geom.layer_mask = inst->layer_mask;
			Transform3D xform = surf_data->is_compressed ? inst->transform * surf_data->aabb_transform : inst->transform;
			push_instance(surf_data->blas, xform, geom, mat, _rt_instance_flags(surf->shader, alpha_tested || (mat.flags & RT_MAT_FLAG_DIELECTRIC)), mask);
		}
	}

	// Phase 2: MultiMeshes (merge compute dispatches, or one TLAS instance per MultiMesh instance).
	RD::ComputeListID compute_list = RD::ComputeListID(-1);
	for (const PendingMMSurface &pending : pending_mm_surfaces) {
		const uint32_t mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
		const uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(pending.mm_rid);
		const uint64_t mm_address = rd->buffer_get_device_address(pending.mm_gpu_buffer);

		SurfaceData merged_sd;
		if (_build_merged_mm_blas(pending.mm_rid, pending.mm_gpu_buffer, pending.mesh_surface, pending.mm_count, pending.surf->surface_index, pending.surface_counter, compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd)) {
			RT_InstanceData geom = merged_sd.geometry;
			geom.layer_mask = pending.layer_mask;
			geom.multimesh_address = mm_address + uint64_t(mm_cur_offset) * mm_stride * sizeof(float);
			geom.mm_stride = mm_stride;
			geom.mm_color_offset = mesh_storage->multimesh_get_color_offset(pending.mm_rid);
			geom.mm_custom_offset = mesh_storage->multimesh_get_custom_data_offset(pending.mm_rid);
			push_instance(merged_sd.blas, pending.transform, geom, pending.material, pending.inst_flags, pending.mask);
			continue;
		}

		// Fallback (too many triangles, compressed mesh): shared mesh BLAS, one TLAS instance per MultiMesh instance.
		if (compute_list != RD::ComputeListID(-1)) {
			// The CPU copy of the instance data may need a GPU readback, not allowed inside a compute list.
			rd->compute_list_end();
			compute_list = RD::ComputeListID(-1);
		}
		RID mesh = mesh_storage->multimesh_get_mesh(pending.mm_rid);
		SurfaceData *surf_data = _process_surface(mesh, pending.surf->surface_index, pending.mesh_surface, pending.surface_counter, dirty_blas_list);
		if (!surf_data || surf_data->blas.is_null()) {
			continue;
		}
		const float *mm_data = mesh_storage->multimesh_get_local_data(pending.mm_rid);
		if (!mm_data) {
			continue;
		}
		RT_InstanceData geom = surf_data->geometry;
		geom.layer_mask = pending.layer_mask;
		geom.flags |= RT_GEOM_FLAG_MULTIMESH_INSTANCE;
		geom.mm_vertices_per_instance = 0;
		geom.mm_stride = mm_stride;
		geom.mm_color_offset = mesh_storage->multimesh_get_color_offset(pending.mm_rid);
		geom.mm_custom_offset = mesh_storage->multimesh_get_custom_data_offset(pending.mm_rid);
		for (uint32_t mi = 0; mi < pending.mm_count; mi++) {
			const float *d = mm_data + (mm_cur_offset + mi) * mm_stride;
			Transform3D mm_xform;
			mm_xform.basis.rows[0] = Vector3(d[0], d[1], d[2]);
			mm_xform.origin.x = d[3];
			mm_xform.basis.rows[1] = Vector3(d[4], d[5], d[6]);
			mm_xform.origin.y = d[7];
			mm_xform.basis.rows[2] = Vector3(d[8], d[9], d[10]);
			mm_xform.origin.z = d[11];
			Transform3D xform = pending.transform * mm_xform;
			if (surf_data->is_compressed) {
				xform = xform * surf_data->aabb_transform;
			}
			geom.multimesh_address = mm_address + uint64_t(mm_cur_offset + mi) * mm_stride * sizeof(float);
			push_instance(surf_data->blas, xform, geom, pending.material, pending.inst_flags, pending.mask);
		}
	}
	if (compute_list != RD::ComputeListID(-1)) {
		rd->compute_list_end();
	}

	// Phase 3: BLAS builds / refits, TLAS.
	for (const RID &blas : dirty_blas_list) {
		rd->blas_build(blas);
	}
	for (const RID &blas : dirty_blas_update_list) {
		rd->blas_update(blas);
	}

	RenderSceneBuffersRD::RaytracingScene &rts = rb->raytracing_scene;
	const uint32_t count = blases.size();
	if (rts.tlas.is_null() || !rd->acceleration_structure_is_valid(rts.tlas) || count > rts.tlas_capacity) {
		if (rts.tlas.is_valid() && rd->acceleration_structure_is_valid(rts.tlas)) {
			rd->free_rid(rts.tlas);
		}
		rts.tlas_capacity = MAX(count * 2, 64u);
		rts.tlas = rd->tlas_create(rts.tlas_capacity, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		ERR_FAIL_COND(rts.tlas.is_null());
		rd->set_resource_name(rts.tlas, "Raytracing scene TLAS");
	}

	LocalVector<RD::AccelerationStructureInstance> instances;
	instances.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		RD::AccelerationStructureInstance &inst = instances[i];
		inst.id = i; // gl_InstanceCustomIndexEXT / rayQueryGetIntersectionInstanceCustomIndexEXT -> instance/material buffers.
		inst.transform = blas_transforms[i];
		inst.blas = blases[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(instance_flags[i]);
		inst.mask = instance_masks[i];
		inst.hit_sbt_range = RD::HitShaderBindingTableRange(1ULL << 32); // One hit group at offset 0 (unused by ray queries).
	}
	rd->tlas_build(rts.tlas, instances);

	// Per-instance data buffers (grow-only).
	auto update_or_grow = [rd](RID &r_buffer, uint32_t &r_capacity, const void *p_data, uint32_t p_size, uint32_t p_min_size, const char *p_name) {
		uint32_t size = MAX(p_size, p_min_size);
		if (r_buffer.is_null() || size > r_capacity) {
			if (r_buffer.is_valid()) {
				rd->free_rid(r_buffer);
			}
			r_capacity = MAX(size, r_capacity * 2);
			r_buffer = rd->storage_buffer_create(r_capacity);
			rd->set_resource_name(r_buffer, p_name);
		}
		if (p_size > 0) {
			rd->buffer_update(r_buffer, 0, p_size, p_data);
		}
	};
	update_or_grow(rts.instance_buffer, rts.instance_buffer_capacity, instance_data.ptr(), instance_data.size() * sizeof(RT_InstanceData), sizeof(RT_InstanceData), "Raytracing scene instances");
	update_or_grow(rts.material_buffer, rts.material_buffer_capacity, material_data.ptr(), material_data.size() * sizeof(RT_MaterialData), sizeof(RT_MaterialData), "Raytracing scene materials");
	rts.instance_count = count;
	rts.frame = RSG::rasterizer->get_frame_number();

	// Mariner E5c: what the path tracer accumulation depends on (it restarts when this changes).
	{
		uint32_t h = hash_murmur3_one_64(count);
		h = hash_murmur3_one_64(dirty_blas_list.size() + dirty_blas_update_list.size(), h);
		bool dielectrics = false;
		for (uint32_t i = 0; i < count; i++) {
			h = hash_murmur3_one_64(blases[i].get_id(), h);
			const Transform3D &t = blas_transforms[i];
			h = hash_murmur3_buffer(&t, sizeof(Transform3D), h);
			dielectrics = dielectrics || (material_data[i].flags & RT_MAT_FLAG_DIELECTRIC);
		}
		if (count > 0) {
			h = hash_murmur3_buffer(material_data.ptr(), count * sizeof(RT_MaterialData), h);
		}
		if (dirty_blas_list.size() + dirty_blas_update_list.size() > 0) {
			h = hash_murmur3_one_64(rts.frame, h); // Rebuilt or refit geometry: never the same scene twice.
		}
		rts.scene_hash = h;
		rts.has_dielectrics = dielectrics;
	}

	if (rts.textures_version != texture_registry.version) {
		rts.textures.resize(texture_registry.textures.size());
		for (uint32_t i = 0; i < texture_registry.textures.size(); i++) {
			rts.textures.write[i] = texture_registry.textures[i];
		}
		rts.textures_version = texture_registry.version;
	}

	RD::get_singleton()->draw_command_end_label();
}
