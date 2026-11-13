/**************************************************************************/
/*  test_mariner_e7b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e7b)

#include "modules/modules_enabled.gen.h" // For glslang.

#if defined(D3D12_ENABLED) && defined(MODULE_GLSLANG_ENABLED)

#include "core/os/os.h"
#include "drivers/d3d12/d3d12_spirv_to_hlsl.h"
#include "drivers/d3d12/rendering_shader_container_d3d12.h"
#include "servers/rendering/rendering_device.h"

#include "modules/glslang/shader_compile.h"

// Mariner engine patch E7b (mesh shaders on Direct3D 12): the stages of a mesh pipeline go SPIR-V -> HLSL
// (SPIRV-Cross) -> DXIL (DXC, dxcompiler.dll). The HLSL gets the D3D12 driver's registers and the Y flip, the container
// gets the DXIL of each stage and the resources each stage uses (root signature). Drawing is validated on the PC
// (engine/tests/e7/mesh_shader.gd with -Driver d3d12).

namespace TestMarinerE7b {

using RD = RenderingDevice;

static const char *TASK_GLSL = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
struct Payload {
	uint first;
};
taskPayloadSharedEXT Payload payload;
void main() {
	payload.first = gl_WorkGroupID.x * 2u;
	EmitMeshTasksEXT(2, 1, 1);
}
)";

static const char *MESH_GLSL = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles, max_vertices = 4, max_primitives = 2) out;
struct Payload {
	uint first;
};
taskPayloadSharedEXT Payload payload;
struct Quad {
	vec4 rect;
	vec4 color;
};
layout(set = 1, binding = 2, std430) restrict readonly buffer Quads {
	Quad quads[];
};
layout(push_constant, std430) uniform Params {
	vec4 tint;
} params;
layout(location = 0) out vec4 v_color[];
layout(location = 1) perprimitiveEXT flat out uint p_quad[];
void main() {
	uint index = payload.first + gl_WorkGroupID.x;
	Quad q = quads[index];
	SetMeshOutputsEXT(4, 2);
	gl_MeshVerticesEXT[0].gl_Position = vec4(q.rect.xy, 0.0, 1.0);
	gl_MeshVerticesEXT[1].gl_Position = vec4(q.rect.zy, 0.0, 1.0);
	gl_MeshVerticesEXT[2].gl_Position = vec4(q.rect.xw, 0.0, 1.0);
	gl_MeshVerticesEXT[3].gl_Position = vec4(q.rect.zw, 0.0, 1.0);
	for (int i = 0; i < 4; i++) {
		v_color[i] = q.color * params.tint;
	}
	gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
	gl_PrimitiveTriangleIndicesEXT[1] = uvec3(2, 1, 3);
	p_quad[0] = index;
	p_quad[1] = index;
}
)";

static const char *FRAGMENT_GLSL = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(location = 0) in vec4 v_color;
layout(location = 1) perprimitiveEXT flat in uint p_quad;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(v_color.rgb * texture(tex, vec2(0.5)).rgb, float(p_quad + 1u) / 255.0);
}
)";

static Vector<uint8_t> compile_stage(RD::ShaderStage p_stage, const char *p_source) {
	String error;
	// The D3D12 container's language and SPIR-V versions (Vulkan 1.1, SPIR-V 1.5).
	const Vector<uint8_t> spirv = compile_glslang_shader(p_stage, p_source, RD::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RD::SHADER_SPIRV_VERSION_1_5, &error);
	INFO(error);
	return spirv;
}

TEST_CASE("[Mariner][E7b] SPIR-V to HLSL: the D3D12 driver's registers, the Y flip of mesh positions") {
	const Vector<uint8_t> mesh = compile_stage(RD::SHADER_STAGE_MESH, MESH_GLSL);
	REQUIRE(mesh.size() > 0);
	D3D12SpirvToHlslResult result;
	const bool ok = d3d12_spirv_to_hlsl(RD::SHADER_STAGE_MESH, (const uint32_t *)mesh.ptr(), mesh.size() / 4, 100000000, 100000, 1234, 65, result);
	INFO(result.error);
	REQUIRE(ok);
	// Set 1, binding 2: register 1 * 100000000 + 2 * 100000; push constants at the given register.
	CHECK(result.hlsl.contains("register(t100200000, space0)"));
	CHECK(result.hlsl.contains("register(b1234, space0)"));
	CHECK(result.hlsl.contains("spvFlipVertY"));
	CHECK(result.hlsl.contains("SetMeshOutputCounts"));
	REQUIRE(result.used_bindings.size() == 1);
	CHECK(result.used_bindings[0] == ((1u << 16) | 2u));
	CHECK(result.uses_push_constants);

	// Invalid SPIR-V: an error, not an exception or a crash.
	const uint32_t garbage[] = { 0x07230203, 0x00010500, 0, 1, 0 };
	D3D12SpirvToHlslResult bad;
	CHECK_FALSE(d3d12_spirv_to_hlsl(RD::SHADER_STAGE_MESH, garbage, 5, 100000000, 100000, 1234, 65, bad));
	CHECK_FALSE(bad.error.is_empty());
}

TEST_CASE("[Mariner][E7b] D3D12 shader container: a mesh pipeline compiled by DXC") {
	if (!RenderingShaderContainerD3D12::is_dxc_available()) {
		MESSAGE("dxcompiler.dll not found next to the executable: skipped.");
		return;
	}
	// d3d12.dll of the system (open_dynamic_library() wants an existing path), or one next to the executable.
	void *lib_d3d12 = nullptr;
	const String system_d3d12 = OS::get_singleton()->get_environment("WINDIR").path_join("System32").path_join("d3d12.dll");
	if (OS::get_singleton()->open_dynamic_library(system_d3d12, lib_d3d12) != OK) {
		const String local_d3d12 = OS::get_singleton()->get_executable_path().get_base_dir().path_join("d3d12.dll");
		REQUIRE(OS::get_singleton()->open_dynamic_library(local_d3d12, lib_d3d12) == OK);
	}
	REQUIRE(lib_d3d12 != nullptr);

	Vector<RD::ShaderStageSPIRVData> stages;
	const RD::ShaderStage stage_kinds[3] = { RD::SHADER_STAGE_TASK, RD::SHADER_STAGE_MESH, RD::SHADER_STAGE_FRAGMENT };
	const char *stage_sources[3] = { TASK_GLSL, MESH_GLSL, FRAGMENT_GLSL };
	for (int i = 0; i < 3; i++) {
		RD::ShaderStageSPIRVData stage_data;
		stage_data.shader_stage = stage_kinds[i];
		stage_data.spirv = compile_stage(stage_kinds[i], stage_sources[i]);
		REQUIRE(stage_data.spirv.size() > 0);
		stages.push_back(stage_data);
	}

	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	Ref<RenderingShaderContainer> container = format.create_container();
	REQUIRE(container.is_valid());
	REQUIRE(container->set_code_from_spirv("mariner_e7b_mesh", stages));

	// DXIL containers for the three stages.
	REQUIRE(container->shaders.size() == 3);
	for (const RenderingShaderContainer::Shader &shader : container->shaders) {
		INFO(RD::SHADER_STAGE_NAMES[shader.shader_stage]);
		Vector<uint8_t> code;
		code.resize(shader.code_decompressed_size);
		REQUIRE(container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags, code.ptrw(), code.size()));
		REQUIRE(code.size() > 32);
		CHECK(memcmp(code.ptr(), "DXBC", 4) == 0);
		// Signed (the hash isn't zero).
		bool hashed = false;
		for (int i = 4; i < 20; i++) {
			hashed = hashed || code[i] != 0;
		}
		CHECK(hashed);
	}

	const RenderingShaderContainerD3D12 *container_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(container.ptr());
	REQUIRE(container_d3d12 != nullptr);
	const RenderingShaderContainerD3D12::ShaderReflectionD3D12 reflection = container_d3d12->get_shader_reflection_d3d12();
	CHECK(reflection.dxil_push_constant_stages == uint32_t(RD::SHADER_STAGE_MESH_BIT));
	CHECK(reflection.nir_runtime_data_root_param_idx == UINT32_MAX);
	CHECK(reflection.root_signature_bytes.size() > 0);
	REQUIRE(reflection.reflection_binding_set_uniforms_d3d12.size() == 2);
	// Set 0: the fragment stage's texture and sampler.
	REQUIRE(reflection.reflection_binding_set_uniforms_d3d12[0].size() == 1);
	CHECK(reflection.reflection_binding_set_uniforms_d3d12[0][0].dxil_stages == uint32_t(RD::SHADER_STAGE_FRAGMENT_BIT));
	CHECK(reflection.reflection_binding_set_uniforms_d3d12[0][0].resource_class == RES_CLASS_SRV);
	CHECK(reflection.reflection_binding_set_uniforms_d3d12[0][0].has_sampler);
	// Set 1: the mesh stage's storage buffer.
	REQUIRE(reflection.reflection_binding_set_uniforms_d3d12[1].size() == 1);
	CHECK(reflection.reflection_binding_set_uniforms_d3d12[1][0].dxil_stages == uint32_t(RD::SHADER_STAGE_MESH_BIT));
	CHECK(reflection.reflection_binding_set_uniforms_d3d12[1][0].resource_class == RES_CLASS_SRV);

	// Serialization keeps it.
	Ref<RenderingShaderContainer> loaded = format.create_container();
	REQUIRE(loaded->from_bytes(container->to_bytes()));
	CHECK(loaded->shaders.size() == 3);
	CHECK(uint32_t(loaded->get_shader_reflection().stages_bits) == uint32_t(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_FRAGMENT_BIT));
}

} // namespace TestMarinerE7b

#endif // D3D12_ENABLED && MODULE_GLSLANG_ENABLED
