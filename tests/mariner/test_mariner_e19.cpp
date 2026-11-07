/**************************************************************************/
/*  test_mariner_e19.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e19)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E19: dynamic resolution (Viewport.dynamic_resolution_*, project settings
// rendering/scaling_3d/dynamic_resolution/*). Here: the controller that picks the next 3D scale from the averaged GPU
// time, the properties and the server calls; the loop with real GPU times on lavapipe by engine/tests/e19/drs.gd.

namespace TestMarinerE19 {

static float next(float p_scale, float p_gpu, float p_target, float p_min = 0.5f, float p_max = 1.0f) {
	return RendererViewport::dynamic_resolution_next_scale(p_scale, p_gpu, p_target, p_min, p_max);
}

TEST_CASE("[Mariner][E19] Dynamic resolution controller") {
	// On target, or in the band between 85 % and 105 % of it: no change.
	CHECK(next(1.0f, 16.0f, 16.0f) == doctest::Approx(1.0f));
	CHECK(next(0.75f, 14.0f, 16.0f) == doctest::Approx(0.75f));
	CHECK(next(0.75f, 16.7f, 16.0f) == doctest::Approx(0.75f));
	// Twice the target: the pixel count halves, 1 / sqrt(2) = 0.707 -> the 0.70 level.
	CHECK(next(1.0f, 32.0f, 16.0f) == doctest::Approx(0.70f));
	// Just over the target: one level down at least.
	CHECK(next(1.0f, 17.0f, 16.0f) == doctest::Approx(0.95f));
	CHECK(next(0.8f, 17.0f, 16.0f) == doctest::Approx(0.75f));
	// Far over: the minimum, never below.
	CHECK(next(1.0f, 160.0f, 16.0f) == doctest::Approx(0.5f));
	CHECK(next(0.5f, 160.0f, 16.0f) == doctest::Approx(0.5f));
	// Headroom: up to the level whose predicted time stays under 90 % of the target (0.5 * sqrt(0.9 / 0.25) = 0.949).
	CHECK(next(0.5f, 4.0f, 16.0f) == doctest::Approx(0.90f));
	// Up to the maximum (the viewport's own scaling_3d_scale), also off the step grid.
	CHECK(next(0.9f, 1.0f, 16.0f) == doctest::Approx(1.0f));
	CHECK(next(0.6f, 1.0f, 16.0f, 0.5f, 0.83f) == doctest::Approx(0.83f));
	// A predicted time over 90 % keeps the scale even with some headroom.
	CHECK(next(0.95f, 13.5f, 16.0f) == doctest::Approx(0.95f));
	// No measurement yet: no change.
	CHECK(next(0.7f, 0.0f, 16.0f) == doctest::Approx(0.7f));
}

TEST_CASE("[Mariner][E19] Viewport dynamic resolution properties") {
	ERR_PRINT_OFF; // No physics server in this test run (the viewport's 2D world asks for one).
	SubViewport *vp = memnew(SubViewport);
	ERR_PRINT_ON;
	CHECK_FALSE(vp->is_dynamic_resolution_enabled());
	CHECK(vp->get_dynamic_resolution_target_ms() == doctest::Approx(16.6667f));
	CHECK(vp->get_dynamic_resolution_min_scale() == doctest::Approx(0.5f));
	vp->set_scaling_3d_scale(0.8f);
	CHECK(vp->get_dynamic_resolution_scale() == doctest::Approx(0.8f));
	vp->set_dynamic_resolution_target_ms(8.0f);
	vp->set_dynamic_resolution_min_scale(0.05f);
	CHECK(vp->get_dynamic_resolution_min_scale() == doctest::Approx(0.1f));
	vp->set_dynamic_resolution_enabled(true);
	CHECK(vp->is_dynamic_resolution_enabled());
	// Before any GPU time the scale starts at the viewport's own scale.
	CHECK(vp->get_dynamic_resolution_scale() == doctest::Approx(0.8f));
	CHECK(ClassDB::has_method("RenderingServer", "viewport_set_dynamic_resolution"));
	CHECK(ClassDB::has_method("RenderingServer", "viewport_get_dynamic_resolution_scale"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/scaling_3d/dynamic_resolution/enabled"));
	ERR_PRINT_OFF;
	memdelete(vp);
	ERR_PRINT_ON;
}

} // namespace TestMarinerE19
