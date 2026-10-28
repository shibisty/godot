/**************************************************************************/
/*  rendering_context_driver_vulkan.cpp                                   */
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

#ifdef VULKAN_ENABLED

#include "rendering_context_driver_vulkan.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/version.h"
#include "drivers/streamline/streamline.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#include "drivers/vulkan/vulkan_hooks.h"

#ifndef DEV_ENABLED
#include "core/os/os.h"
#endif

#include <vk_enum_string_helper.h>

#include <atomic>

/*************************************************/
// Mariner E8: local devices bypass a Vulkan interposer
/*************************************************/

// See RenderingContextDriverVulkan::InterposerBypass. Dispatchable handles (VkDevice, VkQueue, VkCommandBuffer)
// start with the loader's dispatch table pointer, shared by everything created from one VkDevice: that is the
// key (the same thing Vulkan layers use).

namespace {

struct InterposerFunctions {
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
	PFN_vkCreateDevice CreateDevice = nullptr;
	PFN_vkDestroyDevice DestroyDevice = nullptr;
	PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
	PFN_vkCreateImage CreateImage = nullptr;
	PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
	PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
	PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
	PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
	PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
	PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
	PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
	PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
};

struct InterposerBypassState {
	static constexpr uint32_t MAX_DEVICES = 16;
	std::atomic<void *> keys[MAX_DEVICES] = {};
	std::atomic<uint32_t> count = { 0 };
	bool installed = false;
	uint32_t shimmed = 0;
	InterposerFunctions interposer; // What the interposer handed to volk.
	InterposerFunctions loader; // The Vulkan loader's own trampolines.
};

InterposerBypassState interposer_bypass;
thread_local bool interposer_bypass_next_device = false;

PFN_vkGetInstanceProcAddr interposer_for_testing = nullptr;

_FORCE_INLINE_ void *_dispatch_key(const void *p_handle) {
	return p_handle != nullptr ? *(void *const *)p_handle : nullptr;
}

bool _is_bypassed_key(void *p_key) {
	if (p_key == nullptr || interposer_bypass.count.load(std::memory_order_acquire) == 0) {
		return false;
	}
	for (uint32_t i = 0; i < InterposerBypassState::MAX_DEVICES; i++) {
		if (interposer_bypass.keys[i].load(std::memory_order_acquire) == p_key) {
			return true;
		}
	}
	return false;
}

bool _add_bypassed_key(void *p_key) {
	for (uint32_t i = 0; i < InterposerBypassState::MAX_DEVICES; i++) {
		void *expected = nullptr;
		if (interposer_bypass.keys[i].compare_exchange_strong(expected, p_key, std::memory_order_acq_rel)) {
			interposer_bypass.count.fetch_add(1, std::memory_order_acq_rel);
			return true;
		}
	}
	return false;
}

void _remove_bypassed_key(void *p_key) {
	for (uint32_t i = 0; i < InterposerBypassState::MAX_DEVICES; i++) {
		void *expected = p_key;
		if (p_key != nullptr && interposer_bypass.keys[i].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
			interposer_bypass.count.fetch_sub(1, std::memory_order_acq_rel);
			return;
		}
	}
}

#ifdef USE_VOLK
#define BYPASS_ROUTE(m_handle, m_function) (_is_bypassed_key(_dispatch_key((const void *)(m_handle))) ? interposer_bypass.loader.m_function : interposer_bypass.interposer.m_function)

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL _bypass_GetDeviceProcAddr(VkDevice p_device, const char *p_name) {
	return BYPASS_ROUTE(p_device, GetDeviceProcAddr)(p_device, p_name);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_CreateDevice(VkPhysicalDevice p_physical_device, const VkDeviceCreateInfo *p_create_info, const VkAllocationCallbacks *p_allocator, VkDevice *r_device) {
	if (!interposer_bypass_next_device) {
		return interposer_bypass.interposer.CreateDevice(p_physical_device, p_create_info, p_allocator, r_device);
	}
	const VkResult result = interposer_bypass.loader.CreateDevice(p_physical_device, p_create_info, p_allocator, r_device);
	if (result == VK_SUCCESS && !_add_bypassed_key(_dispatch_key(*r_device))) {
		// Too many local devices at once: this one can't be told apart from the main device.
		interposer_bypass.loader.DestroyDevice(*r_device, p_allocator);
		*r_device = VK_NULL_HANDLE;
		ERR_FAIL_V_MSG(VK_ERROR_TOO_MANY_OBJECTS, "Too many local Vulkan devices next to a Vulkan interposer (Streamline).");
	}
	return result;
}

VKAPI_ATTR void VKAPI_CALL _bypass_DestroyDevice(VkDevice p_device, const VkAllocationCallbacks *p_allocator) {
	void *key = _dispatch_key(p_device);
	if (_is_bypassed_key(key)) {
		interposer_bypass.loader.DestroyDevice(p_device, p_allocator);
		_remove_bypassed_key(key);
	} else {
		interposer_bypass.interposer.DestroyDevice(p_device, p_allocator);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_DeviceWaitIdle(VkDevice p_device) {
	return BYPASS_ROUTE(p_device, DeviceWaitIdle)(p_device);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_CreateImage(VkDevice p_device, const VkImageCreateInfo *p_create_info, const VkAllocationCallbacks *p_allocator, VkImage *r_image) {
	return BYPASS_ROUTE(p_device, CreateImage)(p_device, p_create_info, p_allocator, r_image);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_BeginCommandBuffer(VkCommandBuffer p_command_buffer, const VkCommandBufferBeginInfo *p_begin_info) {
	return BYPASS_ROUTE(p_command_buffer, BeginCommandBuffer)(p_command_buffer, p_begin_info);
}

VKAPI_ATTR void VKAPI_CALL _bypass_CmdPipelineBarrier(VkCommandBuffer p_command_buffer, VkPipelineStageFlags p_src_stage_mask, VkPipelineStageFlags p_dst_stage_mask, VkDependencyFlags p_dependency_flags, uint32_t p_memory_barrier_count, const VkMemoryBarrier *p_memory_barriers, uint32_t p_buffer_memory_barrier_count, const VkBufferMemoryBarrier *p_buffer_memory_barriers, uint32_t p_image_memory_barrier_count, const VkImageMemoryBarrier *p_image_memory_barriers) {
	const PFN_vkCmdPipelineBarrier fn = BYPASS_ROUTE(p_command_buffer, CmdPipelineBarrier);
	fn(p_command_buffer, p_src_stage_mask, p_dst_stage_mask, p_dependency_flags, p_memory_barrier_count, p_memory_barriers, p_buffer_memory_barrier_count, p_buffer_memory_barriers, p_image_memory_barrier_count, p_image_memory_barriers);
}

VKAPI_ATTR void VKAPI_CALL _bypass_CmdBindPipeline(VkCommandBuffer p_command_buffer, VkPipelineBindPoint p_bind_point, VkPipeline p_pipeline) {
	const PFN_vkCmdBindPipeline fn = BYPASS_ROUTE(p_command_buffer, CmdBindPipeline);
	fn(p_command_buffer, p_bind_point, p_pipeline);
}

VKAPI_ATTR void VKAPI_CALL _bypass_CmdBindDescriptorSets(VkCommandBuffer p_command_buffer, VkPipelineBindPoint p_bind_point, VkPipelineLayout p_layout, uint32_t p_first_set, uint32_t p_set_count, const VkDescriptorSet *p_sets, uint32_t p_dynamic_offset_count, const uint32_t *p_dynamic_offsets) {
	const PFN_vkCmdBindDescriptorSets fn = BYPASS_ROUTE(p_command_buffer, CmdBindDescriptorSets);
	fn(p_command_buffer, p_bind_point, p_layout, p_first_set, p_set_count, p_sets, p_dynamic_offset_count, p_dynamic_offsets);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_QueuePresentKHR(VkQueue p_queue, const VkPresentInfoKHR *p_present_info) {
	return BYPASS_ROUTE(p_queue, QueuePresentKHR)(p_queue, p_present_info);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_CreateSwapchainKHR(VkDevice p_device, const VkSwapchainCreateInfoKHR *p_create_info, const VkAllocationCallbacks *p_allocator, VkSwapchainKHR *r_swapchain) {
	return BYPASS_ROUTE(p_device, CreateSwapchainKHR)(p_device, p_create_info, p_allocator, r_swapchain);
}

VKAPI_ATTR void VKAPI_CALL _bypass_DestroySwapchainKHR(VkDevice p_device, VkSwapchainKHR p_swapchain, const VkAllocationCallbacks *p_allocator) {
	const PFN_vkDestroySwapchainKHR fn = BYPASS_ROUTE(p_device, DestroySwapchainKHR);
	fn(p_device, p_swapchain, p_allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_GetSwapchainImagesKHR(VkDevice p_device, VkSwapchainKHR p_swapchain, uint32_t *r_count, VkImage *r_images) {
	return BYPASS_ROUTE(p_device, GetSwapchainImagesKHR)(p_device, p_swapchain, r_count, r_images);
}

VKAPI_ATTR VkResult VKAPI_CALL _bypass_AcquireNextImageKHR(VkDevice p_device, VkSwapchainKHR p_swapchain, uint64_t p_timeout, VkSemaphore p_semaphore, VkFence p_fence, uint32_t *r_index) {
	return BYPASS_ROUTE(p_device, AcquireNextImageKHR)(p_device, p_swapchain, p_timeout, p_semaphore, p_fence, r_index);
}

#undef BYPASS_ROUTE
#endif // USE_VOLK

void _interposer_bypass_reset() {
	for (uint32_t i = 0; i < InterposerBypassState::MAX_DEVICES; i++) {
		interposer_bypass.keys[i].store(nullptr, std::memory_order_release);
	}
	interposer_bypass.count.store(0, std::memory_order_release);
	interposer_bypass.installed = false;
	interposer_bypass.shimmed = 0;
	interposer_bypass.interposer = InterposerFunctions();
	interposer_bypass.loader = InterposerFunctions();
}

#ifdef USE_VOLK
// Called right after volkLoadInstance() with the interposer: the volk pointers are the interposer's.
void _interposer_bypass_install(VkInstance p_instance, PFN_vkGetInstanceProcAddr p_loader_get_instance_proc_addr) {
	_interposer_bypass_reset();
	ERR_FAIL_NULL_MSG(p_loader_get_instance_proc_addr, "Vulkan interposer in use but the Vulkan loader could not be loaded: local rendering devices would break it, they are not supported.");

	bool complete = true;
#define BYPASS_SHIM(m_function) \
	interposer_bypass.interposer.m_function = vk##m_function; \
	interposer_bypass.loader.m_function = (PFN_vk##m_function)p_loader_get_instance_proc_addr(p_instance, "vk" #m_function); \
	if (interposer_bypass.interposer.m_function != nullptr && interposer_bypass.loader.m_function != nullptr) { \
		if (interposer_bypass.interposer.m_function != interposer_bypass.loader.m_function) { \
			interposer_bypass.shimmed++; \
		} \
		vk##m_function = _bypass_##m_function; \
	} else if (interposer_bypass.interposer.m_function != nullptr) { \
		complete = false; \
	}

	BYPASS_SHIM(GetDeviceProcAddr)
	BYPASS_SHIM(CreateDevice)
	BYPASS_SHIM(DestroyDevice)
	BYPASS_SHIM(DeviceWaitIdle)
	BYPASS_SHIM(CreateImage)
	BYPASS_SHIM(BeginCommandBuffer)
	BYPASS_SHIM(CmdPipelineBarrier)
	BYPASS_SHIM(CmdBindPipeline)
	BYPASS_SHIM(CmdBindDescriptorSets)
	BYPASS_SHIM(QueuePresentKHR)
	BYPASS_SHIM(CreateSwapchainKHR)
	BYPASS_SHIM(DestroySwapchainKHR)
	BYPASS_SHIM(GetSwapchainImagesKHR)
	BYPASS_SHIM(AcquireNextImageKHR)
#undef BYPASS_SHIM

	// Device creation and destruction must be routable, or a local device would re-initialize the interposer.
	interposer_bypass.installed = complete && vkCreateDevice == _bypass_CreateDevice && vkDestroyDevice == _bypass_DestroyDevice;
	if (interposer_bypass.installed) {
		print_verbose(vformat("Vulkan: interposer in use (%d device functions wrapped); local rendering devices go to the Vulkan loader directly.", interposer_bypass.shimmed));
	} else {
		WARN_PRINT("Vulkan: interposer in use and the loader's functions are incomplete; local rendering devices are not supported.");
	}
}
#endif

} // namespace

bool RenderingContextDriverVulkan::InterposerBypass::is_installed() {
	return interposer_bypass.installed;
}

void RenderingContextDriverVulkan::InterposerBypass::set_next_device_bypassed(bool p_bypassed) {
	interposer_bypass_next_device = p_bypassed;
}

bool RenderingContextDriverVulkan::InterposerBypass::is_device_bypassed(VkDevice p_device) {
	return _is_bypassed_key(_dispatch_key((const void *)p_device));
}

uint32_t RenderingContextDriverVulkan::InterposerBypass::get_bypassed_device_count() {
	return interposer_bypass.count.load(std::memory_order_acquire);
}

uint32_t RenderingContextDriverVulkan::InterposerBypass::get_shimmed_function_count() {
	return interposer_bypass.shimmed;
}

void RenderingContextDriverVulkan::set_interposer_for_testing(PFN_vkGetInstanceProcAddr p_get_instance_proc_addr) {
	interposer_for_testing = p_get_instance_proc_addr;
}

#if defined(VK_TRACK_DRIVER_MEMORY)
/*************************************************/
// Driver memory tracking
/*************************************************/
// Total driver memory and allocation amount.
SafeNumeric<size_t> driver_memory_total_memory;
SafeNumeric<size_t> driver_memory_total_alloc_count;
// Amount of driver memory for every object type.
SafeNumeric<size_t> driver_memory_tracker[RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_COUNT][RenderingContextDriverVulkan::VK_TRACKED_SYSTEM_ALLOCATION_SCOPE_COUNT];
// Amount of allocations for every object type.
SafeNumeric<uint32_t> driver_memory_allocation_count[RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_COUNT][RenderingContextDriverVulkan::VK_TRACKED_SYSTEM_ALLOCATION_SCOPE_COUNT];
#endif

#if defined(VK_TRACK_DEVICE_MEMORY)
/*************************************************/
// Device memory report
/*************************************************/
// Total device memory and allocation amount.
HashMap<uint64_t, size_t> memory_report_table;
// Total memory and allocation amount.
SafeNumeric<uint64_t> memory_report_total_memory;
SafeNumeric<uint64_t> memory_report_total_alloc_count;
// Amount of device memory for every object type.
SafeNumeric<size_t> memory_report_mem_usage[RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_COUNT];
// Amount of device memory allocations for every object type.
SafeNumeric<size_t> memory_report_allocation_count[RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_COUNT];
#endif

const char *RenderingContextDriverVulkan::get_tracked_object_name(uint32_t p_type_index) const {
#if defined(VK_TRACK_DRIVER_MEMORY) || defined(VK_TRACK_DEVICE_MEMORY)
	static constexpr const char *vkTrackedObjectTypeNames[] = { "UNKNOWN",
		"INSTANCE",
		"PHYSICAL_DEVICE",
		"DEVICE",
		"QUEUE",
		"SEMAPHORE",
		"COMMAND_BUFFER",
		"FENCE",
		"DEVICE_MEMORY",
		"BUFFER",
		"IMAGE",
		"EVENT",
		"QUERY_POOL",
		"BUFFER_VIEW",
		"IMAGE_VIEW",
		"SHADER_MODULE",
		"PIPELINE_CACHE",
		"PIPELINE_LAYOUT",
		"RENDER_PASS",
		"PIPELINE",
		"DESCRIPTOR_SET_LAYOUT",
		"SAMPLER",
		"DESCRIPTOR_POOL",
		"DESCRIPTOR_SET",
		"FRAMEBUFFER",
		"COMMAND_POOL",
		"DESCRIPTOR_UPDATE_TEMPLATE_KHR",
		"SURFACE_KHR",
		"SWAPCHAIN_KHR",
		"DEBUG_UTILS_MESSENGER_EXT",
		"DEBUG_REPORT_CALLBACK_EXT",
		"ACCELERATION_STRUCTURE",
		"VMA_BUFFER_OR_IMAGE" };

	return vkTrackedObjectTypeNames[p_type_index];
#else
	return "VK_TRACK_*_MEMORY disabled at build time";
#endif
}

#if defined(VK_TRACK_DRIVER_MEMORY) || defined(VK_TRACK_DEVICE_MEMORY)
uint64_t RenderingContextDriverVulkan::get_tracked_object_type_count() const {
	return VK_TRACKED_OBJECT_TYPE_COUNT;
}
#endif

#if defined(VK_TRACK_DRIVER_MEMORY) || defined(VK_TRACK_DEVICE_MEMORY)
RenderingContextDriverVulkan::VkTrackedObjectType vk_object_to_tracked_object(VkObjectType p_type) {
	if (p_type > VK_OBJECT_TYPE_COMMAND_POOL && p_type != (VkObjectType)RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_VMA) {
		switch (p_type) {
			case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE_KHR;
			case VK_OBJECT_TYPE_SURFACE_KHR:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_SURFACE;
			case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_SWAPCHAIN;
			case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT;
			case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT;
			case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:
			case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
				return RenderingContextDriverVulkan::VK_TRACKED_OBJECT_TYPE_ACCELERATION_STRUCTURE;
			default:
				_err_print_error(FUNCTION_STR, __FILE__, __LINE__, "Unknown VkObjectType enum value " + itos((uint32_t)p_type) + ".Please add it to VkTrackedObjectType, switch statement in "
																																 "vk_object_to_tracked_object and get_tracked_object_name.",
						(int)p_type);
				return (RenderingContextDriverVulkan::VkTrackedObjectType)VK_OBJECT_TYPE_UNKNOWN;
		}
	}

	return (RenderingContextDriverVulkan::VkTrackedObjectType)p_type;
}
#endif

#if defined(VK_TRACK_DEVICE_MEMORY)
uint64_t RenderingContextDriverVulkan::get_device_total_memory() const {
	return memory_report_total_memory.get();
}

uint64_t RenderingContextDriverVulkan::get_device_allocation_count() const {
	return memory_report_total_alloc_count.get();
}

uint64_t RenderingContextDriverVulkan::get_device_memory_by_object_type(uint32_t p_type) const {
	return memory_report_mem_usage[p_type].get();
}

uint64_t RenderingContextDriverVulkan::get_device_allocs_by_object_type(uint32_t p_type) const {
	return memory_report_allocation_count[p_type].get();
}
#endif

#if defined(VK_TRACK_DRIVER_MEMORY)
uint64_t RenderingContextDriverVulkan::get_driver_total_memory() const {
	return driver_memory_total_memory.get();
}

uint64_t RenderingContextDriverVulkan::get_driver_allocation_count() const {
	return driver_memory_total_alloc_count.get();
}

uint64_t RenderingContextDriverVulkan::get_driver_memory_by_object_type(uint32_t p_type) const {
	uint64_t ret = 0;
	for (uint32_t i = 0; i < VK_TRACKED_SYSTEM_ALLOCATION_SCOPE_COUNT; i++) {
		ret += driver_memory_tracker[p_type][i].get();
	}

	return ret;
}

uint64_t RenderingContextDriverVulkan::get_driver_allocs_by_object_type(uint32_t p_type) const {
	uint64_t ret = 0;
	for (uint32_t i = 0; i < VK_TRACKED_SYSTEM_ALLOCATION_SCOPE_COUNT; i++) {
		ret += driver_memory_allocation_count[p_type][i].get();
	}

	return ret;
}
#endif

#if defined(VK_TRACK_DEVICE_MEMORY)
void RenderingContextDriverVulkan::memory_report_callback(const VkDeviceMemoryReportCallbackDataEXT *p_callback_data, void *p_user_data) {
	if (!p_callback_data) {
		return;
	}
	const RenderingContextDriverVulkan::VkTrackedObjectType obj_type = vk_object_to_tracked_object(p_callback_data->objectType);
	uint64_t obj_id = p_callback_data->memoryObjectId;

	if (p_callback_data->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT) {
		// Realloc, update size
		if (memory_report_table.has(obj_id)) {
			memory_report_total_memory.sub(memory_report_table[obj_id]);
			memory_report_mem_usage[obj_type].sub(memory_report_table[obj_id]);

			memory_report_total_memory.add(p_callback_data->size);
			memory_report_mem_usage[obj_type].add(p_callback_data->size);

			memory_report_table[p_callback_data->memoryObjectId] = p_callback_data->size;
		} else {
			memory_report_table[obj_id] = p_callback_data->size;

			memory_report_total_alloc_count.increment();
			memory_report_allocation_count[obj_type].increment();
			memory_report_mem_usage[obj_type].add(p_callback_data->size);
			memory_report_total_memory.add(p_callback_data->size);
		}
	} else if (p_callback_data->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT) {
		if (memory_report_table.has(obj_id)) {
			memory_report_total_alloc_count.decrement();
			memory_report_allocation_count[obj_type].decrement();
			memory_report_mem_usage[obj_type].sub(p_callback_data->size);
			memory_report_total_memory.sub(p_callback_data->size);

			memory_report_table.remove(memory_report_table.find(obj_id));
		}
	}
}
#endif

VkAllocationCallbacks *RenderingContextDriverVulkan::get_allocation_callbacks(VkObjectType p_type) {
#if !defined(VK_TRACK_DRIVER_MEMORY)
	return nullptr;
#else
	if (!Engine::get_singleton()->is_extra_gpu_memory_tracking_enabled()) {
		return nullptr;
	}

#ifdef _MSC_VER
#define LAMBDA_VK_CALL_CONV
#else
#define LAMBDA_VK_CALL_CONV VKAPI_PTR
#endif

	struct TrackedMemHeader {
		size_t size;
		VkSystemAllocationScope allocation_scope;
		VkTrackedObjectType type;
	};
	VkAllocationCallbacks tracking_callbacks = {
		// Allocation function
		nullptr,
		[](
				void *p_user_data,
				size_t size,
				size_t alignment,
				VkSystemAllocationScope allocation_scope) LAMBDA_VK_CALL_CONV -> void * {
			static constexpr size_t tracking_data_size = 32;
			VkTrackedObjectType type = static_cast<VkTrackedObjectType>(*reinterpret_cast<VkTrackedObjectType *>(p_user_data));

			driver_memory_total_memory.add(size);
			driver_memory_total_alloc_count.increment();
			driver_memory_tracker[type][allocation_scope].add(size);
			driver_memory_allocation_count[type][allocation_scope].increment();

			alignment = MAX(alignment, tracking_data_size);

			uint8_t *ret = reinterpret_cast<uint8_t *>(Memory::alloc_aligned_static(size + alignment, alignment));
			if (ret == nullptr) {
				return nullptr;
			}

			// Track allocation
			TrackedMemHeader *header = reinterpret_cast<TrackedMemHeader *>(ret);
			header->size = size;
			header->allocation_scope = allocation_scope;
			header->type = type;
			*reinterpret_cast<size_t *>(ret + alignment - sizeof(size_t)) = alignment;

			// Return first available chunk of memory
			return ret + alignment;
		},

		// Reallocation function
		[](
				void *p_user_data,
				void *p_original,
				size_t size,
				size_t alignment,
				VkSystemAllocationScope allocation_scope) LAMBDA_VK_CALL_CONV -> void * {
			if (p_original == nullptr) {
				VkObjectType type = static_cast<VkObjectType>(*reinterpret_cast<uint32_t *>(p_user_data));
				return get_allocation_callbacks(type)->pfnAllocation(p_user_data, size, alignment, allocation_scope);
			}

			uint8_t *mem = reinterpret_cast<uint8_t *>(p_original);
			// Retrieve alignment
			alignment = *reinterpret_cast<size_t *>(mem - sizeof(size_t));
			// Retrieve allocation data
			TrackedMemHeader *header = reinterpret_cast<TrackedMemHeader *>(mem - alignment);

			// Update allocation size
			driver_memory_total_memory.sub(header->size);
			driver_memory_total_memory.add(size);
			driver_memory_tracker[header->type][header->allocation_scope].sub(header->size);
			driver_memory_tracker[header->type][header->allocation_scope].add(size);

			uint8_t *ret = reinterpret_cast<uint8_t *>(Memory::realloc_aligned_static(header, size + alignment, header->size + alignment, alignment));
			if (ret == nullptr) {
				return nullptr;
			}
			// Update tracker
			header = reinterpret_cast<TrackedMemHeader *>(ret);
			header->size = size;
			return ret + alignment;
		},

		// Free function
		[](
				void *p_user_data,
				void *p_memory) LAMBDA_VK_CALL_CONV {
			if (!p_memory) {
				return;
			}

			uint8_t *mem = reinterpret_cast<uint8_t *>(p_memory);
			size_t alignment = *reinterpret_cast<size_t *>(mem - sizeof(size_t));
			TrackedMemHeader *header = reinterpret_cast<TrackedMemHeader *>(mem - alignment);

			driver_memory_total_alloc_count.decrement();
			driver_memory_total_memory.sub(header->size);
			driver_memory_tracker[header->type][header->allocation_scope].sub(header->size);
			driver_memory_allocation_count[header->type][header->allocation_scope].decrement();

			Memory::free_aligned_static(header);
		},
		// Internal allocation / deallocation. We don't track them as they cannot really be controlled or optimized by the programmer.
		[](
				void *p_user_data,
				size_t size,
				VkInternalAllocationType allocation_type,
				VkSystemAllocationScope allocation_scope) LAMBDA_VK_CALL_CONV {
		},
		[](
				void *p_user_data,
				size_t size,
				VkInternalAllocationType allocation_type,
				VkSystemAllocationScope allocation_scope) LAMBDA_VK_CALL_CONV {
		},
	};

	// Create a callback per object type
	static VkAllocationCallbacks object_callbacks[VK_TRACKED_OBJECT_TYPE_COUNT] = {};
	static uint32_t object_user_data[VK_TRACKED_OBJECT_TYPE_COUNT] = {};

	// Only build the first time
	if (!object_callbacks[0].pfnAllocation) {
		for (uint32_t c = 0; c < VK_TRACKED_OBJECT_TYPE_COUNT; ++c) {
			object_callbacks[c] = tracking_callbacks;
			object_user_data[c] = c;
			object_callbacks[c].pUserData = &object_user_data[c];

			for (uint32_t i = 0; i < VK_TRACKED_SYSTEM_ALLOCATION_SCOPE_COUNT; i++) {
				driver_memory_tracker[c][i].set(0);
				driver_memory_allocation_count[c][i].set(0);
			}
		}
	}

	uint32_t type_index = vk_object_to_tracked_object(p_type);
	return &object_callbacks[type_index];
#endif
}

RenderingContextDriverVulkan::RenderingContextDriverVulkan() {
	// Empty constructor.
}

RenderingContextDriverVulkan::~RenderingContextDriverVulkan() {
	if (debug_messenger != VK_NULL_HANDLE && functions.DestroyDebugUtilsMessengerEXT != nullptr) {
		functions.DestroyDebugUtilsMessengerEXT(instance, debug_messenger, get_allocation_callbacks(VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT));
	}

	if (debug_report != VK_NULL_HANDLE && functions.DestroyDebugReportCallbackEXT != nullptr) {
		functions.DestroyDebugReportCallbackEXT(instance, debug_report, get_allocation_callbacks(VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT));
	}

	if (instance != VK_NULL_HANDLE) {
		vkDestroyInstance(instance, get_allocation_callbacks(VK_OBJECT_TYPE_INSTANCE));
	}
}

Error RenderingContextDriverVulkan::_initialize_vulkan_version() {
	// https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkApplicationInfo.html#_description
	// For Vulkan 1.0 vkEnumerateInstanceVersion is not available, including not in the loader we compile against on Android.
	typedef VkResult(VKAPI_PTR * _vkEnumerateInstanceVersion)(uint32_t *);
	_vkEnumerateInstanceVersion func = (_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
	if (func != nullptr) {
		uint32_t api_version;
		VkResult res = func(&api_version);
		if (res == VK_SUCCESS) {
			instance_api_version = api_version;
		} else {
			// According to the documentation this shouldn't fail with anything except a memory allocation error
			// in which case we're in deep trouble anyway.
			ERR_FAIL_V(ERR_CANT_CREATE);
		}
	} else {
		print_line("vkEnumerateInstanceVersion not available, assuming Vulkan 1.0.");
		instance_api_version = VK_API_VERSION_1_0;
	}

	return OK;
}

void RenderingContextDriverVulkan::_register_requested_instance_extension(const CharString &p_extension_name, bool p_required) {
	ERR_FAIL_COND(requested_instance_extensions.has(p_extension_name));
	requested_instance_extensions[p_extension_name] = p_required;
}

Error RenderingContextDriverVulkan::_initialize_instance_extensions() {
	enabled_instance_extension_names.clear();

	// The surface extension and the platform-specific surface extension are core requirements.
	_register_requested_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME, true);
	if (_get_platform_surface_extension()) {
		_register_requested_instance_extension(_get_platform_surface_extension(), true);
	}

	if (_use_validation_layers()) {
		_register_requested_instance_extension(VK_EXT_DEBUG_REPORT_EXTENSION_NAME, false);
	}

	// This extension allows us to use the properties2 features to query additional device capabilities.
	_register_requested_instance_extension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);

	// This extension allows us to use colorspaces other than SRGB.
	_register_requested_instance_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, false);

#if defined(USE_VOLK) && (defined(MACOS_ENABLED) || defined(IOS_ENABLED))
	_register_requested_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, true);
#endif

	// Only enable debug utils in verbose mode or DEV_ENABLED.
	// End users would get spammed with messages of varying verbosity due to the
	// mess that thirdparty layers/extensions and drivers seem to leave in their
	// wake, making the Windows registry a bottomless pit of broken layer JSON.
#ifdef DEV_ENABLED
	bool want_debug_utils = true;
#else
	bool want_debug_utils = OS::get_singleton()->is_stdout_verbose();
#endif
	if (Engine::get_singleton() && Engine::get_singleton()->is_gpu_markers_enabled()) {
		want_debug_utils = true;
	}
	if (want_debug_utils) {
		_register_requested_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false);
	}

	// Load instance extensions that are available.
	uint32_t instance_extension_count = 0;
	VkResult err = vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
	ERR_FAIL_COND_V(err != VK_SUCCESS && err != VK_INCOMPLETE, ERR_CANT_CREATE);
	ERR_FAIL_COND_V_MSG(instance_extension_count == 0, ERR_CANT_CREATE, "No instance extensions were found.");

	TightLocalVector<VkExtensionProperties> instance_extensions;
	instance_extensions.resize(instance_extension_count);
	err = vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions.ptr());
	if (err != VK_SUCCESS && err != VK_INCOMPLETE) {
		ERR_FAIL_V(ERR_CANT_CREATE);
	}

#ifdef DEV_ENABLED
	for (uint32_t i = 0; i < instance_extension_count; i++) {
		print_verbose(String("Vulkan: Found instance extension ") + String::utf8(instance_extensions[i].extensionName) + String("."));
	}
#endif

	// Enable all extensions that are supported and requested.
	for (uint32_t i = 0; i < instance_extension_count; i++) {
		CharString extension_name(instance_extensions[i].extensionName);
		if (requested_instance_extensions.has(extension_name)) {
			enabled_instance_extension_names.insert(extension_name);
		}
	}

	// Now check our requested extensions.
	for (KeyValue<CharString, bool> &requested_extension : requested_instance_extensions) {
		if (!enabled_instance_extension_names.has(requested_extension.key)) {
			if (requested_extension.value) {
				ERR_FAIL_V_MSG(ERR_BUG, String("Required Vulkan instance extension ") + String::utf8(requested_extension.key) + String(" not found."));
			} else {
				print_verbose(String("Optional Vulkan instance extension ") + String::utf8(requested_extension.key) + String(" not found."));
			}
		}
	}

	return OK;
}

Error RenderingContextDriverVulkan::_find_validation_layers(TightLocalVector<const char *> &r_layer_names) const {
	r_layer_names.clear();

	uint32_t instance_layer_count = 0;
	VkResult err = vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
	ERR_FAIL_COND_V(err != VK_SUCCESS, ERR_CANT_CREATE);
	if (instance_layer_count > 0) {
		TightLocalVector<VkLayerProperties> layer_properties;
		layer_properties.resize(instance_layer_count);
		err = vkEnumerateInstanceLayerProperties(&instance_layer_count, layer_properties.ptr());
		ERR_FAIL_COND_V(err != VK_SUCCESS, ERR_CANT_CREATE);

		// Preferred set of validation layers.
		const std::initializer_list<const char *> preferred = { "VK_LAYER_KHRONOS_validation" };

		// Alternative (deprecated, removed in SDK 1.1.126.0) set of validation layers.
		const std::initializer_list<const char *> lunarg = { "VK_LAYER_LUNARG_standard_validation" };

		// Alternative (deprecated, removed in SDK 1.1.121.1) set of validation layers.
		const std::initializer_list<const char *> google = { "VK_LAYER_GOOGLE_threading", "VK_LAYER_LUNARG_parameter_validation", "VK_LAYER_LUNARG_object_tracker", "VK_LAYER_LUNARG_core_validation", "VK_LAYER_GOOGLE_unique_objects" };

		// Verify all the layers of the list are present.
		for (const std::initializer_list<const char *> &list : { preferred, lunarg, google }) {
			bool layers_found = false;
			for (const char *layer_name : list) {
				layers_found = false;

				for (const VkLayerProperties &properties : layer_properties) {
					if (!strcmp(properties.layerName, layer_name)) {
						layers_found = true;
						break;
					}
				}

				if (!layers_found) {
					break;
				}
			}

			if (layers_found) {
				r_layer_names.reserve(list.size());
				for (const char *layer_name : list) {
					r_layer_names.push_back(layer_name);
				}

				break;
			}
		}
	}

	return OK;
}

VKAPI_ATTR VkBool32 VKAPI_CALL RenderingContextDriverVulkan::_debug_messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT p_message_severity, VkDebugUtilsMessageTypeFlagsEXT p_message_type, const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data, void *p_user_data) {
	// This error needs to be ignored because the AMD allocator will mix up memory types on IGP processors.
	if (strstr(p_callback_data->pMessage, "Mapping an image with layout") != nullptr && strstr(p_callback_data->pMessage, "can result in undefined behavior if this memory is used by the device") != nullptr) {
		return VK_FALSE;
	}
	// This needs to be ignored because Validator is wrong here.
	if (strstr(p_callback_data->pMessage, "Invalid SPIR-V binary version 1.3") != nullptr) {
		return VK_FALSE;
	}
	// This needs to be ignored because Validator is wrong here.
	if (strstr(p_callback_data->pMessage, "Shader requires flag") != nullptr) {
		return VK_FALSE;
	}

	// This needs to be ignored because Validator is wrong here.
	if (strstr(p_callback_data->pMessage, "SPIR-V module not valid: Pointer operand") != nullptr && strstr(p_callback_data->pMessage, "must be a memory object") != nullptr) {
		return VK_FALSE;
	}

	// Suppress warning about incompatible drivers (-9), even though this should arguably be the loader's responsibility.
	// This was generating noise on Linux when Mesa Dozen is provided.
	if (strstr(p_callback_data->pMessage, "Received return code -9 from call to vkCreateInstance in ICD") != nullptr && strstr(p_callback_data->pMessage, "Skipping this driver") != nullptr) {
		return VK_FALSE;
	}

	if (p_callback_data->pMessageIdName && strstr(p_callback_data->pMessageIdName, "UNASSIGNED-CoreValidation-DrawState-ClearCmdBeforeDraw") != nullptr) {
		return VK_FALSE;
	}

	String type_string;
	switch (p_message_type) {
		case (VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT):
			type_string = "GENERAL";
			break;
		case (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT):
			type_string = "VALIDATION";
			break;
		case (VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT):
			type_string = "PERFORMANCE";
			break;
		case (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT):
			type_string = "VALIDATION|PERFORMANCE";
			break;
	}

	String objects_string;
	if (p_callback_data->objectCount > 0) {
		objects_string = "\n\tObjects - " + String::num_int64(p_callback_data->objectCount);
		for (uint32_t object = 0; object < p_callback_data->objectCount; ++object) {
			objects_string +=
					"\n\t\tObject[" + String::num_int64(object) + "]" +
					" - " + string_VkObjectType(p_callback_data->pObjects[object].objectType) +
					", Handle " + String::num_int64(p_callback_data->pObjects[object].objectHandle);

			if (p_callback_data->pObjects[object].pObjectName != nullptr && strlen(p_callback_data->pObjects[object].pObjectName) > 0) {
				objects_string += ", Name \"" + String(p_callback_data->pObjects[object].pObjectName) + "\"";
			}
		}
	}

	String labels_string;
	if (p_callback_data->cmdBufLabelCount > 0) {
		labels_string = "\n\tCommand Buffer Labels - " + String::num_int64(p_callback_data->cmdBufLabelCount);
		for (uint32_t cmd_buf_label = 0; cmd_buf_label < p_callback_data->cmdBufLabelCount; ++cmd_buf_label) {
			labels_string +=
					"\n\t\tLabel[" + String::num_int64(cmd_buf_label) + "]" +
					" - " + p_callback_data->pCmdBufLabels[cmd_buf_label].pLabelName +
					"{ ";

			for (int color_idx = 0; color_idx < 4; ++color_idx) {
				labels_string += String::num(p_callback_data->pCmdBufLabels[cmd_buf_label].color[color_idx]);
				if (color_idx < 3) {
					labels_string += ", ";
				}
			}

			labels_string += " }";
		}
	}

	String error_message(type_string +
			" - Message Id Number: " + String::num_int64(p_callback_data->messageIdNumber) +
			" | Message Id Name: " + p_callback_data->pMessageIdName +
			"\n\t" + p_callback_data->pMessage +
			objects_string + labels_string);

	// Convert VK severity to our own log macros.
	// Note: Which severity we receive depends on what's set in VkDebugUtilsMessengerCreateInfoEXT,
	// we don't enable everything by default.
	switch (p_message_severity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			print_verbose(error_message);
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			print_line(error_message);
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			WARN_PRINT(error_message);
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			ERR_PRINT(error_message);
			CRASH_COND_MSG(Engine::get_singleton()->is_abort_on_gpu_errors_enabled(), "Crashing, because abort on GPU errors is enabled.");
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
			break; // Shouldn't happen, only handling to make compilers happy.
	}

	return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL RenderingContextDriverVulkan::_debug_report_callback(VkDebugReportFlagsEXT p_flags, VkDebugReportObjectTypeEXT p_object_type, uint64_t p_object, size_t p_location, int32_t p_message_code, const char *p_layer_prefix, const char *p_message, void *p_user_data) {
	String debug_message = String("Vulkan Debug Report: object - ") + String::num_int64(p_object) + "\n" + p_message;

	switch (p_flags) {
		case VK_DEBUG_REPORT_DEBUG_BIT_EXT:
		case VK_DEBUG_REPORT_INFORMATION_BIT_EXT:
			print_line(debug_message);
			break;
		case VK_DEBUG_REPORT_WARNING_BIT_EXT:
		case VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT:
			WARN_PRINT(debug_message);
			break;
		case VK_DEBUG_REPORT_ERROR_BIT_EXT:
			ERR_PRINT(debug_message);
			break;
	}

	return VK_FALSE;
}

Error RenderingContextDriverVulkan::_initialize_instance() {
	Error err;
	TightLocalVector<const char *> enabled_extension_names;
	enabled_extension_names.reserve(enabled_instance_extension_names.size());
	for (const CharString &extension_name : enabled_instance_extension_names) {
		enabled_extension_names.push_back(extension_name.ptr());
	}

	// We'll set application version to the Vulkan version we're developing against, even if our instance is based on an older Vulkan
	// version, devices can still support newer versions of Vulkan. The exception is when we're on Vulkan 1.0, we should not set this
	// to anything but 1.0. Note that this value is only used by validation layers to warn us about version issues.
	uint32_t application_api_version = instance_api_version == VK_API_VERSION_1_0 ? VK_API_VERSION_1_0 : VK_API_VERSION_1_2;

	CharString cs = GLOBAL_GET("application/config/name").operator String().utf8();
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = cs.get_data();
	app_info.pEngineName = GODOT_VERSION_NAME;
	app_info.engineVersion = VK_MAKE_VERSION(GODOT_VERSION_MAJOR, GODOT_VERSION_MINOR, GODOT_VERSION_PATCH);
	app_info.apiVersion = application_api_version;

	TightLocalVector<const char *> enabled_layer_names;
	if (_use_validation_layers()) {
		err = _find_validation_layers(enabled_layer_names);
		ERR_FAIL_COND_V(err != OK, err);
	}

	VkInstanceCreateInfo instance_info = {};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

#if defined(USE_VOLK) && (defined(MACOS_ENABLED) || defined(IOS_ENABLED))
	instance_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	instance_info.pApplicationInfo = &app_info;
	instance_info.enabledExtensionCount = enabled_extension_names.size();
	instance_info.ppEnabledExtensionNames = enabled_extension_names.ptr();
	instance_info.enabledLayerCount = enabled_layer_names.size();
	instance_info.ppEnabledLayerNames = enabled_layer_names.ptr();

	// This is info for a temp callback to use during CreateInstance. After the instance is created, we use the instance-based function to register the final callback.
	VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info = {};
	VkDebugReportCallbackCreateInfoEXT debug_report_callback_create_info = {};
	const bool has_debug_utils_extension = enabled_instance_extension_names.has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	const bool has_debug_report_extension = enabled_instance_extension_names.has(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
	if (has_debug_utils_extension) {
		debug_messenger_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debug_messenger_create_info.pNext = nullptr;
		debug_messenger_create_info.flags = 0;
		debug_messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug_messenger_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug_messenger_create_info.pfnUserCallback = _debug_messenger_callback;
		debug_messenger_create_info.pUserData = this;
		instance_info.pNext = &debug_messenger_create_info;
	} else if (has_debug_report_extension) {
		debug_report_callback_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		debug_report_callback_create_info.flags = VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
		debug_report_callback_create_info.pfnCallback = _debug_report_callback;
		debug_report_callback_create_info.pUserData = this;
		instance_info.pNext = &debug_report_callback_create_info;
	}

	err = _create_vulkan_instance(&instance_info, &instance);
	ERR_FAIL_COND_V(err != OK, err);

#ifdef USE_VOLK
	volkLoadInstance(instance);
	if (interposer_in_use) {
		_interposer_bypass_install(instance, loader_get_instance_proc_addr);
	}
#endif

	// Physical device.
	if (enabled_instance_extension_names.has(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
		functions.GetPhysicalDeviceFeatures2 = PFN_vkGetPhysicalDeviceFeatures2(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
		functions.GetPhysicalDeviceProperties2 = PFN_vkGetPhysicalDeviceProperties2(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

		// In Vulkan 1.0, the functions might be accessible under their original extension names.
		if (functions.GetPhysicalDeviceFeatures2 == nullptr) {
			functions.GetPhysicalDeviceFeatures2 = PFN_vkGetPhysicalDeviceFeatures2(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
		}

		if (functions.GetPhysicalDeviceProperties2 == nullptr) {
			functions.GetPhysicalDeviceProperties2 = PFN_vkGetPhysicalDeviceProperties2(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
		}
	}

	// Device.
	functions.GetDeviceProcAddr = PFN_vkGetDeviceProcAddr(vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
#ifdef USE_VOLK
	if (interposer_bypass.installed) {
		// Mariner E8: device functions of local devices come from the loader, not from the interposer.
		functions.GetDeviceProcAddr = _bypass_GetDeviceProcAddr;
	}
#endif

	// Surfaces.
	functions.GetPhysicalDeviceSurfaceSupportKHR = PFN_vkGetPhysicalDeviceSurfaceSupportKHR(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
	functions.GetPhysicalDeviceSurfaceFormatsKHR = PFN_vkGetPhysicalDeviceSurfaceFormatsKHR(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
	functions.GetPhysicalDeviceSurfaceCapabilitiesKHR = PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
	functions.GetPhysicalDeviceSurfacePresentModesKHR = PFN_vkGetPhysicalDeviceSurfacePresentModesKHR(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));

	// Debug utils and report.
	if (has_debug_utils_extension) {
		// Setup VK_EXT_debug_utils function pointers always (we use them for debug labels and names).
		functions.CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
		functions.DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
		functions.CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
		functions.CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
		functions.SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");

		if (!functions.debug_util_functions_available()) {
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, "GetProcAddr: Failed to init VK_EXT_debug_utils\nGetProcAddr: Failure");
		}

		VkResult res = functions.CreateDebugUtilsMessengerEXT(instance, &debug_messenger_create_info, get_allocation_callbacks(VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT), &debug_messenger);
		switch (res) {
			case VK_SUCCESS:
				break;
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				ERR_FAIL_V_MSG(ERR_CANT_CREATE, "CreateDebugUtilsMessengerEXT: out of host memory\nCreateDebugUtilsMessengerEXT Failure");
				break;
			default:
				ERR_FAIL_V_MSG(ERR_CANT_CREATE, "CreateDebugUtilsMessengerEXT: unknown failure\nCreateDebugUtilsMessengerEXT Failure");
				break;
		}
	} else if (has_debug_report_extension) {
		functions.CreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
		functions.DebugReportMessageEXT = (PFN_vkDebugReportMessageEXT)vkGetInstanceProcAddr(instance, "vkDebugReportMessageEXT");
		functions.DestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");

		if (!functions.debug_report_functions_available()) {
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, "GetProcAddr: Failed to init VK_EXT_debug_report\nGetProcAddr: Failure");
		}

		VkResult res = functions.CreateDebugReportCallbackEXT(instance, &debug_report_callback_create_info, get_allocation_callbacks(VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT), &debug_report);
		switch (res) {
			case VK_SUCCESS:
				break;
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				ERR_FAIL_V_MSG(ERR_CANT_CREATE, "CreateDebugReportCallbackEXT: out of host memory\nCreateDebugReportCallbackEXT Failure");
				break;
			default:
				ERR_FAIL_V_MSG(ERR_CANT_CREATE, "CreateDebugReportCallbackEXT: unknown failure\nCreateDebugReportCallbackEXT Failure");
				break;
		}
	}

	return OK;
}

Error RenderingContextDriverVulkan::_initialize_devices() {
	if (VulkanHooks::get_singleton() != nullptr) {
		VkPhysicalDevice physical_device;
		bool device_retrieved = VulkanHooks::get_singleton()->get_physical_device(&physical_device);
		ERR_FAIL_COND_V(!device_retrieved, ERR_CANT_CREATE);

		// When a hook is active, pretend the device returned by the hook is the only device available.
		driver_devices.resize(1);
		driver_device_infos.resize(1);
		physical_devices.resize(1);
		device_queue_families.resize(1);
		physical_devices[0] = physical_device;

	} else {
		uint32_t physical_device_count = 0;
		VkResult err = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
		ERR_FAIL_COND_V(err != VK_SUCCESS, ERR_CANT_CREATE);
		ERR_FAIL_COND_V_MSG(physical_device_count == 0, ERR_CANT_CREATE, "vkEnumeratePhysicalDevices reported zero accessible devices.\n\nDo you have a compatible Vulkan installable client driver (ICD) installed?\nvkEnumeratePhysicalDevices Failure.");

		driver_devices.resize(physical_device_count);
		driver_device_infos.resize(physical_device_count);
		physical_devices.resize(physical_device_count);
		device_queue_families.resize(physical_device_count);
		err = vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.ptr());
		ERR_FAIL_COND_V(err != VK_SUCCESS, ERR_CANT_CREATE);
	}

	// Fill the list of driver devices with the properties from the physical devices.
	for (uint32_t i = 0; i < physical_devices.size(); i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physical_devices[i], &props);

		Device &driver_device = driver_devices[i];
		driver_device.name = String::utf8(props.deviceName);
		driver_device.vendor = props.vendorID;
		driver_device.type = DeviceType(props.deviceType);

		// Mariner E8: memory and version for RenderingServer.get_physical_devices().
		DeviceInfo &info = driver_device_infos[i];
		info.device_id = props.deviceID;
		info.type = driver_device.type;
		info.api_version = vformat("%d.%d.%d", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
		VkPhysicalDeviceMemoryProperties memory_props = {};
		vkGetPhysicalDeviceMemoryProperties(physical_devices[i], &memory_props);
		bool all_heaps_device_local = memory_props.memoryHeapCount > 0;
		for (uint32_t h = 0; h < memory_props.memoryHeapCount; h++) {
			if (memory_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
				info.dedicated_memory += memory_props.memoryHeaps[h].size;
			} else {
				info.shared_memory += memory_props.memoryHeaps[h].size;
				all_heaps_device_local = false;
			}
		}
		// Integrated GPUs and software devices work from system memory; some report one device-local heap only.
		info.uma = driver_device.type == DEVICE_TYPE_INTEGRATED_GPU || driver_device.type == DEVICE_TYPE_CPU || all_heaps_device_local;

		uint32_t queue_family_properties_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_properties_count, nullptr);

		if (queue_family_properties_count > 0) {
			device_queue_families[i].properties.resize(queue_family_properties_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_properties_count, device_queue_families[i].properties.ptr());
		}
	}

	return OK;
}

bool RenderingContextDriverVulkan::_use_validation_layers() const {
	return Engine::get_singleton()->is_validation_layers_enabled();
}

Error RenderingContextDriverVulkan::_create_vulkan_instance(const VkInstanceCreateInfo *p_create_info, VkInstance *r_instance) {
	if (VulkanHooks::get_singleton() != nullptr) {
		return VulkanHooks::get_singleton()->create_vulkan_instance(p_create_info, r_instance) ? OK : ERR_CANT_CREATE;
	} else {
		VkResult err = vkCreateInstance(p_create_info, get_allocation_callbacks(VK_OBJECT_TYPE_INSTANCE), r_instance);
		ERR_FAIL_COND_V_MSG(err == VK_ERROR_INCOMPATIBLE_DRIVER, ERR_CANT_CREATE,
				"Cannot find a compatible Vulkan installable client driver (ICD).\n\n"
				"vkCreateInstance Failure");
		ERR_FAIL_COND_V_MSG(err == VK_ERROR_EXTENSION_NOT_PRESENT, ERR_CANT_CREATE,
				"Cannot find a specified extension library.\n"
				"Make sure your layers path is set appropriately.\n"
				"vkCreateInstance Failure");
		ERR_FAIL_COND_V_MSG(err, ERR_CANT_CREATE,
				"vkCreateInstance failed.\n\n"
				"Do you have a compatible Vulkan installable client driver (ICD) installed?\n"
				"Please look at the Getting Started guide for additional information.\n"
				"vkCreateInstance Failure");
	}

	return OK;
}

Error RenderingContextDriverVulkan::initialize() {
	Error err;

	_interposer_bypass_reset(); // Mariner E8.

#ifdef USE_VOLK
	// With Streamline (NVIDIA DLSS) active, route all Vulkan calls through sl.interposer.dll so it can hook
	// instance/device creation; otherwise load the system Vulkan loader as usual.
	void *sl_get_instance_proc_addr = Streamline::get_singleton() ? Streamline::get_singleton()->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_vkGetInstanceProcAddr) : nullptr;
	if (sl_get_instance_proc_addr == nullptr && interposer_for_testing != nullptr) {
		sl_get_instance_proc_addr = (void *)interposer_for_testing;
	}
	if (sl_get_instance_proc_addr != nullptr) {
		// Mariner E8: keep the loader's own vkGetInstanceProcAddr for local devices (see InterposerBypass).
		if (volkInitialize() == VK_SUCCESS) {
			loader_get_instance_proc_addr = vkGetInstanceProcAddr;
		}
		// Note: function pointer is cast through void function pointer to silence cast-function-type warning on gcc8.
		volkInitializeCustom((PFN_vkGetInstanceProcAddr)(void (*)())sl_get_instance_proc_addr);
		interposer_in_use = true;
	} else if (volkInitialize() != VK_SUCCESS) {
		return FAILED;
	}
#endif

	err = _initialize_vulkan_version();
	ERR_FAIL_COND_V(err != OK, err);

	err = _initialize_instance_extensions();
	ERR_FAIL_COND_V(err != OK, err);

	err = _initialize_instance();
	ERR_FAIL_COND_V(err != OK, err);

	err = _initialize_devices();
	ERR_FAIL_COND_V(err != OK, err);

	return OK;
}

const RenderingContextDriver::Device &RenderingContextDriverVulkan::device_get(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index < driver_devices.size());
	return driver_devices[p_device_index];
}

uint32_t RenderingContextDriverVulkan::device_get_count() const {
	return driver_devices.size();
}

bool RenderingContextDriverVulkan::device_get_info(uint32_t p_device_index, DeviceInfo &r_info) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_device_index, driver_device_infos.size(), false);
	r_info = driver_device_infos[p_device_index];
	return true;
}

bool RenderingContextDriverVulkan::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	DEV_ASSERT(p_device_index < physical_devices.size());

	// Check if any of the queues supported by the device supports presenting to the window's surface.
	const VkPhysicalDevice physical_device = physical_devices[p_device_index];
	const DeviceQueueFamilies &queue_families = device_queue_families[p_device_index];
	for (uint32_t i = 0; i < queue_families.properties.size(); i++) {
		if ((queue_families.properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queue_family_supports_present(physical_device, i, p_surface)) {
			return true;
		}
	}

	return false;
}

RenderingDeviceDriver *RenderingContextDriverVulkan::driver_create() {
	return memnew(RenderingDeviceDriverVulkan(this));
}

void RenderingContextDriverVulkan::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

RenderingContextDriver::SurfaceID RenderingContextDriverVulkan::surface_create(const void *p_platform_data) {
	DEV_ASSERT(false && "Surface creation should not be called on the platform-agnostic version of the driver.");
	return SurfaceID();
}

void RenderingContextDriverVulkan::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	Surface *surface = (Surface *)(p_surface);
	surface->width = p_width;
	surface->height = p_height;
	surface->needs_resize = true;
}

void RenderingContextDriverVulkan::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	Surface *surface = (Surface *)(p_surface);
	surface->vsync_mode = p_vsync_mode;
	surface->needs_resize = true;
}

DisplayServerEnums::VSyncMode RenderingContextDriverVulkan::surface_get_vsync_mode(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->vsync_mode;
}

void RenderingContextDriverVulkan::surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_output = p_enabled;
	surface->needs_resize = true;
}

bool RenderingContextDriverVulkan::surface_get_hdr_output_enabled(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_output;
}

void RenderingContextDriverVulkan::surface_set_hdr_output_reference_luminance(SurfaceID p_surface, float p_reference_luminance) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_reference_luminance = p_reference_luminance;
}

float RenderingContextDriverVulkan::surface_get_hdr_output_reference_luminance(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_reference_luminance;
}

void RenderingContextDriverVulkan::surface_set_hdr_output_max_luminance(SurfaceID p_surface, float p_max_luminance) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_max_luminance = p_max_luminance;
}

float RenderingContextDriverVulkan::surface_get_hdr_output_max_luminance(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_max_luminance;
}

void RenderingContextDriverVulkan::surface_set_hdr_output_linear_luminance_scale(SurfaceID p_surface, float p_linear_luminance_scale) {
	Surface *surface = (Surface *)(p_surface);
	surface->hdr_linear_luminance_scale = p_linear_luminance_scale;
}

float RenderingContextDriverVulkan::surface_get_hdr_output_linear_luminance_scale(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->hdr_linear_luminance_scale;
}

float RenderingContextDriverVulkan::surface_get_hdr_output_max_value(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return MAX(surface->hdr_max_luminance / MAX(surface->hdr_reference_luminance, 1.0f), 1.0f);
}

uint32_t RenderingContextDriverVulkan::surface_get_width(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->width;
}

uint32_t RenderingContextDriverVulkan::surface_get_height(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->height;
}

void RenderingContextDriverVulkan::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	Surface *surface = (Surface *)(p_surface);
	surface->needs_resize = p_needs_resize;
}

bool RenderingContextDriverVulkan::surface_get_needs_resize(SurfaceID p_surface) const {
	Surface *surface = (Surface *)(p_surface);
	return surface->needs_resize;
}

void RenderingContextDriverVulkan::surface_destroy(SurfaceID p_surface) {
	Surface *surface = (Surface *)(p_surface);
	vkDestroySurfaceKHR(instance, surface->vk_surface, get_allocation_callbacks(VK_OBJECT_TYPE_SURFACE_KHR));
	memdelete(surface);
}

bool RenderingContextDriverVulkan::is_debug_utils_enabled() const {
	return enabled_instance_extension_names.has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

bool RenderingContextDriverVulkan::is_colorspace_supported() const {
	return enabled_instance_extension_names.has(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
}

VkInstance RenderingContextDriverVulkan::instance_get() const {
	return instance;
}

VkPhysicalDevice RenderingContextDriverVulkan::physical_device_get(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index < physical_devices.size());
	return physical_devices[p_device_index];
}

uint32_t RenderingContextDriverVulkan::queue_family_get_count(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index < physical_devices.size());
	return device_queue_families[p_device_index].properties.size();
}

VkQueueFamilyProperties RenderingContextDriverVulkan::queue_family_get(uint32_t p_device_index, uint32_t p_queue_family_index) const {
	DEV_ASSERT(p_device_index < physical_devices.size());
	DEV_ASSERT(p_queue_family_index < queue_family_get_count(p_device_index));
	return device_queue_families[p_device_index].properties[p_queue_family_index];
}

bool RenderingContextDriverVulkan::queue_family_supports_present(VkPhysicalDevice p_physical_device, uint32_t p_queue_family_index, SurfaceID p_surface) const {
	DEV_ASSERT(p_physical_device != VK_NULL_HANDLE);
	DEV_ASSERT(p_surface != 0);
	Surface *surface = (Surface *)(p_surface);
	VkBool32 present_supported = false;
	VkResult err = vkGetPhysicalDeviceSurfaceSupportKHR(p_physical_device, p_queue_family_index, surface->vk_surface, &present_supported);
	return err == VK_SUCCESS && present_supported;
}

const RenderingContextDriverVulkan::Functions &RenderingContextDriverVulkan::functions_get() const {
	return functions;
}

#endif // VULKAN_ENABLED
