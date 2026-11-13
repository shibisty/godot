/**************************************************************************/
/*  test_mariner_aa1.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_aa1)

#include "scene/resources/alpha_mask_bake.h"
#include "scene/resources/material.h"
#include "servers/rendering/renderer_rd/forward_mobile/scene_shader_forward_mobile.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch AA1: an analytic alpha-scissor edge (BaseMaterial3D.ALPHA_ANTIALIASING_FRINGE, render_mode
// alpha_fringe, Forward+). The fully covered core is opaque, the edge pixels are blended in the transparent pass, far
// away the mipmapped alpha is the coverage. Measured on the AA bench (aa_bench_results.md); images and checks on
// lavapipe: engine/tests/aa/fringe.gd.

namespace TestMarinerAA1 {

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

TEST_CASE("[Mariner][AA1] render_mode alpha_fringe and its built-ins") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	REQUIRE(fragment.built_ins.has("ALPHA_FRINGE_COORDINATE"));
	REQUIRE(fragment.built_ins.has("ALPHA_FRINGE_FAR"));
	CHECK(fragment.built_ins["ALPHA_FRINGE_COORDINATE"].type == ShaderLanguage::TYPE_VEC2);
	CHECK(fragment.built_ins["ALPHA_FRINGE_FAR"].type == ShaderLanguage::TYPE_VEC2);
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nrender_mode alpha_fringe;\nuniform sampler2D t;\nvoid fragment() {\n\tALPHA = texture(t, UV).a;\n\tALPHA_SCISSOR_THRESHOLD = 0.5;\n\tALPHA_FRINGE_COORDINATE = UV * vec2(textureSize(t, 0));\n\tALPHA_FRINGE_FAR = vec2(1.0, 2.0);\n}\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { ALPHA_FRINGE_FAR = vec2(1.0); }\n", &error) != OK); // Fragment only.
}

TEST_CASE("[Mariner][AA1] BaseMaterial3D alpha antialiasing: fringe") {
	CHECK(int(BaseMaterial3D::ALPHA_ANTIALIASING_FRINGE) == 3);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	material->set_alpha_antialiasing(BaseMaterial3D::ALPHA_ANTIALIASING_FRINGE);
	String code = material_code(material);
	CHECK(code.contains("alpha_fringe"));
	CHECK(code.contains("ALPHA_FRINGE_COORDINATE = UV * vec2(albedo_texture_size);"));
	CHECK_FALSE(code.contains("alpha_to_coverage"));
	CHECK_FALSE(code.contains("ALPHA_ANTIALIASING_EDGE")); // Not the alpha to coverage path (would make it transparent).
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// Only with the alpha scissor: with the alpha hash the mode does nothing.
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_HASH);
	code = material_code(material);
	CHECK_FALSE(code.contains("alpha_fringe"));
	CHECK_FALSE(code.contains("ALPHA_ANTIALIASING_EDGE"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// The alpha to coverage modes are unchanged.
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	material->set_alpha_antialiasing(BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);
	code = material_code(material);
	CHECK(code.contains("alpha_to_coverage_and_one"));
	CHECK(code.contains("ALPHA_ANTIALIASING_EDGE = alpha_antialiasing_edge;"));
	CHECK_FALSE(code.contains("alpha_fringe"));
}

// Mariner AA2: alpha masks (distance fields) baked from an alpha image.
static Ref<Image> make_mask(int p_size, bool p_square) {
	Ref<Image> img = Image::create_empty(p_size, p_size, false, Image::FORMAT_LA8);
	const float c = p_size * 0.5f;
	const float r = p_size * 0.3f;
	for (int y = 0; y < p_size; y++) {
		for (int x = 0; x < p_size; x++) {
			// 4 x 4 supersampled coverage.
			int n = 0;
			for (int sy = 0; sy < 4; sy++) {
				for (int sx = 0; sx < 4; sx++) {
					const float px = x + (sx + 0.5f) / 4.0f - c;
					const float py = y + (sy + 0.5f) / 4.0f - c;
					n += p_square ? (Math::abs(px) < r && Math::abs(py) < r) : (px * px + py * py < r * r);
				}
			}
			img->set_pixel(x, y, Color(1, 1, 1, n / 16.0f));
		}
	}
	return img;
}

// The mask as a texture sampler sees it: bilinear (per channel, then the median for an MSDF) at texel coordinates.
static float mask_sample(const Ref<Image> &p_mask, bool p_msdf, float p_u, float p_v) {
	const int w = p_mask->get_width();
	const int h = p_mask->get_height();
	const float u = p_u - 0.5f;
	const float v = p_v - 0.5f;
	const int x0 = int(Math::floor(u));
	const int y0 = int(Math::floor(v));
	const float fx = u - x0;
	const float fy = v - y0;
	auto px = [&](int p_x, int p_y) { return p_mask->get_pixel(CLAMP(p_x, 0, w - 1), CLAMP(p_y, 0, h - 1)); };
	const Color m = (px(x0, y0) * (1 - fx) + px(x0 + 1, y0) * fx) * (1 - fy) + (px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx) * fy;
	return p_msdf ? MAX(MIN(m.r, m.g), MIN(MAX(m.r, m.g), m.b)) : m.r;
}

// Area inside the contour (8 x 8 samples per texel).
static float mask_area(const Ref<Image> &p_mask, bool p_msdf) {
	int inside = 0;
	for (int y = 0; y < p_mask->get_height() * 8; y++) {
		for (int x = 0; x < p_mask->get_width() * 8; x++) {
			inside += mask_sample(p_mask, p_msdf, (x + 0.5f) / 8.0f, (y + 0.5f) / 8.0f) > 0.5f ? 1 : 0;
		}
	}
	return inside / 64.0f;
}

// Share of wrong inside / outside answers (16 x 16 samples per texel) within a texel of the corners of the square
// [p_lo, p_hi]^2 (texel units).
static float corner_error(const Ref<Image> &p_mask, bool p_msdf, float p_lo, float p_hi) {
	int wrong = 0;
	int total = 0;
	const float corners[4][2] = { { p_lo, p_lo }, { p_hi, p_lo }, { p_lo, p_hi }, { p_hi, p_hi } };
	for (const float *c : corners) {
		for (int y = 0; y < 32; y++) {
			for (int x = 0; x < 32; x++) {
				const float u = c[0] - 1.0f + (x + 0.5f) / 16.0f;
				const float v = c[1] - 1.0f + (y + 0.5f) / 16.0f;
				const bool inside = u > p_lo && u < p_hi && v > p_lo && v < p_hi;
				wrong += (mask_sample(p_mask, p_msdf, u, v) > 0.5f) != inside ? 1 : 0;
				total++;
			}
		}
	}
	return float(wrong) / total;
}

TEST_CASE("[Mariner][AA2] BaseMaterial3D.bake_alpha_mask: SDF and MSDF from an alpha image") {
	// A disc of radius 0.3 * 256 = 76.8 source pixels -> a 64 x 64 SDF, range 4 texels.
	const Ref<Image> disc = make_mask(256, false);
	const Ref<Image> sdf = BaseMaterial3D::bake_alpha_mask(disc, Vector2i(64, 64), BaseMaterial3D::ALPHA_MASK_SDF, 4.0);
	REQUIRE(sdf.is_valid());
	CHECK(sdf->get_format() == Image::FORMAT_L8);
	CHECK(sdf->get_width() == 64);
	CHECK(sdf->get_pixel(32, 32).r > 0.9f); // Deep inside: clamped high.
	CHECK(sdf->get_pixel(1, 1).r < 0.1f); // Far outside.
	// Encoding: 0.5 + d / (2 * range); the contour (r = 19.2 texels) between texels 51 and 52 along the row 32.
	const float d51 = (sdf->get_pixel(51, 32).r - 0.5f) * 8.0f; // Texel centre at 19.5 from the centre: -0.3 texel.
	const float d50 = (sdf->get_pixel(50, 32).r - 0.5f) * 8.0f; // 18.5: +0.7 texel.
	CHECK(Math::abs(d51 + 0.3f) < 0.15f);
	CHECK(Math::abs(d50 - 0.7f) < 0.15f);
	const float exact_disc = Math::PI * 19.2f * 19.2f;
	CHECK(Math::abs(mask_area(sdf, false) / exact_disc - 1.0f) < 0.01f);

	// A square (corners): the MSDF keeps them, the single-channel SDF rounds them off.
	const Ref<Image> square = make_mask(256, true);
	const Ref<Image> sq_sdf = BaseMaterial3D::bake_alpha_mask(square, Vector2i(32, 32), BaseMaterial3D::ALPHA_MASK_SDF, 4.0);
	const Ref<Image> sq_msdf = BaseMaterial3D::bake_alpha_mask(square, Vector2i(32, 32), BaseMaterial3D::ALPHA_MASK_MSDF, 4.0);
	REQUIRE(sq_sdf.is_valid());
	REQUIRE(sq_msdf.is_valid());
	CHECK(sq_msdf->get_format() == Image::FORMAT_RGB8);
	// Square [0.2, 0.8] x 32 texels = [6.4, 25.6].
	const float err_sdf = corner_error(sq_sdf, false, 6.4f, 25.6f);
	const float err_msdf = corner_error(sq_msdf, true, 6.4f, 25.6f);
	INFO("square corners, wrong answers: SDF ", err_sdf, ", MSDF ", err_msdf);
	CHECK(err_msdf < 0.02f);
	CHECK(err_msdf < err_sdf * 0.7f); // 1.2 % against 2.2 % (8 bit, a 32 texel square).
	CHECK(Math::abs(mask_area(sq_msdf, true) / ((0.6f * 32.0f) * (0.6f * 32.0f)) - 1.0f) < 0.01f);

	// Errors: nothing to trace, wrong mode.
	ERR_PRINT_OFF;
	CHECK(BaseMaterial3D::bake_alpha_mask(Image::create_empty(16, 16, false, Image::FORMAT_LA8), Vector2i(8, 8), BaseMaterial3D::ALPHA_MASK_SDF, 4.0).is_null());
	CHECK(BaseMaterial3D::bake_alpha_mask(disc, Vector2i(8, 8), BaseMaterial3D::ALPHA_MASK_ALBEDO, 4.0).is_null());
	ERR_PRINT_ON;
}

TEST_CASE("[Mariner][AA2] BaseMaterial3D.alpha_mask_mode: the generated shader") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	String code = material_code(material);
	CHECK_FALSE(code.contains("texture_alpha_mask"));
	CHECK(code.contains("ALPHA *= albedo.a * albedo_tex.a;"));

	material->set_alpha_mask_mode(BaseMaterial3D::ALPHA_MASK_SDF);
	code = material_code(material);
	CHECK(code.contains("uniform sampler2D texture_alpha_mask"));
	CHECK(code.contains("// Mariner AA2: alpha mask SDF")); // The path tracer reads this marker.
	CHECK(code.contains("ALPHA *= albedo.a * texture(texture_alpha_mask, base_uv).r;"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	material->set_alpha_mask_mode(BaseMaterial3D::ALPHA_MASK_MSDF);
	material->set_alpha_antialiasing(BaseMaterial3D::ALPHA_ANTIALIASING_FRINGE);
	code = material_code(material);
	CHECK(code.contains("// Mariner AA2: alpha mask MSDF"));
	CHECK(code.contains("max(min(alpha_mask_tex.r, alpha_mask_tex.g), min(max(alpha_mask_tex.r, alpha_mask_tex.g), alpha_mask_tex.b))"));
	CHECK(code.contains("ALPHA_FRINGE_FAR = vec2(0.0);")); // No mip selector for a distance field.
	CHECK_FALSE(code.contains("ALPHA_FRINGE_COORDINATE"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// No transparency: no mask.
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_DISABLED);
	code = material_code(material);
	CHECK_FALSE(code.contains("texture_alpha_mask"));
}

// Mariner AA3: wire antialiasing (Persson) - BaseMaterial3D.wire_antialiasing.
TEST_CASE("[Mariner][AA3] BaseMaterial3D.wire_antialiasing: the generated shader") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_wire_antialiasing(true);
	String code = material_code(material);
	CHECK_FALSE(code.contains("wire_fade")); // Needs alpha blending.

	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	code = material_code(material);
	CHECK(code.contains("varying float wire_fade;"));
	CHECK(code.contains("if (!IN_SHADOW_PASS && wire_r > 1.0e-7) {")); // Shadows keep the true tube.
	CHECK(code.contains("wire_r_px = max(wire_rp, 0.5);")); // At least a pixel wide.
	CHECK(code.contains("abs(PROJECTION_MATRIX[1][1])")); // RD projections carry a Y flip.
	CHECK(code.contains("ALPHA *= clamp(wire_r_px - abs(wire_u_px) + 0.5, 0.0, 1.0) * wire_fade;"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	material->set_wire_antialiasing(false);
	code = material_code(material);
	CHECK_FALSE(code.contains("wire_fade"));
}

TEST_CASE("[Mariner][AA4] Mobile: the alpha_fringe_pass specialization bit") {
	// scene_forward_mobile_inc.glsl reads sc_alpha_fringe_pass() as bit 31 of packed_1.
	RendererSceneRenderImplementation::SceneShaderForwardMobile::ShaderSpecialization specialization = {};
	specialization.alpha_fringe_pass = 1;
	CHECK(specialization.packed_0 == 0u);
	CHECK(specialization.packed_1 == (1u << 31));
	specialization = {};
	specialization.directional_light_blend_splits = 255;
	CHECK((specialization.packed_1 >> 31) == 0u);
}

} // namespace TestMarinerAA1
