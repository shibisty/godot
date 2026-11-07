/**************************************************************************/
/*  test_mariner_shadow_cache.cpp                                         */
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

TEST_FORCE_LINK(test_mariner_shadow_cache)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E10: directional shadow cache (Forward+). Static casters of each cascade are kept in a second
// depth texture and copied into the shadow atlas every frame; dynamic casters are drawn on top. Here: the API, the
// settings and the math that keeps the cascades on a fixed grid. The rendering itself (cached = uncached image, hits
// and refreshes, moving objects, sun threshold) is checked with a real renderer by engine/tests/e10/shadow_cache.gd.

namespace TestMarinerShadowCache {

TEST_CASE("[Mariner][E10] RenderingServer API and enum") {
	MethodBind *set_enabled = ClassDB::get_method("RenderingServer", "directional_shadow_cache_set_enabled");
	REQUIRE(set_enabled != nullptr);
	CHECK(set_enabled->get_argument_count() == 1);
	CHECK(set_enabled->get_argument_type(0) == Variant::BOOL);

	MethodBind *stats = ClassDB::get_method("RenderingServer", "directional_shadow_cache_get_stats");
	REQUIRE(stats != nullptr);
	CHECK(stats->get_argument_count() == 0);
	CHECK(stats->get_argument_type(-1) == Variant::DICTIONARY);
	CHECK(stats->is_const());

	MethodBind *mode = ClassDB::get_method("RenderingServer", "instance_geometry_set_shadow_cache_mode");
	REQUIRE(mode != nullptr);
	CHECK(mode->get_argument_count() == 2);
	CHECK(mode->get_argument_type(0) == Variant::RID);
	CHECK(mode->get_argument_type(1) == Variant::INT);

	bool ok = false;
	CHECK(ClassDB::get_integer_constant("RenderingServer", "SHADOW_CACHE_MODE_AUTO", &ok) == 0);
	CHECK(ok);
	CHECK(ClassDB::get_integer_constant("RenderingServer", "SHADOW_CACHE_MODE_STATIC", &ok) == 1);
	CHECK(ClassDB::get_integer_constant("RenderingServer", "SHADOW_CACHE_MODE_DYNAMIC", &ok) == 2);
	CHECK(ClassDB::get_integer_constant_enum("RenderingServer", "SHADOW_CACHE_MODE_STATIC") == StringName("ShadowCacheMode"));
}

TEST_CASE("[Mariner][E10] GeometryInstance3D.shadow_cache_mode") {
	PropertyInfo pi;
	REQUIRE(ClassDB::get_property_info("GeometryInstance3D", "shadow_cache_mode", &pi));
	CHECK(pi.type == Variant::INT);
	CHECK(pi.hint == PROPERTY_HINT_ENUM);
	CHECK(pi.hint_string == "Auto,Static,Dynamic");
	bool valid = false;
	CHECK(int(ClassDB::class_get_default_property_value("MeshInstance3D", "shadow_cache_mode", &valid)) == 0);
	CHECK(valid);
	CHECK(int(GeometryInstance3D::SHADOW_CACHE_MODE_DYNAMIC) == int(RSE::SHADOW_CACHE_MODE_DYNAMIC));

	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_shadow_cache_mode(GeometryInstance3D::SHADOW_CACHE_MODE_STATIC);
	CHECK(mi->get_shadow_cache_mode() == GeometryInstance3D::SHADOW_CACHE_MODE_STATIC);
	mi->set_shadow_cache_mode(GeometryInstance3D::SHADOW_CACHE_MODE_AUTO);
	memdelete(mi);
}

TEST_CASE("[Mariner][E10] Project settings and defaults (off: stock behavior)") {
	struct Setting {
		const char *name;
		Variant value;
	};
	const Setting settings[] = {
		{ "rendering/lights_and_shadows/directional_shadow/cache", false },
		{ "rendering/lights_and_shadows/directional_shadow/cache_static_after_frames", 30 },
		{ "rendering/lights_and_shadows/directional_shadow/cache_light_angle_threshold", 0.1 },
	};
	for (const Setting &setting : settings) {
		INFO(setting.name);
		REQUIRE(ProjectSettings::get_singleton()->has_setting(setting.name));
		CHECK(ProjectSettings::get_singleton()->property_get_revert(setting.name) == setting.value);
	}
}

TEST_CASE("[Mariner][E10] Without a scene renderer cache: stats say disabled, calls are harmless") {
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	rs->directional_shadow_cache_set_enabled(false);
	const Dictionary stats = rs->directional_shadow_cache_get_stats();
	CHECK_FALSE(bool(stats.get("enabled", false)));
	CHECK(int(stats.get("hits", 0)) == 0);
}

TEST_CASE("[Mariner][E10] Cascade radius on a grid: never smaller, at most 1/128 larger, stable under jitter") {
	const real_t radii[] = { 0.7, 3.2, 10.0, 47.5, 128.0, 333.3, 1000.0 }; // On and off grid lines.
	for (real_t r : radii) {
		INFO(r);
		const real_t q = RendererSceneCull::shadow_cache_quantize_radius(r);
		CHECK(q >= r);
		CHECK(q <= r * (1.0 + 1.0 / 128.0) + 1e-6);
		CHECK(RendererSceneCull::shadow_cache_quantize_radius(q) == q); // Idempotent.
		// Frame to frame (with the previous value) the float jitter of the frustum fit never changes the radius,
		// also on a grid line; the result always covers the radius.
		real_t previous = q;
		for (int i = -50; i <= 50; i++) {
			const real_t jittered = r * (1.0 + i * 1e-7);
			const real_t now = RendererSceneCull::shadow_cache_quantize_radius(jittered, previous);
			CHECK(now >= jittered);
			if (i > 0 && now != previous) {
				// Only allowed when the previous value did not cover the radius any more.
				CHECK(jittered > previous);
			}
			previous = now;
		}
		CHECK(previous <= q * (1.0 + 1.0 / 128.0) + 1e-6);
		// A real change (another field of view) is followed.
		CHECK(RendererSceneCull::shadow_cache_quantize_radius(r * 0.5, q) < q * 0.6);
		CHECK(RendererSceneCull::shadow_cache_quantize_radius(r * 2.0, q) >= r * 2.0);
	}
}

TEST_CASE("[Mariner][E10] Static set hash: order-independent, sensitive to members and versions") {
	struct Item {
		uint64_t ptr;
		uint64_t version;
	};
	auto set_hash = [](const LocalVector<Item> &p_items) {
		uint64_t sum = 0;
		uint64_t x = 0;
		for (const Item &it : p_items) {
			const uint64_t h = RendererSceneCull::shadow_cache_mix(it.ptr ^ RendererSceneCull::shadow_cache_mix(it.version));
			sum += h;
			x ^= RendererSceneCull::shadow_cache_mix(h);
		}
		return RendererSceneCull::shadow_cache_mix(sum ^ RendererSceneCull::shadow_cache_mix(x ^ RendererSceneCull::shadow_cache_mix(p_items.size())));
	};
	LocalVector<Item> a = { { 0x1000, 1 }, { 0x2000, 7 }, { 0x3000, 3 } };
	LocalVector<Item> b = { { 0x3000, 3 }, { 0x1000, 1 }, { 0x2000, 7 } };
	CHECK(set_hash(a) == set_hash(b)); // Threads push in any order.
	LocalVector<Item> c = { { 0x1000, 1 }, { 0x2000, 8 }, { 0x3000, 3 } };
	CHECK(set_hash(a) != set_hash(c)); // A version changed.
	LocalVector<Item> d = { { 0x1000, 1 }, { 0x2000, 7 } };
	CHECK(set_hash(a) != set_hash(d)); // A member left.
	LocalVector<Item> e = { { 0x1000, 1 }, { 0x2000, 7 }, { 0x3000, 3 }, { 0x3000, 3 } };
	CHECK(set_hash(a) != set_hash(e));
	CHECK(set_hash(LocalVector<Item>()) != set_hash(a));
}

TEST_CASE("[Mariner][E10b] DirectionalLight3D object shadow: properties, defaults, RenderingServer method") {
	struct Prop {
		const char *name;
		Variant value;
		PropertyHint hint;
	};
	const Prop props[] = {
		{ "object_shadow_layers", 0, PROPERTY_HINT_LAYERS_3D_RENDER },
		{ "object_shadow_size", 2048, PROPERTY_HINT_ENUM },
		{ "object_shadow_from_cascade", 1, PROPERTY_HINT_RANGE },
	};
	for (const Prop &prop : props) {
		INFO(prop.name);
		PropertyInfo pi;
		REQUIRE(ClassDB::get_property_info("DirectionalLight3D", prop.name, &pi));
		CHECK(pi.type == Variant::INT);
		CHECK(pi.hint == prop.hint);
		bool valid = false;
		CHECK(ClassDB::class_get_default_property_value("DirectionalLight3D", prop.name, &valid) == prop.value);
		CHECK(valid);
	}

	MethodBind *m = ClassDB::get_method("RenderingServer", "light_directional_set_object_shadow");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 4);
	CHECK(m->get_argument_type(0) == Variant::RID);
	CHECK(m->get_argument_type(1) == Variant::INT);
	CHECK(m->get_argument_type(2) == Variant::INT);
	CHECK(m->get_argument_type(3) == Variant::INT);

	// Setters clamp: power of two 64..16384, cascade 0..4.
	DirectionalLight3D *light = memnew(DirectionalLight3D);
	light->set_object_shadow_size(3000);
	CHECK(light->get_object_shadow_size() == 4096);
	light->set_object_shadow_size(10);
	CHECK(light->get_object_shadow_size() == 64);
	light->set_object_shadow_size(100000);
	CHECK(light->get_object_shadow_size() == 16384);
	light->set_object_shadow_from_cascade(9);
	CHECK(light->get_object_shadow_from_cascade() == 4);
	light->set_object_shadow_from_cascade(-1);
	CHECK(light->get_object_shadow_from_cascade() == 0);
	light->set_object_shadow_layers(1 << 1);
	CHECK(light->get_object_shadow_layers() == (1u << 1));
	memdelete(light);
}

} // namespace TestMarinerShadowCache
