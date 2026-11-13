/**************************************************************************/
/*  test_mariner_g5.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g5)

#include "core/object/class_db.h"
#include "scene/3d/box_impostor_baker.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G5a: fake interiors behind windows (BaseMaterial3D.interior_*): the view ray into a box room
// behind every window of the UV grid, looked up in a room image of an atlas; glass on top; lit windows at night.
// Images on lavapipe: engine/tests/g/interior.gd.
// G5b: box impostors - BoxImpostorBaker bakes a model into its bounding box with color, normals and relief per face in
// a square atlas; BaseMaterial3D.heightmap_impostor traces the true view ray into the relief, clipped to the face's
// rectangle (CUSTOM0). Images on lavapipe: engine/tests/g/box_impostor.gd.

namespace TestMarinerG5 {

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

TEST_CASE("[Mariner][G5a] BaseMaterial3D interior mapping") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_interior_rooms() == Vector2(1, 1));
	CHECK(material->get_interior_room_size() == Vector3(4, 3, 4));
	CHECK(material->get_interior_atlas_size() == Vector2i(1, 1));
	CHECK(material->get_interior_back_wall() == doctest::Approx(0.5));
	CHECK(material->get_interior_brightness() == doctest::Approx(1.0));
	CHECK(material->get_interior_lights() == doctest::Approx(0.0));
	CHECK(material->get_interior_lit_fraction() == doctest::Approx(0.3));
	CHECK_FALSE(material_code(material).contains("texture_interior"));

	material->set_feature(BaseMaterial3D::FEATURE_INTERIOR, true);
	String code = material_code(material);
	CHECK(code.contains("uniform sampler2D texture_interior : source_color, hint_default_black"));
	CHECK(code.contains("textureGrad(texture_interior"));
	CHECK(code.contains("EMISSION += interior_light"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// With emission, wetness and a height map at once.
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_feature(BaseMaterial3D::FEATURE_WETNESS, true);
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	CHECK_MESSAGE(compile_spatial(material_code(material), &error) == OK, error);

	// Unshaded: the room is the albedo.
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	code = material_code(material);
	CHECK(code.contains("ALBEDO = mix(ALBEDO, interior_light, interior_glass)"));
	CHECK_FALSE(code.contains("EMISSION += interior_light"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// Triplanar mapping has no window grid: no interior.
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	CHECK_FALSE(material_code(material).contains("textureGrad(texture_interior"));

	material->set_interior_atlas_size(Vector2i(0, 3));
	CHECK(material->get_interior_atlas_size() == Vector2i(1, 3));
	material->set_interior_back_wall(2.0);
	CHECK(material->get_interior_back_wall() == doctest::Approx(0.95));
	material->set_interior_lit_fraction(-1.0);
	CHECK(material->get_interior_lit_fraction() == doctest::Approx(0.0));
	CHECK(bool(material->get("interior_enabled")));
	CHECK(ClassDB::has_property("BaseMaterial3D", "interior_texture"));
	CHECK(ClassDB::has_property("BaseMaterial3D", "interior_mask_texture"));
}

TEST_CASE("[Mariner][G5b] BaseMaterial3D.heightmap_impostor") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK_FALSE(material->is_heightmap_impostor_enabled());
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_heightmap_deep_parallax(true);
	CHECK_FALSE(material_code(material).contains("impostor_rect"));

	material->set_heightmap_impostor(true);
	material->set_heightmap_depth_offset(true);
	String code = material_code(material);
	CHECK(code.contains("varying vec4 impostor_rect;"));
	CHECK(code.contains("impostor_rect = CUSTOM0;"));
	CHECK(code.contains("p = view_dir.xy / max(view_dir.z, 0.05)"));
	CHECK(code.contains("PROJECTION_MATRIX[3][3] == 1.0"));
	CHECK(code.contains("if (impostor_miss) {"));
	CHECK(code.contains("textureGrad(texture_heightmap, mid_ofs, impostor_dx, impostor_dy)")); // The binary search.
	CHECK(code.contains("relief_vertex = VERTEX - impostor_eye"));
	// Textures read with the face's gradients (the offset UV jumps at the silhouettes).
	CHECK(code.contains("textureGrad(texture_heightmap, ofs, impostor_dx, impostor_dy)"));
	CHECK(code.contains("textureGrad(texture_albedo, base_uv, impostor_dx, impostor_dy)"));
	CHECK_FALSE(code.contains("texture(texture_heightmap, ofs)"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// With windows (G5a) and the normal map at once.
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	material->set_feature(BaseMaterial3D::FEATURE_INTERIOR, true);
	CHECK_MESSAGE(compile_spatial(material_code(material), &error) == OK, error);

	// Without deep parallax: no impostor mode.
	material->set_heightmap_deep_parallax(false);
	CHECK_FALSE(material_code(material).contains("impostor_rect"));
}

TEST_CASE("[Mariner][G5b] BoxImpostorBaker") {
	Ref<BoxImpostorBaker> baker;
	baker.instantiate();
	CHECK(baker->get_texels_per_meter() == doctest::Approx(8.0));
	CHECK(baker->get_max_size() == 2048);
	CHECK(baker->get_padding() == 4);
	CHECK_FALSE(baker->is_bake_bottom_enabled());
	baker->set_max_size(10);
	CHECK(baker->get_max_size() == 64);
	baker->set_padding(-3);
	CHECK(baker->get_padding() == 0);
	ERR_PRINT_OFF;
	CHECK(baker->bake(nullptr).is_null());
	ERR_PRINT_ON;
	CHECK(ClassDB::class_exists("BoxImpostorBaker"));

	// Packing: no overlaps (with the padding), all inside the square.
	Vector<Size2i> sizes;
	sizes.push_back(Size2i(160, 80));
	sizes.push_back(Size2i(160, 80));
	sizes.push_back(Size2i(64, 80));
	sizes.push_back(Size2i(64, 80));
	sizes.push_back(Size2i(160, 64));
	int side = 0;
	const int pad = 4;
	const Vector<Point2i> pos = BoxImpostorBaker::pack_square(sizes, pad, side);
	REQUIRE(pos.size() == sizes.size());
	CHECK(side > 0);
	CHECK(side < 400);
	for (int i = 0; i < sizes.size(); i++) {
		const Rect2i a = Rect2i(pos[i] - Point2i(pad, pad), sizes[i] + Size2i(2 * pad, 2 * pad));
		CHECK(a.position.x >= 0);
		CHECK(a.position.y >= 0);
		CHECK(a.get_end().x <= side);
		CHECK(a.get_end().y <= side);
		for (int j = i + 1; j < sizes.size(); j++) {
			const Rect2i b = Rect2i(pos[j] - Point2i(pad, pad), sizes[j] + Size2i(2 * pad, 2 * pad));
			CHECK_FALSE(a.intersects(b));
		}
	}
}

TEST_CASE("[Mariner][G5c] BaseMaterial3D.heightmap_impostor_faces: the visual hull") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	material->set_heightmap_deep_parallax(true);
	material->set_heightmap_impostor(true);
	material->set_heightmap_depth_offset(true);
	CHECK_FALSE(material_code(material).contains("impostor_solid"));

	PackedVector4Array faces;
	faces.resize(BaseMaterial3D::IMPOSTOR_FACE_DATA_SIZE);
	faces.fill(Vector4(0, 0, 1, 1));
	material->set_heightmap_impostor_faces(faces);
	CHECK(material->get_heightmap_impostor_faces().size() == BaseMaterial3D::IMPOSTOR_FACE_DATA_SIZE);
	String code = material_code(material);
	CHECK(code.contains("uniform vec4 impostor_faces[26];"));
	CHECK(code.contains("bool impostor_solid("));
	CHECK(code.contains("impostor_obj_eye = (impostor_inv_model * vec4(CAMERA_POSITION_WORLD, 1.0)).xyz;"));
	CHECK(code.contains("NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(impostor_normal_obj, 0.0))).xyz);"));
	CHECK_FALSE(code.contains("NORMAL_MAP ="));
	CHECK(code.contains("DEPTH = hull_clip.z / hull_clip.w;"));
	CHECK(code.contains("if (impostor_miss) {"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// Without the normal map, with windows and wetness.
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, false);
	material->set_feature(BaseMaterial3D::FEATURE_INTERIOR, true);
	material->set_feature(BaseMaterial3D::FEATURE_WETNESS, true);
	CHECK_MESSAGE(compile_spatial(material_code(material), &error) == OK, error);

	// Too few vectors: the faces' mode (G5b).
	material->set_heightmap_impostor_faces(PackedVector4Array());
	CHECK_FALSE(material_code(material).contains("impostor_solid"));

	Ref<BoxImpostorBaker> baker;
	baker.instantiate();
	CHECK(baker->is_hull_enabled());
}

} // namespace TestMarinerG5
