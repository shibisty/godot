/**************************************************************************/
/*  d3d12_spirv_to_hlsl.cpp                                               */
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

#include "d3d12_spirv_to_hlsl.h"

#include <spirv_hlsl.hpp>

#include <exception>

using namespace spirv_cross;

bool d3d12_spirv_to_hlsl(RenderingDeviceCommons::ShaderStage p_stage, const uint32_t *p_words, size_t p_word_count, uint32_t p_set_multiplier, uint32_t p_binding_multiplier, uint32_t p_push_constant_register, uint32_t p_shader_model, D3D12SpirvToHlslResult &r_result) {
	try {
		CompilerHLSL compiler(p_words, p_word_count);

		CompilerGLSL::Options common_options = compiler.get_common_options();
		// Vulkan's clip space has Y down, Direct3D's up (the D3D12 driver flips the vertex stage the same way through
		// Mesa's runtime configuration).
		common_options.vertex.flip_vert_y = p_stage == RenderingDeviceCommons::SHADER_STAGE_MESH || p_stage == RenderingDeviceCommons::SHADER_STAGE_VERTEX;
		compiler.set_common_options(common_options);

		CompilerHLSL::Options hlsl_options;
		hlsl_options.shader_model = p_shader_model;
		hlsl_options.point_size_compat = true;
		hlsl_options.point_coord_compat = true;
		compiler.set_hlsl_options(hlsl_options);

		const spv::ExecutionModel model = compiler.get_execution_model();

		// Registers of every resource, used or not, as the root signature gives them.
		const ShaderResources all_resources = compiler.get_shader_resources();
		auto add_binding = [&](const Resource &p_resource) {
			const uint32_t set = compiler.get_decoration(p_resource.id, spv::DecorationDescriptorSet);
			const uint32_t binding = compiler.get_decoration(p_resource.id, spv::DecorationBinding);
			const uint32_t reg = set * p_set_multiplier + binding * p_binding_multiplier;
			HLSLResourceBinding hlsl_binding;
			hlsl_binding.stage = model;
			hlsl_binding.desc_set = set;
			hlsl_binding.binding = binding;
			hlsl_binding.cbv.register_binding = reg;
			hlsl_binding.uav.register_binding = reg;
			hlsl_binding.srv.register_binding = reg;
			hlsl_binding.sampler.register_binding = reg;
			compiler.add_hlsl_resource_binding(hlsl_binding);
		};
		for (const SmallVector<Resource> *list : { &all_resources.uniform_buffers, &all_resources.storage_buffers, &all_resources.sampled_images, &all_resources.separate_images, &all_resources.storage_images, &all_resources.separate_samplers, &all_resources.acceleration_structures }) {
			for (const Resource &resource : *list) {
				add_binding(resource);
			}
		}
		if (!all_resources.push_constant_buffers.empty()) {
			HLSLResourceBinding push_constants;
			push_constants.stage = model;
			push_constants.desc_set = ResourceBindingPushConstantDescriptorSet;
			push_constants.binding = ResourceBindingPushConstantBinding;
			push_constants.cbv.register_binding = p_push_constant_register;
			compiler.add_hlsl_resource_binding(push_constants);
		}

		// What the stage actually uses (the root signature's visibility and the driver's descriptor tables).
		const ShaderResources active_resources = compiler.get_shader_resources(compiler.get_active_interface_variables());
		for (const SmallVector<Resource> *list : { &active_resources.uniform_buffers, &active_resources.storage_buffers, &active_resources.sampled_images, &active_resources.separate_images, &active_resources.storage_images, &active_resources.separate_samplers, &active_resources.acceleration_structures }) {
			for (const Resource &resource : *list) {
				const uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
				const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				const uint32_t key = (set << 16) | binding;
				if (r_result.used_bindings.find(key) < 0) {
					r_result.used_bindings.push_back(key);
				}
			}
		}
		r_result.uses_push_constants = !active_resources.push_constant_buffers.empty();

		const std::string hlsl = compiler.compile();
		r_result.hlsl = String::utf8(hlsl.c_str(), hlsl.size());
		return true;
	} catch (const std::exception &e) {
		r_result.error = String::utf8(e.what());
		return false;
	}
}
