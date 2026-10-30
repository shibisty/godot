/**************************************************************************/
/*  test_mariner_async_compute.cpp                                        */
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

TEST_FORCE_LINK(test_mariner_async_compute)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_graph.h"

// Mariner engine patch E6 (asynchronous compute on a second, compute-only GPU queue): enum values, project setting,
// GDScript API, the routing rules (what runs on the async queue and why something falls back to the main queue),
// the compute-queue barrier rules of the render graph and the "no async compute queue" path of a RenderingDevice
// without a driver. The queue itself (timeline semaphores, concurrent sharing, D3D12 COMMON transitions, overlap)
// needs a GPU and is validated on the PC (engine/README.md).

namespace TestMarinerAsyncCompute {

static PropertyInfo find_project_setting(const String &p_name) {
	List<PropertyInfo> list;
	ProjectSettings::get_singleton()->get_property_list(&list);
	for (const PropertyInfo &pi : list) {
		if (pi.name == p_name) {
			return pi;
		}
	}
	return PropertyInfo();
}

TEST_CASE("[Mariner][E6] Usage flags: values, binding, no overlap with driver-internal bits") {
	// Upstream values unchanged.
	CHECK(int64_t(RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT) == 512);
	CHECK(int64_t(RenderingDevice::TEXTURE_USAGE_TRANSIENT_BIT) == 2048);
	CHECK(int64_t(RenderingDevice::TEXTURE_USAGE_DEPTH_RESOLVE_ATTACHMENT_BIT) == 4096);
	CHECK(int64_t(RenderingDevice::BUFFER_CREATION_DEVICE_ADDRESS_BIT) == 1);
	CHECK(int64_t(RenderingDevice::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT) == 8);

	// New flags appended.
	CHECK(int64_t(RenderingDevice::TEXTURE_USAGE_ASYNC_COMPUTE_BIT) == 8192);
	CHECK(int64_t(RenderingDevice::TEXTURE_USAGE_MAX_BIT) == int64_t(RenderingDevice::TEXTURE_USAGE_ASYNC_COMPUTE_BIT));
	CHECK(int64_t(RenderingDevice::BUFFER_CREATION_ASYNC_COMPUTE_BIT) == 16);

	bool valid = false;
	CHECK(ClassDB::get_integer_constant("RenderingDevice", "TEXTURE_USAGE_ASYNC_COMPUTE_BIT", &valid) == 8192);
	CHECK(valid);
	CHECK(ClassDB::get_integer_constant_enum("RenderingDevice", "TEXTURE_USAGE_ASYNC_COMPUTE_BIT") == StringName("TextureUsageBits"));
	valid = false;
	CHECK(ClassDB::get_integer_constant("RenderingDevice", "BUFFER_CREATION_ASYNC_COMPUTE_BIT", &valid) == 16);
	CHECK(valid);
	CHECK(ClassDB::get_integer_constant_enum("RenderingDevice", "BUFFER_CREATION_ASYNC_COMPUTE_BIT") == StringName("BufferCreationBits"));

	// The driver-only texture usage bits are derived from TEXTURE_USAGE_MAX_BIT: they must have moved up.
	const uint64_t vrs_bits = uint64_t(RDD::TEXTURE_USAGE_VRS_FRAGMENT_SHADING_RATE_BIT) | uint64_t(RDD::TEXTURE_USAGE_VRS_FRAGMENT_DENSITY_MAP_BIT);
	CHECK((vrs_bits & uint64_t(RenderingDevice::TEXTURE_USAGE_ASYNC_COMPUTE_BIT)) == 0);
	CHECK(uint64_t(RDD::TEXTURE_USAGE_VRS_FRAGMENT_SHADING_RATE_BIT) == (uint64_t(RenderingDevice::TEXTURE_USAGE_ASYNC_COMPUTE_BIT) << 1));

	// The driver buffer bit is unique and never reaches the graphics API (masked in the Vulkan driver).
	const uint64_t async_buffer_bit = uint64_t(RDD::BUFFER_USAGE_ASYNC_COMPUTE_SHARED_BIT);
	const uint64_t other_buffer_bits[] = {
		uint64_t(RDD::BUFFER_USAGE_TRANSFER_FROM_BIT),
		uint64_t(RDD::BUFFER_USAGE_TRANSFER_TO_BIT),
		uint64_t(RDD::BUFFER_USAGE_TEXEL_BIT),
		uint64_t(RDD::BUFFER_USAGE_UNIFORM_BIT),
		uint64_t(RDD::BUFFER_USAGE_STORAGE_BIT),
		uint64_t(RDD::BUFFER_USAGE_INDEX_BIT),
		uint64_t(RDD::BUFFER_USAGE_VERTEX_BIT),
		uint64_t(RDD::BUFFER_USAGE_INDIRECT_BIT),
		uint64_t(RDD::BUFFER_USAGE_SHADER_BINDING_TABLE_BIT),
		uint64_t(RDD::BUFFER_USAGE_DEVICE_ADDRESS_BIT),
		uint64_t(RDD::BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT),
		uint64_t(RDD::BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT),
		uint64_t(RDD::BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT),
	};
	for (uint64_t bit : other_buffer_bits) {
		CHECK((bit & async_buffer_bit) == 0);
	}
}

TEST_CASE("[Mariner][E6] Project setting rendering/rendering_device/async_compute") {
	const String name = "rendering/rendering_device/async_compute";
	REQUIRE(ProjectSettings::get_singleton()->has_setting(name));
	CHECK(ProjectSettings::get_singleton()->property_get_revert(name) == Variant(true));
	CHECK(bool(GLOBAL_GET(name)) == true);
	const PropertyInfo pi = find_project_setting(name);
	CHECK(pi.type == Variant::BOOL);
	CHECK((pi.usage & PROPERTY_USAGE_RESTART_IF_CHANGED) != 0);
}

TEST_CASE("[Mariner][E6] RenderingDevice async compute methods are bound") {
	struct Method {
		const char *name;
		int arguments;
		Variant::Type return_type;
	};
	const Method methods[] = {
		{ "async_compute_begin", 0, Variant::BOOL },
		{ "async_compute_end", 0, Variant::NIL },
		{ "async_compute_is_supported", 0, Variant::BOOL },
		{ "async_compute_set_enabled", 1, Variant::NIL },
		{ "async_compute_is_enabled", 0, Variant::BOOL },
		{ "async_compute_is_active", 0, Variant::BOOL },
		{ "async_compute_get_info", 0, Variant::DICTIONARY },
	};
	for (const Method &m : methods) {
		INFO(m.name);
		MethodBind *method = ClassDB::get_method("RenderingDevice", m.name);
		REQUIRE(method != nullptr);
		CHECK(method->get_argument_count() == m.arguments);
		CHECK(method->get_argument_type(-1) == m.return_type);
	}
	// The compute list API is unchanged.
	MethodBind *push_constant = ClassDB::get_method("RenderingDevice", "compute_list_set_push_constant");
	REQUIRE(push_constant != nullptr);
	CHECK(push_constant->get_argument_count() == 3);
	CHECK(ClassDB::has_method("RenderingDevice", "compute_list_add_barrier"));
}

TEST_CASE("[Mariner][E6] Routing rules: where async compute work runs and why it falls back") {
	using RD = RenderingDevice;
	RD::AsyncComputeRouteQuery ok;
	ok.in_scope = true;
	ok.supported = true;
	ok.enabled = true;
	CHECK(RD::async_compute_route(ok) == RD::ASYNC_COMPUTE_ROUTE_ASYNC);

	// Outside a scope nothing changes, whatever else is true.
	RD::AsyncComputeRouteQuery q = ok;
	q.in_scope = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_NO_SCOPE);

	q = ok;
	q.supported = false;
	q.all_shared = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_UNSUPPORTED);

	q = ok;
	q.enabled = false;
	q.used_by_main_queue = true;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_DISABLED);

	// Usage errors are reported before frame-dependent reasons.
	q = ok;
	q.all_shared = false;
	q.unsupported_resource = true;
	q.new_uniform_set = true;
	q.pending_upload = true;
	q.used_by_main_queue = true;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_RESOURCE_NOT_SHARED);
	q.all_shared = true;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_UNSUPPORTED_RESOURCE);
	q.unsupported_resource = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_NEW_UNIFORM_SET);
	q.new_uniform_set = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_PENDING_UPLOAD);
	q.pending_upload = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_FALLBACK_USED_BY_MAIN_QUEUE);
	q.used_by_main_queue = false;
	CHECK(RD::async_compute_route(q) == RD::ASYNC_COMPUTE_ROUTE_ASYNC);

	// Every reason has a distinct, non-empty name (used in async_compute_get_info()["fallbacks"]).
	HashSet<String> names;
	for (int i = 0; i < RD::ASYNC_COMPUTE_FALLBACK_MAX; i++) {
		const String name = RD::async_compute_fallback_name(RD::AsyncComputeFallback(i));
		CHECK_FALSE(name.is_empty());
		CHECK_FALSE(names.has(name));
		names.insert(name);
	}
	CHECK(String(RD::async_compute_fallback_name(RD::ASYNC_COMPUTE_ROUTE_ASYNC)) == "async");
	CHECK(String(RD::async_compute_fallback_name(RD::ASYNC_COMPUTE_FALLBACK_USED_BY_MAIN_QUEUE)) == "used_by_main_queue");
}

TEST_CASE("[Mariner][E6] Render graph: barriers on the compute-only queue") {
	using RDG = RenderingDeviceGraph;
	typedef BitField<RDD::PipelineStageBits> Stages;
	typedef BitField<RDD::BarrierAccessBits> Access;

	// Compute queue stages pass unchanged.
	const Stages compute = Stages(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT | RDD::PIPELINE_STAGE_COPY_BIT | RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);
	CHECK(uint32_t(RDG::async_compute_sanitize_stages(compute)) == uint32_t(compute));
	CHECK(uint32_t(RDG::async_compute_sanitize_stages(Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT))) == uint32_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT));

	// Graphics stages (from the main queue's last use of a resource) become ALL_COMMANDS, keeping the compute ones.
	const Stages fragment_compute = Stages(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	CHECK(uint32_t(RDG::async_compute_sanitize_stages(fragment_compute)) == uint32_t(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT | RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
	const RDD::PipelineStageBits graphics_only[] = {
		RDD::PIPELINE_STAGE_VERTEX_INPUT_BIT,
		RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT,
		RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		RDD::PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		RDD::PIPELINE_STAGE_RESOLVE_BIT,
		RDD::PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT,
	};
	for (RDD::PipelineStageBits stage : graphics_only) {
		INFO(uint32_t(stage));
		CHECK(uint32_t(RDG::async_compute_sanitize_stages(Stages(stage))) == uint32_t(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
	}

	// Access types of graphics stages are dropped (the semaphore wait already made those writes visible).
	const Access shader_rw = Access(RDD::BARRIER_ACCESS_SHADER_READ_BIT | RDD::BARRIER_ACCESS_SHADER_WRITE_BIT);
	CHECK(uint32_t(RDG::async_compute_sanitize_access(shader_rw)) == uint32_t(shader_rw));
	const Access mixed = Access(RDD::BARRIER_ACCESS_SHADER_READ_BIT | RDD::BARRIER_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | RDD::BARRIER_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | RDD::BARRIER_ACCESS_INDEX_READ_BIT | RDD::BARRIER_ACCESS_RESOLVE_WRITE_BIT);
	CHECK(uint32_t(RDG::async_compute_sanitize_access(mixed)) == uint32_t(RDD::BARRIER_ACCESS_SHADER_READ_BIT));
	const Access transfer = Access(RDD::BARRIER_ACCESS_COPY_READ_BIT | RDD::BARRIER_ACCESS_COPY_WRITE_BIT | RDD::BARRIER_ACCESS_UNIFORM_READ_BIT | RDD::BARRIER_ACCESS_INDIRECT_COMMAND_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT);
	CHECK(uint32_t(RDG::async_compute_sanitize_access(transfer)) == uint32_t(transfer));

	// First use in the async graph: TOP_OF_PIPE plus the access of the last use elsewhere (a copy) - the stage mask
	// must support that access (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819).
	const Stages top_compute = Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT | RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	CHECK(uint32_t(RDG::async_compute_stages_for_access(top_compute, RDD::BARRIER_ACCESS_COPY_WRITE_BIT)) == (uint32_t(top_compute) | uint32_t(RDD::PIPELINE_STAGE_COPY_BIT)));
	CHECK(uint32_t(RDG::async_compute_stages_for_access(Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT), RDD::BARRIER_ACCESS_SHADER_WRITE_BIT)) == uint32_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT | RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK(uint32_t(RDG::async_compute_stages_for_access(Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT), RDD::BARRIER_ACCESS_STORAGE_CLEAR_BIT)) == uint32_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT | RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT));
	CHECK(uint32_t(RDG::async_compute_stages_for_access(Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT), RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT)) == uint32_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT | RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
	// Stages that already cover the access stay as they are.
	CHECK(uint32_t(RDG::async_compute_stages_for_access(compute, RDD::BARRIER_ACCESS_COPY_READ_BIT | RDD::BARRIER_ACCESS_SHADER_READ_BIT)) == uint32_t(compute));
	CHECK(uint32_t(RDG::async_compute_stages_for_access(Stages(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT), RDD::BARRIER_ACCESS_COPY_WRITE_BIT)) == uint32_t(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));
	CHECK(uint32_t(RDG::async_compute_stages_for_access(Stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT), 0)) == uint32_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT));
}

TEST_CASE("[Mariner][E6] Render graph: resource trackers know which queue used them this frame") {
	using RDG = RenderingDeviceGraph;
	RDG graph;
	CHECK_FALSE(graph.is_async_compute_queue());
	graph.set_async_compute_queue(true);
	CHECK(graph.is_async_compute_queue());
	graph.set_async_compute_queue(false);
	CHECK_FALSE(graph.has_commands());
	CHECK_FALSE(graph.take_async_compute_conflict());

	RDG::ResourceTracker *parent = RDG::resource_tracker_create();
	RDG::ResourceTracker *slice = RDG::resource_tracker_create();
	slice->parent = parent;

	// New trackers: not shared, never used by any queue.
	CHECK_FALSE(parent->async_compute_shared);
	CHECK(parent->async_compute_frame == -1);
	CHECK(parent->command_frame == -1);
	CHECK_FALSE(graph.is_tracker_used_this_frame(parent));
	CHECK_FALSE(graph.is_tracker_used_this_frame(slice));
	CHECK_FALSE(graph.is_tracker_used_this_frame(nullptr));

	// A slice counts as used when the texture it belongs to was used in this frame (and vice versa for the parent).
	const int64_t frame = graph.get_tracking_frame();
	parent->command_frame = frame;
	CHECK(graph.is_tracker_used_this_frame(parent));
	CHECK(graph.is_tracker_used_this_frame(slice));
	parent->command_frame = frame - 1;
	slice->command_frame = frame;
	CHECK(graph.is_tracker_used_this_frame(slice));
	CHECK_FALSE(graph.is_tracker_used_this_frame(parent));

	// Frame resets keep the async compute mark (it belongs to the other graph's tracking frame).
	slice->async_compute_frame = 12345;
	slice->reset_if_outdated(frame + 1);
	CHECK(slice->async_compute_frame == 12345);
	CHECK(slice->command_frame == frame + 1);

	RDG::resource_tracker_free(slice);
	RDG::resource_tracker_free(parent);
}

TEST_CASE("[Mariner][E6] RenderingDevice without an async compute queue keeps everything on the main queue") {
	// A RenderingDevice that was never initialized (no driver): the API must be safe and report "unsupported".
	RenderingDevice *rd = memnew(RenderingDevice);

	CHECK_FALSE(rd->async_compute_is_supported());
	CHECK(rd->async_compute_is_enabled());
	CHECK_FALSE(rd->async_compute_is_active());

	rd->async_compute_set_enabled(false);
	CHECK_FALSE(rd->async_compute_is_enabled());
	rd->async_compute_set_enabled(true);
	CHECK(rd->async_compute_is_enabled());

	Dictionary info = rd->async_compute_get_info();
	const char *keys[] = { "supported", "enabled", "active", "reason", "timestamps", "lists_async", "lists_main", "ops_async", "ops_main",
		"dispatches_async", "submitted", "serialized", "gpu_time_usec", "total_lists_async", "total_lists_main", "total_ops_async",
		"total_ops_main", "total_submissions", "serialized_frames", "last_fallback", "fallbacks" };
	for (const char *key : keys) {
		CHECK_MESSAGE(info.has(key), key);
	}
	CHECK(bool(info["supported"]) == false);
	CHECK(bool(info["active"]) == false);
	CHECK(int64_t(info["total_lists_async"]) == 0);

	// The scope works, returns false and a compute list recorded in it goes to the main graph.
	CHECK_FALSE(rd->async_compute_begin());
	ERR_PRINT_OFF;
	CHECK_FALSE(rd->async_compute_begin()); // Nested scopes are rejected.
	ERR_PRINT_ON;
	const RenderingDevice::ComputeListID list = rd->compute_list_begin();
	CHECK(list != int64_t(RenderingDevice::INVALID_ID));
	rd->compute_list_end();
	rd->async_compute_end();
	ERR_PRINT_OFF;
	rd->async_compute_end(); // Without a scope: error, no crash.
	ERR_PRINT_ON;

	info = rd->async_compute_get_info();
	CHECK(int64_t(info["total_lists_async"]) == 0);
	CHECK(int64_t(info["total_lists_main"]) == 1);
	CHECK(String(info["last_fallback"]) == "unsupported");
	const Dictionary fallbacks = info["fallbacks"];
	CHECK(int64_t(fallbacks.get("unsupported", 0)) == 1);

	// Outside a scope nothing is counted.
	const RenderingDevice::ComputeListID list2 = rd->compute_list_begin();
	CHECK(list2 != int64_t(RenderingDevice::INVALID_ID));
	rd->compute_list_end();
	info = rd->async_compute_get_info();
	CHECK(int64_t(info["total_lists_main"]) == 1);

	memdelete(rd);
}

} // namespace TestMarinerAsyncCompute
