/**************************************************************************/
/*  test_mariner_g4b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g4b)

#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G4b: snow - the snow on the world (Environment.snow, the fragment built-in SNOW) and
// BaseMaterial3D.snow_*: white, rough snow on what faces up (with the normal map's detail), up to the steepest slope it
// rests on, the low places of the height map first; nothing under cover (OVERHEAD_COVER, G1b). WETNESS and SNOW also in
// the Compatibility renderer. Images on lavapipe: engine/tests/g/snow.gd.

namespace TestMarinerG4b {

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

static String material_code(const Ref<BaseMaterial3D> &p_material) {
	BaseMaterial3D::flush_changes();
	return RS::get_singleton()->shader_get_code(p_material->get_shader_rid());
}

TEST_CASE("[Mariner][G4b] Environment.snow and SNOW") {
	Ref<Environment> env;
	env.instantiate();
	CHECK(env->get_snow() == doctest::Approx(0.0));
	env->set_snow(0.4);
	CHECK(env->get_snow() == doctest::Approx(0.4));
	env->set_snow(-1.0);
	CHECK(env->get_snow() == doctest::Approx(0.0));
	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_snow");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);

	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("SNOW"));
	CHECK(fragment.built_ins["SNOW"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK(fragment.built_ins["SNOW"].constant);
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(SNOW + WETNESS); }\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { VERTEX.y += SNOW; }\n", &error) != OK); // Fragment only.
}

TEST_CASE("[Mariner][G4b] BaseMaterial3D snow") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_snow_amount() == doctest::Approx(1.0));
	CHECK(material->get_snow_roughness() == doctest::Approx(0.8));
	CHECK(material->get_snow_max_slope() == doctest::Approx(55.0));
	CHECK(material->get_snow_relief() == doctest::Approx(0.5));
	CHECK(material->get_snow_color().is_equal_approx(Color(0.9, 0.92, 0.95)));
	CHECK_FALSE(material_code(material).contains("SNOW"));

	material->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
	String code = material_code(material);
	CHECK(code.contains("SNOW * snow_amount * (1.0 - OVERHEAD_COVER)"));
	CHECK(code.contains("ALBEDO = mix(ALBEDO, snow_color.rgb, snow_cover);"));
	CHECK_FALSE(code.contains("snow_nm")); // No normal map: the surface's normal.
	CHECK_FALSE(code.contains("snow_height")); // No height map.
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// With the normal map: its detail decides what faces up, the snow smooths it; with a height map, the relief.
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	code = material_code(material);
	CHECK(code.contains("TANGENT * snow_nm.x + BINORMAL * snow_nm.y + NORMAL * snow_nm.z"));
	CHECK(code.contains("NORMAL_MAP = mix(NORMAL_MAP, vec3(0.5, 0.5, 1.0), snow_cover * 0.7);"));
	CHECK(code.contains("snow_depth += (0.5 - snow_height) * snow_relief"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
	// The snow comes after the normal map is read and after the wetness (it covers the wet surface).
	material->set_feature(BaseMaterial3D::FEATURE_WETNESS, true);
	code = material_code(material);
	CHECK(code.find("NORMAL_MAP = texture(texture_normal") < code.find("// Snow: Enabled"));
	CHECK(code.find("// Wetness: Enabled") < code.find("// Snow: Enabled"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// Unshaded: still white (the color is what an unshaded surface shows).
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	code = material_code(material);
	CHECK(code.contains("ALBEDO = mix(ALBEDO, snow_color.rgb, snow_cover);"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	material->set_snow_max_slope(120.0);
	CHECK(material->get_snow_max_slope() == doctest::Approx(90.0));
	CHECK(bool(material->get("snow_enabled")));
}

} // namespace TestMarinerG4b
