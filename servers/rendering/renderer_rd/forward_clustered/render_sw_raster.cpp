/**************************************************************************/
/*  render_sw_raster.cpp                                                  */
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


#include "render_sw_raster.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"

using namespace RendererSceneRenderImplementation;

void RenderSoftwareRaster::_initialize() {
	if (initialized) {
		return;
	}
	initialized = true;
	Vector<String> modes;
	modes.push_back("\n#define MODE_DEPTH\n");
	modes.push_back("\n#define MODE_PAYLOAD\n");
	modes.push_back("\n#define MODE_DEPTH\n#define MODE_SHADOW\n");
	shader.initialize(modes);
	version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		shader_rd[i] = shader.version_get_shader(version, i);
		pipeline[i] = shader_rd[i].is_valid() ? RD::get_singleton()->compute_pipeline_create(shader_rd[i]) : RID();
	}
	Vector<uint8_t> zero;
	zero.resize(32);
	memset(zero.ptrw(), 0, zero.size());
	dummy_buffer = RD::get_singleton()->storage_buffer_create(zero.size(), zero);
}

void RenderSoftwareRaster::set_item_source(uint32_t p_index, RID p_buffer, uint32_t p_offset, uint32_t p_capacity, RID p_commands, uint32_t p_command_word) {
	ERR_FAIL_UNSIGNED_INDEX(p_index, items.size());
	Item &item = items[p_index];
	item.source_buffer = p_buffer;
	item.source_offset = p_offset;
	item.source_capacity = p_capacity;
	item.command_buffer = p_commands;
	item.command_word = p_command_word;
}

uint32_t RenderSoftwareRaster::add(RID p_multimesh, const Transform3D &p_transform, uint64_t p_owner) {
	if (items.size() >= MAX_OBJECTS) {
		WARN_PRINT_ONCE("Software raster: more than 127 multimeshes in a frame; the rest are not drawn.");
		return 0;
	}
	Item item;
	item.multimesh = p_multimesh;
	item.transform = p_transform;
	item.owner = p_owner;
	items.push_back(item);
	return items.size(); // Object ids from 1 (0: an empty payload).
}

struct SwRasterPushConstant {
	float view_projection[16];
	float position_scale[4];
	float position_offset[4];
	uint32_t instance_count;
	uint32_t stride;
	uint32_t base_offset;
	uint32_t object_id;
	uint32_t width;
	uint32_t height;
	uint32_t triangle_count;
	uint32_t flags;
};
static_assert(sizeof(SwRasterPushConstant) == 128);

bool RenderSoftwareRaster::_dispatch(RD::ComputeListID p_list, int p_mode, const Item &p_item, uint32_t p_object_id, const Projection &p_view_projection, const Size2i &p_size, uint32_t p_extra_flags, RID p_depth, RID p_payload, RID p_stats, RID p_attributes) {
	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const RID mesh = mesh_storage->_multimesh_get_mesh(p_item.multimesh);
	if (mesh.is_null() || mesh_storage->mesh_get_surface_count(mesh) == 0) {
		return false;
	}
	void *surface = mesh_storage->mesh_get_surface(mesh, 0u);
	const uint64_t format = mesh_storage->mesh_surface_get_format(surface);
	const RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(surface);
	const RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(surface);
	const uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
	const bool culled = p_item.source_buffer.is_valid() && p_item.command_buffer.is_valid(); // Mariner V5-T3.
	const RID instance_buffer = culled ? p_item.source_buffer : mesh_storage->_multimesh_get_buffer_rd_rid(p_item.multimesh);
	if (vertex_buffer.is_null() || index_buffer.is_null() || index_count < 3 || instance_buffer.is_null() || (format & RSE::ARRAY_FLAG_USE_2D_VERTICES) || mesh_storage->multimesh_get_transform_format(p_item.multimesh) != RSE::MULTIMESH_TRANSFORM_3D) {
		return false;
	}
	const bool indirect = culled || mesh_storage->multimesh_is_indirect(p_item.multimesh);
	const RID commands = culled ? p_item.command_buffer : (indirect ? mesh_storage->_multimesh_get_command_buffer_rd_rid(p_item.multimesh) : RID());
	const int allocated = mesh_storage->_multimesh_get_instance_count(p_item.multimesh);
	int visible = mesh_storage->_multimesh_get_visible_instances(p_item.multimesh);
	const uint32_t max_instances = culled ? p_item.source_capacity : uint32_t(visible < 0 ? allocated : MIN(visible, allocated));
	if (max_instances == 0) {
		return false;
	}

	SwRasterPushConstant push = {};
	RendererRD::MaterialStorage::store_camera(p_view_projection * Projection(p_item.transform), push.view_projection);
	const bool compressed = format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	if (compressed) {
		const AABB aabb = mesh_storage->mesh_surface_get_aabb(surface);
		push.position_scale[0] = aabb.size.x;
		push.position_scale[1] = aabb.size.y;
		push.position_scale[2] = aabb.size.z;
		push.position_offset[0] = aabb.position.x;
		push.position_offset[1] = aabb.position.y;
		push.position_offset[2] = aabb.position.z;
	}
	push.instance_count = indirect ? 0xFFFFFFFF : max_instances;
	push.stride = mesh_storage->multimesh_get_stride(p_item.multimesh);
	push.base_offset = (culled ? p_item.source_offset : mesh_storage->multimesh_get_current_offset(p_item.multimesh)) * push.stride;
	push.object_id = p_object_id;
	push.width = p_size.width;
	push.height = p_size.height;
	push.triangle_count = index_count / 3;
	const uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
	const bool index_16 = vertex_count <= 65536 && vertex_count > 0; // As the mesh storage creates the index buffer.
	push.flags = (index_16 ? 1 : 0) | (compressed ? 2 : 0) | ((culled ? p_item.command_word : 0u) << 8) | p_extra_flags;
	// Normals and tangents follow the positions in the vertex buffer; colour and UV are in the attribute buffer.
	uint32_t offsets[RSE::ARRAY_MAX];
	uint32_t vertex_size = 0, normal_size = 0, attrib_size = 0, skin_size = 0;
	RenderingServer::get_singleton()->mesh_surface_make_offsets_from_format(format, vertex_count, index_count, offsets, vertex_size, normal_size, attrib_size, skin_size);
	const RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(surface);
	if (format & RSE::ARRAY_FORMAT_NORMAL) {
		push.flags |= 4;
		const uint32_t normal_word = offsets[RSE::ARRAY_NORMAL] / 4;
		memcpy(&push.position_scale[3], &normal_word, 4);
	}
	if (format & RSE::ARRAY_FORMAT_TANGENT) {
		push.flags |= 8;
	}
	uint32_t attrib_packed = (attrib_size / 4) << 16;
	if (attribute_buffer.is_valid() && (format & RSE::ARRAY_FORMAT_COLOR)) {
		push.flags |= 16;
		attrib_packed |= (offsets[RSE::ARRAY_COLOR] / 4) << 8;
	}
	if (attribute_buffer.is_valid() && (format & RSE::ARRAY_FORMAT_TEX_UV)) {
		push.flags |= 32;
		attrib_packed |= offsets[RSE::ARRAY_TEX_UV] / 4;
	}
	memcpy(&push.position_offset[3], &attrib_packed, 4);

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID set;
	if (p_mode == MODE_SHADOW) {
		set = cache->get_cache(shader_rd[p_mode], 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, vertex_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, index_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, instance_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, commands.is_valid() ? commands : dummy_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_depth),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, p_stats),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, attribute_buffer.is_valid() ? attribute_buffer : dummy_buffer));
	} else {
		set = cache->get_cache(shader_rd[p_mode], 0,
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, vertex_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, index_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, instance_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, commands.is_valid() ? commands : dummy_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_depth),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_payload),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, p_stats),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_attributes),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, attribute_buffer.is_valid() ? attribute_buffer : dummy_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, bounds_buffer));
	}
	if (set.is_null()) {
		return false;
	}
	// One invocation per triangle of an instance; work groups over two dimensions (65535 per dimension).
	const uint64_t threads = uint64_t(max_instances) * push.triangle_count;
	const uint64_t groups = (threads + 63) / 64;
	const uint32_t gx = uint32_t(MIN(groups, uint64_t(65535)));
	const uint32_t gy = uint32_t((groups + gx - 1) / gx);
	rd->compute_list_bind_uniform_set(p_list, set, 0);
	rd->compute_list_set_push_constant(p_list, &push, sizeof(push));
	rd->compute_list_dispatch(p_list, gx, gy, 1);
	return true;
}

void RenderSoftwareRaster::render(const RenderDataRD *p_render_data, RID p_bounds) {
	ERR_FAIL_NULL(p_render_data);
	ERR_FAIL_COND(p_bounds.is_null());
	bounds_buffer = p_bounds;
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	_initialize();
	ERR_FAIL_COND(pipeline[MODE_DEPTH].is_null() || pipeline[MODE_PAYLOAD].is_null());
	RD *rd = RD::get_singleton();

	const StringName scope = SNAME("mariner_swraster");
	const Size2i size = rb->get_internal_size();
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (!rb->has_texture(scope, SNAME("depth")) || rb->get_texture_format(scope, SNAME("depth")).width != uint32_t(size.width) || rb->get_texture_format(scope, SNAME("depth")).height != uint32_t(size.height)) {
		rb->clear_context(scope);
		rb->create_texture(scope, SNAME("depth"), RD::DATA_FORMAT_R32_UINT, usage, RD::TEXTURE_SAMPLES_1, size);
		rb->create_texture(scope, SNAME("payload"), RD::DATA_FORMAT_R32G32B32A32_UINT, usage, RD::TEXTURE_SAMPLES_1, size);
		rb->create_texture(scope, SNAME("attributes"), RD::DATA_FORMAT_R32G32B32A32_UINT, usage, RD::TEXTURE_SAMPLES_1, size);
		rb->create_texture(scope, SNAME("stats"), RD::DATA_FORMAT_R32_UINT, usage, RD::TEXTURE_SAMPLES_1, Size2i(4, 1));
	}
	RID depth = rb->get_texture(scope, SNAME("depth"));
	RID payload = rb->get_texture(scope, SNAME("payload"));
	RID stats = rb->get_texture(scope, SNAME("stats"));
	RID attributes = rb->get_texture(scope, SNAME("attributes"));
	rd->texture_clear(depth, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(payload, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(stats, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->buffer_clear(bounds_buffer, 0, BOUNDS_BUFFER_SIZE);

	const Projection camera = p_render_data->scene_data->get_cam_projection() * Projection(p_render_data->scene_data->cam_transform.affine_inverse());

	rd->draw_command_begin_label("Software raster (Mariner V5)");
	for (int mode = MODE_DEPTH; mode <= MODE_PAYLOAD; mode++) {
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipeline[mode]);
		for (uint32_t object = 0; object < items.size(); object++) {
			_dispatch(cl, mode, items[object], object + 1, camera, size, 0, depth, payload, stats, attributes);
		}
		rd->compute_list_end();
	}
	rd->draw_command_end_label();
}

void RenderSoftwareRaster::render_shadow(const LocalVector<Item> &p_items, const Projection &p_view_projection, bool p_pancake, RID p_framebuffer, const Rect2i &p_rect, RID p_stats, uint64_t p_scene_pass) {
	if (p_items.is_empty() || p_rect.size.width <= 0 || p_rect.size.height <= 0) {
		return;
	}
	_initialize();
	ERR_FAIL_COND(pipeline[MODE_SHADOW].is_null());
	RD *rd = RD::get_singleton();
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (shadow_merge_version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		shadow_merge_shader.initialize(modes);
		shadow_merge_version = shadow_merge_shader.version_create();
		RD::PipelineDepthStencilState dss;
		dss.enable_depth_test = true;
		dss.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL; // Reverse Z, as the shadow passes.
		dss.enable_depth_write = true;
		shadow_merge_pipeline.setup(shadow_merge_shader.version_get_shader(shadow_merge_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), dss, RD::PipelineColorBlendState::create_disabled(0), 0);
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.width = 4;
		tf.height = 1;
		tf.usage_bits = usage;
		shadow_stats = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(shadow_stats, Color(0, 0, 0, 0), 0, 1, 0, 1); // Its first use (later clears: by compute).
		Vector<String> clear_modes;
		clear_modes.push_back("");
		shadow_clear_shader.initialize(clear_modes);
		shadow_clear_version = shadow_clear_shader.version_create();
		shadow_clear_pipeline = rd->compute_pipeline_create(shadow_clear_shader.version_get_shader(shadow_clear_version, 0));
	}
	if (shadow_scratch.is_null() || shadow_scratch_size.width < p_rect.size.width || shadow_scratch_size.height < p_rect.size.height) {
		if (shadow_scratch.is_valid()) {
			rd->free_rid(shadow_scratch);
		}
		shadow_scratch_size = Size2i(MAX(shadow_scratch_size.width, p_rect.size.width), MAX(shadow_scratch_size.height, p_rect.size.height));
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.width = shadow_scratch_size.width;
		tf.height = shadow_scratch_size.height;
		tf.usage_bits = usage;
		shadow_scratch = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(shadow_scratch, "Mariner software raster shadow scratch");
		rd->texture_clear(shadow_scratch, Color(0, 0, 0, 0), 0, 1, 0, 1); // Its first use (later clears: by compute).
	}
	const bool clear_stats = shadow_stats_pass != p_scene_pass;
	shadow_stats_pass = p_scene_pass;
	const RID stats = p_stats.is_valid() ? p_stats : shadow_stats; // Without render buffers: counted, not kept.

	rd->draw_command_begin_label("Software raster shadow (Mariner V5a)");
	RD::ComputeListID cl = rd->compute_list_begin();
	// The pass's part of the scratch (and the stats, first in the scene pass) cleared by compute: texture_clear() more
	// than once a frame is an invalid barrier on Direct3D 12 (sw_raster_clear.glsl).
	RID clear_shader = shadow_clear_shader.version_get_shader(shadow_clear_version, 0);
	rd->compute_list_bind_compute_pipeline(cl, shadow_clear_pipeline);
	for (int k = clear_stats ? 0 : 1; k < 2; k++) {
		const RID target = k == 0 ? stats : shadow_scratch;
		const Size2i size = k == 0 ? Size2i(4, 1) : p_rect.size;
		const int32_t clear_push[4] = { size.width, size.height, 0, 0 };
		rd->compute_list_bind_uniform_set(cl, UniformSetCacheRD::get_singleton()->get_cache(clear_shader, 0, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, target)), 0);
		rd->compute_list_set_push_constant(cl, clear_push, sizeof(clear_push));
		rd->compute_list_dispatch(cl, (size.width + 7) / 8, (size.height + 7) / 8, 1);
	}
	rd->compute_list_add_barrier(cl);
	rd->compute_list_bind_compute_pipeline(cl, pipeline[MODE_SHADOW]);
	bool drawn = false;
	for (const Item &item : p_items) {
		drawn = _dispatch(cl, MODE_SHADOW, item, 0, p_view_projection, p_rect.size, p_pancake ? 64u : 0u, shadow_scratch, RID(), stats, RID()) || drawn;
	}
	rd->compute_list_end();

	if (drawn) {
		struct {
			int32_t origin[2];
			int32_t size[2];
		} push = { { p_rect.position.x, p_rect.position.y }, { p_rect.size.width, p_rect.size.height } };
		RID merge_shader = shadow_merge_shader.version_get_shader(shadow_merge_version, 0);
		const RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID set = UniformSetCacheRD::get_singleton()->get_cache(merge_shader, 0, RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, shadow_scratch), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 1, sampler));
		RD::DrawListID draw_list = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, p_rect);
		rd->draw_list_bind_render_pipeline(draw_list, shadow_merge_pipeline.get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_framebuffer)));
		rd->draw_list_bind_uniform_set(draw_list, set, 0);
		rd->draw_list_set_push_constant(draw_list, &push, sizeof(push));
		rd->draw_list_draw(draw_list, false, 1, 3);
		rd->draw_list_end();
	}
	rd->draw_command_end_label();
}

RenderSoftwareRaster::~RenderSoftwareRaster() {
	if (initialized) {
		RD::get_singleton()->free_rid(dummy_buffer);
		shader.version_free(version);
	}
	if (shadow_scratch.is_valid()) {
		RD::get_singleton()->free_rid(shadow_scratch);
	}
	if (shadow_merge_version.is_valid()) {
		shadow_merge_pipeline.clear();
		RD::get_singleton()->free_rid(shadow_stats);
		shadow_merge_shader.version_free(shadow_merge_version);
		shadow_clear_shader.version_free(shadow_clear_version); // Frees its pipeline.
	}
}
