/**************************************************************************/
/*  test_mariner_e5d.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e5d)

#include "servers/rendering/rendering_device.h"

#include "modules/modules_enabled.gen.h" // For glslang.

#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

// Mariner engine patch E5d (ray tracing on D3D12): the scene's mesh buffers are an array of storage buffers instead of
// buffer device addresses. Shader reflection gives each storage buffer binding its array size (array_count), kept by
// the shader container's serialization, and the waterfall loop the D3D12 scene shaders index the arrays with
// (RT_NONUNIFORM_WATERFALL) compiles. Drawing on D3D12 is validated under Wine + vkd3d-proton and on the PC.

namespace TestMarinerE5d {

using RD = RenderingDevice;

#ifdef MODULE_GLSLANG_ENABLED
static const char *ARRAYS_GLSL = R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
#ifdef WATERFALL
#extension GL_KHR_shader_subgroup_ballot : require
#endif
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) restrict readonly buffer Geometry {
	uint v[];
} geometry[16];
layout(set = 0, binding = 1, std430) restrict writeonly buffer Result {
	uint v[];
} result;
layout(set = 0, binding = 2) uniform sampler material_sampler;
layout(set = 1, binding = 0) uniform texture2D textures[];
uint load(uint p_buffer, uint p_word) {
#ifdef WATERFALL
	uint value = 0u;
	for (;;) {
		uint first = subgroupBroadcastFirst(p_buffer);
		if (first == p_buffer) {
			value = geometry[first].v[p_word];
			break;
		}
	}
	return value;
#else
	return geometry[nonuniformEXT(p_buffer)].v[p_word];
#endif
}
void main() {
	uint i = gl_GlobalInvocationID.x;
	uint t = load(i & 15u, i);
	result.v[i] = t + uint(textureLod(sampler2D(textures[nonuniformEXT(t)], material_sampler), vec2(0.5), 0.0).r * 255.0);
}
)";

static Vector<uint8_t> compile_arrays_shader(bool p_waterfall) {
	String source = ARRAYS_GLSL;
	if (p_waterfall) {
		source = source.replace("#version 450\n", "#version 450\n#define WATERFALL\n");
	}
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(RD::SHADER_STAGE_COMPUTE, source, RD::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RD::SHADER_SPIRV_VERSION_1_3, &error);
	INFO(error);
	return spirv;
}

TEST_CASE("[Mariner][E5d] Storage buffer arrays and the waterfall loop compile") {
	CHECK(compile_arrays_shader(false).size() > 0);
	CHECK(compile_arrays_shader(true).size() > 0);
}

#ifdef VULKAN_ENABLED
TEST_CASE("[Mariner][E5d] Shader reflection: array size of storage buffers, unbounded textures") {
	for (bool waterfall : { false, true }) {
		const String variant = waterfall ? "waterfall" : "nonuniformEXT";
		INFO(variant);
		RD::ShaderStageSPIRVData stage;
		stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
		stage.spirv = compile_arrays_shader(waterfall);
		REQUIRE(stage.spirv.size() > 0);

		RenderingShaderContainerFormatVulkan format;
		Ref<RenderingShaderContainer> container = format.create_container();
		REQUIRE(container.is_valid());
		REQUIRE(container->set_code_from_spirv("mariner_e5d_arrays", Vector<RD::ShaderStageSPIRVData>({ stage })));

		for (int pass = 0; pass < 2; pass++) {
			// Pass 1: the container after a serialization round trip.
			if (pass == 1) {
				Ref<RenderingShaderContainer> loaded = format.create_container();
				REQUIRE(loaded->from_bytes(container->to_bytes()));
				container = loaded;
			}
			const String state = pass == 0 ? "reflected" : "loaded";
			INFO(state);
			const RD::ShaderReflection reflection = container->get_shader_reflection();
			REQUIRE(reflection.uniform_sets.size() == 2);
			REQUIRE(reflection.uniform_sets[0].size() == 3);
			const RD::ShaderUniform &geometry = reflection.uniform_sets[0][0];
			CHECK(geometry.binding == 0);
			CHECK(geometry.type == RD::UNIFORM_TYPE_STORAGE_BUFFER);
			CHECK(geometry.array_count == 16);
			CHECK_FALSE(geometry.writable);
			const RD::ShaderUniform &result = reflection.uniform_sets[0][1];
			CHECK(result.binding == 1);
			CHECK(result.type == RD::UNIFORM_TYPE_STORAGE_BUFFER);
			CHECK(result.array_count == 1);
			REQUIRE(reflection.uniform_sets[1].size() == 1);
			const RD::ShaderUniform &textures = reflection.uniform_sets[1][0];
			CHECK(textures.type == RD::UNIFORM_TYPE_TEXTURE);
			CHECK(textures.unbounded);
			CHECK(textures.length == 0);
		}
	}
}

TEST_CASE("[Mariner][E5d] Uniforms of different array sizes differ") {
	RD::ShaderUniform a;
	a.type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	a.binding = 30;
	RD::ShaderUniform b = a;
	CHECK_FALSE(a != b);
	b.array_count = 4096;
	CHECK(a != b);
	CHECK(a < b);
	CHECK_FALSE(b < a);
}
#endif // VULKAN_ENABLED
#endif // MODULE_GLSLANG_ENABLED

} // namespace TestMarinerE5d
