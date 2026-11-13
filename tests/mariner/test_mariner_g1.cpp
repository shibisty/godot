/**************************************************************************/
/*  test_mariner_g1.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g1)

#include "core/object/class_db.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/effects/overhead_map.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/environment_storage.h"

#include "modules/modules_enabled.gen.h" // For visual_shader.

#ifdef MODULE_VISUAL_SHADER_ENABLED
#include "modules/visual_shader/visual_shader.h"
#endif

// Mariner engine patch G1a: the overhead map (Environment.overhead_map_*) - the depth of the scene from above around
// the camera - and the sky occlusion of the ambient light and the sky reflection from it (Forward+; images on
// lavapipe: engine/tests/g/overhead_map.gd). G1b: the fragment built-ins SKY_OCCLUSION and OVERHEAD_COVER from the map
// (and their VisualShader inputs). G1c: GPU particles collide with the map (GPUParticles3D.collision_overhead_map;
// engine/tests/g/overhead_particles.gd).

namespace TestMarinerG1 {

TEST_CASE("[Mariner][G1a] Environment overhead map properties") {
	Ref<Environment> env;
	env.instantiate();
	CHECK_FALSE(env->is_overhead_map_enabled());
	CHECK(env->get_overhead_map_size() == doctest::Approx(512.0));
	CHECK(env->get_overhead_map_height() == doctest::Approx(256.0));
	CHECK(env->get_overhead_map_resolution() == 1024);
	CHECK(env->get_overhead_map_cull_mask() == 0xFFFFFu);
	CHECK(env->get_overhead_map_sky_occlusion() == doctest::Approx(1.0));
	CHECK(env->get_overhead_map_sky_occlusion_radius() == doctest::Approx(16.0));
	CHECK(env->get_overhead_map_bias() == doctest::Approx(0.3));

	// Limits: the resolution a power of two in 128..4096, the size and height positive, the amount in 0..1.
	env->set_overhead_map_resolution(1000);
	CHECK(env->get_overhead_map_resolution() == 1024);
	env->set_overhead_map_resolution(16);
	CHECK(env->get_overhead_map_resolution() == 128);
	env->set_overhead_map_resolution(100000);
	CHECK(env->get_overhead_map_resolution() == 4096);
	env->set_overhead_map_size(1.0);
	CHECK(env->get_overhead_map_size() == doctest::Approx(16.0));
	env->set_overhead_map_height(-5.0);
	CHECK(env->get_overhead_map_height() == doctest::Approx(4.0));
	env->set_overhead_map_sky_occlusion(2.0);
	CHECK(env->get_overhead_map_sky_occlusion() == doctest::Approx(1.0));
	env->set_overhead_map_bias(-1.0);
	CHECK(env->get_overhead_map_bias() == doctest::Approx(0.0));

	List<PropertyInfo> props;
	ClassDB::get_property_list("Environment", &props);
	int found = 0;
	for (const PropertyInfo &pi : props) {
		if (pi.name.begins_with("overhead_map_")) {
			found++;
		}
	}
	CHECK(found == 8);

	MethodBind *m = ClassDB::get_method("RenderingServer", "environment_set_overhead_map");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 9);
}

TEST_CASE("[Mariner][G1a] Overhead map region follows the camera in whole texels") {
	Vector3 mn, mx;
	// 512 m, 1024 texels: 0.5 m texels; half the height above the camera, the top in whole meters.
	RendererEnvironmentStorage::overhead_map_region(Vector3(10.3, 5.2, -7.7), 512.0, 256.0, 1024, mn, mx);
	CHECK(mn.is_equal_approx(Vector3(10.5 - 256.0, 134.0 - 256.0, -7.5 - 256.0)));
	CHECK(mx.is_equal_approx(Vector3(10.5 + 256.0, 134.0, -7.5 + 256.0)));
	// Less than half a texel of camera motion: the same box (no shimmering).
	Vector3 mn2, mx2;
	RendererEnvironmentStorage::overhead_map_region(Vector3(10.45, 5.7, -7.6), 512.0, 256.0, 1024, mn2, mx2);
	CHECK(mn2.is_equal_approx(mn));
	CHECK(mx2.is_equal_approx(mx));
	// The texel grid holds for any camera position.
	RendererEnvironmentStorage::overhead_map_region(Vector3(-123.37, 0.0, 77.01), 512.0, 256.0, 1024, mn, mx);
	CHECK(Math::is_zero_approx(Math::fposmod(mn.x, 0.5f)));
	CHECK(Math::is_zero_approx(Math::fposmod(mn.z, 0.5f)));
	CHECK((mx - mn).is_equal_approx(Vector3(512.0, 256.0, 512.0)));
}

TEST_CASE("[Mariner][G1a] Overhead map camera and uniform block") {
	// The depth pass camera looks down from the top of the box, orthographic over the box.
	Transform3D xform;
	Projection proj;
	RendererRD::OverheadMap::get_camera(Vector3(-10, -2, -20), Vector3(10, 6, 20), xform, proj);
	CHECK(xform.origin.is_equal_approx(Vector3(0, 6, 0)));
	CHECK((-xform.basis.get_column(Vector3::AXIS_Z)).is_equal_approx(Vector3(0, -1, 0)));
	CHECK(proj.is_orthogonal());
	CHECK(proj.get_z_far() == doctest::Approx(8.0));
	// The block of the shader (overhead_map_data_inc.glsl: four vec4).
	CHECK(sizeof(RendererRD::OverheadMap::UBO) == 64);
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

TEST_CASE("[Mariner][G1b] Fragment built-ins SKY_OCCLUSION and OVERHEAD_COVER") {
	const ShaderLanguage::FunctionInfo &fragment = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL)["fragment"];
	for (const char *name : { "SKY_OCCLUSION", "OVERHEAD_COVER" }) {
		REQUIRE_MESSAGE(fragment.built_ins.has(name), name);
		CHECK(fragment.built_ins[name].type == ShaderLanguage::TYPE_FLOAT);
		CHECK(fragment.built_ins[name].constant);
	}
	String error;
	CHECK_MESSAGE(compile_spatial("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(OVERHEAD_COVER, SKY_OCCLUSION, 0.0); AO = SKY_OCCLUSION; }\n", &error) == OK, error);
	CHECK(compile_spatial("shader_type spatial;\nvoid fragment() { SKY_OCCLUSION = 0.5; }\n", &error) != OK); // Read only.
	CHECK(compile_spatial("shader_type spatial;\nvoid vertex() { float c = OVERHEAD_COVER; }\n", &error) != OK); // Fragment only.
}

#ifdef MODULE_VISUAL_SHADER_ENABLED
TEST_CASE("[Mariner][G1b] VisualShader inputs sky_occlusion and overhead_cover") {
	Ref<VisualShaderNodeInput> input;
	input.instantiate();
	input->set_shader_mode(Shader::MODE_SPATIAL);
	input->set_shader_type(VisualShader::TYPE_FRAGMENT);
	input->set_input_name("sky_occlusion");
	CHECK(input->get_input_real_name() == "SKY_OCCLUSION");
	CHECK(input->get_input_type_by_name("sky_occlusion") == VisualShaderNode::PORT_TYPE_SCALAR);
	input->set_input_name("overhead_cover");
	CHECK(input->get_input_real_name() == "OVERHEAD_COVER");
	CHECK(input->get_input_type_by_name("overhead_cover") == VisualShaderNode::PORT_TYPE_SCALAR);
}
#endif

TEST_CASE("[Mariner][G1c] GPUParticles3D collision with the overhead map") {
	GPUParticles3D *particles = memnew(GPUParticles3D);
	CHECK_FALSE(particles->is_overhead_map_collision());
	particles->set_overhead_map_collision(true);
	CHECK(particles->is_overhead_map_collision());
	PropertyInfo pi;
	CHECK(ClassDB::get_property_info("GPUParticles3D", "collision_overhead_map", &pi));
	memdelete(particles);
	MethodBind *m = ClassDB::get_method("RenderingServer", "particles_set_overhead_map_collision");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);
}

} // namespace TestMarinerG1
