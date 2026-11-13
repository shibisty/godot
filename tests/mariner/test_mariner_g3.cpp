/**************************************************************************/
/*  test_mariner_g3.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g3)

#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"

// Mariner engine patch G3: light clip volumes (Light3D.clip_volume_*) - a box in the light's space outside which an
// omni, spot or area light does not shine (a cabin lamp does not light the deck through the hull, CryEngine's clip
// volumes as an idea). Images on lavapipe: engine/tests/g/light_clip.gd.

namespace TestMarinerG3 {

using RendererRD::LightStorage;

static Vector3 box_local(const float p_rows[3][4], const Vector3 &p_view) {
	Vector3 r;
	for (int i = 0; i < 3; i++) {
		r[i] = p_rows[i][0] * p_view.x + p_rows[i][1] * p_view.y + p_rows[i][2] * p_view.z + p_rows[i][3];
	}
	return r;
}

TEST_CASE("[Mariner][G3] Light3D clip volume properties") {
	OmniLight3D *light = memnew(OmniLight3D);
	CHECK_FALSE(light->is_clip_volume_enabled());
	CHECK(light->get_clip_volume_size().is_equal_approx(Vector3(4, 3, 4)));
	CHECK(light->get_clip_volume_offset().is_zero_approx());
	light->set_clip_volume_enabled(true);
	light->set_clip_volume_offset(Vector3(0, -1, 0));
	light->set_clip_volume_size(Vector3(-1, 2, 3));
	CHECK(light->is_clip_volume_enabled());
	CHECK(light->get_clip_volume_offset().is_equal_approx(Vector3(0, -1, 0)));
	CHECK(light->get_clip_volume_size().is_equal_approx(Vector3(0.001, 2, 3))); // Positive.
	memdelete(light);
	for (const char *name : { "clip_volume_enabled", "clip_volume_offset", "clip_volume_size" }) {
		PropertyInfo pi;
		CHECK_MESSAGE(ClassDB::get_property_info("SpotLight3D", name, &pi), name);
	}
	MethodBind *m = ClassDB::get_method("RenderingServer", "light_set_clip_volume");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 4);
	// The light buffer grew by the box: static_assert in light_storage.h (272 bytes, light_data_inc.glsl).
}

TEST_CASE("[Mariner][G3] Clip box rows in view space") {
	// A light at (10, 2, 0) turned 90 degrees about Y, the box 4 x 2 x 6 m below it (offset -1 in y); the camera at
	// (10, 1, 20) looking along -Z.
	Transform3D light_xform(Basis(Vector3(0, 1, 0), Math::PI * 0.5), Vector3(10, 2, 0));
	Transform3D camera(Basis(), Vector3(10, 1, 20));
	const Transform3D world_to_view = camera.affine_inverse();
	float rows[3][4];
	LightStorage::clip_volume_rows(world_to_view, light_xform, Vector3(0, -1, 0), Vector3(4, 2, 6), rows);
	// The box center (world (10, 1, 0)): local 0.
	CHECK(box_local(rows, world_to_view.xform(Vector3(10, 1, 0))).is_zero_approx());
	// The light's local x is world -z (turned 90 degrees): 2 m along it is the face (+1).
	CHECK(box_local(rows, world_to_view.xform(Vector3(10, 1, -2))).x == doctest::Approx(1.0));
	// Its local z is world +x: 3 m is the face; 3.5 m is outside.
	CHECK(box_local(rows, world_to_view.xform(Vector3(13, 1, 0))).z == doctest::Approx(1.0));
	CHECK(Math::abs(box_local(rows, world_to_view.xform(Vector3(13.5, 1, 0))).z) > 1.0);
	// y: the box spans 0 .. 2 m in world y.
	CHECK(box_local(rows, world_to_view.xform(Vector3(10, 0, 0))).y == doctest::Approx(-1.0));
	CHECK(box_local(rows, world_to_view.xform(Vector3(10, 2.5, 0))).y > 1.0);
	// A scaled light node does not stretch the box.
	Transform3D scaled = light_xform;
	scaled.basis.scale(Vector3(3, 3, 3));
	float rows_scaled[3][4];
	LightStorage::clip_volume_rows(world_to_view, scaled, Vector3(0, -1, 0), Vector3(4, 2, 6), rows_scaled);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			CHECK(rows_scaled[i][j] == doctest::Approx(rows[i][j]));
		}
	}
}

} // namespace TestMarinerG3
