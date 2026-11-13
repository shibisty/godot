/**************************************************************************/
/*  test_mariner_g6b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g6b)

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/planar_reflection_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

// Mariner engine patch G6b: PlanarReflection3D in the editor (a gizmo with the size's handles, the reflection's preview
// with the editor's camera - drawn only, the scene untouched) and no reflection rendered for a mirror out of view.

namespace TestMarinerG6b {

TEST_CASE("[Mariner][G6b][SceneTree] PlanarReflection3D is_in_view") {
	Window *root = SceneTree::get_singleton()->get_root();
	PlanarReflection3D *mirror = memnew(PlanarReflection3D);
	mirror->set_size(Vector2(4, 4));
	Camera3D *camera = memnew(Camera3D);
	root->add_child(mirror);
	root->add_child(camera);
	camera->set_perspective(60.0, 0.1, 100.0);

	// Looking down at the mirror from above: in view.
	camera->look_at_from_position(Vector3(0, 5, 5), Vector3());
	CHECK(mirror->is_in_view(camera));
	// Looking away from it.
	camera->look_at_from_position(Vector3(0, 5, 5), Vector3(0, 5, 20));
	CHECK_FALSE(mirror->is_in_view(camera));
	// An edge of the rectangle in view is enough: the mirror off to the side, its near corner in the frustum.
	camera->look_at_from_position(Vector3(0, 5, 5), Vector3(4.5, 0, 0));
	mirror->set_position(Vector3(6.5, 0, 0));
	CHECK(mirror->is_in_view(camera));
	mirror->set_size(Vector2(0.5, 0.5));
	camera->look_at_from_position(Vector3(0, 5, 5), Vector3(-6, 0, 0));
	CHECK_FALSE(mirror->is_in_view(camera));

	CHECK(ClassDB::has_method("PlanarReflection3D", "is_in_view"));
	memdelete(camera);
	memdelete(mirror);
}

} // namespace TestMarinerG6b
