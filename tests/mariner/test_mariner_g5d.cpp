/**************************************************************************/
/*  test_mariner_g5d.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g5d)

#include "core/object/class_db.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G5d: curtains and blinds in the fake interiors (BaseMaterial3D.interior_curtains / _blinds /
// _curtain_color). Images on lavapipe: engine/tests/g/interior.gd.

namespace TestMarinerG5d {

TEST_CASE("[Mariner][G5d] BaseMaterial3D interior curtains and blinds") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_interior_curtains() == doctest::Approx(0.0));
	CHECK(material->get_interior_blinds() == doctest::Approx(0.0));
	CHECK(material->get_interior_curtain_color().is_equal_approx(Color(0.75, 0.7, 0.6)));
	material->set_interior_curtains(1.5);
	CHECK(material->get_interior_curtains() == doctest::Approx(1.0));
	material->set_interior_blinds(-1.0);
	CHECK(material->get_interior_blinds() == doctest::Approx(0.0));

	material->set_feature(BaseMaterial3D::FEATURE_INTERIOR, true);
	BaseMaterial3D::flush_changes();
	const String code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
	CHECK(code.contains("uniform float interior_curtains"));
	CHECK(code.contains("if (interior_r3 < interior_curtains + interior_blinds)"));
	CHECK(code.contains("interior_light = mix(interior_light, interior_curtain, interior_cover);"));

	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	ERR_PRINT_OFF;
	const Error err = sl.compile(code, info);
	ERR_PRINT_ON;
	CHECK_MESSAGE(err == OK, sl.get_error_text());
	CHECK(ClassDB::has_property("BaseMaterial3D", "interior_curtain_color"));
}

} // namespace TestMarinerG5d
