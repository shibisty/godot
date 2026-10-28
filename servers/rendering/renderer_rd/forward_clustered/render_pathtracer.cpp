/**************************************************************************/
/*  render_pathtracer.cpp                                                 */
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

#include "render_pathtracer.h"

#include "render_raytracing.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

uint32_t RenderPathtracer::Settings::hash() const {
	uint32_t h = hash_murmur3_one_32(uint32_t(debug_mode));
	h = hash_murmur3_one_32(uint32_t(max_bounces), h);
	h = hash_murmur3_one_float(firefly_clamp, h);
	h = hash_murmur3_one_32(accumulate ? 1u : 0u, h);
	h = hash_murmur3_one_float(exposure_normalization, h);
	h = hash_murmur3_one_32(fog ? 1u : 0u, h);
	return hash_fmix32(h);
}

bool RenderPathtracer::camera_changed(const Transform3D &p_a, const Transform3D &p_b, const Projection &p_proj_a, const Projection &p_proj_b) {
	const real_t eps = 1e-5;
	if (!p_a.basis.is_equal_approx(p_b.basis) || p_a.origin.distance_to(p_b.origin) > eps * MAX(real_t(1.0), p_a.origin.length())) {
		return true;
	}
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			if (c == 3 && r < 2) {
				continue; // TAA jitter offset (Projection::add_jitter_offset).
			}
			if (Math::abs(p_proj_a.columns[c][r] - p_proj_b.columns[c][r]) > eps * MAX(real_t(1.0), Math::abs(p_proj_a.columns[c][r]))) {
				return true;
			}
		}
	}
	return false;
}

uint32_t RenderPathtracer::samples_to_trace(uint32_t p_accumulated, uint32_t p_samples_per_pixel, uint32_t p_max_samples, bool p_accumulate) {
	uint32_t spp = CLAMP(p_samples_per_pixel, 1u, 64u);
	if (!p_accumulate) {
		return spp; // Every frame starts over (p_accumulated is 0).
	}
	if (p_max_samples > 0) {
		if (p_accumulated >= p_max_samples) {
			return 0;
		}
		spp = MIN(spp, p_max_samples - p_accumulated);
	}
	return spp;
}

bool RenderPathtracer::is_supported() {
	RD *rd = RD::get_singleton();
	return rd && rd->has_feature(RD::SUPPORTS_RAY_QUERY) && RenderRaytracing::is_supported();
}

bool RenderPathtracer::can_render(const RenderDataRD *p_render_data) {
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || p_render_data->reflection_probe.is_valid() || !rb->has_raytracing_scene() || rb->raytracing_scene.instance_count == 0) {
		return false;
	}
	if (rb->get_view_count() != 1 || rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED) {
		return false;
	}
	if (!rb->get_can_be_storage() || rb->get_base_data_format() != RD::DATA_FORMAT_R16G16B16A16_SFLOAT || !rb->has_internal_texture()) {
		return false;
	}
	return rb->raytracing_scene.textures.size() > 0;
}

void RenderPathtracer::_initialize(RendererSceneRenderRD *p_owner) {
	if (initialized) {
		return;
	}
	initialized = true;
	use_octmap_array = p_owner->is_using_radiance_octmap_array();

	String defines = "\n#define MAX_ROUGHNESS_LOD " + itos(MAX(p_owner->get_roughness_layers() - 1, 1)) + ".0\n";
	Vector<String> variants;
	variants.push_back("\n");
	variants.push_back("\n#define USE_RADIANCE_OCTMAP_ARRAY\n");
	shader.initialize(variants, defines);
	shader_version = shader.version_create();
	for (int i = 0; i < VARIANT_MAX; i++) {
		shader_rd[i] = shader.version_get_shader(shader_version, i);
		pipeline[i] = shader_rd[i].is_valid() ? RD::get_singleton()->compute_pipeline_create(shader_rd[i]) : RID();
	}
}

uint32_t RenderPathtracer::_gather_lights(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, uint32_t &r_sun_index) {
	// Ported from the fork (RenderRaytracing::gather_lights): directional lights first, then the strongest positional
	// lights. Only the lights culled for the view are known here (the fork also gathers lights around the camera).
	light_data.clear();
	r_sun_index = 0xFFFFFFFF;
	if (!p_render_data->lights) {
		return 0;
	}
	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	const Vector3 cam_pos = p_render_data->scene_data->cam_transform.origin;
	const float exposure = p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f;

	auto light_energy = [&](RID p_base, RSE::LightType p_type) -> float {
		float e = (ls->light_is_negative(p_base) ? -1.0f : 1.0f) * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (p_owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_SPOT) {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		return e * exposure;
	};

	struct Scored {
		RID instance;
		float score = 0.0;
		bool operator<(const Scored &p_other) const { return score > p_other.score; }
	};
	LocalVector<Scored> positional;

	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint32_t i = 0; i < (uint32_t)lights.size(); i++) {
		RID instance = lights[i];
		RID base = ls->light_instance_get_base_light(instance);
		RSE::LightType type = ls->light_get_type(base);
		if (type == RSE::LIGHT_AREA) {
			continue; // Not supported by the path tracer.
		}
		if (type != RSE::LIGHT_DIRECTIONAL) {
			Transform3D xform = ls->light_instance_get_base_transform(instance);
			float dist_sq = MAX((xform.origin - cam_pos).length_squared(), 0.01f);
			Color c = ls->light_get_color(base);
			float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
			Scored s;
			s.instance = instance;
			s.score = ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY) * (c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f) * (range > 0.0f ? range * range : 1e12f) / dist_sq;
			positional.push_back(s);
			continue;
		}
		if (ls->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY || light_data.size() >= RT_LIGHTS_MAX) {
			continue;
		}
		RT_LightData ld = {};
		Vector3 dir = -ls->light_instance_get_base_transform(instance).basis.get_column(2).normalized();
		ld.position[0] = dir.x;
		ld.position[1] = dir.y;
		ld.position[2] = dir.z;
		ld.type = RT_LIGHT_TYPE_DIRECTIONAL;
		Color col = ls->light_get_color(base).srgb_to_linear();
		float energy = light_energy(base, RSE::LIGHT_DIRECTIONAL);
		ld.emission[0] = col.r * energy;
		ld.emission[1] = col.g * energy;
		ld.emission[2] = col.b * energy;
		ld.radius = Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f);
		ld.inv_max_range = -1.0f;
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR);
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		if (r_sun_index == 0xFFFFFFFF && energy > 0.0f) {
			r_sun_index = light_data.size();
		}
		light_data.push_back(ld);
	}

	positional.sort();
	for (uint32_t i = 0; i < positional.size() && light_data.size() < RT_LIGHTS_MAX; i++) {
		RID instance = positional[i].instance;
		RID base = ls->light_instance_get_base_light(instance);
		RSE::LightType type = ls->light_get_type(base);
		Transform3D xform = ls->light_instance_get_base_transform(instance);
		RT_LightData ld = {};
		ld.position[0] = xform.origin.x;
		ld.position[1] = xform.origin.y;
		ld.position[2] = xform.origin.z;
		ld.type = type == RSE::LIGHT_SPOT ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_OMNI;
		Color col = ls->light_get_color(base).srgb_to_linear();
		float energy = light_energy(base, type);
		ld.emission[0] = col.r * energy;
		ld.emission[1] = col.g * energy;
		ld.emission[2] = col.b * energy;
		ld.radius = ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
		ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		ld.inv_max_range = range > 0.0f ? 1.0f / range : -1.0f;
		ld.max_range_squared = range > 0.0f ? range * range : 0.0f;
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f; // Rasterizer convention (0.5 -> 1.0).
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		if (type == RSE::LIGHT_SPOT) {
			ld.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			ld.cos_spot_angle = Math::cos(Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)));
			Vector3 spot_dir = -xform.basis.get_column(2).normalized();
			ld.spot_direction[0] = spot_dir.x;
			ld.spot_direction[1] = spot_dir.y;
			ld.spot_direction[2] = spot_dir.z;
		}
		light_data.push_back(ld);
	}
	return light_data.size();
}

bool RenderPathtracer::render(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, const Inputs &p_inputs) {
	ERR_FAIL_NULL_V(p_owner, false);
	if (!can_render(p_render_data)) {
		return false;
	}
	_initialize(p_owner);
	const ShaderVariant variant = use_octmap_array ? VARIANT_RADIANCE_ARRAY : VARIANT_RADIANCE_2D;
	ERR_FAIL_COND_V(pipeline[variant].is_null(), false);

	RD *rd = RD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	RenderSceneBuffersRD::Pathtracing &pt = rb->pathtracing;
	const RenderSceneBuffersRD::RaytracingScene &rts = rb->raytracing_scene;
	RID env = p_render_data->environment;

	RD::get_singleton()->draw_command_begin_label("Path Tracer (Mariner E5c)");

	// Accumulation buffers.
	const Size2i size = rb->get_internal_size();
	bool reset = rb->consume_pathtracing_reset();
	if (pt.accum.is_null() || pt.size != size) {
		if (pt.accum.is_valid()) {
			rd->free_rid(pt.accum);
		}
		if (pt.depth.is_valid()) {
			rd->free_rid(pt.depth);
		}
		RD::TextureFormat tf;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.width = size.width;
		tf.height = size.height;
		tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		pt.accum = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(pt.accum, "Path tracer accumulation");
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		pt.depth = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(pt.depth, "Path tracer depth");
		pt.size = size;
		reset = true;
	}

	// Settings and what the accumulation depends on.
	Settings settings;
	settings.debug_mode = env_storage->environment_get_pathtracing_debug_mode(env);
	settings.max_bounces = env_storage->environment_get_pathtracing_max_bounces(env);
	settings.firefly_clamp = env_storage->environment_get_pathtracing_firefly_clamp(env);
	settings.accumulate = env_storage->environment_get_pathtracing_accumulate(env);
	settings.exposure_normalization = p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f;
	settings.fog = p_inputs.fog_enabled;
	const uint32_t settings_hash = settings.hash();

	uint32_t sun_index = 0xFFFFFFFF;
	const uint32_t light_count = _gather_lights(p_owner, p_render_data, sun_index);
	const uint32_t light_hash = light_count > 0 ? hash_murmur3_buffer(light_data.ptr(), light_count * sizeof(RT_LightData)) : 0;

	const Transform3D &cam_transform = p_render_data->scene_data->cam_transform;
	const Projection &cam_projection = p_render_data->scene_data->cam_projection;
	if (!settings.accumulate || pt.samples == 0 || settings_hash != pt.settings_hash || rts.scene_hash != pt.scene_hash || light_hash != pt.light_hash || camera_changed(cam_transform, pt.cam_transform, cam_projection, pt.cam_projection)) {
		reset = true;
	}
	if (reset) {
		pt.samples = 0;
		pt.settings_hash = settings_hash;
		pt.scene_hash = rts.scene_hash;
		pt.light_hash = light_hash;
		pt.cam_transform = cam_transform;
		pt.cam_projection = cam_projection;
	}

	if (env_storage->environment_get_pathtracing_denoiser(env) != RSE::PT_DENOISER_NONE) {
		WARN_PRINT_ONCE("Path tracing: DLSS Ray Reconstruction is not available in this build (Mariner E5c), no denoiser is used; the image converges by accumulation.");
	}

	const uint32_t spp = samples_to_trace(pt.samples, env_storage->environment_get_pathtracing_samples_per_pixel(env), env_storage->environment_get_pathtracing_max_samples(env), settings.accumulate);

	// Lights.
	if (pt.light_buffer.is_null()) {
		pt.light_buffer = rd->storage_buffer_create(RT_LIGHTS_MAX * sizeof(RT_LightData));
		rd->set_resource_name(pt.light_buffer, "Path tracer lights");
	}
	if (light_count > 0) {
		rd->buffer_update(pt.light_buffer, 0, light_count * sizeof(RT_LightData), light_data.ptr());
	}

	// Parameters.
	PT_Params params = {};
	uint32_t flags = 0;
	if (spp > 0) {
		flags |= PT_FLAG_TRACE;
	}
	if (p_inputs.fog_enabled) {
		flags |= PT_FLAG_FOG;
		if (p_inputs.depth_fog) {
			flags |= PT_FLAG_DEPTH_FOG;
		}
	}
	if (rts.has_dielectrics) {
		flags |= PT_FLAG_DIELECTRICS;
	}
	RID radiance = p_inputs.radiance_texture;
	if (radiance.is_valid()) {
		flags |= PT_FLAG_RADIANCE;
	} else {
		radiance = texture_storage->texture_rd_get_default(use_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	if (pt.samples == 0) {
		flags |= PT_FLAG_WRITE_DEPTH;
	}
	RID volumetric_fog = p_inputs.volumetric_fog_texture;
	if (volumetric_fog.is_valid()) {
		flags |= PT_FLAG_VOLUMETRIC_FOG;
	} else {
		volumetric_fog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	}
	params.counts[0] = light_count;
	params.counts[1] = pt.frame_index++;
	params.counts[2] = pt.samples;
	params.counts[3] = flags;
	params.config[0] = spp;
	params.config[1] = uint32_t(settings.max_bounces);
	params.config[2] = uint32_t(settings.debug_mode);
	params.config[3] = sun_index;
	params.config2[0] = settings.firefly_clamp;
	params.config2[1] = MAX(p_render_data->scene_data->z_far * 2.0f, 1000.0f);
	params.config2[2] = p_inputs.volumetric_fog_inv_length;
	params.config2[3] = p_inputs.volumetric_fog_detail_spread;
	params.ambient[0] = p_inputs.ambient_light.r;
	params.ambient[1] = p_inputs.ambient_light.g;
	params.ambient[2] = p_inputs.ambient_light.b;
	params.size[0] = size.width;
	params.size[1] = size.height;
	params.inv_size[0] = 1.0f / size.width;
	params.inv_size[1] = 1.0f / size.height;
	if (pt.params_buffer.is_null()) {
		pt.params_buffer = rd->uniform_buffer_create(sizeof(PT_Params));
		rd->set_resource_name(pt.params_buffer, "Path tracer parameters");
	}
	rd->buffer_update(pt.params_buffer, 0, sizeof(PT_Params), &params);

	// Uniforms.
	RID shader_rd_variant = shader_rd[variant];
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID linear_clamp = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	RID nearest_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);

	RID set0 = cache->get_cache(shader_rd_variant, 0,
			RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, rts.tlas),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, rts.instance_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, rts.material_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, p_inputs.scene_uniform_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, pt.light_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, pt.params_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, radiance),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 7, linear_clamp),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 8, linear_repeat),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 9, nearest_repeat),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, pt.accum),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, rb->get_internal_texture()),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, pt.depth),
			RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 13, volumetric_fog));
	RID set1 = rb->get_raytracing_texture_uniform_set(shader_rd_variant, 1);
	if (set0.is_null() || set1.is_null()) {
		RD::get_singleton()->draw_command_end_label();
		ERR_FAIL_V_MSG(false, "Path tracing: could not create the uniform sets.");
	}

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline[variant]);
	rd->compute_list_bind_uniform_set(compute_list, set0, 0);
	rd->compute_list_bind_uniform_set(compute_list, set1, 1);
	rd->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	rd->compute_list_end();

	// Primary depth -> depth buffer (transparent pass, sky, compositor effects).
	if (p_inputs.depth_framebuffer.is_valid()) {
		RendererRD::CopyEffects::get_singleton()->copy_r32f_to_depth_fb(pt.depth, p_inputs.depth_framebuffer, Rect2i(Point2i(), size));
	}

	RD::get_singleton()->draw_command_end_label();

	pt.samples += spp;
	rb->set_pathtracing_samples(pt.samples);
	return true;
}

RenderPathtracer::~RenderPathtracer() {
	if (initialized) {
		shader.version_free(shader_version);
	}
}
