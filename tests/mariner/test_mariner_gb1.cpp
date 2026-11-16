/**************************************************************************/
/*  test_mariner_gb1.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_gb1)

#include "scene/resources/compositor.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patches GB1 / GB2: the thin G-buffer from the Forward+ depth prepass (CompositorEffect.needs_gbuffer):
// rgb the albedo (square root), a the surface class / 255 (BaseMaterial3D.surface_class, SURFACE_CLASS); surface decals
// (render_mode surface_decal) drawn into the decal buffer after the prepass and applied by the opaque pass. Images and
// checks on lavapipe: engine/tests/gb/gbuffer.gd, engine/tests/gb/decals.gd.

namespace TestMarinerGB1 {

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

TEST_CASE("[Mariner][GB1] SURFACE_CLASS") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("SURFACE_CLASS"));
	CHECK(fragment.built_ins["SURFACE_CLASS"].type == ShaderLanguage::TYPE_INT);
	CHECK_FALSE(fragment.built_ins["SURFACE_CLASS"].constant); // Written by the shader.
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() {\n\tSURFACE_CLASS = 200;\n}\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { SURFACE_CLASS = 1; }\n", &error) != OK); // Fragment only.
}

TEST_CASE("[Mariner][GB1] BaseMaterial3D.surface_class") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_surface_class() == 0);
	String code = material_code(material);
	CHECK_FALSE(code.contains("SURFACE_CLASS")); // Generic: nothing in the code.
	material->set_surface_class(7);
	code = material_code(material);
	CHECK(code.contains("uniform int surface_class_id"));
	CHECK(code.contains("SURFACE_CLASS = surface_class_id;"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
	// Another class: the same shader, a new value of the uniform (checked on the GPU: gbuffer.gd).
	const RID shader = material->get_shader_rid();
	material->set_surface_class(42);
	material_code(material);
	CHECK(material->get_shader_rid() == shader);
	material->set_surface_class(300);
	CHECK(material->get_surface_class() == 255);
	material->set_surface_class(0);
	CHECK_FALSE(material_code(material).contains("SURFACE_CLASS"));
}

TEST_CASE("[Mariner][GB1] CompositorEffect.needs_gbuffer") {
	CHECK(int(RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_GBUFFER) == 32);
	Ref<CompositorEffect> effect;
	effect.instantiate();
	CHECK_FALSE(effect->get_needs_gbuffer());
	effect->set_needs_gbuffer(true);
	CHECK(effect->get_needs_gbuffer());
}

TEST_CASE("[Mariner][GB2] render_mode surface_decal and its built-ins") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	// What the decal reads (the surface under it) - read only.
	const char *inputs[] = { "DECAL_POSITION", "DECAL_SURFACE_CLASS", "DECAL_SURFACE_ALBEDO", "DECAL_SURFACE_NORMAL" };
	const ShaderLanguage::DataType input_types[] = { ShaderLanguage::TYPE_VEC3, ShaderLanguage::TYPE_INT, ShaderLanguage::TYPE_VEC3, ShaderLanguage::TYPE_VEC3 };
	for (int i = 0; i < 4; i++) {
		REQUIRE_MESSAGE(fragment.built_ins.has(inputs[i]), inputs[i]);
		CHECK(fragment.built_ins[inputs[i]].type == input_types[i]);
		CHECK(fragment.built_ins[inputs[i]].constant);
	}
	// What it adds - written.
	const char *outputs[] = { "DECAL_HEIGHT", "DECAL_WETNESS", "DECAL_ROUGHNESS" };
	for (int i = 0; i < 3; i++) {
		REQUIRE_MESSAGE(fragment.built_ins.has(outputs[i]), outputs[i]);
		CHECK(fragment.built_ins[outputs[i]].type == ShaderLanguage::TYPE_FLOAT);
		CHECK_FALSE(fragment.built_ins[outputs[i]].constant);
	}
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nrender_mode surface_decal;\nvoid fragment() {\n\tfloat dip = 1.0 - smoothstep(0.0, 0.5, length(DECAL_POSITION.xz));\n\tif (DECAL_SURFACE_CLASS == 3) {\n\t\tDECAL_HEIGHT = -0.1 * dip;\n\t\tDECAL_WETNESS = dip;\n\t}\n\tDECAL_ROUGHNESS = -0.2 * dip;\n\tALBEDO = DECAL_SURFACE_ALBEDO * 0.5;\n\tALPHA = dip * max(DECAL_SURFACE_NORMAL.y, 0.0);\n}\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid fragment() { DECAL_SURFACE_CLASS = 1; }\n", &error) != OK); // Read only.
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { DECAL_HEIGHT = 1.0; }\n", &error) != OK); // Fragment only.
}

} // namespace TestMarinerGB1
