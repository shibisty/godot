/**************************************************************************/
/*  test_mariner_m.cpp                                                    */
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

TEST_FORCE_LINK(test_mariner_m)

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch M4 M5 M6 M8 F4 F5 F6: sheen (SHEEN, SHEEN_ROUGHNESS), thin-film iridescence (IRIDESCENCE*),
// physical refraction and reoriented detail normals in BaseMaterial3D, F0 of metals, height and cavity from a normal
// map (Image), specular anti-aliasing under a temporal upscaler. The images on lavapipe: engine/tests/m/*.gd.

namespace TestMarinerM {

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

TEST_CASE("[Mariner][M6] Fragment built-ins SHEEN, SHEEN_ROUGHNESS, IRIDESCENCE*") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	CHECK(fragment.built_ins["SHEEN"].type == ShaderLanguage::TYPE_VEC3);
	CHECK(fragment.built_ins["SHEEN_ROUGHNESS"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK(fragment.built_ins["IRIDESCENCE"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK(fragment.built_ins["IRIDESCENCE_IOR"].type == ShaderLanguage::TYPE_FLOAT);
	CHECK(fragment.built_ins["IRIDESCENCE_THICKNESS"].type == ShaderLanguage::TYPE_FLOAT);
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { SHEEN = vec3(0.8); SHEEN_ROUGHNESS = 0.4; IRIDESCENCE = 1.0; IRIDESCENCE_IOR = 1.4; IRIDESCENCE_THICKNESS = 350.0; }\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { SHEEN = vec3(1.0); }\n", &error) != OK); // Fragment only.
}

TEST_CASE("[Mariner][M6] BaseMaterial3D sheen and iridescence") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_sheen_color().is_equal_approx(Color(1, 1, 1)));
	CHECK(material->get_sheen_roughness() == doctest::Approx(0.5));
	CHECK(material->get_iridescence_ior() == doctest::Approx(1.3));
	CHECK(material->get_iridescence_thickness_max() == doctest::Approx(400.0));
	String code = material_code(material);
	CHECK_FALSE(code.contains("SHEEN"));
	CHECK_FALSE(code.contains("IRIDESCENCE"));

	material->set_feature(BaseMaterial3D::FEATURE_SHEEN, true);
	material->set_feature(BaseMaterial3D::FEATURE_IRIDESCENCE, true);
	code = material_code(material);
	CHECK(code.contains("SHEEN = "));
	CHECK(code.contains("IRIDESCENCE_THICKNESS = "));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	CHECK_MESSAGE(compile_spatial(material_code(material), &error) == OK, error);
	CHECK(bool(material->get("sheen_enabled")));
	CHECK(bool(material->get("iridescence_enabled")));
}

TEST_CASE("[Mariner][F6] BaseMaterial3D physical refraction") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK_FALSE(material->is_refraction_physical());
	CHECK(material->get_refraction_ior() == doctest::Approx(1.5));
	material->set_refraction_ior(10.0);
	CHECK(material->get_refraction_ior() == doctest::Approx(3.0));
	material->set_refraction_ior(1.5);
	material->set_feature(BaseMaterial3D::FEATURE_REFRACTION, true);
	String code = material_code(material);
	CHECK(code.contains("ref_ofs = SCREEN_UV"));
	CHECK_FALSE(code.contains("refraction_ior"));
	material->set_refraction_physical(true);
	for (bool normal_map : { false, true }) {
		material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, normal_map);
		code = material_code(material);
		CHECK(code.contains("refract("));
		CHECK(code.contains("refraction_absorption_distance"));
		String error;
		CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
	}
}

TEST_CASE("[Mariner][M4] BaseMaterial3D reoriented detail normals") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK_FALSE(material->is_detail_normal_rnm());
	material->set_feature(BaseMaterial3D::FEATURE_DETAIL, true);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	CHECK_FALSE(material_code(material).contains("rnm"));
	material->set_detail_normal_rnm(true);
	const String code = material_code(material);
	CHECK(code.contains("rnm_t"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);
}

TEST_CASE("[Mariner][M8] F0 of metals") {
	CHECK(ClassDB::has_method("BaseMaterial3D", "get_metal_albedo"));
	// Gold: linear (1.0, 0.782, 0.344); in sRGB about (1.0, 0.90, 0.62).
	const Color gold = BaseMaterial3D::get_metal_albedo(BaseMaterial3D::METAL_GOLD);
	CHECK(gold.srgb_to_linear().is_equal_approx(Color(1.0, 0.782, 0.344)));
	CHECK(gold.r > gold.g);
	CHECK(gold.g > gold.b);
	const Color silver = BaseMaterial3D::get_metal_albedo(BaseMaterial3D::METAL_SILVER);
	CHECK(silver.r > 0.97);
	const Color iron = BaseMaterial3D::get_metal_albedo(BaseMaterial3D::METAL_IRON).srgb_to_linear();
	CHECK(iron.r == doctest::Approx(0.562).epsilon(0.001));
	ERR_PRINT_OFF;
	CHECK(BaseMaterial3D::get_metal_albedo(BaseMaterial3D::METAL_MAX).is_equal_approx(Color(1, 1, 1)));
	ERR_PRINT_ON;
}

// A normal map of a cone (pit) on a flat 64 x 64 texture: height from it, then cavity.
static Ref<Image> make_pit_normal_map(int p_size, float p_depth_texels, float p_radius_texels) {
	Ref<Image> img = Image::create_empty(p_size, p_size, false, Image::FORMAT_RGBA8);
	const Vector2 c(p_size * 0.5, p_size * 0.5);
	for (int y = 0; y < p_size; y++) {
		for (int x = 0; x < p_size; x++) {
			const Vector2 d = Vector2(x + 0.5, y + 0.5) - c;
			const float r = d.length();
			// Height z = -depth * (1 - r / radius) inside: slope outward = depth / radius.
			Vector3 n(0, 0, 1);
			if (r < p_radius_texels && r > 0.01) {
				const Vector2 g = d / r * (p_depth_texels / p_radius_texels); // dz/dx, dz/dy (image y down).
				n = Vector3(-g.x, g.y, 1.0).normalized(); // Green up.
			}
			img->set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0));
		}
	}
	return img;
}

TEST_CASE("[Mariner][M5] Height and cavity from a normal map") {
	CHECK(ClassDB::has_method("Image", "normal_map_to_height"));
	CHECK(ClassDB::has_method("Image", "normal_map_to_cavity"));
	const Ref<Image> normal_map = make_pit_normal_map(64, 6.0, 12.0);
	const Ref<Image> height = normal_map->normal_map_to_height();
	REQUIRE(height.is_valid());
	CHECK(height->get_format() == Image::FORMAT_RF);
	const float centre = height->get_pixel(32, 32).r;
	const float rim = height->get_pixel(32, 32 + 14).r;
	const float far_value = height->get_pixel(4, 4).r;
	MESSAGE("height: centre ", centre, ", rim ", rim, ", far ", far_value);
	CHECK(rim - centre > 4.0); // About 6 texels deep.
	CHECK(rim - centre < 7.5);
	CHECK(Math::abs(far_value - rim) < 1.0); // Flat outside.

	// Green down (DirectX convention) read as such gives the same height.
	Ref<Image> dx = normal_map->duplicate();
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			Color c = dx->get_pixel(x, y);
			c.g = 1.0 - c.g;
			dx->set_pixel(x, y, c);
		}
	}
	const Ref<Image> height_dx = dx->normal_map_to_height(1.0, true);
	CHECK(height_dx->get_pixel(32, 32).r == doctest::Approx(centre).epsilon(0.02));

	const Ref<Image> cavity = normal_map->normal_map_to_cavity(1.0, 16);
	REQUIRE(cavity.is_valid());
	const float ao_centre = cavity->get_pixel(32, 32).r;
	const float ao_far = cavity->get_pixel(4, 4).r;
	MESSAGE("cavity: centre ", ao_centre, ", flat ", ao_far);
	CHECK(ao_far > 0.97);
	CHECK(ao_centre < 0.9); // Walls rise at about 0.46: cos^2 = 1 / (1 + 0.46^2) = 0.83.
	CHECK(ao_centre > 0.75);
	// Twice as deep: darker.
	CHECK(normal_map->normal_map_to_cavity(2.0, 16)->get_pixel(32, 32).r < ao_centre);
}

} // namespace TestMarinerM
