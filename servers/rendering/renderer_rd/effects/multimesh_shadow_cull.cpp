/**************************************************************************/
/*  multimesh_shadow_cull.cpp                                             */
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

#include "multimesh_shadow_cull.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

MultiMeshShadowCull::MultiMeshShadowCull() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	dummy_buffer = RD::get_singleton()->storage_buffer_create(MAX(sizeof(OcclusionHeader), sizeof(SectionData)));
}

MultiMeshShadowCull::~MultiMeshShadowCull() {
	RD::get_singleton()->free_rid(dummy_buffer);
	if (section_buffer.is_valid()) {
		RD::get_singleton()->free_rid(section_buffer);
	}
	shader.version_free(shader_version);
}

void MultiMeshShadowCull::get_side_planes(const Projection &p_clip, Vector4 r_planes[4]) {
	// Rows of the matrix (Projection is column major): inside of the side planes is w +- x >= 0, w +- y >= 0.
	Vector4 rows[4];
	for (int r = 0; r < 4; r++) {
		rows[r] = Vector4(p_clip.columns[0][r], p_clip.columns[1][r], p_clip.columns[2][r], p_clip.columns[3][r]);
	}
	const Vector4 sides[4] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1] };
	for (int i = 0; i < 4; i++) {
		Vector3 n(sides[i].x, sides[i].y, sides[i].z);
		const real_t len = n.length();
		r_planes[i] = len > 0.0 ? sides[i] / len : Vector4(0.0, 0.0, 0.0, 1.0);
	}
}

void MultiMeshShadowCull::SectionData::set_view(const Transform3D &p_view_from_multimesh, const Projection &p_projection) {
	for (int r = 0; r < 3; r++) {
		view_rows[r][0] = p_view_from_multimesh.basis.rows[r].x;
		view_rows[r][1] = p_view_from_multimesh.basis.rows[r].y;
		view_rows[r][2] = p_view_from_multimesh.basis.rows[r].z;
		view_rows[r][3] = p_view_from_multimesh.origin[r];
	}
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			projection[c * 4 + r] = p_projection.columns[c][r];
		}
	}
}

void MultiMeshShadowCull::SectionData::set_world(const Transform3D &p_world_from_multimesh) {
	for (int r = 0; r < 3; r++) {
		world_rows[r][0] = p_world_from_multimesh.basis.rows[r].x;
		world_rows[r][1] = p_world_from_multimesh.basis.rows[r].y;
		world_rows[r][2] = p_world_from_multimesh.basis.rows[r].z;
		world_rows[r][3] = p_world_from_multimesh.origin[r];
	}
}

void MultiMeshShadowCull::SectionData::set_aabb(const AABB &p_aabb) {
	aabb_min[0] = p_aabb.position.x;
	aabb_min[1] = p_aabb.position.y;
	aabb_min[2] = p_aabb.position.z;
	aabb_max[0] = p_aabb.position.x + p_aabb.size.x;
	aabb_max[1] = p_aabb.position.y + p_aabb.size.y;
	aabb_max[2] = p_aabb.position.z + p_aabb.size.z;
}

void MultiMeshShadowCull::process(const MeshStorage::MultiMeshShadowCull &p_cull, const Pass *p_passes, uint32_t p_pass_count, uint32_t p_sections, const Occlusion *p_occlusion) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RD *rd = RD::get_singleton();
	const RID shader_rd = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rd.is_null());

	const uint32_t buckets = CLAMP(p_cull.buckets, 1u, MAX_LOD_BUCKETS);
	const uint32_t regions = p_sections * buckets;
	rd->buffer_clear(p_cull.counter_buffer, 0, sizeof(uint32_t) * regions);

	// The passes' SectionData (all of them if any pass has some; by section, as the shader indexes them).
	bool has_data = false;
	for (uint32_t i = 0; i < p_pass_count; i++) {
		has_data = has_data || p_passes[i].has_data;
	}
	uint32_t first_section_data = 0;
	if (has_data) {
		const uint64_t frame = rd->get_frames_drawn();
		if (frame != section_frame) {
			section_frame = frame;
			section_cursor = 0;
		}
		if (section_cursor + p_sections > section_capacity) {
			// The buffer in use this frame is freed after the frame (RenderingDevice defers it).
			if (section_buffer.is_valid()) {
				rd->free_rid(section_buffer);
			}
			section_capacity = MAX(section_capacity * 2, MAX(p_sections, 64u));
			section_buffer = rd->storage_buffer_create(section_capacity * sizeof(SectionData));
			section_cursor = 0;
		}
		LocalVector<SectionData> data;
		data.resize(p_sections);
		memset((void *)data.ptr(), 0, sizeof(SectionData) * p_sections);
		for (uint32_t i = 0; i < p_pass_count; i++) {
			if (p_passes[i].has_data && p_passes[i].section < p_sections) {
				data[p_passes[i].section] = p_passes[i].data;
			}
		}
		rd->buffer_update(section_buffer, section_cursor * sizeof(SectionData), p_sections * sizeof(SectionData), data.ptr());
		first_section_data = section_cursor;
		section_cursor += p_sections;
	}
	const bool occlusion = p_occlusion != nullptr && p_occlusion->depth_buffer.is_valid() && p_occlusion->header_buffer.is_valid();

	RD::Uniform u_source(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_cull.source_buffer);
	RD::Uniform u_dest(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_cull.data_buffer);
	RD::Uniform u_source_commands(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_cull.source_command_buffer);
	RD::Uniform u_counters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_cull.counter_buffer);
	RD::Uniform u_dest_commands(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_cull.command_buffer);
	RD::Uniform u_occlusion_depth(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, occlusion ? p_occlusion->depth_buffer : dummy_buffer);
	RD::Uniform u_sections(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, has_data ? section_buffer : dummy_buffer);
	RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_cull.state_buffer.is_valid() ? p_cull.state_buffer : dummy_buffer);
	RD::Uniform u_occlusion(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, occlusion ? p_occlusion->header_buffer : dummy_buffer);
	const RID uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_source, u_dest, u_source_commands, u_counters, u_dest_commands, u_occlusion_depth, u_sections, u_state, u_occlusion);

	PushConstant push_constant = {};
	const Vector3 center = p_cull.mesh_aabb.get_center();
	push_constant.sphere[0] = center.x;
	push_constant.sphere[1] = center.y;
	push_constant.sphere[2] = center.z;
	push_constant.sphere[3] = p_cull.mesh_aabb.size.length() * 0.5;
	push_constant.stride = p_cull.stride;
	push_constant.source_offset = p_cull.source_offset;
	push_constant.capacity = p_cull.capacity;
	push_constant.surface_count = p_cull.surface_count;
	push_constant.sections = regions;
	push_constant.section_data = has_data ? first_section_data + 1 : 0;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	push_constant.mode = 0 | (buckets << 8);
	for (uint32_t i = 0; i < p_pass_count; i++) {
		const Pass &pass = p_passes[i];
		for (int p = 0; p < 4; p++) {
			push_constant.planes[p][0] = pass.planes[p].x;
			push_constant.planes[p][1] = pass.planes[p].y;
			push_constant.planes[p][2] = pass.planes[p].z;
			push_constant.planes[p][3] = pass.planes[p].w;
		}
		push_constant.section = pass.section;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		// The instance count is on the GPU: dispatch for all and let the shader stop at the count.
		rd->compute_list_dispatch_threads(compute_list, p_cull.capacity, 1, 1);
	}
	rd->compute_list_add_barrier(compute_list);
	push_constant.mode = 1 | (buckets << 8);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, regions * p_cull.surface_count, 1, 1);
	rd->compute_list_end();
}
