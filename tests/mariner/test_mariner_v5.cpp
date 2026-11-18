/**************************************************************************/
/*  test_mariner_v5.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_v5)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch V5: the software rasterizer of small triangles. Here: the MultiMesh property and the
// RenderingServer methods, render_mode visibility_resolve (V5-T2). The rasterization itself (visible instance per
// pixel = the hardware rasterizer's, depth test, the stats) is checked on lavapipe by engine/tests/v5/sw_raster.gd,
// the resolve (the attributes and the lit picture = the hardware path's) by engine/tests/v5/sw_resolve.gd.

namespace TestMarinerV5 {

TEST_CASE("[Mariner][V5] MultiMesh.software_raster and the RenderingServer methods") {
	Ref<MultiMesh> mm;
	mm.instantiate();
	CHECK_FALSE(mm->is_software_raster());
	mm->set_software_raster(true);
	CHECK(mm->is_software_raster());
	List<PropertyInfo> props;
	ClassDB::get_property_list("MultiMesh", &props);
	bool found = false;
	for (const PropertyInfo &p : props) {
		found = found || (p.name == "software_raster" && p.type == Variant::BOOL);
	}
	CHECK(found);
	MethodBind *set = ClassDB::get_method("RenderingServer", "multimesh_set_software_raster");
	MethodBind *get = ClassDB::get_method("RenderingServer", "multimesh_get_software_raster");
	REQUIRE(set != nullptr);
	REQUIRE(get != nullptr);
	CHECK(set->get_argument_count() == 2);
	CHECK(get->get_argument_count() == 1);
	// The dummy storage of the headless tests has no software rasterizer: false.
	CHECK_FALSE(RS::get_singleton()->multimesh_get_software_raster(mm->get_rid()));
}

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

TEST_CASE("[Mariner][V5] render_mode visibility_resolve") {
	bool found = false;
	for (const ShaderLanguage::ModeInfo &mode : ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL)) {
		found = found || mode.name == "visibility_resolve";
	}
	CHECK(found);
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nrender_mode visibility_resolve;\nvoid fragment() {\n\tALBEDO = COLOR.rgb * vec3(UV, 1.0);\n\tNORMAL_MAP = vec3(0.5, 0.5, 1.0);\n}\n", &error) == OK, error);
}

} // namespace TestMarinerV5
