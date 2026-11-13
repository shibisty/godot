/**************************************************************************/
/*  test_mariner_f3b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_f3b)

#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For visual_shader.

#ifdef MODULE_VISUAL_SHADER_ENABLED
#include "modules/visual_shader/visual_shader.h"
#endif

// Mariner engine patch F3b E20e: night vision in the color grading (Environment.color_grading_night_vision*), alpha test
// and raytracing_opacity of the ray traced shadows (images on lavapipe: engine/tests/e20/alpha_shadow.gd,
// engine/tests/f/color_grading.gd), VisualShader outputs and fixes.

namespace TestMarinerF3b {

TEST_CASE("[Mariner][F3b] Night vision properties") {
	Ref<Environment> env;
	env.instantiate();
	CHECK(env->get_color_grading_night_vision() == doctest::Approx(0.0));
	CHECK(env->get_color_grading_night_vision_start() == doctest::Approx(0.05));
	CHECK(env->get_color_grading_night_vision_end() == doctest::Approx(0.0005));
	CHECK(env->get_color_grading_night_vision_tint().is_equal_approx(Color(0.70, 0.85, 1.35)));
	env->set_color_grading_night_vision(3.0);
	CHECK(env->get_color_grading_night_vision() == doctest::Approx(1.0));
	env->set("color_grading_night_vision_start", 0.2);
	CHECK(env->get_color_grading_night_vision_start() == doctest::Approx(0.2));
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_color_grading_night_vision"));
	RS::get_singleton()->environment_set_color_grading_night_vision(env->get_rid(), 0.5, Color(0.7, 0.85, 1.35), 0.1, 0.001);
}

#ifdef MODULE_VISUAL_SHADER_ENABLED
TEST_CASE("[Mariner][F3b] VisualShader outputs SHEEN and IRIDESCENCE") {
	Ref<VisualShaderNodeOutput> output;
	output.instantiate();
	output->shader_mode = Shader::MODE_SPATIAL;
	output->shader_type = VisualShader::TYPE_FRAGMENT;
	HashMap<String, int> ports;
	for (int i = 0; i < output->get_input_port_count(); i++) {
		ports[output->get_input_port_name(i)] = i;
	}
	REQUIRE(ports.has("Sheen"));
	CHECK(output->get_input_port_type(ports["Sheen"]) == VisualShaderNode::PORT_TYPE_VECTOR_3D);
	CHECK(ports.has("Sheen Roughness"));
	CHECK(ports.has("Iridescence"));
	CHECK(ports.has("Iridescence IOR"));
	CHECK(ports.has("Iridescence Thickness"));
	// Appended after the existing ports (resources store the port index).
	CHECK(ports["Sheen"] > ports["Bent Normal Map"]);
}
#endif

} // namespace TestMarinerF3b
