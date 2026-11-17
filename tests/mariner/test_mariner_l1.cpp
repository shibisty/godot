/**************************************************************************/
/*  test_mariner_l1.cpp                                                   */
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


TEST_FORCE_LINK(test_mariner_l1)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_ddgi.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/environment_storage.h"

// Mariner engine patch L1: screen space probes of the DDGI (Lumen / GI-1.0 style probes on the visible surface,
// traced in the hardware raytracing scene). Here: the Environment API, its snapping / clamping, the RenderingServer
// method and the probe grid. The light itself (contact bounce, stability, toggling) is checked with a real renderer
// on lavapipe by engine/tests/e12/screen_probes.gd.

namespace TestMarinerL1 {

using namespace RendererSceneRenderImplementation;

TEST_CASE("[Mariner][L1] Environment screen probe properties and defaults") {
	Ref<Environment> env;
	env.instantiate();
	CHECK_FALSE(env->is_ddgi_screen_probes_enabled());
	CHECK(env->get_ddgi_screen_probes_size() == 16);
	CHECK(env->get_ddgi_screen_probes_hysteresis() == doctest::Approx(0.9));
	List<PropertyInfo> props;
	ClassDB::get_property_list("Environment", &props);
	int found = 0;
	for (const PropertyInfo &p : props) {
		if (p.name == "ddgi_screen_probes_enabled") {
			CHECK(p.type == Variant::BOOL);
			found++;
		} else if (p.name == "ddgi_screen_probes_size") {
			CHECK(p.type == Variant::INT);
			CHECK(p.hint == PROPERTY_HINT_ENUM);
			found++;
		} else if (p.name == "ddgi_screen_probes_hysteresis") {
			CHECK(p.type == Variant::FLOAT);
			found++;
		}
	}
	CHECK(found == 3);
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_ddgi_screen_probes"));
	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_ddgi_screen_probes");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 4);
}

TEST_CASE("[Mariner][L1] Environment screen probe setters snap, clamp and reach the environment storage") {
	Ref<Environment> env;
	env.instantiate();
	RendererEnvironmentStorage *storage = RendererEnvironmentStorage::get_singleton();
	REQUIRE(storage != nullptr);
	const RID rid = env->get_rid();
	CHECK_FALSE(storage->environment_get_ddgi_screen_probes(rid));
	CHECK(storage->environment_get_ddgi_screen_probes_size(rid) == 16);

	env->set_ddgi_screen_probes_enabled(true);
	env->set_ddgi_screen_probes_size(8);
	env->set_ddgi_screen_probes_hysteresis(0.5);
	CHECK(storage->environment_get_ddgi_screen_probes(rid));
	CHECK(storage->environment_get_ddgi_screen_probes_size(rid) == 8);
	CHECK(storage->environment_get_ddgi_screen_probes_hysteresis(rid) == doctest::Approx(0.5));

	// Sizes other than 8 and 16 snap; the hysteresis stays below 1 (the history must fade out).
	env->set_ddgi_screen_probes_size(3);
	CHECK(env->get_ddgi_screen_probes_size() == 8);
	env->set_ddgi_screen_probes_size(12);
	CHECK(env->get_ddgi_screen_probes_size() == 16);
	env->set_ddgi_screen_probes_size(64);
	CHECK(env->get_ddgi_screen_probes_size() == 16);
	env->set_ddgi_screen_probes_hysteresis(1.0);
	CHECK(env->get_ddgi_screen_probes_hysteresis() == doctest::Approx(0.98));
	env->set_ddgi_screen_probes_hysteresis(-1.0);
	CHECK(env->get_ddgi_screen_probes_hysteresis() == 0.0);
	CHECK(storage->environment_get_ddgi_screen_probes_hysteresis(rid) == 0.0);

	// Directly through the server: the storage snaps / clamps as well.
	RS::get_singleton()->environment_set_ddgi_screen_probes(rid, false, 5, 7.0);
	CHECK_FALSE(storage->environment_get_ddgi_screen_probes(rid));
	CHECK(storage->environment_get_ddgi_screen_probes_size(rid) == 8);
	CHECK(storage->environment_get_ddgi_screen_probes_hysteresis(rid) == doctest::Approx(0.98));
}

TEST_CASE("[Mariner][L1] Screen probe grid covers the internal size") {
	CHECK(RenderDDGI::screen_probe_grid(Size2i(1920, 1080), 16) == Size2i(120, 68));
	CHECK(RenderDDGI::screen_probe_grid(Size2i(1920, 1080), 8) == Size2i(240, 135));
	CHECK(RenderDDGI::screen_probe_grid(Size2i(320, 192), 16) == Size2i(20, 12));
	CHECK(RenderDDGI::screen_probe_grid(Size2i(1, 1), 16) == Size2i(1, 1));
	// Every pixel falls into a probe tile of the grid.
	const Size2i s(1283, 721);
	for (uint32_t tile : { 8u, 16u }) {
		const Size2i g = RenderDDGI::screen_probe_grid(s, tile);
		CHECK(g.width * int(tile) >= s.width);
		CHECK(g.height * int(tile) >= s.height);
		CHECK((g.width - 1) * int(tile) < s.width);
		CHECK((g.height - 1) * int(tile) < s.height);
	}
}

TEST_CASE("[Mariner][L2] Environment reflection properties, clamping and the RenderingServer method") {
	Ref<Environment> env;
	env.instantiate();
	RendererEnvironmentStorage *storage = RendererEnvironmentStorage::get_singleton();
	REQUIRE(storage != nullptr);
	const RID rid = env->get_rid();
	CHECK_FALSE(env->is_ddgi_reflections_enabled());
	CHECK(env->get_ddgi_reflections_max_roughness() == doctest::Approx(0.5));
	CHECK_FALSE(storage->environment_get_ddgi_reflections(rid));
	CHECK(storage->environment_get_ddgi_reflections_max_roughness(rid) == doctest::Approx(0.5));

	env->set_ddgi_reflections_enabled(true);
	env->set_ddgi_reflections_max_roughness(0.35);
	CHECK(storage->environment_get_ddgi_reflections(rid));
	CHECK(storage->environment_get_ddgi_reflections_max_roughness(rid) == doctest::Approx(0.35));
	env->set_ddgi_reflections_max_roughness(0.0); // At least the fade width (0.05).
	CHECK(env->get_ddgi_reflections_max_roughness() == doctest::Approx(0.05));
	env->set_ddgi_reflections_max_roughness(3.0);
	CHECK(env->get_ddgi_reflections_max_roughness() == doctest::Approx(1.0));

	RS::get_singleton()->environment_set_ddgi_reflections(rid, false, -1.0);
	CHECK_FALSE(storage->environment_get_ddgi_reflections(rid));
	CHECK(storage->environment_get_ddgi_reflections_max_roughness(rid) == doctest::Approx(0.05));

	List<PropertyInfo> props;
	ClassDB::get_property_list("Environment", &props);
	int found = 0;
	for (const PropertyInfo &p : props) {
		if (p.name == "ddgi_reflections_enabled" || p.name == "ddgi_reflections_max_roughness") {
			found++;
		}
	}
	CHECK(found == 2);
	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_ddgi_reflections");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 3);
}

} // namespace TestMarinerL1
