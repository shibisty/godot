/**************************************************************************/
/*  test_mariner_g5e.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g5e)

#include "core/object/class_db.h"
#include "scene/3d/octahedral_impostor_baker.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_preprocessor.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G5e: octahedral impostors with depth (OctahedralImpostorBaker). The bake and the images on
// lavapipe: engine/tests/g/octahedral_impostor.gd.

namespace TestMarinerG5e {

TEST_CASE("[Mariner][G5e] OctahedralImpostorBaker octahedral map") {
	const int n = 12;
	// The full sphere: the grid's center looks from above, its corners from below.
	CHECK(OctahedralImpostorBaker::frame_direction(Vector2(5.5, 5.5), n, false).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(OctahedralImpostorBaker::frame_direction(Vector2(0, 0), n, false).is_equal_approx(Vector3(0, -1, 0)));
	CHECK(OctahedralImpostorBaker::frame_direction(Vector2(11, 5.5), n, false).is_equal_approx(Vector3(1, 0, 0)));
	// The hemisphere: the center from above, the corners on the horizon.
	CHECK(OctahedralImpostorBaker::frame_direction(Vector2(5.5, 5.5), n, true).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(Math::abs(OctahedralImpostorBaker::frame_direction(Vector2(0, 0), n, true).y) < 1e-5);
	CHECK(Math::abs(OctahedralImpostorBaker::frame_direction(Vector2(11, 0), n, true).y) < 1e-5);

	for (int hemi = 0; hemi < 2; hemi++) {
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				const Vector3 d = OctahedralImpostorBaker::frame_direction(Vector2(x, y), n, hemi);
				CHECK(d.is_normalized());
				if (hemi) {
					CHECK(d.y >= -1e-5);
				}
				const Vector2 g = OctahedralImpostorBaker::direction_to_grid(d, n, hemi);
				// The octahedron's fold (the full sphere's edges) maps both ways; the direction must come back.
				CHECK(OctahedralImpostorBaker::frame_direction(g, n, hemi).is_equal_approx(d));
				const Basis b = OctahedralImpostorBaker::frame_basis(d);
				CHECK(b.get_column(2).is_equal_approx(d));
				CHECK(b.get_column(0).cross(b.get_column(1)).is_equal_approx(d)); // Right-handed.
				CHECK(b.is_orthonormal());
			}
		}
	}
	// Below the horizon the hemisphere clamps to it.
	CHECK(OctahedralImpostorBaker::direction_to_grid(Vector3(1, -1, 0).normalized(), n, true).is_equal_approx(OctahedralImpostorBaker::direction_to_grid(Vector3(1, 0, 0), n, true)));
}

TEST_CASE("[Mariner][G5e] OctahedralImpostorBaker shader and properties") {
	Ref<OctahedralImpostorBaker> baker;
	baker.instantiate();
	CHECK(baker->get_frames() == 12);
	CHECK(baker->get_frame_size() == 128);
	baker->set_frames(100);
	CHECK(baker->get_frames() == 32);
	baker->set_frame_size(1);
	CHECK(baker->get_frame_size() == 16);
	CHECK(ClassDB::has_property("OctahedralImpostorBaker", "hemisphere"));
	ERR_PRINT_OFF;
	CHECK(baker->bake(nullptr).is_null());
	ERR_PRINT_ON;

	const String raw = OctahedralImpostorBaker::get_shader()->get_code();
	CHECK(raw.contains("#if CURRENT_RENDERER == RENDERER_COMPATIBILITY")); // The depth of OpenGL's clip space.
	String code;
	ShaderPreprocessor pp;
	REQUIRE(pp.preprocess(raw, "", code) == OK);
	CHECK(code.contains("uniform sampler2D impostor_normal_depth"));
	CHECK(code.contains("DEPTH = clip.z / clip.w;"));
	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	ERR_PRINT_OFF;
	const Error err = sl.compile(code, info);
	ERR_PRINT_ON;
	CHECK_MESSAGE(err == OK, String(sl.get_error_text() + " at line " + itos(sl.get_error_line())));
}

} // namespace TestMarinerG5e
