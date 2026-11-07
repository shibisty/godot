/**************************************************************************/
/*  test_mariner_e16.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e16)

#include "core/object/class_db.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch E16: parallax mapping with self-shadowing and depth offset in BaseMaterial3D, on the fragment
// built-ins MAIN_LIGHT_DIRECTION / MAIN_LIGHT_SHADOW. Here: the built-ins, the properties and the generated shader
// (it parses as a spatial shader, uses the built-ins only when asked); the image (shadow in the relief, contact with
// an object on it) on lavapipe by engine/tests/e16/parallax.gd.

namespace TestMarinerE16 {

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

TEST_CASE("[Mariner][E16] Fragment built-ins MAIN_LIGHT_DIRECTION and MAIN_LIGHT_SHADOW") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("MAIN_LIGHT_DIRECTION"));
	CHECK(fragment.built_ins["MAIN_LIGHT_DIRECTION"].type == ShaderLanguage::TYPE_VEC3);
	CHECK(fragment.built_ins["MAIN_LIGHT_DIRECTION"].constant);
	REQUIRE(fragment.built_ins.has("MAIN_LIGHT_SHADOW"));
	CHECK(fragment.built_ins["MAIN_LIGHT_SHADOW"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK_FALSE(fragment.built_ins["MAIN_LIGHT_SHADOW"].constant);

	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { MAIN_LIGHT_SHADOW = clamp(MAIN_LIGHT_DIRECTION.y, 0.0, 1.0); }\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid fragment() { MAIN_LIGHT_DIRECTION = vec3(1.0); }\n", &error) != OK); // Read only.
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { float s = MAIN_LIGHT_SHADOW; }\n", &error) != OK); // Fragment only.
}

TEST_CASE("[Mariner][E16] BaseMaterial3D height self-shadow and depth offset") {
	for (const char *name : { "heightmap_self_shadow", "heightmap_self_shadow_strength", "heightmap_depth_offset" }) {
		PropertyInfo pi;
		CHECK_MESSAGE(ClassDB::get_property_info("StandardMaterial3D", name, &pi), name);
	}

	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK_FALSE(material->is_heightmap_self_shadow_enabled());
	CHECK_FALSE(material->is_heightmap_depth_offset_enabled());
	CHECK(material->get_heightmap_self_shadow_strength() == doctest::Approx(1.0));
	material->set_heightmap_self_shadow_strength(3.0);
	CHECK(material->get_heightmap_self_shadow_strength() == doctest::Approx(1.0));

	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_heightmap_deep_parallax(true);
	BaseMaterial3D::flush_changes();
	String code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
	CHECK(code.contains("texture_heightmap"));
	CHECK_FALSE(code.contains("MAIN_LIGHT_SHADOW"));
	CHECK_FALSE(code.contains("DEPTH ="));

	for (bool deep : { true, false }) {
		for (bool invert : { false, true }) {
			material->set_heightmap_deep_parallax(deep);
			material->set_flag(BaseMaterial3D::FLAG_INVERT_HEIGHTMAP, invert);
			material->set_heightmap_self_shadow(true);
			material->set_heightmap_depth_offset(true);
			BaseMaterial3D::flush_changes();
			code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
			CHECK(code.contains("MAIN_LIGHT_SHADOW ="));
			CHECK(code.contains("MAIN_LIGHT_DIRECTION"));
			CHECK(code.contains("DEPTH ="));
			CHECK(code.contains("LIGHT_VERTEX ="));
			String error;
			CHECK_MESSAGE(compile_spatial(code, &error) == OK, vformat("deep %d invert %d: %s", int(deep), int(invert), error));
		}
	}
}

} // namespace TestMarinerE16
