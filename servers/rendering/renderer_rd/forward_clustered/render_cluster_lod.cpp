/**************************************************************************/
/*  render_cluster_lod.cpp                                                */
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


#include "render_cluster_lod.h"

#include "servers/rendering/renderer_rd/effects/multimesh_shadow_cull.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererSceneRenderImplementation;

struct ClusterSelectPushConstant {
	float planes[4][4];
	float camera[4];
	float misc[4];
	uint32_t cluster_count;
	uint32_t pad[3];
};
static_assert(sizeof(ClusterSelectPushConstant) == 112);

struct ClusterSelectRequest {
	ClusterSelectPushConstant push;
	RID uniform_set;
	RID commands;
};
static thread_local LocalVector<ClusterSelectRequest> cluster_select_requests;

void RenderClusterLOD::_initialize() {
	if (initialized) {
		return;
	}
	initialized = true;
	Vector<String> modes;
	modes.push_back("\n");
	modes.push_back("\n#define MODE_SHADOW_COUNT\n");
	modes.push_back("\n#define MODE_SHADOW_PREFIX\n");
	modes.push_back("\n#define MODE_SHADOW_WRITE\n");
	shader.initialize(modes);
	version = shader.version_create();
	for (int m = 0; m < MODE_MAX; m++) {
		shader_rd[m] = shader.version_get_shader(version, m);
		pipeline[m] = shader_rd[m].is_valid() ? RD::get_singleton()->compute_pipeline_create(shader_rd[m]) : RID();
	}
	dummy_buffer = RD::get_singleton()->storage_buffer_create(16);
}

uint32_t RenderClusterLOD::_stream_bindings(void *p_mesh_surface, RID r_bindings[3]) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const RID stream = mesh_storage->mesh_surface_get_cluster_stream(p_mesh_surface);
	RendererRD::ClusterStreaming *streaming = mesh_storage->get_cluster_streaming();
	if (stream.is_null() || !streaming || !streaming->is_enabled()) {
		r_bindings[0] = dummy_buffer;
		r_bindings[1] = dummy_buffer;
		r_bindings[2] = dummy_buffer;
		return 0;
	}
	r_bindings[0] = stream;
	r_bindings[1] = streaming->get_residency();
	r_bindings[2] = streaming->get_feedback();
	return 0x80000000u | streaming->get_feedback_words();
}

void RenderClusterLOD::_free_cut(Cut &r_cut) {
	RD *rd = RD::get_singleton();
	// The index arrays depend on their buffers: freed first.
	for (RID *rid : { &r_cut.camera_array, &r_cut.shadow_array, &r_cut.camera_buffer, &r_cut.shadow_buffer, &r_cut.commands }) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
}

void RenderClusterLOD::begin(const RenderDataRD *p_render_data) {
	_initialize();
	cluster_select_requests.clear();
	stats = RID();
	Ref<RenderSceneBuffersRD> rb = p_render_data ? p_render_data->render_buffers : Ref<RenderSceneBuffersRD>();
	if (rb.is_valid()) {
		const StringName scope = SNAME("mariner_cluster_lod");
		if (!rb->has_texture(scope, SNAME("stats"))) {
			const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			rb->create_texture(scope, SNAME("stats"), RD::DATA_FORMAT_R32_UINT, usage, RD::TEXTURE_SAMPLES_1, Size2i(8, 1));
		}
		stats = rb->get_texture(scope, SNAME("stats"));
	}
	if (stats.is_null()) {
		if (private_stats.is_null()) {
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R32_UINT;
			tf.width = 8;
			tf.height = 1;
			tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			private_stats = RD::get_singleton()->texture_create(tf, RD::TextureView());
		}
		stats = private_stats;
	}
	RD::get_singleton()->texture_clear(stats, Color(0, 0, 0, 0), 0, 1, 0, 1);
}

const RenderClusterLOD::Cut *RenderClusterLOD::select(uint64_t p_key, void *p_mesh_surface, const Transform3D &p_transform, const RenderDataRD *p_render_data) {
	ERR_FAIL_COND_V(pipeline[MODE_CAMERA].is_null() || stats.is_null(), nullptr);
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RID clusters;
	RID indices;
	uint32_t cluster_count = 0;
	uint32_t index_count = 0;
	if (!mesh_storage->mesh_surface_get_cluster_lod(p_mesh_surface, clusters, indices, cluster_count, index_count)) {
		return nullptr;
	}
	RD *rd = RD::get_singleton();
	Cut &cut = cuts[p_key];
	if (cut.capacity != index_count || cut.commands.is_null()) {
		_free_cut(cut);
		cut.capacity = index_count;
		cut.camera_buffer = rd->index_buffer_create(index_count, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, RD::BUFFER_CREATION_AS_STORAGE_BIT);
		cut.shadow_buffer = rd->index_buffer_create(index_count, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, RD::BUFFER_CREATION_AS_STORAGE_BIT);
		cut.camera_array = rd->index_array_create(cut.camera_buffer, 0, index_count);
		cut.shadow_array = rd->index_array_create(cut.shadow_buffer, 0, index_count);
		Vector<uint8_t> zero;
		zero.resize_initialized(10 * sizeof(uint32_t));
		cut.commands = rd->storage_buffer_create(zero.size(), zero, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	cut.frame = RSG::rasterizer->get_frame_number();

	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	ClusterSelectRequest request;
	ClusterSelectPushConstant &push = request.push;
	push = {};
	const Transform3D mesh_from_world = p_transform.affine_inverse();
	const Vector3 camera = mesh_from_world.xform(scene_data->cam_transform.origin);
	push.camera[0] = camera.x;
	push.camera[1] = camera.y;
	push.camera[2] = camera.z;
	// Pixels per unit of error / distance (the camera's vertical scale over half the viewport's height), over the
	// threshold; distances and errors in the mesh's space (their ratio is that of world space for a uniform scale).
	Size2i size(1920, 1080);
	if (p_render_data->render_buffers.is_valid()) {
		size = p_render_data->render_buffers->get_internal_size();
	}
	const float pixels = Math::abs(scene_data->cam_projection.columns[1][1]) * float(MAX(size.height, 1)) * 0.5f;
	// screen_mesh_lod_threshold is the viewport's mesh_lod_threshold (pixels) over its width.
	const float threshold = scene_data->screen_mesh_lod_threshold * float(MAX(size.width, 1));
	push.camera[3] = threshold > 0.0f ? pixels / threshold : 1e30f;
	const Basis basis = p_transform.basis;
	const float scale = MAX(basis.get_column(0).length(), MAX(basis.get_column(1).length(), basis.get_column(2).length()));
	push.misc[0] = scale > 0.0f ? 1.0f / scale : 1.0f;
	push.misc[1] = scene_data->cam_orthogonal ? 0.0f : 1.0f;
	push.cluster_count = cluster_count;
	RID stream_bindings[3];
	push.pad[2] = _stream_bindings(p_mesh_surface, stream_bindings);
	Vector4 planes[4];
	RendererRD::MultiMeshShadowCull::get_side_planes(scene_data->cam_projection * Projection(scene_data->cam_transform.affine_inverse()) * Projection(p_transform), planes);
	for (int p = 0; p < 4; p++) {
		push.planes[p][0] = planes[p].x;
		push.planes[p][1] = planes[p].y;
		push.planes[p][2] = planes[p].z;
		push.planes[p][3] = planes[p].w;
	}
	request.commands = cut.commands;
	request.uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader_rd[MODE_CAMERA], 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, clusters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, indices),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, cut.camera_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cut.shadow_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, cut.commands),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, stats),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, stream_bindings[0]),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, stream_bindings[1]),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, stream_bindings[2]));
	if (request.uniform_set.is_null()) {
		return nullptr;
	}
	cluster_select_requests.push_back(request);
	return &cut;
}

void RenderClusterLOD::end() {
	if (cluster_select_requests.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	rd->draw_command_begin_label("Cluster LOD (Mariner V2)");
	static const uint32_t initial[10] = { 0, 1, 0, 0, 0, 0, 1, 0, 0, 0 };
	for (const ClusterSelectRequest &request : cluster_select_requests) {
		rd->buffer_update(request.commands, 0, sizeof(initial), initial);
	}
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline[MODE_CAMERA]);
	for (const ClusterSelectRequest &request : cluster_select_requests) {
		rd->compute_list_bind_uniform_set(cl, request.uniform_set, 0);
		rd->compute_list_set_push_constant(cl, &request.push, sizeof(ClusterSelectPushConstant));
		rd->compute_list_dispatch_threads(cl, request.push.cluster_count, 1, 1);
	}
	rd->compute_list_end();
	rd->draw_command_end_label();
	cluster_select_requests.clear();
}

int RenderClusterLOD::shadow_add(void *p_mesh_surface, const Transform3D &p_transform, const Projection &p_projection, const Transform3D &p_light_transform, const Size2i &p_size) {
	_initialize();
	if (pipeline[MODE_SHADOW_WRITE].is_null() || p_size.x <= 0 || p_size.y <= 0) {
		return -1;
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	ShadowRequest request;
	uint32_t index_count = 0;
	if (!mesh_storage->mesh_surface_get_cluster_lod(p_mesh_surface, request.clusters, request.indices, request.cluster_count, index_count)) {
		return -1;
	}
	ClusterSelectPushConstant push = {};
	const Transform3D mesh_from_world = p_transform.affine_inverse();
	const Vector3 light = mesh_from_world.xform(p_light_transform.origin);
	push.camera[0] = light.x;
	push.camera[1] = light.y;
	push.camera[2] = light.z;
	const Basis basis = p_transform.basis;
	const float scale = MAX(basis.get_column(0).length(), MAX(basis.get_column(1).length(), basis.get_column(2).length()));
	push.misc[0] = scale > 0.0f ? 1.0f / scale : 1.0f;
	// The error below one texel of the pass's shadow map. Orthogonal: a texel's world size, the same wherever the camera
	// is (cached cascades stay valid); the error in the mesh's space times its scale. Perspective: as the camera's cut,
	// from the light.
	if (p_projection.is_orthogonal()) {
		const float texel = 2.0f / (Math::abs(p_projection.columns[1][1]) * float(p_size.y));
		push.misc[1] = 0.0f;
		push.camera[3] = scale / MAX(texel, 1e-6f);
	} else {
		push.misc[1] = 1.0f;
		push.camera[3] = Math::abs(p_projection.columns[1][1]) * float(p_size.y) * 0.5f;
	}
	Vector4 planes[4];
	RendererRD::MultiMeshShadowCull::get_side_planes(p_projection * Projection(p_light_transform.affine_inverse()) * Projection(p_transform), planes);
	for (int p = 0; p < 4; p++) {
		push.planes[p][0] = planes[p].x;
		push.planes[p][1] = planes[p].y;
		push.planes[p][2] = planes[p].z;
		push.planes[p][3] = planes[p].w;
	}
	push.cluster_count = request.cluster_count;
	push.pad[0] = shadow_requests.size(); // The request.
	push.pad[2] = _stream_bindings(p_mesh_surface, request.stream);
	static_assert(sizeof(push) == sizeof(request.push));
	memcpy(request.push, &push, sizeof(push));
	// A cut has at most the full detail's indices: the surface's own index count.
	shadow_index_total += mesh_storage->mesh_surface_get_index_count(p_mesh_surface);
	shadow_requests.push_back(request);
	return int(shadow_requests.size() - 1);
}

void RenderClusterLOD::shadow_process() {
	if (shadow_requests.is_empty()) {
		return;
	}
	RD *rd = RD::get_singleton();
	const uint32_t count = shadow_requests.size();
	// The shared buffer: room for the full detail of every request, at most 32M indices (128 MB; cuts are far smaller
	// than full detail, a request that does not fit draws nothing and is counted).
	const uint32_t needed = uint32_t(MIN(shadow_index_total, uint64_t(32u << 20)));
	if (needed > shared_capacity) {
		if (shared_array.is_valid()) {
			rd->free_rid(shared_array);
			rd->free_rid(shared_buffer);
		}
		shared_capacity = MAX(needed, MAX(shared_capacity * 2u, 1u << 16));
		shared_capacity = MIN(shared_capacity, 32u << 20);
		shared_buffer = rd->index_buffer_create(shared_capacity, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, RD::BUFFER_CREATION_AS_STORAGE_BIT);
		shared_array = rd->index_array_create(shared_buffer, 0, shared_capacity);
	}
	if (count > request_capacity) {
		if (request_buffer.is_valid()) {
			rd->free_rid(request_buffer);
			rd->free_rid(shadow_commands);
		}
		request_capacity = MAX(count, request_capacity * 2u);
		request_buffer = rd->storage_buffer_create(request_capacity * 3 * sizeof(uint32_t));
		shadow_commands = rd->storage_buffer_create(request_capacity * 5 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (stats.is_null()) {
		begin(nullptr);
	}
	rd->draw_command_begin_label("Cluster LOD shadows (Mariner V3)");
	rd->buffer_clear(request_buffer, 0, count * 3 * sizeof(uint32_t));
	RD::ComputeListID cl = rd->compute_list_begin();
	for (int mode : { MODE_SHADOW_COUNT, MODE_SHADOW_WRITE }) {
		if (mode == MODE_SHADOW_WRITE) {
			// Each request's range and command, after all are counted.
			rd->compute_list_add_barrier(cl);
			rd->compute_list_bind_compute_pipeline(cl, pipeline[MODE_SHADOW_PREFIX]);
			rd->compute_list_bind_uniform_set(cl, UniformSetCacheRD::get_singleton()->get_cache(shader_rd[MODE_SHADOW_PREFIX], 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, request_buffer), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, shadow_commands), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, stats)), 0);
			ClusterSelectPushConstant push = {};
			push.pad[0] = count;
			push.pad[1] = shared_capacity;
			rd->compute_list_set_push_constant(cl, &push, sizeof(push));
			rd->compute_list_dispatch(cl, 1, 1, 1);
			rd->compute_list_add_barrier(cl);
		}
		rd->compute_list_bind_compute_pipeline(cl, pipeline[mode]);
		for (const ShadowRequest &request : shadow_requests) {
			RID set;
			if (mode == MODE_SHADOW_COUNT) {
				set = UniformSetCacheRD::get_singleton()->get_cache(shader_rd[mode], 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, request.clusters), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, request.indices), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, request_buffer), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, shadow_commands), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, stats), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, request.stream[0]), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, request.stream[1]), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, request.stream[2]));
			} else {
				set = UniformSetCacheRD::get_singleton()->get_cache(shader_rd[mode], 0, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, request.clusters), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, request.indices), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, shared_buffer), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, request_buffer), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, shadow_commands), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, stats), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, request.stream[0]), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, request.stream[1]), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, request.stream[2]));
			}
			ClusterSelectPushConstant push;
			memcpy(&push, request.push, sizeof(push));
			push.pad[1] = shared_capacity;
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_set_push_constant(cl, &push, sizeof(push));
			rd->compute_list_dispatch_threads(cl, request.cluster_count, 1, 1);
		}
	}
	rd->compute_list_end();
	rd->draw_command_end_label();
	shadow_requests.clear();
	shadow_index_total = 0;
}

void RenderClusterLOD::purge(uint64_t p_frame) {
	LocalVector<uint64_t> gone;
	for (KeyValue<uint64_t, Cut> &E : cuts) {
		if (E.value.frame + 30 < p_frame) {
			_free_cut(E.value);
			gone.push_back(E.key);
		}
	}
	for (uint64_t key : gone) {
		cuts.erase(key);
	}
}

RenderClusterLOD::~RenderClusterLOD() {
	for (KeyValue<uint64_t, Cut> &E : cuts) {
		_free_cut(E.value);
	}
	if (private_stats.is_valid()) {
		RD::get_singleton()->free_rid(private_stats);
	}
	for (RID rid : { shared_array, shared_buffer, request_buffer, shadow_commands, dummy_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	if (initialized) {
		shader.version_free(version);
	}
}
