/**************************************************************************/
/*  test_mariner_hiz.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_hiz)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "servers/rendering/renderer_scene_hiz_occlusion.h"

// Mariner engine patch E13: occlusion culling of all instances by the depth of earlier frames. Here: the API and the
// CPU reprojection of a read back depth grid into a moved camera (with and without the reference frame of a ship).
// The whole path (GPU reduction, read back, culling of instances) is checked on lavapipe by engine/tests/e13/hiz.gd.

namespace TestMarinerHiZ {

static Projection test_projection() {
	Projection p;
	p.set_perspective(70.0, 16.0 / 9.0, 0.05, 1000.0);
	return p;
}

// A wall (plane z = -p_wall in the camera's view) seen from p_cam: each cell holds the farthest distance of its corners.
static RendererSceneHiZOcclusion::Snapshot wall_snapshot(const Transform3D &p_cam, float p_wall, const Transform3D &p_reference = Transform3D(), float p_radius = 0.0) {
	RendererSceneHiZOcclusion::Snapshot s;
	s.size = Size2i(64, 36);
	s.cam_transform = p_cam;
	s.projection = test_projection();
	s.reference = p_reference;
	s.reference_radius = p_radius;
	s.frame = 1;
	s.valid = true;
	s.distances.resize(64 * 36);
	const Projection inv = s.projection.inverse();
	for (int y = 0; y < 36; y++) {
		for (int x = 0; x < 64; x++) {
			float d = 0.0;
			for (int c = 0; c < 4; c++) {
				const Vector3 dir = inv.xform(Vector3(2.0f * (x + (c & 1)) / 64 - 1.0f, 2.0f * (y + (c >> 1)) / 36 - 1.0f, -1.0f)).normalized();
				d = MAX(d, p_wall / -dir.z);
			}
			s.distances[y * 64 + x] = d;
		}
	}
	return s;
}

static bool occluded(const RendererSceneHiZOcclusion::Buffer &p_buffer, const Transform3D &p_cam, const Vector3 &p_center, float p_half = 0.5) {
	const real_t bounds[6] = { p_center.x - p_half, p_center.y - p_half, p_center.z - p_half, p_center.x + p_half, p_center.y + p_half, p_center.z + p_half };
	uint64_t timeout = 0;
	return p_buffer.is_occluded(bounds, p_cam.origin, p_cam.affine_inverse(), test_projection(), 0.05, false, timeout);
}

TEST_CASE("[Mariner][E13] Viewport and RenderingServer API") {
	PropertyInfo pi;
	CHECK(ClassDB::get_property_info("Viewport", "use_hiz_occlusion_culling", &pi));
	CHECK(pi.type == Variant::BOOL);
	CHECK(ClassDB::get_property_info("Viewport", "hiz_occlusion_reference_radius", &pi));
	CHECK(pi.type == Variant::FLOAT);
	CHECK(ClassDB::get_property_info("Viewport", "hiz_occlusion_reference", &pi));
	CHECK(pi.type == Variant::TRANSFORM3D);
	MethodBind *m = ClassDB::get_method("RenderingServer", "viewport_set_use_hiz_occlusion_culling");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);
	m = ClassDB::get_method("RenderingServer", "viewport_set_hiz_occlusion_reference");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 3);
	CHECK(m->get_argument_type(1) == Variant::TRANSFORM3D);
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/occlusion_culling/hiz_buffer_width"));
	CHECK(int(GLOBAL_GET("rendering/occlusion_culling/hiz_buffer_width")) == 256);
}

TEST_CASE("[Mariner][E13] Reprojection: same camera, sky, invalid") {
	const Transform3D cam;
	RendererSceneHiZOcclusion::Buffer buffer;
	RendererSceneHiZOcclusion::Snapshot s = wall_snapshot(cam, 10.0);
	REQUIRE(buffer.reproject(s, cam, test_projection(), Transform3D(), 0.0));
	CHECK(buffer.last_covered == 64 * 36);
	CHECK(occluded(buffer, cam, Vector3(0, 0, -20)));
	CHECK(occluded(buffer, cam, Vector3(-8, 3, -30)));
	CHECK_FALSE(occluded(buffer, cam, Vector3(0, 0, -5))); // In front of the wall.
	CHECK_FALSE(occluded(buffer, cam, Vector3(0, 0, -10.2), 0.5)); // Through the wall.

	// The right half sees the sky: no occluder there.
	for (int y = 0; y < 36; y++) {
		for (int x = 32; x < 64; x++) {
			s.distances[y * 64 + x] = RendererSceneHiZOcclusion::NO_SURFACE;
		}
	}
	REQUIRE(buffer.reproject(s, cam, test_projection(), Transform3D(), 0.0));
	CHECK(buffer.last_covered == 32 * 36);
	CHECK(occluded(buffer, cam, Vector3(-6, 0, -20)));
	CHECK_FALSE(occluded(buffer, cam, Vector3(6, 0, -20)));

	s.valid = false;
	CHECK_FALSE(buffer.reproject(s, cam, test_projection(), Transform3D(), 0.0));
	CHECK_FALSE(occluded(buffer, cam, Vector3(0, 0, -20)));
}

TEST_CASE("[Mariner][E13] Reprojection: moved camera, revealed parts stay unknown") {
	const Transform3D old_cam;
	const Transform3D new_cam(Basis(), Vector3(4, 0, 0));
	RendererSceneHiZOcclusion::Buffer buffer;
	REQUIRE(buffer.reproject(wall_snapshot(old_cam, 10.0), new_cam, test_projection(), Transform3D(), 0.0));
	// The old view covered x in [-12.4, 12.4] on the wall; the new one sees up to x = 16.4: a strip on the right is new.
	CHECK(buffer.last_covered < 64 * 36);
	CHECK(buffer.last_covered > 64 * 36 * 3 / 4);
	CHECK(occluded(buffer, new_cam, Vector3(4, 0, -20)));
	CHECK(occluded(buffer, new_cam, Vector3(-10, 0, -20)));
	CHECK_FALSE(occluded(buffer, new_cam, Vector3(4 + 23, 0, -20))); // Behind the revealed strip (x = 15.5 on the wall).
	CHECK_FALSE(occluded(buffer, new_cam, Vector3(4, 0, -8))); // In front.

	// Turned around: nothing of the old view is in front.
	const Transform3D turned(Basis(Vector3(0, 1, 0), Math::PI), Vector3());
	REQUIRE(buffer.reproject(wall_snapshot(old_cam, 10.0), turned, test_projection(), Transform3D(), 0.0));
	CHECK(buffer.last_covered == 0);
	CHECK_FALSE(occluded(buffer, turned, turned.xform(Vector3(0, 0, -20))));
}

TEST_CASE("[Mariner][E13] Reprojection: geometry near the camera moves with the reference frame") {
	// The camera is on a ship that moved 4 m along +X since the snapshot; the wall is part of the ship (10 m away).
	const Transform3D old_ship;
	const Transform3D new_ship(Basis(), Vector3(4, 0, 0));
	const Transform3D old_cam;
	const Transform3D new_cam = new_ship * old_cam;
	RendererSceneHiZOcclusion::Buffer buffer;

	// With the reference frame (radius 50 m): the wall is where it was relative to the ship, all cells covered.
	REQUIRE(buffer.reproject(wall_snapshot(old_cam, 10.0, old_ship, 50.0), new_cam, test_projection(), new_ship, 50.0));
	CHECK(buffer.last_covered == 64 * 36);
	CHECK(occluded(buffer, new_cam, new_ship.xform(Vector3(11 * 2, 0, -20))));

	// Without it the wall would stay in the world: the right strip is unknown and the object there visible.
	REQUIRE(buffer.reproject(wall_snapshot(old_cam, 10.0), new_cam, test_projection(), new_ship, 0.0));
	CHECK_FALSE(occluded(buffer, new_cam, new_ship.xform(Vector3(11 * 2, 0, -20))));

	// Beyond the radius the world frame applies: a wall 100 m away does not move with the ship.
	REQUIRE(buffer.reproject(wall_snapshot(old_cam, 100.0, old_ship, 50.0), new_cam, test_projection(), new_ship, 50.0));
	CHECK(buffer.last_covered < 64 * 36);
}

} // namespace TestMarinerHiZ
