/**************************************************************************/
/*  test_mariner_g7b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g7b)

#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

#include "modules/modules_enabled.gen.h" // For visual_shader.

#ifdef MODULE_VISUAL_SHADER_ENABLED
#include "modules/visual_shader/visual_shader.h"
#include "modules/visual_shader/vs_nodes/visual_shader_nodes.h"
#endif

// Mariner engine patch G7b: the fork's shader features in VisualShader - the Displace (vertex) and HeightNormal
// (fragment) nodes of G7, the REFLECTION output of G6, the WETNESS (G4) and SNOW (G4b) inputs.

namespace TestMarinerG7b {

#ifdef MODULE_VISUAL_SHADER_ENABLED
static Error compile_spatial(const String &p_code, String *r_error) {
	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	ERR_PRINT_OFF;
	const Error err = sl.compile(p_code, info);
	ERR_PRINT_ON;
	*r_error = sl.get_error_text();
	return err;
}

static int output_port(VisualShader::Type p_type, const String &p_name) {
	Ref<VisualShaderNodeOutput> output;
	output.instantiate();
	output->shader_mode = Shader::MODE_SPATIAL;
	output->shader_type = p_type;
	for (int i = 0; i < output->get_input_port_count(); i++) {
		if (output->get_input_port_name(i) == p_name) {
			return i;
		}
	}
	return -1;
}

TEST_CASE("[Mariner][G7b] VisualShader inputs, REFLECTION output") {
	Ref<VisualShaderNodeInput> input;
	input.instantiate();
	input->set_shader_mode(Shader::MODE_SPATIAL);
	input->set_shader_type(VisualShader::TYPE_FRAGMENT);
	input->set_input_name("wetness");
	CHECK(input->get_input_real_name() == "WETNESS");
	CHECK(input->get_input_type_by_name("wetness") == VisualShaderNode::PORT_TYPE_SCALAR);
	input->set_input_name("snow");
	CHECK(input->get_input_real_name() == "SNOW");

	const int reflection = output_port(VisualShader::TYPE_FRAGMENT, "Reflection");
	REQUIRE(reflection >= 0);
	// Appended after the existing ports (resources store the port index).
	CHECK(reflection > output_port(VisualShader::TYPE_FRAGMENT, "Iridescence Thickness"));
}

TEST_CASE("[Mariner][G7b] VisualShader Displace and HeightNormal") {
	Ref<VisualShader> vs;
	vs.instantiate();
	vs->set_mode(Shader::MODE_SPATIAL);

	// Vertex: the height map displaces the vertex.
	Ref<VisualShaderNodeTexture2DParameter> vtex;
	vtex.instantiate();
	vtex->set_parameter_name("height_v");
	Ref<VisualShaderNodeDisplace> displace;
	displace.instantiate();
	CHECK(displace->get_input_port_type(0) == VisualShaderNode::PORT_TYPE_SAMPLER);
	CHECK(displace->is_input_port_default(1, Shader::MODE_SPATIAL));
	vs->add_node(VisualShader::TYPE_VERTEX, vtex, Vector2(), 2);
	vs->add_node(VisualShader::TYPE_VERTEX, displace, Vector2(), 3);
	vs->connect_nodes(VisualShader::TYPE_VERTEX, 2, 0, 3, 0);
	vs->connect_nodes(VisualShader::TYPE_VERTEX, 3, 0, VisualShader::NODE_ID_OUTPUT, output_port(VisualShader::TYPE_VERTEX, "Vertex"));

	// Fragment: the relief's normal, the world's wetness and snow, a reflection.
	Ref<VisualShaderNodeTexture2DParameter> ftex;
	ftex.instantiate();
	ftex->set_parameter_name("height_f");
	Ref<VisualShaderNodeHeightNormal> normal;
	normal.instantiate();
	vs->add_node(VisualShader::TYPE_FRAGMENT, ftex, Vector2(), 2);
	vs->add_node(VisualShader::TYPE_FRAGMENT, normal, Vector2(), 3);
	vs->connect_nodes(VisualShader::TYPE_FRAGMENT, 2, 0, 3, 0);
	vs->connect_nodes(VisualShader::TYPE_FRAGMENT, 3, 0, VisualShader::NODE_ID_OUTPUT, output_port(VisualShader::TYPE_FRAGMENT, "Normal"));
	Ref<VisualShaderNodeInput> snow;
	snow.instantiate();
	snow->set_input_name("snow");
	vs->add_node(VisualShader::TYPE_FRAGMENT, snow, Vector2(), 4);
	vs->connect_nodes(VisualShader::TYPE_FRAGMENT, 4, 0, VisualShader::NODE_ID_OUTPUT, output_port(VisualShader::TYPE_FRAGMENT, "Roughness"));
	Ref<VisualShaderNodeInput> wetness;
	wetness.instantiate();
	wetness->set_input_name("wetness");
	vs->add_node(VisualShader::TYPE_FRAGMENT, wetness, Vector2(), 5);
	vs->connect_nodes(VisualShader::TYPE_FRAGMENT, 5, 0, VisualShader::NODE_ID_OUTPUT, output_port(VisualShader::TYPE_FRAGMENT, "Metallic"));
	Ref<VisualShaderNodeVec4Constant> mirror;
	mirror.instantiate();
	mirror->set_constant(Quaternion(1, 0, 0, 0.5));
	vs->add_node(VisualShader::TYPE_FRAGMENT, mirror, Vector2(), 6);
	vs->connect_nodes(VisualShader::TYPE_FRAGMENT, 6, 0, VisualShader::NODE_ID_OUTPUT, output_port(VisualShader::TYPE_FRAGMENT, "Reflection"));

	const String code = vs->get_code();
	CHECK(code.contains("VERTEX + normalize(NORMAL) * ((textureLod(height_v,"));
	CHECK(code.contains("textureSize(height_f, 0)"));
	CHECK(code.contains("SNOW"));
	CHECK(code.contains("WETNESS"));
	CHECK(code.contains("REFLECTION ="));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, String(error + "\n" + code));

	// Nothing connected to the height map: the vertex and the normal as they are.
	vs->disconnect_nodes(VisualShader::TYPE_VERTEX, 2, 0, 3, 0);
	vs->disconnect_nodes(VisualShader::TYPE_FRAGMENT, 2, 0, 3, 0);
	const String bare = vs->get_code();
	CHECK_FALSE(bare.contains("textureLod(height_v"));
	CHECK_FALSE(bare.contains("textureSize(height_f"));
	CHECK_MESSAGE(compile_spatial(bare, &error) == OK, error);
}
#endif

} // namespace TestMarinerG7b
