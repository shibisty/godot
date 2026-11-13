/**************************************************************************/
/*  test_mariner_f3c.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_f3c)

#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch F3c: channel mixer, contrast, vibrance and curves of the color grading
// (Environment.color_grading_*; images on lavapipe: engine/tests/f/color_grading.gd).

namespace TestMarinerF3c {

TEST_CASE("[Mariner][F3c] Color grading mixer, contrast, vibrance, curves") {
	Ref<Environment> env;
	env.instantiate();
	CHECK(env->get_color_grading_mixer_red() == Vector3(1, 0, 0));
	CHECK(env->get_color_grading_mixer_green() == Vector3(0, 1, 0));
	CHECK(env->get_color_grading_mixer_blue() == Vector3(0, 0, 1));
	CHECK(env->get_color_grading_contrast() == doctest::Approx(1.0));
	CHECK(env->get_color_grading_vibrance() == doctest::Approx(1.0));
	CHECK(env->get_color_grading_curves_shadow_gamma() == Vector3(1, 1, 1));
	CHECK(env->get_color_grading_curves_mid_point().is_equal_approx(Vector3(0.18, 0.18, 0.18)));
	CHECK(env->get_color_grading_curves_highlight_scale() == Vector3(1, 1, 1));

	// Limits: no negative contrast / vibrance / scale, gamma and mid point above zero.
	env->set_color_grading_contrast(-1.0);
	CHECK(env->get_color_grading_contrast() == doctest::Approx(0.0));
	env->set_color_grading_vibrance(-2.0);
	CHECK(env->get_color_grading_vibrance() == doctest::Approx(0.0));
	env->set_color_grading_curves_shadow_gamma(Vector3(0, 2, -1));
	CHECK(env->get_color_grading_curves_shadow_gamma().is_equal_approx(Vector3(0.01, 2, 0.01)));
	env->set_color_grading_curves_mid_point(Vector3(0, 0.5, 0.18));
	CHECK(env->get_color_grading_curves_mid_point().is_equal_approx(Vector3(1e-4, 0.5, 0.18)));
	env->set_color_grading_curves_highlight_scale(Vector3(-1, 0.5, 2));
	CHECK(env->get_color_grading_curves_highlight_scale().is_equal_approx(Vector3(0, 0.5, 2)));
	env->set("color_grading_mixer_red", Vector3(0.5, 0.25, 0.25));
	CHECK(env->get_color_grading_mixer_red() == Vector3(0.5, 0.25, 0.25));

	CHECK(ClassDB::has_method("RenderingServer", "environment_set_color_grading_channel_mixer"));
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_color_grading_curves"));
	RS::get_singleton()->environment_set_color_grading_channel_mixer(env->get_rid(), Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(1, 0, 0));
	RS::get_singleton()->environment_set_color_grading_curves(env->get_rid(), 1.2, 1.5, Vector3(1.2, 1, 1), Vector3(0.18, 0.18, 0.18), Vector3(0.8, 1, 1));
}

} // namespace TestMarinerF3c
