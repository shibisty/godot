/**************************************************************************/
/*  d3d12_spirv_to_hlsl.h                                                 */
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

#pragma once

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device_commons.h"

// Mariner E7b: SPIR-V to HLSL (SPIRV-Cross) for the stages of mesh pipelines on Direct3D 12, which Mesa's SPIR-V to
// DXIL translation has no mesh / amplification stages for. The HLSL is compiled to DXIL by DXC
// (RenderingShaderContainerD3D12). Resources get the registers the rest of the D3D12 driver uses (space 0, register
// set * p_set_multiplier + binding * p_binding_multiplier, the push constants a cbuffer at p_push_constant_register).
// Compiled with exceptions (SPIRV-Cross reports errors with them), so it's a separate translation unit with a plain
// interface.

struct D3D12SpirvToHlslResult {
	String hlsl;
	String error;
	// Active resources of the stage: set << 16 | binding.
	LocalVector<uint32_t> used_bindings;
	bool uses_push_constants = false;
};

bool d3d12_spirv_to_hlsl(RenderingDeviceCommons::ShaderStage p_stage, const uint32_t *p_words, size_t p_word_count, uint32_t p_set_multiplier, uint32_t p_binding_multiplier, uint32_t p_push_constant_register, uint32_t p_shader_model, D3D12SpirvToHlslResult &r_result);
