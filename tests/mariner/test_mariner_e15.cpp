/**************************************************************************/
/*  test_mariner_e15.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e15)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/resources/camera_attributes.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

// Mariner engine patch E15a: motion blur by the motion vectors (CameraAttributes.motion_blur_*). Here: the API, the
// defaults and the clamping; the blur itself (moving object, still background, camera motion weight) on lavapipe by
// engine/tests/e15/motion_blur.gd. E15b: lens flare and lens dirt (CameraAttributes.lens_*), the image by
// engine/tests/e15/lens_flare.gd.

namespace TestMarinerE15 {

TEST_CASE("[Mariner][E15a] Motion blur API and settings") {
	struct Prop {
		const char *name;
		Variant::Type type;
		Variant value;
	};
	const Prop props[] = {
		{ "motion_blur_enabled", Variant::BOOL, false },
		{ "motion_blur_intensity", Variant::FLOAT, 0.5 },
		{ "motion_blur_camera_motion", Variant::FLOAT, 1.0 },
		{ "motion_blur_max_length", Variant::FLOAT, 0.04 },
	};
	for (const char *cls : { "CameraAttributesPractical", "CameraAttributesPhysical" }) {
		for (const Prop &p : props) {
			PropertyInfo pi;
			REQUIRE_MESSAGE(ClassDB::get_property_info(cls, p.name, &pi), p.name);
			CHECK_MESSAGE(pi.type == p.type, p.name);
			bool valid = false;
			Variant def = ClassDB::class_get_default_property_value(cls, p.name, &valid);
			CHECK_MESSAGE(valid, p.name);
			if (p.type == Variant::FLOAT) {
				CHECK_MESSAGE(double(def) == doctest::Approx(double(p.value)), p.name);
			} else {
				CHECK_MESSAGE(def == p.value, p.name);
			}
		}
	}
	MethodBind *m = ClassDB::get_method("RenderingServer", "camera_attributes_set_motion_blur");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 5);
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/camera/motion_blur/samples"));
	CHECK(int(GLOBAL_GET("rendering/camera/motion_blur/samples")) == 12);

	Ref<CameraAttributesPractical> attributes;
	attributes.instantiate();
	RendererCameraAttributes *storage = RendererCameraAttributes::get_singleton();
	REQUIRE(storage != nullptr);
	const RID rid = attributes->get_rid();
	CHECK_FALSE(storage->camera_attributes_uses_motion_blur(rid));
	attributes->set_motion_blur_enabled(true);
	CHECK(storage->camera_attributes_uses_motion_blur(rid));
	attributes->set_motion_blur_intensity(9.0);
	attributes->set_motion_blur_camera_motion(-1.0);
	attributes->set_motion_blur_max_length(0.5);
	CHECK(attributes->get_motion_blur_intensity() == doctest::Approx(4.0));
	CHECK(storage->camera_attributes_get_motion_blur_intensity(rid) == doctest::Approx(4.0));
	CHECK(storage->camera_attributes_get_motion_blur_camera_motion(rid) == doctest::Approx(0.0));
	CHECK(storage->camera_attributes_get_motion_blur_max_length(rid) == doctest::Approx(0.25));
	attributes->set_motion_blur_intensity(0.0);
	CHECK_FALSE(storage->camera_attributes_uses_motion_blur(rid)); // Zero intensity: off.
}

TEST_CASE("[Mariner][E15b] Lens flare API and settings") {
	struct Prop {
		const char *name;
		Variant::Type type;
		Variant value;
	};
	const Prop props[] = {
		{ "lens_flare_enabled", Variant::BOOL, false },
		{ "lens_flare_intensity", Variant::FLOAT, 0.02 },
		{ "lens_flare_threshold", Variant::FLOAT, 4.0 },
		{ "lens_flare_ghosts", Variant::INT, 4 },
		{ "lens_flare_ghost_spacing", Variant::FLOAT, 0.35 },
		{ "lens_flare_halo_width", Variant::FLOAT, 0.45 },
		{ "lens_flare_chromatic", Variant::FLOAT, 0.006 },
		{ "lens_flare_sun_streaks", Variant::FLOAT, 1.0 },
		{ "lens_flare_streak_count", Variant::INT, 6 },
		{ "lens_dirt_intensity", Variant::FLOAT, 1.0 },
	};
	for (const char *cls : { "CameraAttributesPractical", "CameraAttributesPhysical" }) {
		for (const Prop &p : props) {
			PropertyInfo pi;
			REQUIRE_MESSAGE(ClassDB::get_property_info(cls, p.name, &pi), p.name);
			CHECK_MESSAGE(pi.type == p.type, p.name);
			bool valid = false;
			Variant def = ClassDB::class_get_default_property_value(cls, p.name, &valid);
			CHECK_MESSAGE(valid, p.name);
			if (p.type == Variant::FLOAT) {
				CHECK_MESSAGE(double(def) == doctest::Approx(double(p.value)), p.name);
			} else {
				CHECK_MESSAGE(def == p.value, p.name);
			}
		}
		PropertyInfo pi;
		CHECK(ClassDB::get_property_info(cls, "lens_dirt_texture", &pi));
		CHECK(pi.type == Variant::OBJECT);
	}
	MethodBind *m = ClassDB::get_method("RenderingServer", "camera_attributes_set_lens_flare");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 10);
	m = ClassDB::get_method("RenderingServer", "camera_attributes_set_lens_dirt");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 3);

	Ref<CameraAttributesPractical> attributes;
	attributes.instantiate();
	RendererCameraAttributes *storage = RendererCameraAttributes::get_singleton();
	REQUIRE(storage != nullptr);
	const RID rid = attributes->get_rid();
	RendererCameraAttributes::LensFlare lf;
	CHECK_FALSE(storage->camera_attributes_get_lens_flare(rid, lf));
	attributes->set_lens_flare_enabled(true);
	attributes->set_lens_flare_ghosts(20);
	attributes->set_lens_flare_streak_count(1);
	attributes->set_lens_flare_threshold(-1.0);
	REQUIRE(storage->camera_attributes_get_lens_flare(rid, lf));
	CHECK(lf.ghosts == 8);
	CHECK(lf.streak_count == 2);
	CHECK(lf.threshold == 0.0);
	CHECK(lf.dirt_texture.is_null());
	attributes->set_lens_flare_intensity(0.0);
	attributes->set_lens_flare_sun_streaks(0.0);
	CHECK_FALSE(storage->camera_attributes_get_lens_flare(rid, lf)); // Nothing to add: off.
}

} // namespace TestMarinerE15
