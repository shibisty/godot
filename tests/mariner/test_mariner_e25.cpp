/**************************************************************************/
/*  test_mariner_e25.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mariner_e25)

#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/effects/multimesh_shadow_cull.h"
#include "servers/rendering/renderer_scene_occlusion_cull.h"

// Mariner engine patch E25a: GPU static batches. Geometry instances with GeometryInstance3D.gpu_static go into
// indirect multimeshes of the scenario that the renderer culls on the GPU per camera (and per shadow pass, E14b).
// Here: the API, and that the four side planes of a perspective camera (what the camera culling tests) keep what is
// in front and inside, and drop what is outside or behind. The batches themselves (eligibility, edits, picking, the
// same frame as the stock way, the instances the camera draws) on lavapipe by engine/tests/e25/gpu_static.gd.

namespace TestMarinerE25 {

TEST_CASE("[Mariner][E25a] GPU static batches: API") {
	CHECK(ClassDB::has_method("GeometryInstance3D", "set_gpu_static"));
	CHECK(ClassDB::has_method("GeometryInstance3D", "is_gpu_static_active"));
	CHECK(ClassDB::has_method("RenderingServer", "instance_geometry_set_gpu_static"));
	CHECK(ClassDB::has_method("RenderingServer", "instance_geometry_is_gpu_static_active"));
	CHECK(ClassDB::has_method("RenderingServer", "scenario_get_gpu_static_info"));
	CHECK(ClassDB::has_method("RenderingServer", "multimesh_set_indirect_view_culling"));
	CHECK(ClassDB::has_method("RenderingServer", "multimesh_get_view_command_buffer_rd_rid"));
	List<PropertyInfo> props;
	ClassDB::get_property_list("GeometryInstance3D", &props, true);
	bool found = false;
	for (const PropertyInfo &p : props) {
		found = found || p.name == "gpu_static";
	}
	CHECK(found);
}

static bool inside(const Vector4 p_planes[4], const Vector3 &p_center, real_t p_radius) {
	for (int i = 0; i < 4; i++) {
		if (Vector3(p_planes[i].x, p_planes[i].y, p_planes[i].z).dot(p_center) + p_planes[i].w < -p_radius) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[Mariner][E25a] Camera culling: side planes of a perspective view") {
	Projection proj;
	proj.set_perspective(60.0, 4.0 / 3.0, 0.05, 500.0);
	Transform3D cam;
	cam.origin = Vector3(10, 5, 10);
	cam = cam.looking_at(Vector3(10, 5, 0), Vector3(0, 1, 0)); // Looks along -Z.
	Vector4 planes[4];
	// An instance's transform folds into the planes (multimesh space): here identity.
	RendererRD::MultiMeshShadowCull::get_side_planes(proj * Projection(cam.affine_inverse()), planes);
	CHECK(inside(planes, Vector3(10, 5, 0), 0.5)); // In front, center.
	CHECK(inside(planes, Vector3(10, 5, -300), 0.5)); // Far in front (no far plane: drawn).
	CHECK_FALSE(inside(planes, Vector3(10, 5, 20), 0.5)); // Behind the camera.
	CHECK_FALSE(inside(planes, Vector3(40, 5, 5), 0.5)); // Far to the side.
	CHECK(inside(planes, Vector3(13.5, 5, 5), 0.5)); // Inside, near the right edge (x 13.85 at this depth).
	CHECK(inside(planes, Vector3(14.2, 5, 5), 0.5)); // Just outside the right edge, but within its radius.
	CHECK_FALSE(inside(planes, Vector3(15.0, 5, 5), 0.5)); // Outside by more than its radius.
	CHECK_FALSE(inside(planes, Vector3(10, 5, 11.5), 0.5)); // Behind the camera by more than its radius.

	// The multimesh's own transform: the same planes in its space.
	Transform3D mm;
	mm.origin = Vector3(0, 0, -100);
	RendererRD::MultiMeshShadowCull::get_side_planes(proj * Projection(cam.affine_inverse()) * Projection(mm), planes);
	CHECK(inside(planes, Vector3(10, 5, 100), 0.5)); // = world (10, 5, 0).
	CHECK_FALSE(inside(planes, Vector3(10, 5, 120), 0.5)); // = world (10, 5, 20), behind.
}

TEST_CASE("[Mariner][E25a] GeometryInstance3D.gpu_static") {
	MeshInstance3D *mi = memnew(MeshInstance3D);
	CHECK_FALSE(mi->is_gpu_static());
	mi->set_gpu_static(true);
	CHECK(mi->is_gpu_static());
	CHECK(bool(mi->get("gpu_static")));
	CHECK_FALSE(mi->is_gpu_static_active()); // Not in a scenario, no mesh.
	memdelete(mi);
}

// Mariner E25b: the camera sections of GPU static batches are also culled by the occlusion depth pyramid the CPU
// culls the other instances with (RendererSceneOcclusionCull::HZBuffer), uploaded as is. Here: the layout the renderer
// uploads (mips back to back, offsets as the shader reads them) and the size limits. The culling itself (as many
// hidden as on the CPU, the same frame, above the wall all drawn, ignore_occlusion_culling, the depth of earlier
// frames) on lavapipe by engine/tests/e25/gpu_occlusion.gd.

class TestHZBuffer : public RendererSceneOcclusionCull::HZBuffer {
public:
	void fill(float p_value) {
		for (float &f : data) {
			f = p_value;
		}
	}
};

TEST_CASE("[Mariner][E25b] Occlusion pyramid layout for the GPU") {
	TestHZBuffer hz;
	CHECK(hz.is_empty());
	hz.resize(Size2i(320, 180));
	REQUIRE_FALSE(hz.is_empty());
	const LocalVector<Size2i> &sizes = hz.get_mip_sizes();
	CHECK(sizes[0] == Size2i(320, 180));
	CHECK(sizes[sizes.size() - 1] == Size2i(1, 1));
	CHECK(sizes.size() <= RendererRD::MultiMeshShadowCull::OCCLUSION_MAX_MIPS);
	uint32_t offset = 0;
	for (uint32_t i = 0; i < sizes.size(); i++) {
		CHECK(hz.get_mip_offset(i) == offset); // Back to back from the start.
		offset += sizes[i].x * sizes[i].y;
	}
	CHECK(offset == hz.get_data().size());
	hz.fill(7.0f);
	CHECK(hz.get_data()[hz.get_mip_offset(sizes.size() - 1)] == 7.0f);

	// The largest viewport occlusion buffer still fits the shader's mip table.
	TestHZBuffer big;
	big.resize(Size2i(8192, 8192));
	CHECK(big.get_mip_sizes().size() <= RendererRD::MultiMeshShadowCull::OCCLUSION_MAX_MIPS);
}

TEST_CASE("[Mariner][E25b] ignore_occlusion_culling reaches the renderer's geometry instance") {
	class TestInstance : public RenderGeometryInstanceBase {
	public:
		virtual void _mark_dirty() override {}
		virtual void set_use_lightmap(RID, const Rect2 &, int) override {}
		virtual void set_lightmap_capture(const Color *) override {}
		virtual void clear_light_instances() override {}
		virtual void pair_light_instance(const RID, RSE::LightType, uint32_t) override {}
		virtual void pair_reflection_probe_instances(const RID *, uint32_t) override {}
		virtual void pair_decal_instances(const RID *, uint32_t) override {}
		virtual void pair_voxel_gi_instances(const RID *, uint32_t) override {}
		virtual void set_softshadow_projector_pairing(bool, bool) override {}
	};
	TestInstance inst;
	CHECK_FALSE(inst.ignore_occlusion_culling);
	RenderGeometryInstance *base = &inst;
	base->set_ignore_occlusion_culling(true);
	CHECK(inst.ignore_occlusion_culling);

	// Mariner E25c: the visibility range of a GPU static batch's members.
	CHECK_FALSE(inst.has_gpu_visibility_range());
	base->set_gpu_visibility_range(0.0, 30.0, 0.0, 2.0);
	CHECK(inst.has_gpu_visibility_range());
	CHECK(inst.gpu_visibility_range[1] == 30.0f);
	CHECK(inst.gpu_visibility_range[3] == 2.0f);
	base->set_gpu_visibility_range(0.0, 0.0, 0.0, 0.0);
	CHECK_FALSE(inst.has_gpu_visibility_range());
}

// Mariner E25c: mesh LOD and visibility range per instance in the GPU culling (camera and shadow sections), and the
// fix of LOD for indirect multimeshes (their commands hold the full mesh's index count: no LOD index buffer for them).
// Here: the section data the shader reads (layout, transforms as rows, the box). The culling itself (the same frame as
// the stock way with LOD and with a visibility range, shadows too; two LOD buckets drawn; an indirect multimesh far
// away drawn whole) on lavapipe by engine/tests/e25/gpu_lod.gd.
TEST_CASE("[Mariner][E25c] Section data for LOD and visibility range") {
	using Cull = RendererRD::MultiMeshShadowCull;
	CHECK(sizeof(Cull::SectionData) == 528); // std430 struct of 33 vec4.
	CHECK(Cull::MAX_LOD_BUCKETS == 8);
	CHECK(Cull::MAX_LOD_SURFACES == 8);
	Cull::SectionData data = {};
	Transform3D t(Basis(Vector3(0, 1, 0), Math::PI / 2.0) * Basis::from_scale(Vector3(2, 2, 2)), Vector3(10, 20, 30));
	data.set_world(t);
	// Rows: the shader's dot(row, (p, 1)) is t.xform(p).
	const Vector3 p(1, 2, 3);
	const Vector3 expected = t.xform(p);
	for (int r = 0; r < 3; r++) {
		const float v = data.world_rows[r][0] * p.x + data.world_rows[r][1] * p.y + data.world_rows[r][2] * p.z + data.world_rows[r][3];
		CHECK(v == doctest::Approx(expected[r]));
	}
	data.set_aabb(AABB(Vector3(-1, -2, -3), Vector3(2, 4, 6)));
	CHECK(data.aabb_min[1] == -2.0f);
	CHECK(data.aabb_max[2] == 3.0f);
	Projection proj;
	proj.set_perspective(60.0, 1.0, 0.1, 100.0);
	data.set_view(Transform3D(), proj);
	CHECK(data.projection[0] == doctest::Approx(proj.columns[0][0])); // Column major, as mat4.
	CHECK(data.projection[11] == doctest::Approx(proj.columns[2][3]));
}

} // namespace TestMarinerE25
