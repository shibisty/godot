/**************************************************************************/
/*  test_mariner_g4c.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g4c)

#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G4c: WET_FILM - the wet film (0..1) a material writes in the fragment; Forward+ smooths the
// roughness after the fragment the way a film of water does on rough ground (x0.45 within 0.06..0.25, toward the
// horizon 0.04 by Fresnel), and wet surface decals (GB2) go through the same model. Mobile and Compatibility declare
// the built-in and ignore it. Images and numbers on lavapipe: engine/tests/g/wet_film.gd.

namespace TestMarinerG4c {

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

TEST_CASE("[Mariner][G4c] WET_FILM") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("WET_FILM"));
	CHECK(fragment.built_ins["WET_FILM"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK_FALSE(fragment.built_ins["WET_FILM"].constant); // Written by the shader.
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() {\n\tROUGHNESS = 0.9;\n\tWET_FILM = 0.5;\n}\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { WET_FILM = 1.0; }\n", &error) != OK); // Fragment only.
}

} // namespace TestMarinerG4c
