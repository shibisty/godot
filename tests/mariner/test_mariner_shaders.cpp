/**************************************************************************/
/*  test_mariner_shaders.cpp                                              */
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

TEST_FORCE_LINK(test_mariner_shaders)

#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patches E1 (PREVIOUS_FRAME, render_mode motion_vectors_transparent) and
// E4 (render_mode opaque_late): shader type registration, parsing and the compiler flags the
// Forward+ renderer reads. The renderers themselves need a GPU and are not covered here.

namespace TestMarinerShaders {

static Error compile_shader(RSE::ShaderMode p_mode, const String &p_code, String *r_error = nullptr) {
	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(p_mode);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(p_mode);
	info.shader_types = ShaderTypes::get_singleton()->get_types();

	ERR_PRINT_OFF;
	const Error err = sl.compile(p_code, info);
	ERR_PRINT_ON;
	if (r_error) {
		*r_error = sl.get_error_text();
	}
	return err;
}

static bool has_mode(RSE::ShaderMode p_mode, const StringName &p_name) {
	for (const ShaderLanguage::ModeInfo &mode : ShaderTypes::get_singleton()->get_modes(p_mode)) {
		if (mode.name == p_name) {
			return true;
		}
	}
	return false;
}

static bool has_built_in(RSE::ShaderMode p_mode, const StringName &p_function, const StringName &p_name) {
	const HashMap<StringName, ShaderLanguage::FunctionInfo> &functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	return functions.has(p_function) && functions[p_function].built_ins.has(p_name);
}

// Flags Forward+ (SceneShaderForwardClustered::ShaderData::set_code) collects from the shader compiler.
struct ForwardPlusFlags {
	bool uses_previous_frame = false;
	bool uses_transparent_motion_vectors = false;
	bool uses_opaque_late = false;
	bool uses_depth_prepass_alpha = false;
	bool uses_alpha = false;
	bool uses_alpha_clip = false;
	bool uses_screen_texture = false;
	bool uses_depth_texture = false;
	bool uses_normal_texture = false;

	// Mirror of SceneShaderForwardClustered::ShaderData::uses_alpha_pass() (alpha / screen read part, E4 rule):
	// reading the screen, depth or normal-roughness texture makes a material transparent unless it is opaque_late.
	bool forced_transparent() const {
		const bool has_read_screen_alpha = !uses_opaque_late && (uses_screen_texture || uses_depth_texture || uses_normal_texture);
		const bool has_base_alpha = (uses_alpha && !uses_alpha_clip) || has_read_screen_alpha;
		return has_base_alpha || has_read_screen_alpha;
	}
};

static Error compile_forward_plus(const String &p_code, ForwardPlusFlags &r_flags, ShaderCompiler::GeneratedCode &r_gen_code) {
	ShaderCompiler::DefaultIdentifierActions default_actions;
	// The renames of Forward+ (scene_shader_forward_clustered.cpp) for the built-ins used below.
	default_actions.renames["PREVIOUS_FRAME"] = "is_prev_frame";
	default_actions.renames["VERTEX"] = "vertex";
	default_actions.renames["ALBEDO"] = "albedo";
	default_actions.renames["ALPHA"] = "alpha";
	default_actions.renames["SCREEN_UV"] = "screen_uv";

	ShaderCompiler compiler;
	compiler.initialize(default_actions);

	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.usage_flag_pointers["PREVIOUS_FRAME"] = &r_flags.uses_previous_frame;
	actions.usage_flag_pointers["ALPHA"] = &r_flags.uses_alpha;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &r_flags.uses_alpha_clip;
	actions.render_mode_flags["motion_vectors_transparent"] = &r_flags.uses_transparent_motion_vectors;
	actions.render_mode_flags["opaque_late"] = &r_flags.uses_opaque_late;
	actions.render_mode_flags["depth_prepass_alpha"] = &r_flags.uses_depth_prepass_alpha;
	actions.uniforms = &uniforms;

	ERR_PRINT_OFF;
	const Error err = compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, "", r_gen_code);
	ERR_PRINT_ON;

	r_flags.uses_screen_texture = r_gen_code.uses_screen_texture;
	r_flags.uses_depth_texture = r_gen_code.uses_depth_texture;
	r_flags.uses_normal_texture = r_gen_code.uses_normal_roughness_texture;
	return err;
}

TEST_SUITE("[Mariner][Shaders]") {
	TEST_CASE("[Mariner][E1] PREVIOUS_FRAME is a read-only bool built-in of the spatial vertex function only") {
		REQUIRE(ShaderTypes::get_singleton() != nullptr);

		const HashMap<StringName, ShaderLanguage::FunctionInfo> &spatial = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
		REQUIRE(spatial.has("vertex"));
		REQUIRE(spatial["vertex"].built_ins.has("PREVIOUS_FRAME"));
		CHECK(spatial["vertex"].built_ins["PREVIOUS_FRAME"].type == ShaderLanguage::TYPE_BOOL);
		CHECK(spatial["vertex"].built_ins["PREVIOUS_FRAME"].constant);

		CHECK_FALSE(has_built_in(RSE::SHADER_SPATIAL, "fragment", "PREVIOUS_FRAME"));
		CHECK_FALSE(has_built_in(RSE::SHADER_SPATIAL, "light", "PREVIOUS_FRAME"));
		CHECK_FALSE(has_built_in(RSE::SHADER_CANVAS_ITEM, "vertex", "PREVIOUS_FRAME"));
		CHECK_FALSE(has_built_in(RSE::SHADER_PARTICLES, "start", "PREVIOUS_FRAME"));
		CHECK_FALSE(has_built_in(RSE::SHADER_PARTICLES, "process", "PREVIOUS_FRAME"));

		// Neighbouring upstream built-ins are untouched.
		CHECK(has_built_in(RSE::SHADER_SPATIAL, "vertex", "VERTEX_ID"));
		CHECK(has_built_in(RSE::SHADER_SPATIAL, "vertex", "INSTANCE_ID"));
		CHECK(has_built_in(RSE::SHADER_SPATIAL, "vertex", "ROUGHNESS"));
	}

	TEST_CASE("[Mariner][E1] PREVIOUS_FRAME parses in spatial vertex() and is rejected elsewhere") {
		String error;
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
uniform float wave_time;
uniform float wave_time_prev;
void vertex() {
	float t = PREVIOUS_FRAME ? wave_time_prev : wave_time;
	VERTEX.y += sin(VERTEX.x + t);
	if (PREVIOUS_FRAME) {
		VERTEX.y += 0.0;
	}
}
)",
					  &error) == OK);
		CHECK_MESSAGE(error.is_empty(), error);

		// Read-only.
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
void vertex() {
	PREVIOUS_FRAME = true;
}
)") != OK);

		// bool: no implicit conversion to float.
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
void vertex() {
	float f = PREVIOUS_FRAME;
}
)") != OK);

		// Not available in fragment() / light().
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
void fragment() {
	ALBEDO = PREVIOUS_FRAME ? vec3(1.0) : vec3(0.0);
}
)") != OK);
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
void light() {
	DIFFUSE_LIGHT = PREVIOUS_FRAME ? vec3(1.0) : vec3(0.0);
}
)") != OK);

		// Not available in other shader types.
		CHECK(compile_shader(RSE::SHADER_CANVAS_ITEM, R"(
shader_type canvas_item;
void vertex() {
	VERTEX.x += PREVIOUS_FRAME ? 1.0 : 0.0;
}
)") != OK);
		CHECK(compile_shader(RSE::SHADER_PARTICLES, R"(
shader_type particles;
void process() {
	VELOCITY.x = PREVIOUS_FRAME ? 1.0 : 0.0;
}
)") != OK);
	}

	TEST_CASE("[Mariner][E1] render_mode motion_vectors_transparent is spatial-only") {
		CHECK(has_mode(RSE::SHADER_SPATIAL, "motion_vectors_transparent"));
		CHECK_FALSE(has_mode(RSE::SHADER_CANVAS_ITEM, "motion_vectors_transparent"));
		CHECK_FALSE(has_mode(RSE::SHADER_PARTICLES, "motion_vectors_transparent"));
		CHECK_FALSE(has_mode(RSE::SHADER_SKY, "motion_vectors_transparent"));
		CHECK_FALSE(has_mode(RSE::SHADER_FOG, "motion_vectors_transparent"));

		String error;
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
render_mode blend_mix, depth_draw_always, cull_disabled, motion_vectors_transparent;
void fragment() {
	ALBEDO = vec3(0.1, 0.3, 0.4);
	ALPHA = 0.8;
}
)",
					  &error) == OK);
		CHECK_MESSAGE(error.is_empty(), error);

		CHECK(compile_shader(RSE::SHADER_CANVAS_ITEM, R"(
shader_type canvas_item;
render_mode motion_vectors_transparent;
void fragment() {
	COLOR = vec4(1.0);
}
)") != OK);

		// A mode that does not exist is still rejected (the patch did not loosen render mode validation).
		// Note: upstream matches option-less modes by prefix, so "motion_vectors_transparent_x" would be accepted.
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
render_mode motion_vector_transparent;
void fragment() {}
)") != OK);
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
render_mode opaque_lat;
void fragment() {}
)") != OK);
	}

	TEST_CASE("[Mariner][E1] Forward+ compiler flags and renames for PREVIOUS_FRAME and motion_vectors_transparent") {
		ForwardPlusFlags flags;
		ShaderCompiler::GeneratedCode gen_code;
		REQUIRE(compile_forward_plus(R"(
shader_type spatial;
render_mode depth_draw_always, motion_vectors_transparent;
uniform float wave_time;
uniform float wave_time_prev;
void vertex() {
	float t = PREVIOUS_FRAME ? wave_time_prev : wave_time;
	VERTEX.y += sin(VERTEX.x + t);
}
void fragment() {
	ALBEDO = vec3(0.1, 0.3, 0.4);
	ALPHA = 0.8;
}
)",
						flags, gen_code) == OK);

		CHECK(flags.uses_previous_frame);
		CHECK(flags.uses_transparent_motion_vectors);
		CHECK_FALSE(flags.uses_opaque_late);
		REQUIRE(gen_code.code.has("vertex"));
		CHECK(gen_code.code["vertex"].contains("is_prev_frame"));
		CHECK_FALSE(gen_code.code["vertex"].contains("PREVIOUS_FRAME"));

		// Without the render mode / built-in the flags stay off.
		ForwardPlusFlags plain_flags;
		ShaderCompiler::GeneratedCode plain_code;
		REQUIRE(compile_forward_plus(R"(
shader_type spatial;
render_mode depth_prepass_alpha;
void vertex() {
	VERTEX.y += 1.0;
}
void fragment() {
	ALBEDO = vec3(1.0);
	ALPHA = 0.5;
}
)",
						plain_flags, plain_code) == OK);
		CHECK_FALSE(plain_flags.uses_previous_frame);
		CHECK_FALSE(plain_flags.uses_transparent_motion_vectors);
		CHECK(plain_flags.uses_depth_prepass_alpha);
		CHECK(plain_flags.uses_alpha);
	}

	TEST_CASE("[Mariner][E4] render_mode opaque_late is spatial-only and parses with screen/depth textures") {
		CHECK(has_mode(RSE::SHADER_SPATIAL, "opaque_late"));
		CHECK_FALSE(has_mode(RSE::SHADER_CANVAS_ITEM, "opaque_late"));
		CHECK_FALSE(has_mode(RSE::SHADER_PARTICLES, "opaque_late"));
		CHECK_FALSE(has_mode(RSE::SHADER_SKY, "opaque_late"));
		CHECK_FALSE(has_mode(RSE::SHADER_FOG, "opaque_late"));

		// Upstream spatial render modes around the inserted ones are still registered.
		CHECK(has_mode(RSE::SHADER_SPATIAL, "depth_prepass_alpha"));
		CHECK(has_mode(RSE::SHADER_SPATIAL, "depth_test"));
		CHECK(has_mode(RSE::SHADER_SPATIAL, "sss_mode_skin"));
		CHECK(has_mode(RSE::SHADER_SPATIAL, "cull"));

		String error;
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
render_mode opaque_late, cull_back;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;
void fragment() {
	float d = texture(depth_tex, SCREEN_UV).r;
	ALBEDO = mix(texture(screen_tex, SCREEN_UV).rgb, vec3(0.0, 0.2, 0.3), d);
}
)",
					  &error) == OK);
		CHECK_MESSAGE(error.is_empty(), error);

		// Both Mariner modes together (late opaque water that also writes motion vectors).
		CHECK(compile_shader(RSE::SHADER_SPATIAL, R"(
shader_type spatial;
render_mode opaque_late, motion_vectors_transparent;
void vertex() {
	VERTEX.y += PREVIOUS_FRAME ? 0.0 : 1.0;
}
)") == OK);

		CHECK(compile_shader(RSE::SHADER_CANVAS_ITEM, R"(
shader_type canvas_item;
render_mode opaque_late;
void fragment() {
	COLOR = vec4(1.0);
}
)") != OK);
	}

	TEST_CASE("[Mariner][E4] Forward+ classification: screen texture + opaque_late stays opaque") {
		const String water = R"(
shader_type spatial;
render_mode %s;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;
void fragment() {
	float d = texture(depth_tex, SCREEN_UV).r;
	ALBEDO = mix(texture(screen_tex, SCREEN_UV).rgb, vec3(0.0, 0.2, 0.3), d);
}
)";

		ForwardPlusFlags late;
		ShaderCompiler::GeneratedCode late_code;
		REQUIRE(compile_forward_plus(vformat(water, "opaque_late"), late, late_code) == OK);
		CHECK(late.uses_opaque_late);
		CHECK(late.uses_screen_texture);
		CHECK(late.uses_depth_texture);
		CHECK_FALSE(late.forced_transparent());

		ForwardPlusFlags stock;
		ShaderCompiler::GeneratedCode stock_code;
		REQUIRE(compile_forward_plus(vformat(water, "cull_back"), stock, stock_code) == OK);
		CHECK_FALSE(stock.uses_opaque_late);
		CHECK(stock.uses_screen_texture);
		CHECK(stock.forced_transparent());

		// opaque_late does not make real alpha blending opaque.
		ForwardPlusFlags late_alpha;
		ShaderCompiler::GeneratedCode late_alpha_code;
		REQUIRE(compile_forward_plus(R"(
shader_type spatial;
render_mode opaque_late;
void fragment() {
	ALBEDO = vec3(1.0);
	ALPHA = 0.5;
}
)",
						late_alpha, late_alpha_code) == OK);
		CHECK(late_alpha.uses_opaque_late);
		CHECK(late_alpha.uses_alpha);
		CHECK(late_alpha.forced_transparent());
	}
}

} // namespace TestMarinerShaders
