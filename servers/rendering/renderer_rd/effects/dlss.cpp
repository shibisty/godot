/**************************************************************************/
/*  dlss.cpp                                                              */
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

#include "dlss.h"

#ifdef STREAMLINE_ENABLED
#define ENABLE_DLSS 1
#endif

#ifdef ENABLE_DLSS
#include "drivers/streamline/streamline_context.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#endif

using namespace RendererRD;

#ifdef ENABLE_DLSS

// Texture layout/state constants (avoid including Vulkan/D3D12 headers here).
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY = 5; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR = 0x40; // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
static constexpr float DLSS_OPTIMAL_MODE_MAX_DISTANCE = 1000000.0f;

namespace RendererRD {
class DLSSContextInner : public DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::Constants constants;
	sl::DLSSOptions currentDlssOptions;
	sl::DLSSOptimalSettings currentOptimalSettings;

	DLSSContextInner();
	virtual ~DLSSContextInner();

	// Picks the DLSS quality mode (DLAA, Quality, Balanced, Performance, Ultra Performance) whose optimal render
	// size is closest to the internal size Godot renders at (scaling_3d_scale), within that mode's allowed range.
	sl::DLSSMode find_optimal_mode(uint32_t outputWidth, uint32_t outputHeight, uint32_t desiredWidth, uint32_t desiredHeight, sl::DLSSOptimalSettings &out_optimalSettings) {
		if (StreamlineContext::get().slDLSSGetOptimalSettings == nullptr || !StreamlineContext::get().streamline_capabilities.dlss_available) {
			return sl::DLSSMode::eOff;
		}

		sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
		sl::DLSSOptimalSettings settings[sizeof(modes) / sizeof(modes[0])];
		bool validSettings[sizeof(modes) / sizeof(modes[0])];
		Vector2 distance[sizeof(modes) / sizeof(modes[0])];
		memset(validSettings, 0, sizeof(validSettings));

		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			sl::DLSSOptions dlssOptions = {};
			dlssOptions.outputWidth = outputWidth;
			dlssOptions.outputHeight = outputHeight;
			dlssOptions.mode = modes[i];
			sl::Result result = StreamlineContext::get().slDLSSGetOptimalSettings(dlssOptions, settings[i]);
			if (result != sl::Result::eOk) {
				continue;
			}

			sl::DLSSOptimalSettings &optimalSettings = settings[i];
			if (desiredWidth >= optimalSettings.renderWidthMin &&
					desiredWidth <= optimalSettings.renderWidthMax &&
					desiredHeight >= optimalSettings.renderHeightMin &&
					desiredHeight <= optimalSettings.renderHeightMax) {
				validSettings[i] = true;
				distance[i] = Vector2(Math::abs((float)optimalSettings.optimalRenderWidth - (float)desiredWidth), Math::abs((float)optimalSettings.optimalRenderHeight - (float)desiredHeight));
			}
		}

		// Now select the closest match.
		Vector2 closestDistance(DLSS_OPTIMAL_MODE_MAX_DISTANCE, DLSS_OPTIMAL_MODE_MAX_DISTANCE);
		int closestDistanceMatch = -1;
		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			if (validSettings[i]) {
				if (distance[i].length_squared() < closestDistance.length_squared()) {
					closestDistanceMatch = i;
					closestDistance = distance[i];
				}
			}
		}

		if (closestDistanceMatch != -1) {
			out_optimalSettings = settings[closestDistanceMatch];
			return modes[closestDistanceMatch];
		}

		ERR_FAIL_V_MSG(sl::DLSSMode::eOff, vformat("Couldn't find an appropriate DLSS mode for rendering at %dx%d into %dx%d (DLSS supports 3D resolution scales of roughly 0.33 to 1.0).", desiredWidth, desiredHeight, outputWidth, outputHeight));
	}
}; // end class
}; // end namespace RendererRD

static Vector<unsigned int> g_dlss_freeViewportIndices;
static unsigned int g_dlss_viewportIndex = 1;

DLSSContextInner::~DLSSContextInner() {
	// Frame generation must not keep running on a Streamline viewport whose tags are gone (resize, upscaler change).
	if (StreamlineContext::get().dlssg_active() && (uint32_t)StreamlineContext::get().dlssg_viewport == (uint32_t)viewport) {
		StreamlineContext::get().dlssg_disable("its DLSS context was freed");
	}
	g_dlss_freeViewportIndices.push_back((unsigned int)viewport);
}

// Fills an sl::Resource for a Godot texture that the render graph has put into the shader-read state (driver
// callbacks declared with CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE).
static void _sl_resource_from_texture(RID p_texture, bool p_is_d3d12, sl::Resource &r_resource) {
	RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_texture);
	uint64_t texture_image = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_texture);
	uint64_t texture_view = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
	uint64_t texture_device_memory = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture);
	uint64_t texture_state = p_is_d3d12 ? DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR : DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY;
	uint64_t texture_native_format = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
	uint64_t texture_usage_flags = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture);
	if (p_is_d3d12) {
		// On D3D12, Godot's "texture view" handle is the ID3D12Resource.
		r_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_view, texture_state);
	} else {
		r_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_image, (void *)texture_device_memory, (void *)texture_view, texture_state);
	}
	r_resource.width = texture_format.width;
	r_resource.height = texture_format.height;
	r_resource.nativeFormat = texture_native_format;
	r_resource.arrayLayers = texture_format.array_layers;
	r_resource.flags = 0;
	r_resource.mipLevels = texture_format.mipmaps;
	r_resource.usage = texture_usage_flags;
}

DLSSContextInner::DLSSContextInner() {
	if (g_dlss_freeViewportIndices.size() == 0) {
		g_dlss_freeViewportIndices.push_back(g_dlss_viewportIndex++);
	}
	viewport = g_dlss_freeViewportIndices[g_dlss_freeViewportIndices.size() - 1];
	g_dlss_freeViewportIndices.remove_at(g_dlss_freeViewportIndices.size() - 1);
}

DLSSEffect::DLSSEffect() {
	// Initialize motion vector decode shader.
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));
}

DLSSEffect::~DLSSEffect() {
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	DLSSContextInner *context = memnew(RendererRD::DLSSContextInner);

	context->currentDlssOptions.mode = context->find_optimal_mode(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height, context->currentOptimalSettings);
	context->currentDlssOptions.outputWidth = p_target_size.width;
	context->currentDlssOptions.outputHeight = p_target_size.height;

	context->is_d3d12 = (RD::get_singleton()->get_device_api_name().to_lower() == "d3d12");

	return context;
}

static sl::float4x4 sl_make_identity_matrix() {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	ret.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	ret.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	ret.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return ret;
}

static sl::float4x4 sl_convert_matrix(const Projection &mtx) {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(mtx.columns[0].x, mtx.columns[1].x, mtx.columns[2].x, mtx.columns[3].x));
	ret.setRow(1, sl::float4(mtx.columns[0].y, mtx.columns[1].y, mtx.columns[2].y, mtx.columns[3].y));
	ret.setRow(2, sl::float4(mtx.columns[0].z, mtx.columns[1].z, mtx.columns[2].z, mtx.columns[3].z));
	ret.setRow(3, sl::float4(mtx.columns[0].w, mtx.columns[1].w, mtx.columns[2].w, mtx.columns[3].w));
	return ret;
}

static sl::float3 sl_convert_vector(const Vector3 &vec) {
	return sl::float3(vec.x, vec.y, vec.z);
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;
	ERR_FAIL_NULL(context);

	// Delay enablement.
	if (context->delay > 0) {
		--context->delay;
		return;
	}

	// If DLSS is not loaded/supported or no DLSS mode fits the resolution, escape early (the renderer falls back to
	// the internal texture because is_ready() returns false).
	if (StreamlineContext::get().slDLSSSetOptions == nullptr || !StreamlineContext::get().streamline_capabilities.dlss_available || context->currentDlssOptions.mode == sl::DLSSMode::eOff) {
		return;
	}

	// Begin frame if needed.
	if (StreamlineContext::get().last_token == nullptr) {
		StreamlineContext::get().get_new_frame_token();
		if (StreamlineContext::get().last_token == nullptr) {
			return;
		}
	}

	context->last_parameters = p_params;
	context->last_effect = this;

	// Godot marks pixels without motion vectors (e.g. sky) with -1; derive camera motion for those from depth.
	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);

		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++) {
				push_constants[4 + i * 4 + j] = p_params.reprojection.columns[i][j];
			}
		}
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// Inject DLSS into the render graph.
	RD::CallbackResource res[4];
	int num_resources = 0;
	res[num_resources++].rid = p_params.color;
	res[num_resources++].rid = p_params.output;
	res[num_resources++].rid = p_params.depth;
	res[num_resources++].rid = p_params.velocity;

	for (int i = 0; i < num_resources; i++) {
		res[i].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, p_params.context, VectorView<RD::CallbackResource>(res, num_resources));
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;
	StreamlineContext &sl_context = StreamlineContext::get();

	if (sl_context.last_token == nullptr) {
		return;
	}

	void *nativeCmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(p_cmd_buffer);
	ERR_FAIL_NULL_MSG(nativeCmdlist, "DLSS: the rendering driver does not expose native command buffers.");

	// Helper function for tagging resources.
	auto assignResource = [context](sl::Resource *resources, sl::ResourceTag *resourceTags, int &numResources, RID textureRID, sl::BufferType bufferType, sl::ResourceLifecycle lifecycle) {
		if (!textureRID.is_valid() || textureRID.is_null()) {
			return;
		}
		_sl_resource_from_texture(textureRID, context->is_d3d12, resources[numResources]);
		resourceTags[numResources] = sl::ResourceTag(resources + numResources, bufferType, lifecycle, nullptr);
		++numResources;
	};

	// DLSS Super Resolution / DLAA options.
	{
		// Godot's luminance buffer is not a DLSS exposure texture (and the fork never tagged it), so always let DLSS
		// compute exposure itself; the fork disabled it when the camera used auto exposure, leaving DLSS at exposure 1.0.
		context->currentDlssOptions.useAutoExposure = sl::Boolean::eTrue;
		context->currentDlssOptions.colorBuffersHDR = sl::Boolean::eTrue;

		char dlssPreset = p_params.preset;
		if (dlssPreset == '?') {
			dlssPreset = sl_context.dlss_default_preset;
		}

		sl::DLSSPreset preset = sl::DLSSPreset::eDefault;
		if (dlssPreset >= 'F' && dlssPreset <= 'O') {
			preset = (sl::DLSSPreset)((int)sl::DLSSPreset::ePresetF + ((int)dlssPreset - (int)'F'));
		}
		context->currentDlssOptions.dlaaPreset = preset;
		context->currentDlssOptions.qualityPreset = preset;
		context->currentDlssOptions.balancedPreset = preset;
		context->currentDlssOptions.performancePreset = preset;
		context->currentDlssOptions.ultraPerformancePreset = preset;

		sl::Result result = sl_context.slDLSSSetOptions(context->viewport, context->currentDlssOptions);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slDLSSSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Common constants.
	if (sl_context.slSetConstants != nullptr) {
		sl::float4x4 mtxIdentity = sl_make_identity_matrix();
		context->constants.cameraViewToClip = sl_convert_matrix(p_params.cam_projection); // projection mtx (unjittered)
		context->constants.clipToCameraView = sl_convert_matrix(p_params.cam_projection.inverse()); // projection mtx (unjittered, inverted)
		context->constants.clipToLensClip = mtxIdentity; // keep identity unless some lens distortion is applied
		context->constants.clipToPrevClip = sl_convert_matrix(p_params.reprojection); // reprojection matrix
		context->constants.prevClipToClip = sl_convert_matrix(p_params.reprojection.inverse()); // inverted reprojection matrix

		context->constants.cameraPos = sl_convert_vector(p_params.cam_transform.get_origin());
		context->constants.cameraFwd = sl_convert_vector(-p_params.cam_transform.get_basis().rows[2]);
		context->constants.cameraUp = sl_convert_vector(p_params.cam_transform.get_basis().rows[1]);
		context->constants.cameraRight = sl_convert_vector(p_params.cam_transform.get_basis().rows[0]);

		context->constants.cameraNear = p_params.z_near;
		context->constants.cameraFar = p_params.z_far;
		context->constants.cameraFOV = Math::deg_to_rad(p_params.fovy);
		context->constants.cameraMotionIncluded = sl::Boolean::eTrue;
		context->constants.cameraAspectRatio = static_cast<float>(context->currentDlssOptions.outputWidth) / static_cast<float>(context->currentDlssOptions.outputHeight);
		context->constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		context->constants.depthInverted = p_params.reverse_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->constants.motionVectors3D = sl::Boolean::eFalse;
		context->constants.motionVectorsDilated = sl::Boolean::eFalse;
		context->constants.motionVectorsJittered = sl::Boolean::eFalse;
		context->constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
		context->constants.mvecScale = sl::float2(1.0f, 1.0f);
		context->constants.orthographicProjection = sl::Boolean::eFalse;
		context->constants.reset = p_params.reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		sl::Result result = sl_context.slSetConstants(context->constants, *sl_context.last_token, context->viewport);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetConstants. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Tag resources.
	if (sl_context.slSetTag != nullptr) {
		sl::Resource resources[4];
		sl::ResourceTag resourceTags[4];
		int numResources = 0;

		assignResource(resources, resourceTags, numResources, p_params.color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);

		sl::Result result = sl_context.slSetTag(context->viewport, resourceTags, numResources, nativeCmdlist);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetTag. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// DLSS Frame Generation (E3, game only). It uses the depth and motion vectors tagged above (valid until present)
	// and the HUD-less colour tagged by RendererCompositorRD after the 3D scene, before the 2D canvases.
	bool want_dlssg = p_params.dlss_g;
	if (want_dlssg && RD::get_singleton()->screen_get_data_format(DisplayServerEnums::MAIN_WINDOW_ID) == RD::DATA_FORMAT_R16G16B16A16_SFLOAT) {
		// Godot's HDR output is FP16 scRGB, which DLSS-G does not support (DLSSGStatus::eFailHDRFormatNotSupported).
		WARN_PRINT_ONCE("DLSS Frame Generation does not support FP16 (scRGB) HDR output. Frame generation stays off while HDR output is enabled.");
		want_dlssg = false;
	}
	sl_context.dlssg_evaluate(context->viewport, want_dlssg);

	// Evaluate DLSS Super Resolution / DLAA.
	if (context->currentDlssOptions.mode != sl::DLSSMode::eOff && sl_context.slEvaluateFeature != nullptr) {
		const sl::BaseStructure *inputs[] = { &context->viewport };
		sl::Result result = sl_context.slEvaluateFeature(sl::kFeatureDLSS, *sl_context.last_token, inputs, 1, nativeCmdlist);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for DLSS Super Resolution. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}
}

void RendererRD::DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSContextInner *self = (DLSSContextInner *)p_userdata;
	self->last_effect->_upscale_internal(p_command_buffer, self->last_parameters);
}

bool DLSSEffect::frame_generation_supported() {
	const StreamlineContext &sl_context = StreamlineContext::get();
	return sl_context.is_game && sl_context.slDLSSGSetOptions != nullptr && sl_context.streamline_capabilities.dlss_g_available && sl_context.streamline_capabilities.reflex_available && sl_context.streamline_capabilities.pcl_available;
}

static RID g_dlssg_hudless_texture;

void DLSSEffect::frame_generation_tag_hudless(RID p_texture) {
	if (!frame_generation_supported() || p_texture.is_null()) {
		return;
	}
	g_dlssg_hudless_texture = p_texture;
	RD::CallbackResource res;
	res.rid = p_texture;
	res.usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_tag_hudless_graph_callback, nullptr, VectorView<RD::CallbackResource>(&res, 1));
}

void DLSSEffect::_tag_hudless_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	StreamlineContext &sl_context = StreamlineContext::get();
	// Only while frame generation runs, on the viewport it runs on (the tag goes with that viewport's depth and
	// motion vectors). In the first frame after enabling, DLSS may be evaluated after this callback: that frame
	// is interpolated without the HUD-less colour.
	if (!sl_context.dlssg_active() || sl_context.last_token == nullptr || sl_context.slSetTag == nullptr || g_dlssg_hudless_texture.is_null()) {
		return;
	}
	void *native_cmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(p_command_buffer);
	ERR_FAIL_NULL(native_cmdlist);

	const bool is_d3d12 = RD::get_singleton()->get_device_api_name().to_lower() == "d3d12";
	sl::Resource resource;
	_sl_resource_from_texture(g_dlssg_hudless_texture, is_d3d12, resource);
	sl::ResourceTag tag(&resource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr);
	sl::Result result = sl_context.slSetTag(sl_context.dlssg_viewport, &tag, 1, native_cmdlist);
	ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to tag the HUD-less colour for DLSS Frame Generation. Result: " + String(StreamlineContext::result_to_string(result)));
	if (!sl_context.dlssg_hudless_reported) {
		sl_context.dlssg_hudless_reported = true;
		print_verbose(vformat("Streamline: HUD-less colour (%dx%d) tagged for DLSS Frame Generation.", resource.width, resource.height));
	}
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	DLSSContextInner *context = (DLSSContextInner *)p_context;
	if (context == nullptr) {
		return false;
	}
	if (context->currentDlssOptions.mode == sl::DLSSMode::eOff) {
		return false; // Unsupported mode (DLSS unavailable or no mode fits the resolution).
	}
	if (context->delay > 0) {
		return false; // Still warming up.
	}
	return true;
}
#else
DLSSEffect::DLSSEffect() {}
DLSSEffect::~DLSSEffect() {}
DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	return nullptr;
}
void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {}
bool DLSSEffect::is_ready(DLSSContext *p_context) {
	return false;
}
void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}
bool DLSSEffect::frame_generation_supported() {
	return false;
}
void DLSSEffect::frame_generation_tag_hudless(RID p_texture) {}
void DLSSEffect::_tag_hudless_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}
#endif
