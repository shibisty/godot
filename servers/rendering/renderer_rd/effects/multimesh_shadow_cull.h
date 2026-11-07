/**************************************************************************/
/*  multimesh_shadow_cull.h                                               */
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

#pragma once

#include "core/math/vector4.h"
#include "servers/rendering/renderer_rd/shaders/effects/multimesh_shadow_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Mariner E14b: per shadow pass instances of indirect multimeshes (MultiMesh.indirect_shadow_culling).
class MultiMeshShadowCull {
	struct PushConstant {
		float planes[4][4];
		float sphere[4];
		uint32_t stride;
		uint32_t source_offset;
		uint32_t section;
		uint32_t capacity;
		uint32_t mode; // Mode, LOD buckets << 8.
		uint32_t surface_count;
		uint32_t sections;
		uint32_t section_data; // First SectionData + 1, 0: none.
	};
	static_assert(sizeof(PushConstant) == 112);

	MultimeshShadowCullShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID dummy_buffer; // Bindings a pass doesn't use.

public:
	static constexpr uint32_t MAX_LOD_BUCKETS = MeshStorage::MultiMeshShadowCull::MAX_LOD_BUCKETS;
	static constexpr uint32_t MAX_LOD_SURFACES = MeshStorage::MultiMeshShadowCull::MAX_LOD_SURFACES;

	// Mariner E25b / E25c: what else than the side planes culls the instances of a section (multimesh_shadow_cull.glsl).
	enum SectionFlags : uint32_t {
		SECTION_OCCLUSION = 1, // By the occlusion pyramid (Occlusion), with view_rows, projection and the mesh box.
		SECTION_RANGE = 2, // By the visibility range (fade disabled) from camera, with world_rows and the mesh box.
		SECTION_LOD = 4, // Into LOD buckets by the distance from camera (lod_limits, lod_counts).
		SECTION_LOD_ORTHOGONAL = 8, // LOD distance 1 (orthogonal camera, as the renderer).
	};
	struct SectionData {
		float view_rows[3][4];
		float projection[16];
		float aabb_min[4];
		float aabb_max[4];
		float world_rows[3][4];
		float camera[4]; // xyz: camera position (world); w: lod_distance_multiplier / lod_bias.
		float range[4]; // Begin, end, begin margin, end margin.
		float lod_limits[MAX_LOD_BUCKETS];
		uint32_t info[4]; // Buckets, SectionFlags.
		uint32_t lod_counts[MAX_LOD_BUCKETS * MAX_LOD_SURFACES];

		void set_view(const Transform3D &p_view_from_multimesh, const Projection &p_projection);
		void set_world(const Transform3D &p_world_from_multimesh);
		void set_aabb(const AABB &p_aabb);
	};
	static_assert(sizeof(SectionData) == 528);

	struct Pass {
		Vector4 planes[4]; // Side planes of the pass in the multimesh's space: inside where dot(xyz, p) + w >= 0.
		uint32_t section = 0;
		bool has_data = false; // Use data (otherwise only the side planes).
		SectionData data = {};
	};

	// Mariner E25b: the occlusion pyramid of a scene render.
	static constexpr uint32_t OCCLUSION_MAX_MIPS = 24;
	struct OcclusionHeader {
		uint32_t mips[OCCLUSION_MAX_MIPS][4];
		uint32_t info[4]; // Mip count, frame, timers, orthogonal.
		float misc[4]; // Z near.
	};
	static_assert(sizeof(OcclusionHeader) == 416);
	struct Occlusion {
		RID depth_buffer; // Floats: the pyramid.
		RID header_buffer; // OcclusionHeader.
	};

private:
	RID section_buffer; // SectionData of the passes, written from section_cursor on for each process() of a frame.
	uint32_t section_capacity = 0;
	uint32_t section_cursor = 0;
	uint64_t section_frame = UINT64_MAX;

public:
	MultiMeshShadowCull();
	~MultiMeshShadowCull();

	// Side planes (left, right, bottom, top) of p_clip (clip from the multimesh's space), normalized, normals inside.
	static void get_side_planes(const Projection &p_clip, Vector4 r_planes[4]);

	// Fills p_sections sections of p_cull (times p_cull.buckets): one per pass. p_occlusion: the pyramid for passes
	// with SECTION_OCCLUSION, nullptr: none.
	void process(const MeshStorage::MultiMeshShadowCull &p_cull, const Pass *p_passes, uint32_t p_pass_count, uint32_t p_sections, const Occlusion *p_occlusion = nullptr);
};

} // namespace RendererRD
