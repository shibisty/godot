/**************************************************************************/
/*  test_mariner_second_gpu.cpp                                           */
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

TEST_FORCE_LINK(test_mariner_second_gpu)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_context_driver_vulkan.h"

#include <cstring>
#endif

// Mariner engine patch E8 (compute on a second physical GPU): the API (RenderingServer.get_physical_devices(),
// RenderingServer.create_local_rendering_device_on()), the "no RenderingDevice" paths, the dictionary layout and,
// when a Vulkan driver is available (on the build server: Mesa lavapipe, a CPU device), a real compute job on a
// local device of every physical device, from a worker thread too, plus the routing of local devices past a
// Vulkan interposer (as NVIDIA Streamline's sl.interposer.dll), with a fake interposer. Set
// GODOT_MARINER_TEST_NO_VULKAN=1 to skip the Vulkan cases.

namespace TestMarinerSecondGPU {

using RD = RenderingDevice;

TEST_CASE("[Mariner][E8] RenderingServer methods are bound") {
	MethodBind *list = ClassDB::get_method("RenderingServer", "get_physical_devices");
	REQUIRE(list != nullptr);
	CHECK(list->get_argument_count() == 0);
	CHECK(list->get_argument_type(-1) == Variant::ARRAY);
	CHECK(list->is_const());

	MethodBind *create = ClassDB::get_method("RenderingServer", "create_local_rendering_device_on");
	REQUIRE(create != nullptr);
	CHECK(create->get_argument_count() == 1);
	CHECK(create->get_argument_type(0) == Variant::INT);
	CHECK(create->get_argument_type(-1) == Variant::OBJECT);
	CHECK(create->get_default_argument_count() == 0);
	CHECK(create->is_const());

	List<MethodInfo> methods;
	ClassDB::get_method_list("RenderingServer", &methods, true);
	bool found = false;
	for (const MethodInfo &mi : methods) {
		if (mi.name == "create_local_rendering_device_on") {
			found = true;
			REQUIRE(mi.arguments.size() == 1);
			CHECK(mi.arguments[0].name == "device_index");
			CHECK(mi.return_val.class_name == StringName("RenderingDevice"));
		}
	}
	CHECK(found);

	// The upstream local device API is unchanged.
	MethodBind *local = ClassDB::get_method("RenderingServer", "create_local_rendering_device");
	REQUIRE(local != nullptr);
	CHECK(local->get_argument_count() == 0);
}

TEST_CASE("[Mariner][E8] Device type enum used in the device list") {
	const struct {
		const char *name;
		int64_t value;
	} values[] = {
		{ "DEVICE_TYPE_OTHER", 0 },
		{ "DEVICE_TYPE_INTEGRATED_GPU", 1 },
		{ "DEVICE_TYPE_DISCRETE_GPU", 2 },
		{ "DEVICE_TYPE_VIRTUAL_GPU", 3 },
		{ "DEVICE_TYPE_CPU", 4 },
		{ "DEVICE_TYPE_MAX", 5 },
	};
	for (const auto &v : values) {
		INFO(v.name);
		bool valid = false;
		CHECK(ClassDB::get_integer_constant("RenderingDevice", v.name, &valid) == v.value);
		CHECK(valid);
		CHECK(ClassDB::get_integer_constant_enum("RenderingDevice", v.name) == StringName("DeviceType"));
	}
	// Driver values are the same (they are VkPhysicalDeviceType).
	CHECK(int64_t(RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU));
	CHECK(int64_t(RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU));
	CHECK(int64_t(RenderingContextDriver::DEVICE_TYPE_CPU) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_CPU));
}

TEST_CASE("[Mariner][E8] Device dictionary layout") {
	RenderingContextDriver::Device device;
	device.name = "Test iGPU";
	device.vendor = RenderingContextDriver::Vendor::VENDOR_INTEL;
	device.type = RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU; // As Godot's D3D12 list calls Intel iGPUs.
	RenderingContextDriver::DeviceInfo info;
	info.device_id = 0xA7A0;
	info.dedicated_memory = 128ull << 20;
	info.shared_memory = 16ull << 30;
	info.uma = true;
	info.type = RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU; // Refined by the driver.
	info.api_version = "1.3.289";

	const Dictionary d = RD::physical_device_to_dictionary(1, device, info, false, "Vulkan");
	const char *keys[] = { "index", "name", "vendor_id", "vendor_name", "device_id", "type", "type_name", "dedicated_memory", "shared_memory", "uma", "is_main", "api", "api_version" };
	CHECK(d.size() == int(std_size(keys)));
	for (const char *key : keys) {
		INFO(key);
		CHECK(d.has(key));
	}
	CHECK(int64_t(d["index"]) == 1);
	CHECK(String(d["name"]) == "Test iGPU");
	CHECK(int64_t(d["vendor_id"]) == 0x8086);
	CHECK(String(d["vendor_name"]) == "Intel");
	CHECK(int64_t(d["device_id"]) == 0xA7A0);
	CHECK(int64_t(d["type"]) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU));
	CHECK(String(d["type_name"]) == "Integrated");
	CHECK(int64_t(d["dedicated_memory"]) == int64_t(128) << 20);
	CHECK(int64_t(d["shared_memory"]) == int64_t(16) << 30);
	CHECK(bool(d["uma"]));
	CHECK_FALSE(bool(d["is_main"]));
	CHECK(String(d["api"]) == "Vulkan");
	CHECK(String(d["api_version"]) == "1.3.289");

	// A driver that knows nothing more reports the basic type and a guess for UMA.
	class NoInfoContext : public RenderingContextDriver {
	public:
		Device dev;
		virtual Error initialize() override { return OK; }
		virtual const Device &device_get(uint32_t p_device_index) const override { return dev; }
		virtual uint32_t device_get_count() const override { return 1; }
		virtual bool device_supports_present(uint32_t, SurfaceID) const override { return false; }
		virtual RenderingDeviceDriver *driver_create() override { return nullptr; }
		virtual void driver_free(RenderingDeviceDriver *) override {}
		virtual SurfaceID surface_create(const void *) override { return 0; }
		virtual void surface_set_size(SurfaceID, uint32_t, uint32_t) override {}
		virtual void surface_set_vsync_mode(SurfaceID, DisplayServerEnums::VSyncMode) override {}
		virtual DisplayServerEnums::VSyncMode surface_get_vsync_mode(SurfaceID) const override { return DisplayServerEnums::VSYNC_ENABLED; }
		virtual void surface_set_hdr_output_enabled(SurfaceID, bool) override {}
		virtual bool surface_get_hdr_output_enabled(SurfaceID) const override { return false; }
		virtual void surface_set_hdr_output_reference_luminance(SurfaceID, float) override {}
		virtual float surface_get_hdr_output_reference_luminance(SurfaceID) const override { return 0; }
		virtual void surface_set_hdr_output_max_luminance(SurfaceID, float) override {}
		virtual float surface_get_hdr_output_max_luminance(SurfaceID) const override { return 0; }
		virtual void surface_set_hdr_output_linear_luminance_scale(SurfaceID, float) override {}
		virtual float surface_get_hdr_output_linear_luminance_scale(SurfaceID) const override { return 0; }
		virtual float surface_get_hdr_output_max_value(SurfaceID) const override { return 0; }
		virtual uint32_t surface_get_width(SurfaceID) const override { return 0; }
		virtual uint32_t surface_get_height(SurfaceID) const override { return 0; }
		virtual void surface_set_needs_resize(SurfaceID, bool) override {}
		virtual bool surface_get_needs_resize(SurfaceID) const override { return false; }
		virtual void surface_destroy(SurfaceID) override {}
		virtual bool is_debug_utils_enabled() const override { return false; }
	};
	NoInfoContext ctx;
	ctx.dev.type = RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU;
	RenderingContextDriver::DeviceInfo no_info;
	no_info.dedicated_memory = 7;
	CHECK_FALSE(ctx.device_get_info(0, no_info));
	CHECK(no_info.type == RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU);
	CHECK(no_info.uma);
	CHECK(no_info.dedicated_memory == 0);
	ctx.dev.type = RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU;
	CHECK_FALSE(ctx.device_get_info(0, no_info));
	CHECK_FALSE(no_info.uma);
	CHECK_FALSE(ctx.device_get_info(5, no_info)); // Out of range: defaults, no crash.
	CHECK(no_info.type == RenderingContextDriver::DEVICE_TYPE_OTHER);
}

TEST_CASE("[Mariner][E8] Without a RenderingDevice: empty list, no local device, no crash") {
	REQUIRE(RenderingDevice::get_singleton() == nullptr); // Headless test run (dummy renderer).
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);

	CHECK(rs->get_physical_devices().is_empty());
	ERR_PRINT_OFF;
	CHECK(rs->create_local_rendering_device_on(0) == nullptr);
	CHECK(rs->create_local_rendering_device_on(-1) == nullptr);
	ERR_PRINT_ON;
	// Through the binding, as from GDScript.
	Variant ret = rs->call("get_physical_devices");
	CHECK(ret.get_type() == Variant::ARRAY);
	CHECK(Array(ret).is_empty());

	// An uninitialized RenderingDevice (no driver): still empty, creating fails cleanly.
	RenderingDevice *rd = memnew(RenderingDevice);
	CHECK(rs->get_physical_devices().is_empty());
	CHECK(rd->get_physical_devices().is_empty());
	ERR_PRINT_OFF;
	CHECK(rd->create_local_device_on(0) == nullptr);
	CHECK(rs->create_local_rendering_device_on(0) == nullptr);
	ERR_PRINT_ON;
	memdelete(rd);
	CHECK(RenderingDevice::get_singleton() == nullptr);
}

#ifdef VULKAN_ENABLED

static bool vulkan_tests_disabled() {
	return OS::get_singleton()->get_environment("GODOT_MARINER_TEST_NO_VULKAN") == "1";
}

// Creates and initializes a Vulkan context; nullptr (and a message) when there's no Vulkan driver.
static RenderingContextDriverVulkan *create_vulkan_context() {
	RenderingContextDriverVulkan *ctx = memnew(RenderingContextDriverVulkan);
	ERR_PRINT_OFF;
	const Error err = ctx->initialize();
	ERR_PRINT_ON;
	if (err != OK || ctx->device_get_count() == 0) {
		memdelete(ctx);
		return nullptr;
	}
	return ctx;
}

static const char *ADD_GLSL = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) restrict buffer Data {
	uint v[];
} data;
void main() {
	uint i = gl_GlobalInvocationID.x;
	data.v[i] = data.v[i] * 3u + 7u;
}
)";

static constexpr uint32_t JOB_COUNT = 256;

// The smoke job of the game (scripts/render/compute_devices.gd): v[i] = v[i] * 3 + 7 on the device, check on the CPU.
static bool run_add_job(RenderingDevice *p_rd, String &r_error) {
	String compile_error;
	const Vector<uint8_t> spirv = p_rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, ADD_GLSL, RD::SHADER_LANGUAGE_GLSL, &compile_error, false);
	if (spirv.is_empty()) {
		r_error = "compile: " + compile_error;
		return false;
	}
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = spirv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	const RID shader = p_rd->shader_create_from_spirv(stages, "Mariner E8 test");
	if (shader.is_null()) {
		r_error = "shader_create_from_spirv";
		return false;
	}

	Vector<uint8_t> input;
	input.resize(JOB_COUNT * 4);
	uint32_t *w = (uint32_t *)input.ptrw();
	for (uint32_t i = 0; i < JOB_COUNT; i++) {
		w[i] = i;
	}
	const RID buffer = p_rd->storage_buffer_create(input.size(), input);
	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u.binding = 0;
	u.append_id(buffer);
	const RID uniform_set = p_rd->uniform_set_create(VectorView<RD::Uniform>(&u, 1), shader, 0);
	const RID pipeline = p_rd->compute_pipeline_create(shader);

	const RD::ComputeListID cl = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(cl, pipeline);
	p_rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	p_rd->compute_list_dispatch(cl, JOB_COUNT / 64, 1, 1);
	p_rd->compute_list_end();
	p_rd->submit();
	p_rd->sync();

	const Vector<uint8_t> output = p_rd->buffer_get_data(buffer);
	bool ok = output.size() == int(JOB_COUNT * 4);
	if (ok) {
		const uint32_t *r = (const uint32_t *)output.ptr();
		for (uint32_t i = 0; i < JOB_COUNT && ok; i++) {
			if (r[i] != i * 3u + 7u) {
				ok = false;
				r_error = vformat("result[%d] = %d, expected %d", i, r[i], i * 3u + 7u);
			}
		}
	} else {
		r_error = vformat("read back %d bytes", output.size());
	}

	p_rd->free_rid(pipeline);
	p_rd->free_rid(uniform_set);
	p_rd->free_rid(buffer);
	p_rd->free_rid(shader);
	return ok;
}

struct ThreadJob {
	RenderingDevice *main_rd = nullptr;
	int32_t device_index = 0;
	bool created = false;
	bool ok = false;
	String error;
};

static void thread_job(void *p_userdata) {
	ThreadJob *job = (ThreadJob *)p_userdata;
	// Created, used and freed on this worker thread (a local device belongs to the thread that creates it).
	RenderingDevice *local = job->main_rd->create_local_device_on(job->device_index);
	job->created = local != nullptr;
	if (local) {
		job->ok = run_add_job(local, job->error);
		memdelete(local);
	}
}

TEST_CASE("[Mariner][E8] Vulkan: physical device list and compute on a local device of each one") {
	if (vulkan_tests_disabled()) {
		MESSAGE("GODOT_MARINER_TEST_NO_VULKAN=1: skipped.");
		return;
	}
	RenderingContextDriverVulkan *ctx = create_vulkan_context();
	if (ctx == nullptr) {
		MESSAGE("No Vulkan driver (ICD) here: skipped. On the build server Mesa lavapipe provides one.");
		return;
	}
	REQUIRE(RenderingDevice::get_singleton() == nullptr);

	// The process' "main" device (here without a window, so it is local to the test, but it is the singleton).
	RenderingDevice *rd = memnew(RenderingDevice);
	REQUIRE(RenderingDevice::get_singleton() == rd);
	REQUIRE(rd->initialize(ctx) == OK);

	const uint32_t count = ctx->device_get_count();
	const TypedArray<Dictionary> list = rd->get_physical_devices();
	REQUIRE(uint32_t(list.size()) == count);
	int mains = 0;
	for (uint32_t i = 0; i < count; i++) {
		const Dictionary d = list[i];
		INFO(String(d["name"]));
		CHECK(int64_t(d["index"]) == int64_t(i));
		CHECK_FALSE(String(d["name"]).is_empty());
		CHECK(int64_t(d["type"]) >= 0);
		CHECK(int64_t(d["type"]) < int64_t(RenderingDeviceEnums::DEVICE_TYPE_MAX));
		CHECK(String(d["api"]) == "Vulkan");
		CHECK(String(d["api_version"]).begins_with("1."));
		CHECK(int64_t(d["dedicated_memory"]) + int64_t(d["shared_memory"]) > 0);
		if (int64_t(d["type"]) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_CPU) || int64_t(d["type"]) == int64_t(RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU)) {
			CHECK(bool(d["uma"]));
		}
		if (bool(d["is_main"])) {
			mains++;
			CHECK(int64_t(d["index"]) == int64_t(rd->get_device_index()));
			CHECK(String(d["name"]) == rd->get_device_name());
		}
		MESSAGE(vformat("Vulkan device #%d: %s (%s, %d MiB local, uma %s)", i, String(d["name"]), String(d["type_name"]), int64_t(d["dedicated_memory"]) >> 20, bool(d["uma"]) ? "yes" : "no"));
	}
	CHECK(mains == 1);

	// The RenderingServer API sees the same list through the singleton.
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	CHECK(rs->get_physical_devices().size() == list.size());

	// Out of range: null and an error, no crash.
	ERR_PRINT_OFF;
	CHECK(rd->create_local_device_on(int32_t(count)) == nullptr);
	CHECK(rd->create_local_device_on(-1) == nullptr);
	CHECK(rs->create_local_rendering_device_on(int(count) + 3) == nullptr);
	ERR_PRINT_ON;

	// A local device on every physical device (the same as the main one included) runs the job.
	for (uint32_t i = 0; i < count; i++) {
		const Dictionary d = list[i];
		INFO(String(d["name"]));
		RenderingDevice *local = rs->create_local_rendering_device_on(int(i));
		REQUIRE(local != nullptr);
		CHECK(local != rd);
		CHECK(RenderingDevice::get_singleton() == rd);
		CHECK(local->get_device_index() == i);
		CHECK(local->get_device_name() == String(d["name"]));
		CHECK(int64_t(local->get_device_type()) == int64_t(ctx->device_get(i).type));
		CHECK_FALSE(local->async_compute_is_supported()); // E6: local devices use one queue.
		// The local device lists the same devices; "is_main" stays the singleton's.
		const TypedArray<Dictionary> local_list = local->get_physical_devices();
		REQUIRE(local_list.size() == list.size());
		for (int j = 0; j < local_list.size(); j++) {
			CHECK(bool(Dictionary(local_list[j])["is_main"]) == bool(Dictionary(list[j])["is_main"]));
		}
		String error;
		CHECK_MESSAGE(run_add_job(local, error), error);
		memdelete(local);
	}

	// From a worker thread, as the game does it.
	ThreadJob job;
	job.main_rd = rd;
	job.device_index = int32_t(count - 1);
	Thread thread;
	thread.start(thread_job, &job);
	thread.wait_to_finish();
	CHECK(job.created);
	CHECK_MESSAGE(job.ok, job.error);

	// The main device still works after local devices came and went.
	String error;
	CHECK_MESSAGE(run_add_job(rd, error), error);

	memdelete(rd);
	CHECK(RenderingDevice::get_singleton() == nullptr);
	memdelete(ctx);
}

#ifdef USE_VOLK

// A fake Vulkan interposer built like NVIDIA Streamline's sl.interposer.dll: it wraps a few device functions and
// remembers "the" device (the last one created through it), as SL does with its global device dispatch table.
// A call for any other device through its wrappers would, in SL, go to the wrong driver: counted as "foreign".
namespace FakeInterposer {

PFN_vkGetInstanceProcAddr loader_gipa = nullptr;
VkInstance instance = VK_NULL_HANDLE;
void *device_key = nullptr;
int create_device_calls = 0;
int calls = 0;
int foreign_calls = 0;

PFN_vkCreateDevice real_CreateDevice = nullptr;
PFN_vkDestroyDevice real_DestroyDevice = nullptr;
PFN_vkBeginCommandBuffer real_BeginCommandBuffer = nullptr;
PFN_vkCmdBindPipeline real_CmdBindPipeline = nullptr;
PFN_vkCmdPipelineBarrier real_CmdPipelineBarrier = nullptr;
PFN_vkCreateImage real_CreateImage = nullptr;

void *key(const void *p_handle) {
	return *(void *const *)p_handle;
}

void count(const void *p_handle) {
	calls++;
	if (key(p_handle) != device_key) {
		foreign_calls++;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice p_physical_device, const VkDeviceCreateInfo *p_info, const VkAllocationCallbacks *p_allocator, VkDevice *r_device) {
	create_device_calls++;
	VkResult res = real_CreateDevice(p_physical_device, p_info, p_allocator, r_device);
	if (res == VK_SUCCESS) {
		device_key = key(*r_device);
	}
	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice p_device, const VkAllocationCallbacks *p_allocator) {
	count(p_device);
	real_DestroyDevice(p_device, p_allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer p_command_buffer, const VkCommandBufferBeginInfo *p_info) {
	count(p_command_buffer);
	return real_BeginCommandBuffer(p_command_buffer, p_info);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer p_command_buffer, VkPipelineBindPoint p_bind_point, VkPipeline p_pipeline) {
	count(p_command_buffer);
	real_CmdBindPipeline(p_command_buffer, p_bind_point, p_pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer p_command_buffer, VkPipelineStageFlags p_src, VkPipelineStageFlags p_dst, VkDependencyFlags p_flags, uint32_t p_memory_count, const VkMemoryBarrier *p_memory, uint32_t p_buffer_count, const VkBufferMemoryBarrier *p_buffer, uint32_t p_image_count, const VkImageMemoryBarrier *p_image) {
	count(p_command_buffer);
	real_CmdPipelineBarrier(p_command_buffer, p_src, p_dst, p_flags, p_memory_count, p_memory, p_buffer_count, p_buffer, p_image_count, p_image);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice p_device, const VkImageCreateInfo *p_info, const VkAllocationCallbacks *p_allocator, VkImage *r_image) {
	count(p_device);
	return real_CreateImage(p_device, p_info, p_allocator, r_image);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance p_instance, const char *p_name) {
	if (p_instance != VK_NULL_HANDLE) {
		instance = p_instance;
	}
	const PFN_vkVoidFunction real = loader_gipa(p_instance, p_name);
	if (real == nullptr || p_instance == VK_NULL_HANDLE) {
		return real;
	}
#define FAKE_WRAP(m_name) \
	if (strcmp(p_name, "vk" #m_name) == 0) { \
		real_##m_name = (PFN_vk##m_name)real; \
		return (PFN_vkVoidFunction) & m_name; \
	}
	FAKE_WRAP(CreateDevice)
	FAKE_WRAP(DestroyDevice)
	FAKE_WRAP(BeginCommandBuffer)
	FAKE_WRAP(CmdBindPipeline)
	FAKE_WRAP(CmdPipelineBarrier)
	FAKE_WRAP(CreateImage)
#undef FAKE_WRAP
	return real;
}

} // namespace FakeInterposer

TEST_CASE("[Mariner][E8] Vulkan: local devices bypass an interposer (Streamline), the main device does not") {
	if (vulkan_tests_disabled()) {
		MESSAGE("GODOT_MARINER_TEST_NO_VULKAN=1: skipped.");
		return;
	}
	// The loader's own entry point (what the fake interposer forwards to).
	if (volkInitialize() != VK_SUCCESS) {
		MESSAGE("No Vulkan loader here: skipped.");
		return;
	}
	FakeInterposer::loader_gipa = vkGetInstanceProcAddr;
	FakeInterposer::device_key = nullptr;
	FakeInterposer::create_device_calls = 0;
	FakeInterposer::calls = 0;
	FakeInterposer::foreign_calls = 0;

	RenderingContextDriverVulkan::set_interposer_for_testing(&FakeInterposer::GetInstanceProcAddr);
	RenderingContextDriverVulkan *ctx = create_vulkan_context();
	RenderingContextDriverVulkan::set_interposer_for_testing(nullptr);
	if (ctx == nullptr) {
		MESSAGE("No Vulkan driver (ICD) here: skipped.");
		return;
	}
	CHECK(ctx->is_interposer_in_use());
	REQUIRE(RenderingContextDriverVulkan::InterposerBypass::is_installed());
	CHECK(RenderingContextDriverVulkan::InterposerBypass::get_shimmed_function_count() == 6); // The six wraps above.
	CHECK(RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() == 0);

	// The main device goes through the interposer, as with Streamline.
	RenderingDeviceDriver *main_driver = ctx->driver_create();
	main_driver->set_local_device(false, false);
	REQUIRE(main_driver->initialize(0, 1) == OK);
	CHECK(FakeInterposer::create_device_calls == 1);
	const VkDevice main_device = (VkDevice)main_driver->get_resource_native_handle(RDD::DRIVER_RESOURCE_LOGICAL_DEVICE, 0);
	CHECK(FakeInterposer::device_key == *(void *const *)main_device);
	CHECK_FALSE(RenderingContextDriverVulkan::InterposerBypass::is_device_bypassed(main_device));
	{
		// A command buffer of the main device: the interposer sees its calls.
		RDD::CommandQueueFamilyID family = main_driver->command_queue_family_get(RDD::COMMAND_QUEUE_FAMILY_COMPUTE_BIT);
		REQUIRE(bool(family));
		RDD::CommandPoolID pool = main_driver->command_pool_create(family, RDD::COMMAND_BUFFER_TYPE_PRIMARY);
		RDD::CommandBufferID cb = main_driver->command_buffer_create(pool);
		const int before = FakeInterposer::calls;
		CHECK(main_driver->command_buffer_begin(cb));
		main_driver->command_buffer_end(cb);
		CHECK(FakeInterposer::calls == before + 1);
		main_driver->command_pool_free(pool);
	}

	// Local devices (a full RenderingDevice on each physical device) never reach the interposer.
	RenderingDevice *rd = memnew(RenderingDevice);
	REQUIRE(rd->initialize(ctx) == OK);
	CHECK(RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() == 1);
	for (uint32_t i = 0; i < ctx->device_get_count(); i++) {
		RenderingDevice *local = rd->create_local_device_on(int32_t(i));
		REQUIRE(local != nullptr);
		CHECK(RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() == 2);
		String error;
		CHECK_MESSAGE(run_add_job(local, error), error);
		memdelete(local);
		CHECK(RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() == 1);
	}
	String error;
	CHECK_MESSAGE(run_add_job(rd, error), error);
	memdelete(rd);
	CHECK(RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() == 0);
	CHECK(FakeInterposer::create_device_calls == 1);
	CHECK(FakeInterposer::foreign_calls == 0);

	// The main device is destroyed through the interposer.
	const int before_destroy = FakeInterposer::calls;
	ctx->driver_free(main_driver);
	CHECK(FakeInterposer::calls > before_destroy); // vkDestroyDevice at least.
	CHECK(FakeInterposer::foreign_calls == 0);
	memdelete(ctx);
}

#endif // USE_VOLK

#endif // VULKAN_ENABLED

} // namespace TestMarinerSecondGPU
