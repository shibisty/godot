/**************************************************************************/
/*  test_mariner_e24c.cpp                                                 */
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

TEST_FORCE_LINK(test_mariner_e24c)

#include "modules/modules_enabled.gen.h" // For glslang.

#if defined(D3D12_ENABLED) && defined(MODULE_GLSLANG_ENABLED)

#include "core/os/os.h"
#include "drivers/d3d12/rendering_shader_container_d3d12.h"
#include "servers/rendering/rendering_device.h"

#include "modules/glslang/shader_compile.h"

// Mariner engine patch E24c (FSR 3 frame generation on Direct3D 12): the D3D12 driver encodes a register as
// set * 10^8 + binding * 10^5 (the multipliers are compiled into Mesa too), so binding 1000 read back as the next set
// and the FidelityFX samplers (bindings 1000 and 1001 upstream) crashed the container (an out of bounds Vector access
// in _nir_report_resource()). The samplers are at 900 and 901 now, and a binding over the limit is an error.
// The extra present of the generated frame is validated by tests/e24/fg.gd with -Driver d3d12.

namespace TestMarinerE24c {

using RD = RenderingDevice;

static String compute_source(int p_sampler_binding) {
	return vformat(R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D src;
layout(set = 0, binding = %d) uniform sampler smp;
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	imageStore(dst, p, textureLod(sampler2D(src, smp), (vec2(p) + 0.5) / 64.0, 0.0));
}
)",
			p_sampler_binding);
}

static Vector<RD::ShaderStageSPIRVData> compute_stages(int p_sampler_binding) {
	String error;
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = compile_glslang_shader(RD::SHADER_STAGE_COMPUTE, compute_source(p_sampler_binding), RD::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RD::SHADER_SPIRV_VERSION_1_5, &error);
	INFO(error);
	Vector<RD::ShaderStageSPIRVData> stages;
	if (stage.spirv.size() > 0) {
		stages.push_back(stage);
	}
	return stages;
}

TEST_CASE("[Mariner][E24c] D3D12 shader container: sampler binding 999 compiles, 1000 is an error, not a crash") {
	void *lib_d3d12 = nullptr;
	const String system_d3d12 = OS::get_singleton()->get_environment("WINDIR").path_join("System32").path_join("d3d12.dll");
	if (OS::get_singleton()->open_dynamic_library(system_d3d12, lib_d3d12) != OK) {
		const String local_d3d12 = OS::get_singleton()->get_executable_path().get_base_dir().path_join("d3d12.dll");
		REQUIRE(OS::get_singleton()->open_dynamic_library(local_d3d12, lib_d3d12) == OK);
	}
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);

	const Vector<RD::ShaderStageSPIRVData> ok_stages = compute_stages(999);
	REQUIRE(ok_stages.size() == 1);
	Ref<RenderingShaderContainer> ok_container = format.create_container();
	REQUIRE(ok_container->set_code_from_spirv("mariner_e24c_999", ok_stages));
	const RenderingShaderContainerD3D12 *container_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(ok_container.ptr());
	REQUIRE(container_d3d12 != nullptr);
	const RenderingShaderContainerD3D12::ShaderReflectionD3D12 reflection = container_d3d12->get_shader_reflection_d3d12();
	REQUIRE(reflection.reflection_binding_set_uniforms_d3d12.size() == 1);
	REQUIRE(reflection.reflection_binding_set_uniforms_d3d12[0].size() == 3);
	bool sampler_seen = false;
	for (const RenderingShaderContainerD3D12::ReflectionBindingDataD3D12 &uniform : reflection.reflection_binding_set_uniforms_d3d12[0]) {
		sampler_seen = sampler_seen || uniform.has_sampler;
		CHECK(uniform.dxil_stages == uint32_t(RD::SHADER_STAGE_COMPUTE_BIT));
	}
	CHECK(sampler_seen);

	const Vector<RD::ShaderStageSPIRVData> bad_stages = compute_stages(1000);
	REQUIRE(bad_stages.size() == 1);
	Ref<RenderingShaderContainer> bad_container = format.create_container();
	ERR_PRINT_OFF;
	const bool bad_ok = bad_container->set_code_from_spirv("mariner_e24c_1000", bad_stages);
	ERR_PRINT_ON;
	CHECK_FALSE(bad_ok);
}

} // namespace TestMarinerE24c

#endif // D3D12_ENABLED && MODULE_GLSLANG_ENABLED
