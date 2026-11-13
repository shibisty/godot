/**************************************************************************/
/*  test_mariner_e14c.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e14c)

#include "core/object/class_db.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch E14c: a second vec4 of custom data per MultiMesh instance - MultiMesh.use_custom_data2,
// set / get_instance_custom_data2, the vertex built-in INSTANCE_CUSTOM2; the RenderingServer's
// multimesh_allocate_data(..., custom_data2_format) and multimesh_instance_set / get_custom_data2. Images:
// engine/tests/g/custom_data2.gd.

namespace TestMarinerE14c {

TEST_CASE("[Mariner][E14c] MultiMesh.use_custom_data2") {
	Ref<MultiMesh> mm;
	mm.instantiate();
	CHECK_FALSE(mm->is_using_custom_data2());
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(true);
	mm->set_use_custom_data2(true);
	mm->set_instance_count(2);
	CHECK(mm->is_using_custom_data2());
	// Toggling it needs no instances.
	ERR_PRINT_OFF;
	mm->set_use_custom_data2(false);
	ERR_PRINT_ON;
	CHECK(mm->is_using_custom_data2());

	// The buffer: 12 + 4 + 4 floats per instance; another size is refused.
	PackedFloat32Array buffer;
	buffer.resize(2 * 20);
	mm->set("buffer", buffer);
	buffer.resize(2 * 16);
	ERR_PRINT_OFF;
	mm->set("buffer", buffer);
	ERR_PRINT_ON;

	Ref<MultiMesh> plain;
	plain.instantiate();
	plain->set_instance_count(1);
	ERR_PRINT_OFF;
	plain->set_instance_custom_data2(0, Color(1, 0, 0));
	CHECK(plain->get_instance_custom_data2(0) == Color());
	ERR_PRINT_ON;

	CHECK(ClassDB::has_property("MultiMesh", "use_custom_data2"));
	MethodBind *m = ClassDB::get_method("RenderingServer", "multimesh_allocate_data");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 7);
	CHECK(ClassDB::has_method("RenderingServer", "multimesh_instance_set_custom_data2"));
	CHECK(ClassDB::has_method("RenderingServer", "multimesh_uses_custom_data2"));
}

TEST_CASE("[Mariner][E14c] INSTANCE_CUSTOM2") {
	const ShaderLanguage::FunctionInfo &vertex = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["vertex"];
	REQUIRE(vertex.built_ins.has("INSTANCE_CUSTOM2"));
	CHECK(vertex.built_ins["INSTANCE_CUSTOM2"].type == ShaderLanguage::TYPE_VEC4);
	CHECK(vertex.built_ins["INSTANCE_CUSTOM2"].constant);

	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	ERR_PRINT_OFF;
	const Error ok = sl.compile("shader_type spatial;\nvoid vertex() { COLOR = INSTANCE_CUSTOM + INSTANCE_CUSTOM2; }\n", info);
	const Error frag = sl.compile("shader_type spatial;\nvoid fragment() { ALBEDO = INSTANCE_CUSTOM2.rgb; }\n", info);
	ERR_PRINT_ON;
	CHECK(ok == OK);
	CHECK(frag != OK); // Vertex only, as INSTANCE_CUSTOM.
}

} // namespace TestMarinerE14c
