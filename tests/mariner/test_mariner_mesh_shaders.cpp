/**************************************************************************/
/*  test_mariner_mesh_shaders.cpp                                         */
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

TEST_FORCE_LINK(test_mariner_mesh_shaders)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_device_graph.h"

#include "modules/modules_enabled.gen.h" // For glslang.

#ifdef VULKAN_ENABLED
#include "thirdparty/misc/smolv.h"

#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

// Mariner engine patch E7 (mesh / task shaders): enum values and bindings, the stage rules of mesh pipelines, shader
// file sections (#[task], #[mesh]) compiled by glslang to SPIR-V with the EXT mesh execution models, the shader
// container (reflection, stages, serialization) and the "unsupported" path of a RenderingDevice without a driver.
// Drawing (VK_EXT_mesh_shader) needs a GPU and is validated on the PC (engine/README.md).

namespace TestMarinerMeshShaders {

using RD = RenderingDevice;

#ifdef MODULE_GLSLANG_ENABLED
static const char *TASK_GLSL = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
struct Payload {
	uint ids[32];
};
taskPayloadSharedEXT Payload payload;
layout(set = 0, binding = 0, std430) restrict readonly buffer Instances {
	vec4 data[];
} instances;
layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
} params;
void main() {
	payload.ids[gl_LocalInvocationIndex] = gl_GlobalInvocationID.x;
	EmitMeshTasksEXT(min(params.count, 32u), 1, 1);
}
)";

static const char *MESH_GLSL = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;
struct Payload {
	uint ids[32];
};
taskPayloadSharedEXT Payload payload;
layout(set = 0, binding = 0, std430) restrict readonly buffer Instances {
	vec4 data[];
} instances;
layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
} params;
layout(location = 0) out vec3 v_color[];
void main() {
	vec4 p = instances.data[payload.ids[gl_WorkGroupID.x]];
	SetMeshOutputsEXT(3, 1);
	gl_MeshVerticesEXT[0].gl_Position = vec4(p.xy, 0.0, 1.0);
	gl_MeshVerticesEXT[1].gl_Position = vec4(p.xy + vec2(p.z, 0.0), 0.0, 1.0);
	gl_MeshVerticesEXT[2].gl_Position = vec4(p.xy + vec2(0.0, p.z), 0.0, 1.0);
	v_color[0] = vec3(1.0, 0.0, 0.0);
	v_color[1] = vec3(0.0, 1.0, 0.0);
	v_color[2] = vec3(0.0, 0.0, 1.0);
	gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
)";

static const char *FRAGMENT_GLSL = R"(#version 450
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(v_color, 1.0);
}
)";

// SPIR-V execution models of the EXT mesh shading stages (SPV_EXT_mesh_shader).
static const uint32_t SPV_EXECUTION_MODEL_TASK_EXT = 5364;
static const uint32_t SPV_EXECUTION_MODEL_MESH_EXT = 5365;
static const uint32_t SPV_EXECUTION_MODEL_FRAGMENT = 4;

// Execution model of the first OpEntryPoint, or UINT32_MAX.
static uint32_t spirv_execution_model(const uint32_t *p_words, size_t p_word_count) {
	if (p_word_count < 5 || p_words[0] != 0x07230203) {
		return UINT32_MAX;
	}
	size_t i = 5;
	while (i < p_word_count) {
		const uint32_t word_count = p_words[i] >> 16;
		const uint32_t opcode = p_words[i] & 0xffff;
		if (word_count == 0) {
			break;
		}
		if (opcode == 15 /* OpEntryPoint */ && i + 1 < p_word_count) {
			return p_words[i + 1];
		}
		i += word_count;
	}
	return UINT32_MAX;
}

static uint32_t spirv_execution_model(const Vector<uint8_t> &p_spirv) {
	return spirv_execution_model(reinterpret_cast<const uint32_t *>(p_spirv.ptr()), p_spirv.size() / 4);
}

static String shader_file_text(bool p_task, bool p_mesh, bool p_vertex) {
	String text;
	if (p_task) {
		text += "#[task]\n\n" + String(TASK_GLSL) + "\n";
	}
	if (p_mesh) {
		text += "#[mesh]\n\n" + String(MESH_GLSL) + "\n";
	}
	if (p_vertex) {
		text += "#[vertex]\n\n#version 450\nvoid main() {\n\tgl_Position = vec4(0.0);\n}\n\n";
	}
	text += "#[fragment]\n\n" + String(FRAGMENT_GLSL);
	return text;
}
#endif // MODULE_GLSLANG_ENABLED

TEST_CASE("[Mariner][E7] Shader stages, features and limits are appended") {
	// Stages: appended after the ray tracing ones, the older values are unchanged.
	CHECK(int(RD::SHADER_STAGE_VERTEX) == 0);
	CHECK(int(RD::SHADER_STAGE_FRAGMENT) == 1);
	CHECK(int(RD::SHADER_STAGE_COMPUTE) == 4);
	CHECK(int(RD::SHADER_STAGE_INTERSECTION) == 9);
	CHECK(int(RD::SHADER_STAGE_TASK) == 10);
	CHECK(int(RD::SHADER_STAGE_MESH) == 11);
	CHECK(int(RD::SHADER_STAGE_MAX) == 12);
	CHECK(int(RD::SHADER_STAGE_TASK_BIT) == 1024);
	CHECK(int(RD::SHADER_STAGE_MESH_BIT) == 2048);
	CHECK(String(RD::SHADER_STAGE_NAMES[RD::SHADER_STAGE_TASK]) == "Task");
	CHECK(String(RD::SHADER_STAGE_NAMES[RD::SHADER_STAGE_MESH]) == "Mesh");

	// Features: appended after SUPPORTS_HDR_OUTPUT.
	CHECK(int(RD::SUPPORTS_RAYTRACING_PIPELINE) == 12);
	CHECK(int(RD::SUPPORTS_HDR_OUTPUT) == 13);
	CHECK(int(RD::SUPPORTS_MESH_SHADER) == 14);
	CHECK(int(RD::SUPPORTS_MESH_SHADER_INDIRECT_COUNT) == 15);

	// Limits: appended after LIMIT_MAX_SHADER_VARYINGS (48), consecutive.
	CHECK(int(RD::LIMIT_METALFX_TEMPORAL_SCALER_MIN_SCALE) == 46);
	CHECK(int(RD::LIMIT_MAX_SHADER_VARYINGS) == 48);
	CHECK(int(RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_X) == 49);
	CHECK(int(RD::LIMIT_MAX_TASK_PAYLOAD_SIZE) == 57);
	CHECK(int(RD::LIMIT_MAX_MESH_WORKGROUP_COUNT_X) == 58);
	CHECK(int(RD::LIMIT_MAX_MESH_OUTPUT_VERTICES) == 66);
	CHECK(int(RD::LIMIT_MAX_MESH_OUTPUT_PRIMITIVES) == 67);
	CHECK(int(RD::LIMIT_MAX_MESH_PREFERRED_MESH_WORKGROUP_INVOCATIONS) == 70);

	// Bound in ClassDB, in their enums.
	struct Constant {
		const char *name;
		int64_t value;
		const char *enum_name;
	};
	const Constant constants[] = {
		{ "SHADER_STAGE_TASK", RD::SHADER_STAGE_TASK, "ShaderStage" },
		{ "SHADER_STAGE_MESH", RD::SHADER_STAGE_MESH, "ShaderStage" },
		{ "SHADER_STAGE_MAX", RD::SHADER_STAGE_MAX, "ShaderStage" },
		{ "SHADER_STAGE_TASK_BIT", RD::SHADER_STAGE_TASK_BIT, "ShaderStage" },
		{ "SHADER_STAGE_MESH_BIT", RD::SHADER_STAGE_MESH_BIT, "ShaderStage" },
		{ "SUPPORTS_MESH_SHADER", RD::SUPPORTS_MESH_SHADER, "Features" },
		{ "SUPPORTS_MESH_SHADER_INDIRECT_COUNT", RD::SUPPORTS_MESH_SHADER_INDIRECT_COUNT, "Features" },
		{ "LIMIT_MAX_TASK_WORKGROUP_COUNT_X", RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_X, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_COUNT_Y", RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_Y, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_COUNT_Z", RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_Z, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT", RD::LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_INVOCATIONS", RD::LIMIT_MAX_TASK_WORKGROUP_INVOCATIONS, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_SIZE_X", RD::LIMIT_MAX_TASK_WORKGROUP_SIZE_X, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_SIZE_Y", RD::LIMIT_MAX_TASK_WORKGROUP_SIZE_Y, "Limit" },
		{ "LIMIT_MAX_TASK_WORKGROUP_SIZE_Z", RD::LIMIT_MAX_TASK_WORKGROUP_SIZE_Z, "Limit" },
		{ "LIMIT_MAX_TASK_PAYLOAD_SIZE", RD::LIMIT_MAX_TASK_PAYLOAD_SIZE, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_COUNT_X", RD::LIMIT_MAX_MESH_WORKGROUP_COUNT_X, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_COUNT_Y", RD::LIMIT_MAX_MESH_WORKGROUP_COUNT_Y, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_COUNT_Z", RD::LIMIT_MAX_MESH_WORKGROUP_COUNT_Z, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT", RD::LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_INVOCATIONS", RD::LIMIT_MAX_MESH_WORKGROUP_INVOCATIONS, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_SIZE_X", RD::LIMIT_MAX_MESH_WORKGROUP_SIZE_X, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_SIZE_Y", RD::LIMIT_MAX_MESH_WORKGROUP_SIZE_Y, "Limit" },
		{ "LIMIT_MAX_MESH_WORKGROUP_SIZE_Z", RD::LIMIT_MAX_MESH_WORKGROUP_SIZE_Z, "Limit" },
		{ "LIMIT_MAX_MESH_OUTPUT_VERTICES", RD::LIMIT_MAX_MESH_OUTPUT_VERTICES, "Limit" },
		{ "LIMIT_MAX_MESH_OUTPUT_PRIMITIVES", RD::LIMIT_MAX_MESH_OUTPUT_PRIMITIVES, "Limit" },
		{ "LIMIT_MAX_MESH_SHARED_MEMORY_SIZE", RD::LIMIT_MAX_MESH_SHARED_MEMORY_SIZE, "Limit" },
		{ "LIMIT_MAX_MESH_PREFERRED_TASK_WORKGROUP_INVOCATIONS", RD::LIMIT_MAX_MESH_PREFERRED_TASK_WORKGROUP_INVOCATIONS, "Limit" },
		{ "LIMIT_MAX_MESH_PREFERRED_MESH_WORKGROUP_INVOCATIONS", RD::LIMIT_MAX_MESH_PREFERRED_MESH_WORKGROUP_INVOCATIONS, "Limit" },
	};
	for (const Constant &c : constants) {
		INFO(c.name);
		bool ok = false;
		CHECK(ClassDB::get_integer_constant("RenderingDevice", c.name, &ok) == c.value);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant_enum("RenderingDevice", c.name) == StringName(c.enum_name));
	}
	// 22 consecutive values (the enum has no gaps between the first and the last mesh limit).
	CHECK(int(RD::LIMIT_MAX_MESH_PREFERRED_MESH_WORKGROUP_INVOCATIONS) - int(RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_X) + 1 == 22);

	// Driver pipeline stages match VK_PIPELINE_STAGE_TASK/MESH_SHADER_BIT_EXT and don't overlap the others.
	CHECK(uint32_t(RDD::PIPELINE_STAGE_TASK_SHADER_BIT) == 0x00080000u);
	CHECK(uint32_t(RDD::PIPELINE_STAGE_MESH_SHADER_BIT) == 0x00100000u);
	const uint32_t others[] = { RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT, RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT, RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT,
		RDD::PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT, RDD::PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT, RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT };
	for (uint32_t other : others) {
		CHECK((other & (RDD::PIPELINE_STAGE_TASK_SHADER_BIT | RDD::PIPELINE_STAGE_MESH_SHADER_BIT)) == 0);
	}
}

TEST_CASE("[Mariner][E7] Mesh pipeline stage rules") {
	typedef BitField<RD::ShaderStage> Stages;
	CHECK(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_VERTEX_BIT | RD::SHADER_STAGE_FRAGMENT_BIT)).is_empty());
	CHECK(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_COMPUTE_BIT)).is_empty());
	CHECK(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_FRAGMENT_BIT)).is_empty());
	CHECK(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_FRAGMENT_BIT)).is_empty());
	CHECK(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT)).is_empty()); // Depth-only.

	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_FRAGMENT_BIT)).is_empty());
	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_VERTEX_BIT | RD::SHADER_STAGE_FRAGMENT_BIT)).is_empty());
	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_TESSELATION_CONTROL_BIT)).is_empty());
	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_TESSELATION_EVALUATION_BIT)).is_empty());
	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_COMPUTE_BIT)).is_empty());
	CHECK_FALSE(RD::validate_mesh_shader_stages(Stages(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_RAYGEN_BIT)).is_empty());
}

TEST_CASE("[Mariner][E7] RDShaderSource and RDShaderSPIRV have task and mesh stages") {
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RD::SHADER_STAGE_TASK, "task code");
	source->set_stage_source(RD::SHADER_STAGE_MESH, "mesh code");
	CHECK(source->get_stage_source(RD::SHADER_STAGE_TASK) == "task code");
	CHECK(String(source->get("source_task")) == "task code");
	CHECK(String(source->get("source_mesh")) == "mesh code");
	source->set("source_mesh", "other");
	CHECK(source->get_stage_source(RD::SHADER_STAGE_MESH) == "other");
	CHECK(source->get_stage_source(RD::SHADER_STAGE_VERTEX).is_empty());

	Ref<RDShaderSPIRV> spirv;
	spirv.instantiate();
	Vector<uint8_t> bytes;
	bytes.push_back(1);
	bytes.push_back(2);
	spirv->set("bytecode_task", bytes);
	spirv->set("compile_error_mesh", "broken");
	CHECK(spirv->get_stage_bytecode(RD::SHADER_STAGE_TASK) == bytes);
	CHECK(spirv->get_stage_compile_error(RD::SHADER_STAGE_MESH) == "broken");
	CHECK(spirv->get_stage_bytecode(RD::SHADER_STAGE_MESH).is_empty());

	// Shader file sections.
	CHECK(String(RDShaderFile::SHADER_FILE_STAGE_SECTIONS[RD::SHADER_STAGE_TASK]) == "task");
	CHECK(String(RDShaderFile::SHADER_FILE_STAGE_SECTIONS[RD::SHADER_STAGE_MESH]) == "mesh");
	CHECK(String(RDShaderFile::SHADER_FILE_STAGE_SECTIONS[RD::SHADER_STAGE_INTERSECTION]) == "intersection");
	for (int i = 0; i < RD::SHADER_STAGE_MAX; i++) {
		CHECK(RDShaderFile::SHADER_FILE_STAGE_SECTIONS[i] != nullptr);
	}
}

#ifdef MODULE_GLSLANG_ENABLED
TEST_CASE("[Mariner][E7] Shader files with #[task] / #[mesh] compile to EXT mesh shading SPIR-V") {
	Ref<RDShaderFile> file;
	file.instantiate();
	REQUIRE(file->parse_versions_from_text(shader_file_text(true, true, false)) == OK);
	CHECK(file->get_base_error().is_empty());
	Ref<RDShaderSPIRV> spirv = file->get_spirv();
	REQUIRE(spirv.is_valid());
	CHECK(spirv->get_stage_compile_error(RD::SHADER_STAGE_TASK).is_empty());
	CHECK(spirv->get_stage_compile_error(RD::SHADER_STAGE_MESH).is_empty());
	CHECK(spirv->get_stage_compile_error(RD::SHADER_STAGE_FRAGMENT).is_empty());
	CHECK(spirv_execution_model(spirv->get_stage_bytecode(RD::SHADER_STAGE_TASK)) == SPV_EXECUTION_MODEL_TASK_EXT);
	CHECK(spirv_execution_model(spirv->get_stage_bytecode(RD::SHADER_STAGE_MESH)) == SPV_EXECUTION_MODEL_MESH_EXT);
	CHECK(spirv_execution_model(spirv->get_stage_bytecode(RD::SHADER_STAGE_FRAGMENT)) == SPV_EXECUTION_MODEL_FRAGMENT);
	CHECK(spirv->get_stage_bytecode(RD::SHADER_STAGE_VERTEX).is_empty());

	// Mesh without a task stage.
	REQUIRE(file->parse_versions_from_text(shader_file_text(false, true, false)) == OK);
	CHECK(file->get_spirv()->get_stage_bytecode(RD::SHADER_STAGE_TASK).is_empty());
	CHECK(spirv_execution_model(file->get_spirv()->get_stage_bytecode(RD::SHADER_STAGE_MESH)) == SPV_EXECUTION_MODEL_MESH_EXT);

	// Invalid stage combinations are file errors.
	ERR_PRINT_OFF;
	CHECK(file->parse_versions_from_text(shader_file_text(true, false, false)) == ERR_PARSE_ERROR);
	CHECK(file->get_base_error().contains("#[task]"));
	CHECK(file->parse_versions_from_text(shader_file_text(false, true, true)) == ERR_PARSE_ERROR);
	CHECK(file->get_base_error().contains("#[mesh]"));
	ERR_PRINT_ON;

	// A broken mesh stage reports its error on the mesh stage.
	const String broken = String("#[mesh]\n\n") + String(MESH_GLSL).replace("SetMeshOutputsEXT(3, 1);", "SetMeshOutputsEXT(3, 1)") + "\n#[fragment]\n\n" + FRAGMENT_GLSL;
	CHECK(file->parse_versions_from_text(broken) == ERR_PARSE_ERROR);
	CHECK_FALSE(file->get_spirv()->get_stage_compile_error(RD::SHADER_STAGE_MESH).is_empty());
	CHECK(file->get_spirv()->get_stage_compile_error(RD::SHADER_STAGE_FRAGMENT).is_empty());

	// The glslang stage table maps the new stages.
	String error;
	const Vector<uint8_t> task = compile_glslang_shader(RD::SHADER_STAGE_TASK, TASK_GLSL, RD::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RD::SHADER_SPIRV_VERSION_1_4, &error);
	CHECK_MESSAGE(error.is_empty(), error);
	CHECK(spirv_execution_model(task) == SPV_EXECUTION_MODEL_TASK_EXT);
}

#ifdef VULKAN_ENABLED
TEST_CASE("[Mariner][E7] Shader container: reflection and serialization of task / mesh stages") {
	Ref<RDShaderFile> file;
	file.instantiate();
	REQUIRE(file->parse_versions_from_text(shader_file_text(true, true, false)) == OK);
	Ref<RDShaderSPIRV> spirv = file->get_spirv();

	Vector<RD::ShaderStageSPIRVData> stages;
	for (RD::ShaderStage stage : { RD::SHADER_STAGE_TASK, RD::SHADER_STAGE_MESH, RD::SHADER_STAGE_FRAGMENT }) {
		RD::ShaderStageSPIRVData data;
		data.shader_stage = stage;
		data.spirv = spirv->get_stage_bytecode(stage);
		stages.push_back(data);
	}

	RenderingShaderContainerFormatVulkan format;
	Ref<RenderingShaderContainer> container = format.create_container();
	REQUIRE(container.is_valid());
	REQUIRE(container->set_code_from_spirv("mariner_mesh_test", stages));

	const RD::ShaderReflection reflection = container->get_shader_reflection();
	CHECK(reflection.pipeline_type == RD::PIPELINE_TYPE_RASTERIZATION);
	CHECK(uint32_t(reflection.stages_bits) == uint32_t(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_MESH_BIT | RD::SHADER_STAGE_FRAGMENT_BIT));
	REQUIRE(reflection.stages_vector.size() == 3);
	CHECK(reflection.stages_vector[0] == RD::SHADER_STAGE_FRAGMENT);
	CHECK(reflection.stages_vector[1] == RD::SHADER_STAGE_TASK);
	CHECK(reflection.stages_vector[2] == RD::SHADER_STAGE_MESH);
	CHECK(reflection.vertex_input_mask == 0);
	CHECK(reflection.fragment_output_mask == 1);
	CHECK(reflection.push_constant_size == 16);
	CHECK(reflection.push_constant_stages.has_flag(RD::SHADER_STAGE_TASK_BIT));
	CHECK(reflection.push_constant_stages.has_flag(RD::SHADER_STAGE_MESH_BIT));
	CHECK_FALSE(reflection.push_constant_stages.has_flag(RD::SHADER_STAGE_FRAGMENT_BIT));
	REQUIRE(reflection.uniform_sets.size() == 1);
	REQUIRE(reflection.uniform_sets[0].size() == 1);
	const RD::ShaderUniform &instances = reflection.uniform_sets[0][0];
	CHECK(instances.type == RD::UNIFORM_TYPE_STORAGE_BUFFER);
	CHECK(instances.binding == 0);
	CHECK(uint32_t(instances.stages) == uint32_t(RD::SHADER_STAGE_TASK_BIT | RD::SHADER_STAGE_MESH_BIT));

	// Serialization keeps the stages (the code survives the smolv + zstd round trip).
	const PackedByteArray bytes = container->to_bytes();
	Ref<RenderingShaderContainer> loaded = format.create_container();
	REQUIRE(loaded->from_bytes(bytes));
	const RD::ShaderReflection loaded_reflection = loaded->get_shader_reflection();
	CHECK(uint32_t(loaded_reflection.stages_bits) == uint32_t(reflection.stages_bits));
	CHECK(uint32_t(loaded_reflection.push_constant_stages) == uint32_t(reflection.push_constant_stages));
	REQUIRE(loaded->shaders.size() == 3);
	for (const RenderingShaderContainer::Shader &shader : loaded->shaders) {
		INFO(RD::SHADER_STAGE_NAMES[shader.shader_stage]);
		Vector<uint8_t> smolv_bytes;
		smolv_bytes.resize(shader.code_decompressed_size);
		REQUIRE(loaded->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags & ~RenderingShaderContainerVulkan::COMPRESSION_FLAG_SMOLV, smolv_bytes.ptrw(), smolv_bytes.size()));
		Vector<uint8_t> decoded;
		decoded.resize(smolv::GetDecodedBufferSize(smolv_bytes.ptr(), smolv_bytes.size()));
		REQUIRE(decoded.size() > 0);
		REQUIRE(smolv::Decode(smolv_bytes.ptr(), smolv_bytes.size(), decoded.ptrw(), decoded.size()));
		const uint32_t expected = shader.shader_stage == RD::SHADER_STAGE_TASK ? SPV_EXECUTION_MODEL_TASK_EXT : (shader.shader_stage == RD::SHADER_STAGE_MESH ? SPV_EXECUTION_MODEL_MESH_EXT : SPV_EXECUTION_MODEL_FRAGMENT);
		CHECK(spirv_execution_model(decoded) == expected);
	}

	// Mixing vertex and mesh stages is rejected.
	Ref<RenderingShaderContainer> mixed = format.create_container();
	REQUIRE(file->parse_versions_from_text(shader_file_text(false, false, true)) == OK);
	Vector<RD::ShaderStageSPIRVData> mixed_stages = stages;
	RD::ShaderStageSPIRVData vertex;
	vertex.shader_stage = RD::SHADER_STAGE_VERTEX;
	vertex.spirv = file->get_spirv()->get_stage_bytecode(RD::SHADER_STAGE_VERTEX);
	mixed_stages.push_back(vertex);
	ERR_PRINT_OFF;
	CHECK_FALSE(mixed->set_code_from_spirv("mariner_mesh_mixed", mixed_stages));
	ERR_PRINT_ON;
}
#endif // VULKAN_ENABLED
#endif // MODULE_GLSLANG_ENABLED

TEST_CASE("[Mariner][E7] RenderingDevice mesh shading API") {
	struct Method {
		const char *name;
		int arguments;
		int default_arguments;
	};
	const Method methods[] = {
		{ "draw_list_draw_mesh_tasks", 4, 2 },
		{ "draw_list_draw_mesh_tasks_indirect", 5, 3 },
		{ "draw_list_draw_mesh_tasks_indirect_count", 7, 1 },
	};
	for (const Method &m : methods) {
		INFO(m.name);
		MethodBind *method = ClassDB::get_method("RenderingDevice", m.name);
		REQUIRE(method != nullptr);
		CHECK(method->get_argument_count() == m.arguments);
		CHECK(method->get_default_argument_count() == m.default_arguments);
		CHECK(method->get_argument_type(-1) == Variant::NIL);
	}
	CHECK(RD::MESH_TASKS_INDIRECT_COMMAND_SIZE == 12);
	// The vertex draw API is unchanged.
	MethodBind *draw = ClassDB::get_method("RenderingDevice", "draw_list_draw");
	REQUIRE(draw != nullptr);
	CHECK(draw->get_argument_count() == 4);
}

TEST_CASE("[Mariner][E7] RenderingDevice without a driver reports no mesh shader support") {
	RenderingDevice *rd = memnew(RenderingDevice);
	CHECK_FALSE(rd->has_feature(RD::SUPPORTS_MESH_SHADER));
	CHECK_FALSE(rd->has_feature(RD::SUPPORTS_MESH_SHADER_INDIRECT_COUNT));
	CHECK(rd->limit_get(RD::LIMIT_MAX_MESH_OUTPUT_VERTICES) == 0);
	CHECK(rd->limit_get(RD::LIMIT_MAX_TASK_WORKGROUP_COUNT_X) == 0);

	// Drawing outside a draw list is an error, not a crash.
	ERR_PRINT_OFF;
	rd->draw_list_draw_mesh_tasks(0, 1, 1, 1);
	rd->draw_list_draw_mesh_tasks_indirect(0, RID());
	rd->draw_list_draw_mesh_tasks_indirect_count(0, RID(), 0, RID(), 0, 1);
	ERR_PRINT_ON;
	memdelete(rd);
}

TEST_CASE("[Mariner][E7] Render graph: mesh stages are graphics stages for the async compute queue") {
	using RDG = RenderingDeviceGraph;
	typedef BitField<RDD::PipelineStageBits> Stages;
	CHECK(uint32_t(RDG::async_compute_sanitize_stages(Stages(RDD::PIPELINE_STAGE_TASK_SHADER_BIT))) == uint32_t(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
	CHECK(uint32_t(RDG::async_compute_sanitize_stages(Stages(RDD::PIPELINE_STAGE_MESH_SHADER_BIT | RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT))) == uint32_t(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT | RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
}

} // namespace TestMarinerMeshShaders
