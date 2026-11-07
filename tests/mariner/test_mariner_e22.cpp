/**************************************************************************/
/*  test_mariner_e22.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e22)

#include "core/object/class_db.h"
#include "scene/3d/gpu_particles_3d.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E22: screen space collision of GPU particles (GPUParticles3D.collision_screen_space), on the
// depth of the largest camera view of the previous frame. Here: the property and the server call; the particles
// (rain on a deck and a box, with and without the flag) on lavapipe by engine/tests/e22/particles_depth.gd.

namespace TestMarinerE22 {

TEST_CASE("[Mariner][E22] GPUParticles3D.collision_screen_space") {
	GPUParticles3D *p = memnew(GPUParticles3D);
	CHECK_FALSE(p->is_screen_space_collision());
	p->set_screen_space_collision(true);
	CHECK(p->is_screen_space_collision());
	CHECK(bool(p->get("collision_screen_space")));
	p->set_screen_space_collision(false);
	CHECK_FALSE(p->is_screen_space_collision());
	memdelete(p);
	CHECK(ClassDB::has_method("RenderingServer", "particles_set_screen_space_collision"));
}

} // namespace TestMarinerE22
