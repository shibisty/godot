/**************************************************************************/
/*  test_mariner_g4.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g4)

#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G4: wet materials - the wetness of the world (Environment.wetness, the fragment built-in WETNESS)
// and BaseMaterial3D.wetness_*: porous surfaces darken, a film of water smooths them, puddles stand where the height map
// is low on flat ground; nothing under cover (OVERHEAD_COVER, G1b). Images on lavapipe: engine/tests/g/wetness.gd.

namespace TestMarinerG4 {

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

TEST_CASE("[Mariner][G4] Environment.wetness and WETNESS") {
	Ref<Environment> env;
	env.instantiate();
	CHECK(env->get_wetness() == doctest::Approx(0.0));
	env->set_wetness(0.7);
	CHECK(env->get_wetness() == doctest::Approx(0.7));
	env->set_wetness(2.0);
	CHECK(env->get_wetness() == doctest::Approx(1.0));
	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_wetness");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);

	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("WETNESS"));
	CHECK(fragment.built_ins["WETNESS"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK(fragment.built_ins["WETNESS"].constant);
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { ROUGHNESS = mix(0.8, 0.1, WETNESS); }\n", &error) == OK, error);
}

TEST_CASE("[Mariner][G4] BaseMaterial3D wetness") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_wetness_amount() == doctest::Approx(1.0));
	CHECK(material->get_wetness_porosity() == doctest::Approx(0.5));
	CHECK(material->get_wetness_puddles() == doctest::Approx(0.0));
	CHECK_FALSE(material_code(material).contains("WETNESS"));

	material->set_feature(BaseMaterial3D::FEATURE_WETNESS, true);
	String code = material_code(material);
	CHECK(code.contains("WETNESS * wetness_amount * (1.0 - OVERHEAD_COVER)"));
	CHECK_FALSE(code.contains("float puddle")); // No height map: no puddles.
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// With a height map (and a normal map): puddles flatten the normal.
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	code = material_code(material);
	CHECK(code.contains("float puddle"));
	CHECK(code.contains("NORMAL_MAP = mix(NORMAL_MAP, vec3(0.5, 0.5, 1.0), puddle)"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
	material->set_flag(BaseMaterial3D::FLAG_INVERT_HEIGHTMAP, true);
	CHECK(material_code(material).contains("1.0 - texture(texture_heightmap, base_uv).r"));

	// Unshaded: nothing to wet.
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	CHECK_FALSE(material_code(material).contains("WETNESS"));

	material->set_wetness_porosity(3.0);
	CHECK(material->get_wetness_porosity() == doctest::Approx(1.0));
	CHECK(bool(material->get("wetness_enabled")));
}

} // namespace TestMarinerG4
