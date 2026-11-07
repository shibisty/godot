/**************************************************************************/
/*  ffx3_backend.cpp                                                      */
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

#include "ffx3_backend.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// clang-format off
// The library files every FidelityFX component shares.
FFX3_LIBRARY_BEGIN
namespace FFX3 {
#include "thirdparty/amd-fsr3/ffx_assert.cpp"
#include "thirdparty/amd-fsr3/ffx_message.cpp"
#include "thirdparty/amd-fsr3/ffx_object_management.cpp"
} // namespace FFX3
FFX3_LIBRARY_END
// clang-format on

using namespace RendererRD;
using namespace FFX3;

// Device.

FFX3Device::FFX3Device() {
	RenderingDevice *rd = RD::get_singleton();
	// No wave64 / shader model 6.6 permutations: one GLSL source per pass.
	capabilities.maximumSupportedShaderModel = FFX_SHADER_MODEL_5_1;
	capabilities.waveLaneCountMin = 32;
	capabilities.waveLaneCountMax = 32;
	capabilities.fp16Supported = rd->has_feature(RD::SUPPORTS_HALF_FLOAT);
	capabilities.raytracingSupported = false;

	RD::SamplerState state;
	state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	state.min_lod = -1000.0f;
	state.max_lod = 1000.0f;
	state.anisotropy_max = 1.0;
	point_clamp_sampler = rd->sampler_create(state);
	state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	linear_clamp_sampler = rd->sampler_create(state);
}

void FFX3Device::free_shaders() {
	for (KeyValue<uint32_t, FFX3Effect> &E : effects) {
		for (uint32_t i = 0; i < E.value.pass_count; i++) {
			FFX3Pass &pass = E.value.passes[i];
			// Pipelines are freed together with their shader.
			if (pass.shader && !pass.shares_shader && pass.shader_version.is_valid()) {
				pass.shader->version_free(pass.shader_version);
			}
			pass.shader_version = RID();
		}
	}
	effects.clear();
}

FFX3Device::~FFX3Device() {
	RenderingDevice *rd = RD::get_singleton();
	rd->free_rid(point_clamp_sampler);
	rd->free_rid(linear_clamp_sampler);
}

// Layout helpers.

void RendererRD::ffx3_layout(FFX3Pass &r_pass, bool p_fp16_variant, std::initializer_list<FFX3Binding> p_srvs, std::initializer_list<FFX3Binding> p_uavs, std::initializer_list<FFX3Binding> p_srv_buffers, std::initializer_list<FFX3Binding> p_uav_buffers, std::initializer_list<FFX3Binding> p_cbs) {
	r_pass.has_fp16_variant = p_fp16_variant;
	auto fill = [](LocalVector<FFX3Binding> &r_list, std::initializer_list<FFX3Binding> p_list) {
		r_list.clear();
		for (const FFX3Binding &b : p_list) {
			r_list.push_back(b);
		}
	};
	fill(r_pass.srvs, p_srvs);
	fill(r_pass.uavs, p_uavs);
	fill(r_pass.srv_buffers, p_srv_buffers);
	fill(r_pass.uav_buffers, p_uav_buffers);
	fill(r_pass.cbs, p_cbs);
}

String RendererRD::ffx3_check_layout_names(const FFX3Effect &p_effect, const wchar_t *const *p_srv_names, uint32_t p_srv_count, const wchar_t *const *p_uav_names, uint32_t p_uav_count, const wchar_t *const *p_cb_names, uint32_t p_cb_count, uint32_t p_skip_pass) {
	auto known = [](const wchar_t *const *p_names, uint32_t p_count, const wchar_t *p_name) {
		for (uint32_t i = 0; i < p_count; i++) {
			if (wcscmp(p_names[i], p_name) == 0) {
				return true;
			}
		}
		return false;
	};
	String errors;
	for (uint32_t p = 0; p < p_effect.pass_count; p++) {
		if (p == p_skip_pass) {
			continue;
		}
		const FFX3Pass &pass = p_effect.passes[p];
		if (pass.cbs.is_empty()) {
			errors += vformat("pass %d: no bindings; ", p);
		}
		for (const FFX3Binding &b : pass.srvs) {
			if (!known(p_srv_names, p_srv_count, b.name)) {
				errors += vformat("pass %d: unknown SRV %s; ", p, String(b.name));
			}
		}
		for (const FFX3Binding &b : pass.srv_buffers) {
			if (!known(p_srv_names, p_srv_count, b.name)) {
				errors += vformat("pass %d: unknown SRV buffer %s; ", p, String(b.name));
			}
		}
		for (const FFX3Binding &b : pass.uavs) {
			if (!known(p_uav_names, p_uav_count, b.name)) {
				errors += vformat("pass %d: unknown UAV %s; ", p, String(b.name));
			}
		}
		for (const FFX3Binding &b : pass.uav_buffers) {
			if (!known(p_uav_names, p_uav_count, b.name)) {
				errors += vformat("pass %d: unknown UAV buffer %s; ", p, String(b.name));
			}
		}
		for (const FFX3Binding &b : pass.cbs) {
			if (!known(p_cb_names, p_cb_count, b.name)) {
				errors += vformat("pass %d: unknown CB %s; ", p, String(b.name));
			}
		}
	}
	return errors;
}

// Resources.

uint32_t FFX3Context::Resources::add(RID p_rid, bool p_owned, bool p_buffer, const FfxResourceDescription &p_description) {
	uint32_t index;
	if (free_list.is_empty()) {
		index = rids.size();
		rids.resize(index + 1);
		owned.resize(index + 1);
		is_buffer.resize(index + 1);
		mip_slice_rids.resize(index + 1);
		descriptions.resize(index + 1);
	} else {
		index = free_list[free_list.size() - 1];
		free_list.resize(free_list.size() - 1);
	}
	rids[index] = p_rid;
	owned[index] = p_owned;
	is_buffer[index] = p_buffer;
	mip_slice_rids[index].clear();
	descriptions[index] = p_description;
	if (!p_owned) {
		dynamic_list.push_back(index);
	}
	return index;
}

void FFX3Context::Resources::remove(uint32_t p_index) {
	ERR_FAIL_UNSIGNED_INDEX(p_index, rids.size());
	RenderingDevice *rd = RD::get_singleton();
	for (RID &slice : mip_slice_rids[p_index]) {
		if (slice.is_valid()) {
			rd->free_rid(slice);
		}
	}
	mip_slice_rids[p_index].clear();
	if (owned[p_index] && rids[p_index].is_valid()) {
		rd->free_rid(rids[p_index]);
	}
	rids[p_index] = RID();
	owned[p_index] = false;
	is_buffer[p_index] = false;
	descriptions[p_index] = {};
	dynamic_list.erase(p_index);
	free_list.push_back(p_index);
}

static const struct {
	FfxSurfaceFormat ffx;
	RD::DataFormat rd;
} ffx3_formats[] = {
	{ FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT },
	{ FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS, RD::DATA_FORMAT_R32G32B32A32_SFLOAT },
	{ FFX_SURFACE_FORMAT_R32G32B32A32_UINT, RD::DATA_FORMAT_R32G32B32A32_UINT },
	{ FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
	{ FFX_SURFACE_FORMAT_R16G16B16A16_TYPELESS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT },
	{ FFX_SURFACE_FORMAT_R32G32_FLOAT, RD::DATA_FORMAT_R32G32_SFLOAT },
	{ FFX_SURFACE_FORMAT_R32_UINT, RD::DATA_FORMAT_R32_UINT },
	{ FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8B8A8_UNORM },
	{ FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS, RD::DATA_FORMAT_R8G8B8A8_UNORM },
	{ FFX_SURFACE_FORMAT_R8G8B8A8_SNORM, RD::DATA_FORMAT_R8G8B8A8_SNORM },
	{ FFX_SURFACE_FORMAT_R8G8B8A8_SRGB, RD::DATA_FORMAT_R8G8B8A8_SRGB },
	{ FFX_SURFACE_FORMAT_B8G8R8A8_UNORM, RD::DATA_FORMAT_B8G8R8A8_UNORM },
	{ FFX_SURFACE_FORMAT_B8G8R8A8_TYPELESS, RD::DATA_FORMAT_B8G8R8A8_UNORM },
	{ FFX_SURFACE_FORMAT_B8G8R8A8_SRGB, RD::DATA_FORMAT_B8G8R8A8_SRGB },
	{ FFX_SURFACE_FORMAT_R10G10B10A2_UNORM, RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ FFX_SURFACE_FORMAT_R11G11B10_FLOAT, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32 },
	{ FFX_SURFACE_FORMAT_R16G16_FLOAT, RD::DATA_FORMAT_R16G16_SFLOAT },
	{ FFX_SURFACE_FORMAT_R16G16_UINT, RD::DATA_FORMAT_R16G16_UINT },
	{ FFX_SURFACE_FORMAT_R16G16_SINT, RD::DATA_FORMAT_R16G16_SINT },
	{ FFX_SURFACE_FORMAT_R16_FLOAT, RD::DATA_FORMAT_R16_SFLOAT },
	{ FFX_SURFACE_FORMAT_R16_UINT, RD::DATA_FORMAT_R16_UINT },
	{ FFX_SURFACE_FORMAT_R16_UNORM, RD::DATA_FORMAT_R16_UNORM },
	{ FFX_SURFACE_FORMAT_R16_SNORM, RD::DATA_FORMAT_R16_SNORM },
	{ FFX_SURFACE_FORMAT_R8_UNORM, RD::DATA_FORMAT_R8_UNORM },
	{ FFX_SURFACE_FORMAT_R8_UINT, RD::DATA_FORMAT_R8_UINT },
	{ FFX_SURFACE_FORMAT_R8G8_UNORM, RD::DATA_FORMAT_R8G8_UNORM },
	{ FFX_SURFACE_FORMAT_R8G8_UINT, RD::DATA_FORMAT_R8G8_UINT },
	{ FFX_SURFACE_FORMAT_R32_FLOAT, RD::DATA_FORMAT_R32_SFLOAT },
	{ FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP, RD::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32 },
};

RD::DataFormat FFX3Context::to_rd_format(FfxSurfaceFormat p_format) {
	for (const auto &f : ffx3_formats) {
		if (f.ffx == p_format) {
			return f.rd;
		}
	}
	return RD::DATA_FORMAT_MAX;
}

FfxSurfaceFormat FFX3Context::to_ffx_format(RD::DataFormat p_format) {
	for (const auto &f : ffx3_formats) {
		if (f.rd == p_format) {
			return f.ffx;
		}
	}
	return FFX_SURFACE_FORMAT_UNKNOWN;
}

FfxResource FFX3Context::wrap_texture(RID *p_rid, const wchar_t *p_name) {
	FfxResource res = {};
	if (p_rid == nullptr || p_rid->is_null()) {
		return res;
	}
	RD::TextureFormat tf = RD::get_singleton()->texture_get_format(*p_rid);
	res.resource = p_rid;
	switch (tf.texture_type) {
		case RD::TEXTURE_TYPE_1D:
			res.description.type = FFX_RESOURCE_TYPE_TEXTURE1D;
			break;
		case RD::TEXTURE_TYPE_3D:
			res.description.type = FFX_RESOURCE_TYPE_TEXTURE3D;
			break;
		default:
			res.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
			break;
	}
	res.description.format = to_ffx_format(tf.format);
	res.description.width = tf.width;
	res.description.height = tf.height;
	res.description.depth = tf.depth;
	res.description.mipCount = tf.mipmaps;
	res.description.flags = FFX_RESOURCE_FLAGS_NONE;
	res.description.usage = (tf.usage_bits & RD::TEXTURE_USAGE_STORAGE_BIT) ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
	res.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	wcsncpy(res.name, p_name, FFX_RESOURCE_NAME_SIZE - 1);
	return res;
}

// FfxInterface callbacks.

static FFX3Context &ffx3_context(FfxInterface *p_interface) {
	return *reinterpret_cast<FFX3Context *>(p_interface->scratchBuffer);
}

static FfxVersionNumber ffx3_get_sdk_version(FfxInterface *p_interface) {
	return FFX_SDK_MAKE_VERSION(1, 1, 4);
}

static FfxErrorCode ffx3_get_effect_gpu_memory_usage(FfxInterface *p_interface, FfxUInt32 p_effect_context_id, FfxEffectMemoryUsage *r_usage) {
	*r_usage = {};
	return FFX_OK;
}

static FfxErrorCode ffx3_create_backend_context(FfxInterface *p_interface, FfxEffect p_effect, FfxEffectBindlessConfig *p_bindless_config, FfxUInt32 *r_effect_context_id) {
	FFX3Context &ctx = ffx3_context(p_interface);
	RenderingDevice *rd = RD::get_singleton();
	for (uint32_t i = 0; i < FFX3Context::UBO_RING_SIZE; i++) {
		if (ctx.ubo_ring[i].is_null()) {
			ctx.ubo_ring[i] = rd->uniform_buffer_create(FFX3Context::UBO_SIZE);
			ERR_FAIL_COND_V(ctx.ubo_ring[i].is_null(), FFX_ERROR_BACKEND_API_ERROR);
		}
	}
	if (ctx.placeholder_uav.is_null()) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		tf.width = 1;
		tf.height = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		ctx.placeholder_uav = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND_V(ctx.placeholder_uav.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	}
	*r_effect_context_id = 0; // One effect per interface.
	return FFX_OK;
}

static FfxErrorCode ffx3_get_device_capabilities(FfxInterface *p_interface, FfxDeviceCapabilities *r_capabilities) {
	*r_capabilities = ffx3_context(p_interface).device->capabilities;
	return FFX_OK;
}

static FfxErrorCode ffx3_destroy_backend_context(FfxInterface *p_interface, FfxUInt32 p_effect_context_id) {
	FFX3Context &ctx = ffx3_context(p_interface);
	for (RID &ubo : ctx.ubo_ring) {
		if (ubo.is_valid()) {
			RD::get_singleton()->free_rid(ubo);
			ubo = RID();
		}
	}
	return FFX_OK;
}

static FfxErrorCode ffx3_create_resource(FfxInterface *p_interface, const FfxCreateResourceDescription *p_desc, FfxUInt32 p_effect_context_id, FfxResourceInternal *r_resource) {
	ERR_FAIL_COND_V(p_desc->heapType != FFX_HEAP_TYPE_DEFAULT, FFX_ERROR_INVALID_ARGUMENT);
	RenderingDevice *rd = RD::get_singleton();
	FFX3Context &ctx = ffx3_context(p_interface);
	FfxResourceDescription res_desc = p_desc->resourceDescription;
	const FfxResourceInitData &init = p_desc->initData;
	const bool has_init = init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER || init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE;

	if (res_desc.type == FFX_RESOURCE_TYPE_BUFFER) {
		// Structured buffers (frame interpolation counters): "size" bytes.
		const uint32_t size = MAX(res_desc.size, 4u);
		Vector<uint8_t> data;
		if (has_init) {
			data.resize(size);
			memset(data.ptrw(), 0, size);
			if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER) {
				memcpy(data.ptrw(), init.buffer, MIN(size_t(size), init.size));
			} else {
				memset(data.ptrw(), init.value, MIN(size_t(size), init.size));
			}
		}
		RID buffer = rd->storage_buffer_create(size, Span<uint8_t>(data.ptr(), data.size()));
		ERR_FAIL_COND_V(buffer.is_null(), FFX_ERROR_BACKEND_API_ERROR);
		if (p_desc->name) {
			rd->set_resource_name(buffer, String(p_desc->name));
		}
		r_resource->internalIndex = ctx.resources.add(buffer, true, true, res_desc);
		return FFX_OK;
	}

	ERR_FAIL_COND_V(res_desc.type != FFX_RESOURCE_TYPE_TEXTURE1D && res_desc.type != FFX_RESOURCE_TYPE_TEXTURE2D && res_desc.type != FFX_RESOURCE_TYPE_TEXTURE3D, FFX_ERROR_INVALID_ARGUMENT);
	if (res_desc.mipCount == 0) {
		res_desc.mipCount = uint32_t(1 + std::floor(std::log2(MAX(MAX(res_desc.width, res_desc.height), MAX(res_desc.depth, 1u)))));
	}

	RD::TextureFormat tf;
	switch (res_desc.type) {
		case FFX_RESOURCE_TYPE_TEXTURE1D:
			tf.texture_type = RD::TEXTURE_TYPE_1D;
			break;
		case FFX_RESOURCE_TYPE_TEXTURE3D:
			tf.texture_type = RD::TEXTURE_TYPE_3D;
			break;
		default:
			tf.texture_type = RD::TEXTURE_TYPE_2D;
			break;
	}
	tf.format = FFX3Context::to_rd_format(res_desc.format);
	ERR_FAIL_COND_V_MSG(tf.format == RD::DATA_FORMAT_MAX, FFX_ERROR_INVALID_ARGUMENT, vformat("FidelityFX: unsupported surface format %d.", int(res_desc.format)));
	tf.width = MAX(res_desc.width, 1u);
	tf.height = MAX(res_desc.height, 1u);
	tf.depth = MAX(res_desc.depth, 1u);
	tf.mipmaps = res_desc.mipCount;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	if (res_desc.usage & FFX_RESOURCE_USAGE_UAV) {
		tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	}

	Vector<Vector<uint8_t>> initial_data;
	if (has_init) {
		// Only single-mip defaults (1x1, the 128x1 Lanczos table) are initialized, with exactly their size in bytes.
		ERR_FAIL_COND_V(tf.mipmaps != 1, FFX_ERROR_INVALID_ARGUMENT);
		Vector<uint8_t> bytes;
		bytes.resize(init.size);
		if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER) {
			memcpy(bytes.ptrw(), init.buffer, init.size);
		} else {
			memset(bytes.ptrw(), init.value, init.size);
		}
		initial_data.push_back(bytes);
	}

	RID texture = rd->texture_create(tf, RD::TextureView(), initial_data);
	ERR_FAIL_COND_V(texture.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	if (p_desc->name) {
		rd->set_resource_name(texture, String(p_desc->name));
	}
	r_resource->internalIndex = ctx.resources.add(texture, true, false, res_desc);
	return FFX_OK;
}

static FfxErrorCode ffx3_register_resource(FfxInterface *p_interface, const FfxResource *p_resource, FfxUInt32 p_effect_context_id, FfxResourceInternal *r_resource) {
	if (p_resource->resource == nullptr) {
		r_resource->internalIndex = -1;
		return FFX_OK;
	}
	const RID &rid = *reinterpret_cast<const RID *>(p_resource->resource);
	ERR_FAIL_COND_V(rid.is_null(), FFX_ERROR_INVALID_ARGUMENT);
	r_resource->internalIndex = ffx3_context(p_interface).resources.add(rid, false, p_resource->description.type == FFX_RESOURCE_TYPE_BUFFER, p_resource->description);
	return FFX_OK;
}

static FfxResource ffx3_get_resource(FfxInterface *p_interface, FfxResourceInternal p_resource) {
	FFX3Context &ctx = ffx3_context(p_interface);
	FfxResource res = {};
	if (ctx.resources.is_valid(p_resource.internalIndex)) {
		res.resource = &ctx.resources.rids[p_resource.internalIndex];
		res.description = ctx.resources.descriptions[p_resource.internalIndex];
		res.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	}
	return res;
}

static FfxErrorCode ffx3_unregister_resources(FfxInterface *p_interface, FfxCommandList p_command_list, FfxUInt32 p_effect_context_id) {
	FFX3Context &ctx = ffx3_context(p_interface);
	LocalVector<uint32_t> dynamic_list(ctx.resources.dynamic_list);
	for (uint32_t index : dynamic_list) {
		ctx.resources.remove(index);
	}
	return FFX_OK;
}

static FfxErrorCode ffx3_register_static_resource(FfxInterface *p_interface, const FfxStaticResourceDescription *p_desc, FfxUInt32 p_effect_context_id) {
	return FFX_ERROR_BACKEND_API_ERROR; // Bindless only; not used by these components.
}

static FfxResourceDescription ffx3_get_resource_description(FfxInterface *p_interface, FfxResourceInternal p_resource) {
	FFX3Context &ctx = ffx3_context(p_interface);
	if (ctx.resources.is_valid(p_resource.internalIndex)) {
		return ctx.resources.descriptions[p_resource.internalIndex];
	}
	return {};
}

static FfxErrorCode ffx3_destroy_resource(FfxInterface *p_interface, FfxResourceInternal p_resource, FfxUInt32 p_effect_context_id) {
	// Also reached through ffxSafeReleaseCopyResource() with "index + 1" (the SDK's own backends keep an upload copy
	// next to initialized resources; this one doesn't), so unknown, borrowed or already released indices are ignored.
	FFX3Context &ctx = ffx3_context(p_interface);
	if (ctx.resources.is_valid(p_resource.internalIndex) && ctx.resources.owned[p_resource.internalIndex]) {
		ctx.resources.remove(p_resource.internalIndex);
	}
	return FFX_OK;
}

static FfxErrorCode ffx3_map_resource(FfxInterface *p_interface, FfxResourceInternal p_resource, void **r_ptr) {
	return FFX_ERROR_BACKEND_API_ERROR;
}

static FfxErrorCode ffx3_unmap_resource(FfxInterface *p_interface, FfxResourceInternal p_resource) {
	return FFX_ERROR_BACKEND_API_ERROR;
}

static FfxErrorCode ffx3_stage_constant_buffer(FfxInterface *p_interface, void *p_data, FfxUInt32 p_size, FfxConstantBuffer *r_buffer) {
	FFX3Context &ctx = ffx3_context(p_interface);
	ERR_FAIL_COND_V(p_size > FFX3Context::UBO_SIZE, FFX_ERROR_OUT_OF_RANGE);
	uint32_t *staging = ctx.cb_staging[ctx.cb_staging_index];
	ctx.cb_staging_index = (ctx.cb_staging_index + 1) % FFX3Context::CB_STAGING_COUNT;
	memset(staging, 0, FFX3Context::UBO_SIZE);
	memcpy(staging, p_data, p_size);
	r_buffer->num32BitEntries = p_size / sizeof(uint32_t);
	r_buffer->data = staging;
	return FFX_OK;
}

static FfxErrorCode ffx3_create_pipeline(FfxInterface *p_interface, FfxEffect p_effect, FfxPass p_pass, uint32_t p_permutation_options, const FfxPipelineDescription *p_desc, FfxUInt32 p_effect_context_id, FfxPipelineState *r_pipeline) {
	FFX3Device &device = *ffx3_context(p_interface).device;
	FFX3Effect *effect = device.effects.getptr(uint32_t(p_effect));
	ERR_FAIL_NULL_V_MSG(effect, FFX_ERROR_INVALID_ARGUMENT, vformat("FidelityFX: effect %d has no Godot passes.", int(p_effect)));
	ERR_FAIL_UNSIGNED_INDEX_V(p_pass, effect->pass_count, FFX_ERROR_INVALID_ARGUMENT);
	FFX3Pass &pass = effect->passes[p_pass];
	*r_pipeline = {};
	r_pipeline->passId = p_pass;
	if (pass.shader == nullptr) {
		return FFX_OK; // A pass without a GLSL version (never dispatched by Godot).
	}

	const uint32_t variant = effect->select_variant ? effect->select_variant(p_pass, p_permutation_options, pass) : 0;
	ERR_FAIL_UNSIGNED_INDEX_V(variant, 4u, FFX_ERROR_INVALID_ARGUMENT);
	FFX3PipelineVariant &pv = pass.variants[variant];
	if (pv.pipeline.is_null()) {
		pv.shader = pass.shader->version_get_shader(pass.shader_version, variant);
		ERR_FAIL_COND_V(pv.shader.is_null(), FFX_ERROR_BACKEND_API_ERROR);
		pv.pipeline = RD::get_singleton()->compute_pipeline_create(pv.shader);
		ERR_FAIL_COND_V(pv.pipeline.is_null(), FFX_ERROR_BACKEND_API_ERROR);
	}
	pv.pass = &pass;
	r_pipeline->pipeline = reinterpret_cast<FfxPipeline>(&pv);
	r_pipeline->rootSignature = reinterpret_cast<FfxRootSignature>(effect);

	ERR_FAIL_COND_V(pass.srvs.size() > FFX_MAX_NUM_SRVS || pass.uavs.size() > FFX_MAX_NUM_UAVS || pass.srv_buffers.size() > FFX_MAX_NUM_SRVS || pass.uav_buffers.size() > FFX_MAX_NUM_UAVS || pass.cbs.size() > FFX_MAX_NUM_CONST_BUFFERS, FFX_ERROR_OUT_OF_RANGE);
	auto copy = [](const LocalVector<FFX3Binding> &p_from, FfxResourceBinding *r_to, uint32_t &r_count) {
		r_count = p_from.size();
		for (uint32_t i = 0; i < p_from.size(); i++) {
			r_to[i].slotIndex = p_from[i].slot;
			wcsncpy(r_to[i].name, p_from[i].name, FFX_RESOURCE_NAME_SIZE - 1);
		}
	};
	copy(pass.srvs, r_pipeline->srvTextureBindings, r_pipeline->srvTextureCount);
	copy(pass.uavs, r_pipeline->uavTextureBindings, r_pipeline->uavTextureCount);
	copy(pass.srv_buffers, r_pipeline->srvBufferBindings, r_pipeline->srvBufferCount);
	copy(pass.uav_buffers, r_pipeline->uavBufferBindings, r_pipeline->uavBufferCount);
	copy(pass.cbs, r_pipeline->constantBufferBindings, r_pipeline->constCount);
	wcsncpy(r_pipeline->name, p_desc->name, FFX_RESOURCE_NAME_SIZE - 1);
	return FFX_OK;
}

static FfxErrorCode ffx3_destroy_pipeline(FfxInterface *p_interface, FfxPipelineState *p_pipeline, FfxUInt32 p_effect_context_id) {
	return FFX_OK; // Pipelines belong to the effect (shared by every context) and are freed with it.
}

static FfxErrorCode ffx3_schedule_gpu_job(FfxInterface *p_interface, const FfxGpuJobDescription *p_job) {
	FFX3Context &ctx = ffx3_context(p_interface);
	FFX3Context::Job job;
	job.type = p_job->jobType;
	switch (p_job->jobType) {
		case FFX_GPU_JOB_CLEAR_FLOAT: {
			job.target = p_job->clearJobDescriptor.target.internalIndex;
			memcpy(job.color, p_job->clearJobDescriptor.color, sizeof(job.color));
		} break;
		case FFX_GPU_JOB_COPY: {
			job.source = p_job->copyJobDescriptor.src.internalIndex;
			job.target = p_job->copyJobDescriptor.dst.internalIndex;
		} break;
		case FFX_GPU_JOB_COMPUTE: {
			const FfxComputeJobDescription &cj = p_job->computeJobDescriptor;
			job.pipeline = reinterpret_cast<const FFX3PipelineVariant *>(cj.pipeline.pipeline);
			job.effect = reinterpret_cast<const FFX3Effect *>(cj.pipeline.rootSignature);
			ERR_FAIL_NULL_V(job.pipeline, FFX_ERROR_INVALID_ARGUMENT);
			memcpy(job.dimensions, cj.dimensions, sizeof(job.dimensions));
			for (uint32_t i = 0; i < cj.pipeline.srvTextureCount; i++) {
				job.srvs.push_back({ cj.pipeline.srvTextureBindings[i].slotIndex, cj.srvTextures[i].resource.internalIndex, 0 });
			}
			for (uint32_t i = 0; i < cj.pipeline.uavTextureCount; i++) {
				job.uavs.push_back({ cj.pipeline.uavTextureBindings[i].slotIndex, cj.uavTextures[i].resource.internalIndex, cj.uavTextures[i].mip });
			}
			for (uint32_t i = 0; i < cj.pipeline.srvBufferCount; i++) {
				job.srv_buffers.push_back({ cj.pipeline.srvBufferBindings[i].slotIndex, cj.srvBuffers[i].resource.internalIndex, 0 });
			}
			for (uint32_t i = 0; i < cj.pipeline.uavBufferCount; i++) {
				job.uav_buffers.push_back({ cj.pipeline.uavBufferBindings[i].slotIndex, cj.uavBuffers[i].resource.internalIndex, 0 });
			}
			for (uint32_t i = 0; i < cj.pipeline.constCount; i++) {
				job.cbs.push_back({ cj.pipeline.constantBufferBindings[i].slotIndex, cj.cbs[i].num32BitEntries, cj.cbs[i].data });
			}
		} break;
		case FFX_GPU_JOB_DISCARD:
		case FFX_GPU_JOB_BARRIER: {
			return FFX_OK; // RD tracks resource usage itself.
		}
		default: {
			ERR_FAIL_V_MSG(FFX_ERROR_INVALID_ARGUMENT, vformat("FidelityFX: unsupported GPU job type %d.", int(p_job->jobType)));
		}
	}
	ctx.jobs.push_back(job);
	return FFX_OK;
}

static RID ffx3_storage_view(FFX3Context &p_ctx, int32_t p_index, uint32_t p_mip) {
	FFX3Context::Resources &res = p_ctx.resources;
	const uint32_t mip_count = MAX(res.descriptions[p_index].mipCount, 1u);
	if (mip_count == 1) {
		return res.rids[p_index];
	}
	// Storage images must be single-mip views. Pyramids bind a fixed number of levels (13 for the frame interpolation's
	// inpainting pyramid); those past the texture's last level are never written (the SPD constants stop before them),
	// so they get the last level, like the SDK's own backends do.
	p_mip = MIN(p_mip, mip_count - 1);
	LocalVector<RID> &slices = res.mip_slice_rids[p_index];
	if (slices.is_empty()) {
		slices.resize(mip_count);
	}
	if (slices[p_mip].is_null()) {
		slices[p_mip] = RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), res.rids[p_index], 0, p_mip, 1);
	}
	return slices[p_mip];
}

static FfxErrorCode ffx3_execute_gpu_jobs(FfxInterface *p_interface, FfxCommandList p_command_list, FfxUInt32 p_effect_context_id) {
	FFX3Context &ctx = ffx3_context(p_interface);
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	FfxErrorCode result = FFX_OK;

	for (const FFX3Context::Job &job : ctx.jobs) {
		if (job.type == FFX_GPU_JOB_CLEAR_FLOAT) {
			if (!ctx.resources.is_valid(job.target)) {
				result = FFX_ERROR_INVALID_ARGUMENT;
				break;
			}
			if (ctx.resources.is_buffer[job.target]) {
				rd->buffer_clear(ctx.resources.rids[job.target], 0, MAX(ctx.resources.descriptions[job.target].size, 4u) & ~3u);
			} else {
				const FfxResourceDescription &desc = ctx.resources.descriptions[job.target];
				rd->texture_clear(ctx.resources.rids[job.target], Color(job.color[0], job.color[1], job.color[2], job.color[3]), 0, MAX(desc.mipCount, 1u), 0, 1);
			}
		} else if (job.type == FFX_GPU_JOB_COPY) {
			if (!ctx.resources.is_valid(job.source) || !ctx.resources.is_valid(job.target)) {
				result = FFX_ERROR_INVALID_ARGUMENT;
				break;
			}
			if (ctx.resources.is_buffer[job.source] || ctx.resources.is_buffer[job.target]) {
				result = FFX_ERROR_INVALID_ARGUMENT; // Not used by these components.
				break;
			}
			const FfxResourceDescription &desc = ctx.resources.descriptions[job.source];
			for (uint32_t mip = 0; mip < MAX(desc.mipCount, 1u); mip++) {
				rd->texture_copy(ctx.resources.rids[job.source], ctx.resources.rids[job.target], Vector3(), Vector3(), Vector3(MAX(desc.width >> mip, 1u), MAX(desc.height >> mip, 1u), MAX(desc.depth, 1u)), mip, mip, 0, 0);
			}
		} else if (job.type == FFX_GPU_JOB_COMPUTE) {
			thread_local LocalVector<RD::Uniform> uniforms;
			uniforms.clear();
			for (const FFX3Context::Job::Binding &b : job.srvs) {
				if (!ctx.resources.is_valid(b.resource)) {
					result = FFX_ERROR_INVALID_ARGUMENT;
					break;
				}
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, b.slot, ctx.resources.rids[b.resource]));
			}
			for (const FFX3Context::Job::Binding &b : job.uavs) {
				if (!ctx.resources.is_valid(b.resource)) {
					result = FFX_ERROR_INVALID_ARGUMENT;
					break;
				}
				RID view = ffx3_storage_view(ctx, b.resource, b.mip);
				if (view.is_null()) {
					result = FFX_ERROR_BACKEND_API_ERROR;
					break;
				}
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, b.slot, view));
			}
			for (const LocalVector<FFX3Context::Job::Binding> *list : { &job.srv_buffers, &job.uav_buffers }) {
				for (const FFX3Context::Job::Binding &b : *list) {
					if (!ctx.resources.is_valid(b.resource)) {
						result = FFX_ERROR_INVALID_ARGUMENT;
						break;
					}
					uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, b.slot, ctx.resources.rids[b.resource]));
				}
			}
			if (result != FFX_OK) {
				ERR_PRINT("FidelityFX: a resource of a compute job is missing.");
				break;
			}
			for (uint32_t slot : job.pipeline->pass->placeholder_uavs) {
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, slot, ctx.placeholder_uav));
			}
			for (const FFX3Context::Job::CB &cb : job.cbs) {
				RID buffer = ctx.ubo_ring[ctx.ubo_ring_index];
				ctx.ubo_ring_index = (ctx.ubo_ring_index + 1) % FFX3Context::UBO_RING_SIZE;
				rd->buffer_update(buffer, 0, FFX3Context::UBO_SIZE, cb.data);
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, cb.slot, buffer));
			}
			if (job.effect) {
				for (const Pair<uint32_t, bool> &sampler : job.effect->samplers) {
					uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, sampler.first, sampler.second ? ctx.device->linear_clamp_sampler : ctx.device->point_clamp_sampler));
				}
			}

			RD::ComputeListID compute_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(compute_list, job.pipeline->pipeline);
			rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(job.pipeline->shader, 0, uniforms), 0);
			rd->compute_list_dispatch(compute_list, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
			rd->compute_list_end();
		}
	}

	ctx.jobs.clear();
	return result;
}

void FFX3Context::setup_interface(FfxInterface &r_interface, uint32_t p_effect) {
	effect = p_effect;
	r_interface.fpGetSDKVersion = ffx3_get_sdk_version;
	r_interface.fpGetEffectGpuMemoryUsage = ffx3_get_effect_gpu_memory_usage;
	r_interface.fpCreateBackendContext = ffx3_create_backend_context;
	r_interface.fpGetDeviceCapabilities = ffx3_get_device_capabilities;
	r_interface.fpDestroyBackendContext = ffx3_destroy_backend_context;
	r_interface.fpCreateResource = ffx3_create_resource;
	r_interface.fpRegisterResource = ffx3_register_resource;
	r_interface.fpGetResource = ffx3_get_resource;
	r_interface.fpUnregisterResources = ffx3_unregister_resources;
	r_interface.fpRegisterStaticResource = ffx3_register_static_resource;
	r_interface.fpGetResourceDescription = ffx3_get_resource_description;
	r_interface.fpDestroyResource = ffx3_destroy_resource;
	r_interface.fpMapResource = ffx3_map_resource;
	r_interface.fpUnmapResource = ffx3_unmap_resource;
	r_interface.fpStageConstantBufferDataFunc = ffx3_stage_constant_buffer;
	r_interface.fpCreatePipeline = ffx3_create_pipeline;
	r_interface.fpDestroyPipeline = ffx3_destroy_pipeline;
	r_interface.fpScheduleGpuJob = ffx3_schedule_gpu_job;
	r_interface.fpExecuteGpuJobs = ffx3_execute_gpu_jobs;
	r_interface.scratchBuffer = this;
	r_interface.scratchBufferSize = sizeof(FFX3Context);
	r_interface.device = device;
}

void FFX3Context::release() {
	RenderingDevice *rd = RD::get_singleton();
	jobs.clear();
	for (RID &ubo : ubo_ring) {
		if (ubo.is_valid()) {
			rd->free_rid(ubo);
			ubo = RID();
		}
	}
	for (uint32_t i = 0; i < resources.rids.size(); i++) {
		if (resources.rids[i].is_valid()) {
			resources.remove(i);
		}
	}
	if (placeholder_uav.is_valid()) {
		rd->free_rid(placeholder_uav);
		placeholder_uav = RID();
	}
}
