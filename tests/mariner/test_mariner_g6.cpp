/**************************************************************************/
/*  test_mariner_g6.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g6)

#include "core/object/class_db.h"
#include "scene/3d/planar_reflection_3d.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G6: planar reflections. REFLECTION (spatial fragment) replaces the indirect specular light (over
// the probes, the sky and SSR); RS.camera_set_clip_plane() (gl_ClipDistance, RD::SUPPORTS_CLIP_DISTANCE) and
// RS.camera_set_raw_output(); PlanarReflection3D renders the mirrored camera; BaseMaterial3D.planar_reflection_*.
// Images on lavapipe: engine/tests/g/planar_reflection.gd.

namespace TestMarinerG6 {

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

TEST_CASE("[Mariner][G6] REFLECTION built-in") {
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { REFLECTION = vec4(1.0, 0.0, 0.0, 0.5); }\n", &error) == OK, error);
	// An output: not readable in vertex(), not a constant.
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { REFLECTION = vec4(1.0); }\n", &error) != OK);
}

TEST_CASE("[Mariner][G6] BaseMaterial3D.planar_reflection") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_planar_reflection_amount() == doctest::Approx(1.0));
	CHECK(material->get_planar_reflection_distortion() == doctest::Approx(0.03));
	BaseMaterial3D::flush_changes();
	CHECK_FALSE(RS::get_singleton()->shader_get_code(material->get_shader_rid()).contains("texture_planar_reflection"));

	material->set_feature(BaseMaterial3D::FEATURE_PLANAR_REFLECTION, true);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	BaseMaterial3D::flush_changes();
	String code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
	CHECK(code.contains("uniform sampler2D texture_planar_reflection"));
	CHECK(code.contains("vec2 pr_uv = vec2(1.0 - SCREEN_UV.x, SCREEN_UV.y);"));
	CHECK(code.contains("REFLECTION = vec4(pr_color, planar_reflection_amount);"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// Unshaded: no reflection.
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	BaseMaterial3D::flush_changes();
	CHECK_FALSE(RS::get_singleton()->shader_get_code(material->get_shader_rid()).contains("REFLECTION ="));

	material->set_planar_reflection_amount(2.0);
	CHECK(material->get_planar_reflection_amount() == doctest::Approx(1.0));
	CHECK(ClassDB::has_property("BaseMaterial3D", "planar_reflection_texture"));
}

TEST_CASE("[Mariner][G6] PlanarReflection3D") {
	PlanarReflection3D *node = memnew(PlanarReflection3D);
	CHECK(node->get_size() == Vector2(10, 10));
	CHECK(node->get_resolution_scale() == doctest::Approx(0.5));
	CHECK(node->get_surface_layer() == 20);
	CHECK(node->get_cull_mask() == 0xFFFFF);
	node->set_surface_layer(30);
	CHECK(node->get_surface_layer() == 20);
	node->set_resolution_scale(0.0);
	CHECK(node->get_resolution_scale() == doctest::Approx(0.05));
	CHECK(node->get_reflection_texture().is_null()); // Outside the tree: no viewport yet.
	memdelete(node);

	// The mirrored camera: the position across the plane, a right-handed basis, the view direction reflected, X turned.
	const Plane floor(Vector3(0, 1, 0), 0.0);
	Transform3D cam;
	cam.origin = Vector3(1, 2, 3);
	cam.basis = Basis::looking_at(Vector3(0, -1, -2).normalized());
	const Transform3D m = PlanarReflection3D::mirror_transform(cam, floor);
	CHECK(m.origin.is_equal_approx(Vector3(1, -2, 3)));
	CHECK(m.basis.determinant() == doctest::Approx(1.0));
	const Vector3 fwd = -cam.basis.get_column(2);
	const Vector3 mfwd = -m.basis.get_column(2);
	CHECK(mfwd.is_equal_approx(Vector3(fwd.x, -fwd.y, fwd.z)));
	CHECK(m.basis.get_column(0).is_equal_approx(-cam.basis.get_column(0) * Vector3(1, -1, 1)));
	// A tilted plane through a point.
	const Plane tilted(Vector3(0, 1, 1).normalized(), Vector3(0, 0.5, 0));
	const Transform3D mt = PlanarReflection3D::mirror_transform(cam, tilted);
	CHECK(tilted.distance_to(mt.origin) == doctest::Approx(-tilted.distance_to(cam.origin)));

	// The rendering server's camera options exist (RD::SUPPORTS_CLIP_DISTANCE decides the shaders' clipping).
	const RID c = RS::get_singleton()->camera_create();
	RS::get_singleton()->camera_set_clip_plane(c, true, floor);
	RS::get_singleton()->camera_set_raw_output(c, true);
	RS::get_singleton()->free_rid(c);
	CHECK(ClassDB::class_exists("PlanarReflection3D"));
}

} // namespace TestMarinerG6
