/**************************************************************************/
/*  test_mariner_f.cpp                                                    */
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

TEST_FORCE_LINK(test_mariner_f)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/effects/tone_mapper.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patches F1 (GTAO with a visibility bitmask, Environment.ssao_gtao_thickness), F3 (color grading,
// Environment.color_grading_*) and M1-M3 (multiple-scattering diffuse, micro-shadowing, multi-bounce occlusion; project
// settings rendering/shading/overrides/*). The images on lavapipe: engine/tests/f/*.gd.

namespace TestMarinerF {

TEST_CASE("[Mariner][F1] GTAO thickness property") {
	Ref<Environment> env;
	env.instantiate();
	CHECK(env->get_ssao_gtao_thickness() == doctest::Approx(0.5));
	env->set_ssao_gtao_thickness(1.25);
	CHECK(float(env->get("ssao_gtao_thickness")) == doctest::Approx(1.25));
	env->set_ssao_gtao_thickness(-1.0);
	CHECK(env->get_ssao_gtao_thickness() == doctest::Approx(0.0));
	// The server call keeps its old three-argument form (thickness has a default).
	RS::get_singleton()->environment_set_ssao_gtao(env->get_rid(), true, true);
	RS::get_singleton()->environment_set_ssao_gtao(env->get_rid(), true, true, 0.3);
}

TEST_CASE("[Mariner][F3] Color grading properties") {
	Ref<Environment> env;
	env.instantiate();
	CHECK_FALSE(env->is_color_grading_enabled());
	CHECK(env->get_color_grading_temperature() == doctest::Approx(6500.0));
	CHECK(env->is_color_grading_gamut_mapping());
	CHECK(env->get_color_grading_tonal_ranges().is_equal_approx(Vector4(0.0, 0.333, 0.55, 1.0)));
	env->set_color_grading_enabled(true);
	env->set_color_grading_temperature(100.0);
	CHECK(env->get_color_grading_temperature() == doctest::Approx(1667.0));
	env->set_color_grading_tint(3.0);
	CHECK(env->get_color_grading_tint() == doctest::Approx(1.0));
	env->set_color_grading_slope(Vector3(1.1, 1.0, 0.9));
	CHECK(Vector3(env->get("color_grading_slope")).is_equal_approx(Vector3(1.1, 1.0, 0.9)));
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_color_grading"));
	RS::get_singleton()->environment_set_color_grading(env->get_rid(), true, 5000.0, 0.1, Color(1, 1, 1), Color(1, 1, 1), Color(1, 1, 1), Vector4(0, 0.3, 0.6, 1), Vector3(1, 1, 1), Vector3(), Vector3(1, 1, 1), 1.0, true);
}

TEST_CASE("[Mariner][F3] White balance matrix") {
	// 6500 K without tint: no change.
	CHECK(RendererRD::ToneMapper::white_balance_matrix(6500.0, 0.0).is_equal_approx(Basis()));
	const Vector3 luminance(0.2126, 0.7152, 0.0722);
	// The white balance of a camera: set to tungsten (3200 K), daylight white turns blue; to 10000 K, it turns warm.
	const Vector3 cold = RendererRD::ToneMapper::white_balance_matrix(3200.0, 0.0).xform(Vector3(1, 1, 1));
	CHECK(cold.z > cold.x * 1.3);
	CHECK(cold.dot(luminance) == doctest::Approx(1.0).epsilon(0.001)); // White keeps its luminance.
	const Vector3 warm = RendererRD::ToneMapper::white_balance_matrix(10000.0, 0.0).xform(Vector3(1, 1, 1));
	CHECK(warm.x > warm.z);
	// A white point near 6500 K hardly changes anything.
	const Vector3 near_white = RendererRD::ToneMapper::white_balance_matrix(6600.0, 0.0).xform(Vector3(1, 1, 1));
	CHECK(near_white.is_equal_approx(Vector3(1, 1, 1)) == false);
	CHECK(near_white.distance_to(Vector3(1, 1, 1)) < 0.05);
	// Positive tint: magenta (green down, red and blue up).
	const Vector3 magenta = RendererRD::ToneMapper::white_balance_matrix(6500.0, 0.5).xform(Vector3(1, 1, 1));
	CHECK(magenta.y < magenta.x);
	CHECK(magenta.y < magenta.z);
}

TEST_CASE("[Mariner][M1-M3] Shading overrides") {
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/shading/overrides/multiscatter_diffuse"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/shading/overrides/micro_shadowing"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/shading/overrides/multi_bounce_ao"));
	CHECK(bool(GLOBAL_GET("rendering/shading/overrides/multiscatter_diffuse")));
	CHECK(bool(GLOBAL_GET("rendering/shading/overrides/micro_shadowing")));
	CHECK(bool(GLOBAL_GET("rendering/shading/overrides/multi_bounce_ao")));
}

} // namespace TestMarinerF
