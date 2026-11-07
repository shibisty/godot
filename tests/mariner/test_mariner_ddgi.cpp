/**************************************************************************/
/*  test_mariner_ddgi.cpp                                                 */
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

TEST_FORCE_LINK(test_mariner_ddgi)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "core/templates/hash_set.h"
#include "scene/3d/ddgi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_ddgi.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/environment_storage.h"

// Mariner engine patch E12: dynamic diffuse GI probes (DDGI) on the raytracing scene, in place of the ambient light.
// Here: the Environment API and its clamping, the grid placement / scrolling, the atlas layout and the ray rotations.
// The image (color bleeding, dark interiors, scrolling with the camera) is checked with a real renderer on lavapipe by
// engine/tests/e12/ddgi.gd.

namespace TestMarinerDDGI {

using namespace RendererSceneRenderImplementation;

TEST_CASE("[Mariner][E12] Environment DDGI properties") {
	struct Prop {
		const char *name;
		Variant::Type type;
		Variant value;
	};
	const Prop props[] = {
		{ "ddgi_enabled", Variant::BOOL, false },
		{ "ddgi_probe_count", Variant::VECTOR3I, Vector3i(24, 8, 24) },
		{ "ddgi_probe_spacing", Variant::FLOAT, 2.0 },
		{ "ddgi_rays_per_probe", Variant::INT, 128 },
		{ "ddgi_hysteresis", Variant::FLOAT, 0.97 },
		{ "ddgi_energy", Variant::FLOAT, 1.0 },
		{ "ddgi_bias", Variant::FLOAT, 0.3 },
		{ "ddgi_specular_occlusion", Variant::FLOAT, 1.0 },
		{ "ddgi_probes_per_frame", Variant::INT, 0 },
		{ "ddgi_relocation", Variant::BOOL, true },
		{ "ddgi_sky_energy", Variant::FLOAT, 1.0 },
		{ "ddgi_lights_energy", Variant::FLOAT, 1.0 },
		{ "ddgi_sea_enabled", Variant::BOOL, false },
		{ "ddgi_sea_level", Variant::FLOAT, 0.0 },
		{ "ddgi_sea_color", Variant::COLOR, Color(0.02, 0.08, 0.1) },
	};
	for (const Prop &p : props) {
		PropertyInfo pi;
		REQUIRE_MESSAGE(ClassDB::get_property_info("Environment", p.name, &pi), p.name);
		CHECK_MESSAGE(pi.type == p.type, p.name);
		bool valid = false;
		Variant def = ClassDB::class_get_default_property_value("Environment", p.name, &valid);
		CHECK_MESSAGE(valid, p.name);
		if (p.type == Variant::FLOAT) {
			CHECK_MESSAGE(double(def) == doctest::Approx(double(p.value)), p.name);
		} else {
			CHECK_MESSAGE(def == p.value, p.name);
		}
	}

	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_ddgi");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 9);
	CHECK(m->get_argument_type(2) == Variant::VECTOR3I);
	m = ClassDB::get_method("RenderingServer", "environment_set_ddgi_update");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 3);
	m = ClassDB::get_method("RenderingServer", "environment_set_ddgi_channels");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 3);
	m = ClassDB::get_method("RenderingServer", "environment_set_ddgi_sea");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 4);
	CHECK(m->get_argument_type(3) == Variant::COLOR);
}

TEST_CASE("[Mariner][E12] Environment DDGI setters clamp and reach the environment storage") {
	Ref<Environment> env;
	env.instantiate();
	RendererEnvironmentStorage *storage = RendererEnvironmentStorage::get_singleton();
	REQUIRE(storage != nullptr);
	const RID rid = env->get_rid();

	CHECK_FALSE(storage->environment_get_ddgi_enabled(rid));
	CHECK(storage->environment_get_ddgi_probe_count(rid) == Vector3i(24, 8, 24));

	env->set_ddgi_enabled(true);
	env->set_ddgi_probe_count(Vector3i(16, 6, 20));
	env->set_ddgi_probe_spacing(1.5);
	env->set_ddgi_rays_per_probe(100); // Rounded up to a multiple of 32.
	env->set_ddgi_hysteresis(0.9);
	env->set_ddgi_energy(1.5);
	env->set_ddgi_bias(0.2);
	env->set_ddgi_specular_occlusion(0.5);
	CHECK(storage->environment_get_ddgi_enabled(rid));
	CHECK(storage->environment_get_ddgi_probe_count(rid) == Vector3i(16, 6, 20));
	CHECK(storage->environment_get_ddgi_probe_spacing(rid) == doctest::Approx(1.5));
	CHECK(storage->environment_get_ddgi_rays_per_probe(rid) == 128);
	CHECK(env->get_ddgi_rays_per_probe() == 128);
	CHECK(storage->environment_get_ddgi_hysteresis(rid) == doctest::Approx(0.9));
	CHECK(storage->environment_get_ddgi_energy(rid) == doctest::Approx(1.5));
	CHECK(storage->environment_get_ddgi_bias(rid) == doctest::Approx(0.2));
	CHECK(storage->environment_get_ddgi_specular_occlusion(rid) == doctest::Approx(0.5));

	env->set_ddgi_probes_per_frame(500);
	env->set_ddgi_relocation(false);
	CHECK(storage->environment_get_ddgi_probes_per_frame(rid) == 500);
	CHECK_FALSE(storage->environment_get_ddgi_relocation(rid));
	env->set_ddgi_sky_energy(4.0);
	env->set_ddgi_lights_energy(-1.0);
	CHECK(storage->environment_get_ddgi_sky_energy(rid) == doctest::Approx(4.0));
	CHECK(storage->environment_get_ddgi_lights_energy(rid) == 0.0);
	CHECK_FALSE(storage->environment_get_ddgi_sea(rid));
	env->set_ddgi_sea_enabled(true);
	env->set_ddgi_sea_level(-1.25);
	env->set_ddgi_sea_color(Color(0.1, 0.2, 0.3));
	CHECK(storage->environment_get_ddgi_sea(rid));
	CHECK(storage->environment_get_ddgi_sea_level(rid) == doctest::Approx(-1.25));
	CHECK(storage->environment_get_ddgi_sea_color(rid).is_equal_approx(Color(0.1, 0.2, 0.3)));
	env->set_ddgi_probes_per_frame(-3);
	CHECK(env->get_ddgi_probes_per_frame() == 0);

	env->set_ddgi_rays_per_probe(1000);
	CHECK(env->get_ddgi_rays_per_probe() == 256);
	env->set_ddgi_rays_per_probe(1);
	CHECK(env->get_ddgi_rays_per_probe() == 32);
	env->set_ddgi_hysteresis(2.0);
	CHECK(env->get_ddgi_hysteresis() == doctest::Approx(0.995));
	env->set_ddgi_probe_spacing(0.0);
	CHECK(env->get_ddgi_probe_spacing() == doctest::Approx(0.1));
	env->set_ddgi_energy(-1.0);
	CHECK(env->get_ddgi_energy() == 0.0);
	env->set_ddgi_specular_occlusion(3.0);
	CHECK(env->get_ddgi_specular_occlusion() == 1.0);

	// Probe count: per axis limits, then the atlas limits (x * y tiles in a row, at most 16384 probes).
	env->set_ddgi_probe_count(Vector3i(1, 1, 1));
	CHECK(env->get_ddgi_probe_count() == Vector3i(2, 2, 2));
	env->set_ddgi_probe_count(Vector3i(100, 100, 100));
	const Vector3i c = env->get_ddgi_probe_count();
	CHECK(c.x == 64);
	CHECK(c.x * c.y <= 896);
	CHECK(c.x * c.y * c.z <= 16384);
	CHECK(storage->environment_get_ddgi_probe_count(rid) == c);
	CHECK(RenderDDGI::distance_atlas_size(c).width <= 16384);
	CHECK(RenderDDGI::distance_atlas_size(c).height <= 16384);
}

TEST_CASE("[Mariner][E12] Grid placement follows the camera with hysteresis") {
	const Vector3i count(8, 4, 8);
	const float spacing = 2.0;
	// Camera at the origin: the grid is centered (cells -4..3, -2..1, -4..3).
	Vector3i g = RenderDDGI::grid_min_for(Vector3(), count, spacing, Vector3i(), false);
	CHECK(g == Vector3i(-4, -2, -4));
	// Small moves keep the grid (less than 0.75 cells from its center cell).
	CHECK(RenderDDGI::grid_min_for(Vector3(1.4, -1.4, 0.5), count, spacing, g, true) == g);
	// A larger move scrolls it to the cell under the camera.
	Vector3i g2 = RenderDDGI::grid_min_for(Vector3(1.6, 0.0, 0.0), count, spacing, g, true);
	CHECK(g2 == Vector3i(-3, -2, -4));
	// Moving back a little does not scroll back (no flicker on a cell boundary).
	CHECK(RenderDDGI::grid_min_for(Vector3(1.0, 0.0, 0.0), count, spacing, g2, true) == g2);
	// Far away, negative coordinates.
	CHECK(RenderDDGI::grid_min_for(Vector3(-101.0, 7.0, 1000.0), count, spacing, g2, true) == Vector3i(-54, 2, 496));
}

TEST_CASE("[Mariner][E12] Atlas layout and ray rotations") {
	CHECK(RenderDDGI::irradiance_atlas_size(Vector3i(24, 8, 24)) == Size2i(24 * 8 * 10, 24 * 10));
	CHECK(RenderDDGI::distance_atlas_size(Vector3i(24, 8, 24)) == Size2i(24 * 8 * 18, 24 * 18));

	// Rotations are orthonormal, differ per frame and are the same for the same frame.
	for (uint32_t frame = 0; frame < 16; frame++) {
		const Basis b = RenderDDGI::ray_rotation(frame);
		CHECK(b.is_orthogonal());
		CHECK(b.determinant() == doctest::Approx(1.0));
		CHECK(b.is_equal_approx(RenderDDGI::ray_rotation(frame)));
		CHECK_FALSE(b.is_equal_approx(RenderDDGI::ray_rotation(frame + 1)));
	}
	CHECK(sizeof(DDGI_Params) == 304);
	CHECK(sizeof(DDGI_ParamsBlock) == 608);
}

TEST_CASE("[Mariner][E12] Probe slots to update: new cells first, then round robin") {
	const Vector3i count(4, 2, 4); // 32 slots.
	const Vector3i g(-2, -1, -2);
	LocalVector<uint32_t> slots;
	uint32_t rr = 0;

	// Slot <-> cell: every cell of the grid is in exactly one slot, the slot is the cell mod count.
	HashSet<Vector3i> cells;
	for (uint32_t i = 0; i < 32; i++) {
		const Vector3i c = RenderDDGI::slot_to_cell(i, Vector3i(5, -3, 7), count);
		CHECK(c.x >= 5);
		CHECK(c.x < 9);
		CHECK(c.y >= -3);
		CHECK(c.y < -1);
		CHECK(c.z >= 7);
		CHECK(c.z < 11);
		CHECK(Math::posmod(c.x, 4) + Math::posmod(c.y, 2) * 4 + Math::posmod(c.z, 4) * 8 == int(i));
		cells.insert(c);
	}
	CHECK(cells.size() == 32);

	// No budget: all slots, in order.
	CHECK(RenderDDGI::build_slot_list(count, g, true, g, 0, rr, slots) == 0);
	CHECK(slots.size() == 32);
	CHECK(slots[31] == 31);
	// First frame: all slots are new, also with a budget.
	CHECK(RenderDDGI::build_slot_list(count, Vector3i(), false, g, 8, rr, slots) == 32);
	CHECK(slots.size() == 32);
	// Budget without movement: 8 slots per frame, round robin, all covered in 4 frames.
	rr = 0;
	HashSet<uint32_t> seen;
	for (int frame = 0; frame < 4; frame++) {
		CHECK(RenderDDGI::build_slot_list(count, g, true, g, 8, rr, slots) == 0);
		CHECK(slots.size() == 8);
		for (uint32_t s : slots) {
			seen.insert(s);
		}
	}
	CHECK(seen.size() == 32);
	// The grid moves one cell along +x: the 2 x 4 = 8 slots of the new x plane come first, then the budget is full.
	const Vector3i g2 = g + Vector3i(1, 0, 0);
	CHECK(RenderDDGI::build_slot_list(count, g, true, g2, 12, rr, slots) == 8);
	CHECK(slots.size() == 12);
	for (uint32_t i = 0; i < 8; i++) {
		CHECK(RenderDDGI::slot_to_cell(slots[i], g2, count).x == g2.x + 3); // The entered plane.
	}
	HashSet<uint32_t> unique;
	for (uint32_t s : slots) {
		unique.insert(s);
	}
	CHECK(unique.size() == 12); // No slot twice (two work groups would write the same texels).
}

TEST_CASE("[Mariner][E12] DDGIVolume3D: properties, probe layout and the RenderingServer method") {
	struct Prop {
		const char *name;
		Variant value;
	};
	const Prop props[] = {
		{ "size", Vector3(20, 8, 60) },
		{ "probe_spacing", 1.0 },
		{ "rays_per_probe", 128 },
		{ "probes_per_frame", 0 },
	};
	for (const Prop &p : props) {
		bool valid = false;
		Variant def = ClassDB::class_get_default_property_value("DDGIVolume3D", p.name, &valid);
		CHECK_MESSAGE(valid, p.name);
		CHECK_MESSAGE(def == p.value, p.name);
	}
	DDGIVolume3D *v = memnew(DDGIVolume3D);
	v->set_size(Vector3(-1, 4, 10));
	CHECK(v->get_size() == Vector3(0.1, 4, 10));
	v->set_rays_per_probe(70);
	CHECK(v->get_rays_per_probe() == 96);
	v->set_probe_spacing(0.0);
	CHECK(v->get_probe_spacing() == doctest::Approx(0.1));
	CHECK(v->get_aabb() == AABB(Vector3(-0.05, -2, -5), Vector3(0.1, 4, 10)));
	memdelete(v);

	// About one probe per spacing, corners included; a long ship hits the x limit and the spacing grows.
	CHECK(DDGIVolume3D::probe_count_for(Vector3(10, 4, 20), 1.0) == Vector3i(11, 5, 21));
	CHECK(DDGIVolume3D::probe_count_for(Vector3(120, 20, 18), 1.0).x == 64);

	Vector3i count;
	Vector3 spacing;
	Transform3D g2w;
	const Transform3D ship(Basis(Vector3(0, 1, 0), 0.5), Vector3(100, 2, -40));
	RenderDDGI::local_volume_layout(ship, Vector3(10, 4, 20), 1.0, count, spacing, g2w);
	CHECK(count == Vector3i(11, 5, 21));
	CHECK(spacing.is_equal_approx(Vector3(1, 1, 1)));
	// Probe (0, 0, 0) at the box corner, the last probe at the opposite corner, in the ship's frame.
	CHECK(g2w.xform(Vector3()).is_equal_approx(ship.xform(Vector3(-5, -2, -10))));
	CHECK(g2w.xform(Vector3(10, 4, 20)).is_equal_approx(ship.xform(Vector3(5, 2, 10))));
	RenderDDGI::local_volume_layout(ship, Vector3(120, 20, 18), 1.0, count, spacing, g2w);
	CHECK(spacing.x == doctest::Approx(120.0 / 63.0));
	CHECK(count.x * count.y <= 896);
	CHECK(count.x * count.y * count.z <= 16384);
	CHECK(spacing.y == doctest::Approx(20.0 / (count.y - 1)));
	// Scale of the node does not reach the grid (rigid).
	RenderDDGI::local_volume_layout(Transform3D(Basis().scaled(Vector3(2, 2, 2)), Vector3()), Vector3(4, 4, 4), 1.0, count, spacing, g2w);
	CHECK(g2w.basis.is_orthonormal());

	MethodBind *m = ClassDB::get_method("RenderingServer", "scenario_set_ddgi_volume");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 7);
	CHECK(m->get_argument_type(2) == Variant::TRANSFORM3D);

	// Indirect MultiMesh (GPU culled per camera) stays out of the raytracing scene by default.
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/raytracing/include_indirect_multimesh"));
	CHECK_FALSE(bool(GLOBAL_GET("rendering/raytracing/include_indirect_multimesh")));
}

TEST_CASE("[Mariner][E12] GeometryInstance3D raytracing mode and opacity") {
	bool valid = false;
	CHECK(int(ClassDB::class_get_default_property_value("MeshInstance3D", "raytracing_mode", &valid)) == GeometryInstance3D::RAYTRACING_MODE_DEFAULT);
	CHECK(valid);
	CHECK(double(ClassDB::class_get_default_property_value("MeshInstance3D", "raytracing_opacity", &valid)) == 1.0);
	CHECK(valid);
	MeshInstance3D *m = memnew(MeshInstance3D);
	m->set_raytracing_mode(GeometryInstance3D::RAYTRACING_MODE_ONLY);
	CHECK(m->get_raytracing_mode() == GeometryInstance3D::RAYTRACING_MODE_ONLY);
	m->set_raytracing_opacity(1.5);
	CHECK(m->get_raytracing_opacity() == 1.0);
	m->set_raytracing_opacity(-0.5);
	CHECK(m->get_raytracing_opacity() == 0.0);
	memdelete(m);
	MethodBind *mb = ClassDB::get_method("RenderingServer", "instance_geometry_set_raytracing");
	REQUIRE(mb != nullptr);
	CHECK(mb->get_argument_count() == 3);
	CHECK(int(RSE::RAYTRACING_MODE_DEFAULT) == int(GeometryInstance3D::RAYTRACING_MODE_DEFAULT));
	CHECK(int(RSE::RAYTRACING_MODE_ONLY) == int(GeometryInstance3D::RAYTRACING_MODE_ONLY));
	CHECK(int(RSE::RAYTRACING_MODE_DISABLED) == int(GeometryInstance3D::RAYTRACING_MODE_DISABLED));
}

} // namespace TestMarinerDDGI
