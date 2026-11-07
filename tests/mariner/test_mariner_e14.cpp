/**************************************************************************/
/*  test_mariner_e14.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mariner_e14)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "servers/rendering/dummy/storage/material_storage.h"
#include "servers/rendering/instance_uniforms.h"
#include "servers/rendering/renderer_rd/effects/multimesh_shadow_cull.h"
#include "servers/rendering/rendering_server_globals.h"

// Mariner engine patch E14a: instance uniforms take only the slots they use, a failed allocation is retried.
// E14b: indirect multimeshes cull their instances per shadow pass; here the API and the pass planes, the culling and
// the shadows themselves on lavapipe by engine/tests/e14/shadow_cull.gd.
// Here: InstanceUniforms against a recording material storage (the slot counts asked for, reallocation, failure). The
// allocator of the Forward+ renderer (12 000 instances, colors, reallocation, freeing and adding again) is checked
// on lavapipe by engine/tests/e14/instance_uniforms.gd.

namespace TestMarinerE14 {

class RecordingMaterialStorage : public RendererDummy::MaterialStorage {
public:
	HashMap<RID, List<InstanceShaderParam>> params; // Per material.
	LocalVector<uint32_t> allocations; // p_elements of each call.
	uint32_t frees = 0;
	bool fail = false;
	int32_t next = 100;

	RecordingMaterialStorage() :
			RendererDummy::MaterialStorage(false) {}

	virtual void material_get_instance_shader_parameters(RID p_material, List<InstanceShaderParam> *r_parameters) override {
		if (const List<InstanceShaderParam> *list = params.getptr(p_material)) {
			for (const InstanceShaderParam &p : *list) {
				r_parameters->push_back(p);
			}
		}
	}
	virtual int32_t global_shader_parameters_instance_allocate(RID p_instance, uint32_t p_elements) override {
		allocations.push_back(p_elements);
		if (fail) {
			return -1;
		}
		next += 16;
		return next;
	}
	virtual void global_shader_parameters_instance_free(RID p_instance) override { frees++; }
};

static void add_material(RecordingMaterialStorage &p_storage, RID p_material, const Vector<int> &p_indices) {
	List<RendererMaterialStorage::InstanceShaderParam> list;
	for (int index : p_indices) {
		RendererMaterialStorage::InstanceShaderParam p;
		p.info = PropertyInfo(Variant::FLOAT, vformat("p%d", index));
		p.index = index;
		p.default_value = 0.0;
		list.push_back(p);
	}
	p_storage.params[p_material] = list;
}

TEST_CASE("[Mariner][E14a] Instance uniforms allocate the slots they use") {
	RecordingMaterialStorage storage;
	RendererMaterialStorage *saved = RSG::material_storage;
	RSG::material_storage = &storage;

	const RID self = RID::from_uint64(1);
	const RID two = RID::from_uint64(10);
	const RID sparse = RID::from_uint64(11);
	const RID four = RID::from_uint64(12);
	add_material(storage, two, { 0, 1 });
	add_material(storage, sparse, { 0, 5 });
	add_material(storage, four, { 0, 1, 2, 3 });

	{
		InstanceUniforms iu;
		iu.materials_start();
		iu.materials_append(two);
		CHECK(iu.materials_finish(self)); // Allocated: the offset must be set.
		REQUIRE(storage.allocations.size() == 1);
		CHECK(storage.allocations[0] == 2); // Was 16.
		const int32_t first = iu.location();
		CHECK(first >= 0);

		// Same materials again: nothing to do.
		iu.materials_start();
		iu.materials_append(two);
		CHECK_FALSE(iu.materials_finish(self));
		CHECK(storage.allocations.size() == 1);

		// More slots needed: freed and allocated again, the new location reported.
		iu.materials_start();
		iu.materials_append(four);
		CHECK(iu.materials_finish(self));
		CHECK(storage.frees == 1);
		REQUIRE(storage.allocations.size() == 2);
		CHECK(storage.allocations[1] == 4);
		CHECK(iu.location() != first);

		// Fewer slots: the block is kept.
		iu.materials_start();
		iu.materials_append(two);
		CHECK_FALSE(iu.materials_finish(self));
		CHECK(storage.allocations.size() == 2);

		// No instance uniforms any more: freed.
		iu.materials_start();
		CHECK(iu.materials_finish(self));
		CHECK_FALSE(iu.is_allocated());
		CHECK(storage.frees == 2);
	}
	{
		// Indices with a gap: up to the highest.
		InstanceUniforms iu;
		iu.materials_start();
		iu.materials_append(sparse);
		CHECK(iu.materials_finish(self));
		CHECK(storage.allocations[storage.allocations.size() - 1] == 6);
		iu.free(self);
	}
	{
		// A failed allocation leaves the instance unallocated and is retried on the next update.
		storage.fail = true;
		InstanceUniforms iu;
		iu.materials_start();
		iu.materials_append(two);
		iu.materials_finish(self);
		CHECK_FALSE(iu.is_allocated());
		const uint32_t calls = storage.allocations.size();
		storage.fail = false;
		iu.materials_start();
		iu.materials_append(two);
		CHECK(iu.materials_finish(self));
		CHECK(iu.is_allocated());
		CHECK(storage.allocations.size() == calls + 1);
		iu.free(self);
	}

	RSG::material_storage = saved;
}

static float side(const Vector4 &p_plane, const Vector3 &p_point) {
	return p_plane.x * p_point.x + p_plane.y * p_point.y + p_plane.z * p_point.z + p_plane.w;
}

static bool inside(const Vector4 p_planes[4], const Vector3 &p_point, float p_radius = 0.0) {
	for (int i = 0; i < 4; i++) {
		if (side(p_planes[i], p_point) < -p_radius) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[Mariner][E14b] API") {
	MethodBind *m = ClassDB::get_method("RenderingServer", "multimesh_set_indirect_shadow_culling");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);
	CHECK(ClassDB::get_method("RenderingServer", "multimesh_get_shadow_command_buffer_rd_rid") != nullptr);
}

TEST_CASE("[Mariner][E14b] Side planes of shadow passes") {
	Vector4 planes[4];
	// A cascade: orthographic, 20 x 10 m.
	Projection ortho;
	ortho.set_orthogonal(-10, 10, -5, 5, 0.1, 100);
	RendererRD::MultiMeshShadowCull::get_side_planes(ortho, planes);
	CHECK(inside(planes, Vector3(0, 0, -50)));
	CHECK(inside(planes, Vector3(9.9, 4.9, -500))); // Near / far are not tested (casters before the cascade).
	CHECK_FALSE(inside(planes, Vector3(11, 0, -50)));
	CHECK_FALSE(inside(planes, Vector3(-11, 0, -50)));
	CHECK_FALSE(inside(planes, Vector3(0, 6, -50)));
	CHECK(inside(planes, Vector3(11, 0, -50), 1.5)); // A sphere reaching in.
	CHECK(side(planes[1], Vector3(12, 0, -1)) == doctest::Approx(-2.0)); // Normalized: meters.

	// A spot light: perspective 90 degrees.
	Projection persp;
	persp.set_perspective(90, 1.0, 0.1, 100);
	RendererRD::MultiMeshShadowCull::get_side_planes(persp, planes);
	CHECK(inside(planes, Vector3(0, 0, -10)));
	CHECK_FALSE(inside(planes, Vector3(12, 0, -10)));
	CHECK(side(planes[1], Vector3(12, 0, -10)) == doctest::Approx(-Math::SQRT2).epsilon(0.001));
	CHECK(inside(planes, Vector3(12, 0, -10), 1.5));

	// In the space of a multimesh moved 30 m along X: its point (-25, 0, -50) is (5, 0, -50) in the cascade.
	RendererRD::MultiMeshShadowCull::get_side_planes(ortho * Projection(Transform3D(Basis(), Vector3(30, 0, 0))), planes);
	CHECK(inside(planes, Vector3(-25, 0, -50)));
	CHECK_FALSE(inside(planes, Vector3(0, 0, -50)));
}

} // namespace TestMarinerE14
