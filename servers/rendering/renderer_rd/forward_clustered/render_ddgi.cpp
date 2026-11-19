/**************************************************************************/
/*  render_ddgi.cpp                                                       */
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

#include "render_ddgi.h"

#include "render_raytracing.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

Vector3i RenderDDGI::grid_min_for(const Vector3 &p_camera, const Vector3i &p_count, float p_spacing, const Vector3i &p_previous, bool p_has_previous) {
	const float spacing = MAX(p_spacing, 0.001f);
	const Vector3 cell = p_camera / spacing; // Camera position in cells.
	const Vector3i half = p_count / 2;
	if (p_has_previous) {
		const Vector3 previous_center = Vector3(p_previous + half);
		if (Math::abs(cell.x - previous_center.x) <= 0.75f && Math::abs(cell.y - previous_center.y) <= 0.75f && Math::abs(cell.z - previous_center.z) <= 0.75f) {
			return p_previous;
		}
	}
	const Vector3i center(int32_t(Math::floor(cell.x + 0.5f)), int32_t(Math::floor(cell.y + 0.5f)), int32_t(Math::floor(cell.z + 0.5f)));
	return center - half;
}

Size2i RenderDDGI::irradiance_atlas_size(const Vector3i &p_count) {
	return Size2i(p_count.x * p_count.y, p_count.z) * (IRRADIANCE_TEXELS + 2);
}

Size2i RenderDDGI::distance_atlas_size(const Vector3i &p_count) {
	return Size2i(p_count.x * p_count.y, p_count.z) * (DISTANCE_TEXELS + 2);
}

Basis RenderDDGI::ray_rotation(uint32_t p_frame) {
	// Uniform random rotation from three hashed numbers (Shoemake, "Uniform random rotations", Graphics Gems III).
	uint32_t h = hash_murmur3_one_32(p_frame * 3u + 1u);
	const float u1 = (h & 0xFFFFFF) / float(0x1000000);
	h = hash_murmur3_one_32(p_frame * 3u + 2u);
	const float u2 = (h & 0xFFFFFF) / float(0x1000000);
	h = hash_murmur3_one_32(p_frame * 3u + 3u);
	const float u3 = (h & 0xFFFFFF) / float(0x1000000);
	const float a = Math::sqrt(1.0f - u1);
	const float b = Math::sqrt(u1);
	const Quaternion q(a * Math::sin(Math::TAU * u2), a * Math::cos(Math::TAU * u2), b * Math::sin(Math::TAU * u3), b * Math::cos(Math::TAU * u3));
	return Basis(q.normalized());
}

static int32_t _ddgi_posmod(int32_t p_a, int32_t p_b) {
	const int32_t r = p_a % p_b;
	return r < 0 ? r + p_b : r;
}

Vector3i RenderDDGI::slot_to_cell(uint32_t p_slot, const Vector3i &p_grid_min, const Vector3i &p_count) {
	const Vector3i slot(int32_t(p_slot % uint32_t(p_count.x)), int32_t((p_slot / uint32_t(p_count.x)) % uint32_t(p_count.y)), int32_t(p_slot / uint32_t(p_count.x * p_count.y)));
	return p_grid_min + Vector3i(_ddgi_posmod(slot.x - p_grid_min.x, p_count.x), _ddgi_posmod(slot.y - p_grid_min.y, p_count.y), _ddgi_posmod(slot.z - p_grid_min.z, p_count.z));
}

uint32_t RenderDDGI::build_slot_list(const Vector3i &p_count, const Vector3i &p_old_grid_min, bool p_has_old, const Vector3i &p_grid_min, uint32_t p_budget, uint32_t &r_round_robin, LocalVector<uint32_t> &r_slots) {
	const uint32_t total = uint32_t(p_count.x * p_count.y * p_count.z);
	r_slots.clear();
	if (p_budget == 0 || p_budget >= total) {
		r_slots.resize(total);
		for (uint32_t i = 0; i < total; i++) {
			r_slots[i] = i;
		}
		r_round_robin = 0;
		return p_has_old && p_old_grid_min == p_grid_min ? 0 : total;
	}
	LocalVector<uint8_t> taken;
	taken.resize(total);
	memset(taken.ptr(), 0, total);
	const bool moved = !p_has_old || p_old_grid_min != p_grid_min;
	if (moved) {
		const Vector3i old_max = p_old_grid_min + p_count;
		for (uint32_t i = 0; i < total; i++) {
			const Vector3i cell = slot_to_cell(i, p_grid_min, p_count);
			const bool was_inside = p_has_old && cell.x >= p_old_grid_min.x && cell.y >= p_old_grid_min.y && cell.z >= p_old_grid_min.z && cell.x < old_max.x && cell.y < old_max.y && cell.z < old_max.z;
			if (!was_inside) {
				r_slots.push_back(i);
				taken[i] = 1;
			}
		}
	}
	const uint32_t entered = r_slots.size();
	r_round_robin %= total;
	for (uint32_t visited = 0; visited < total && r_slots.size() < p_budget; visited++) {
		const uint32_t s = r_round_robin;
		r_round_robin = (r_round_robin + 1) % total;
		if (!taken[s]) {
			r_slots.push_back(s);
		}
	}
	return entered;
}

void RenderDDGI::local_volume_layout(const Transform3D &p_transform, const Vector3 &p_size, float p_spacing, Vector3i &r_count, Vector3 &r_spacing, Transform3D &r_grid_to_world) {
	const Vector3 size = p_size.maxf(0.1);
	r_count = RendererEnvironmentStorage::ddgi_volume_probe_count(size, p_spacing);
	r_spacing = size / Vector3(r_count - Vector3i(1, 1, 1)).maxf(1.0);
	r_grid_to_world = p_transform.orthonormalized() * Transform3D(Basis(), -size * 0.5);
}

void RenderDDGI::store_transform(const Transform3D &p_xform, float *r_mat4) {
	for (int c = 0; c < 3; c++) {
		for (int r = 0; r < 3; r++) {
			r_mat4[c * 4 + r] = p_xform.basis.rows[r][c];
		}
		r_mat4[c * 4 + 3] = 0.0f;
	}
	r_mat4[12] = p_xform.origin.x;
	r_mat4[13] = p_xform.origin.y;
	r_mat4[14] = p_xform.origin.z;
	r_mat4[15] = 1.0f;
}

bool RenderDDGI::is_supported() {
	return RenderPathtracer::is_supported();
}

bool RenderDDGI::can_update(const RenderDataRD *p_render_data) {
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || p_render_data->reflection_probe.is_valid() || !rb->has_raytracing_scene()) {
		return false;
	}
	return rb->raytracing_scene.textures.size() > 0;
}

void RenderDDGI::_initialize(RendererSceneRenderRD *p_owner) {
	if (initialized) {
		return;
	}
	initialized = true;
	use_octmap_array = p_owner->is_using_radiance_octmap_array();
	RD *rd = RD::get_singleton();

	String defines = "\n#define MAX_ROUGHNESS_LOD " + itos(MAX(p_owner->get_roughness_layers() - 1, 1)) + ".0\n" + RenderRaytracing::get_scene_shader_defines();
	Vector<String> variants;
	variants.push_back("\n");
	variants.push_back("\n#define USE_RADIANCE_OCTMAP_ARRAY\n");
	variants.push_back("\n#define MODE_SCREEN_PROBES\n"); // Mariner L1.
	variants.push_back("\n#define MODE_SCREEN_PROBES\n#define USE_RADIANCE_OCTMAP_ARRAY\n");
	variants.push_back("\n#define MODE_REFLECTIONS\n"); // Mariner L2.
	variants.push_back("\n#define MODE_REFLECTIONS\n#define USE_RADIANCE_OCTMAP_ARRAY\n");
	trace_shader.initialize(variants, defines);
	trace_version = trace_shader.version_create();
	for (int i = 0; i < TRACE_MAX; i++) {
		trace_shader_rd[i] = trace_shader.version_get_shader(trace_version, i);
		trace_pipeline[i] = trace_shader_rd[i].is_valid() ? rd->compute_pipeline_create(trace_shader_rd[i]) : RID();
	}

	Vector<String> update_variants;
	update_variants.push_back("\n");
	update_shader.initialize(update_variants);
	update_version = update_shader.version_create();
	update_shader_rd = update_shader.version_get_shader(update_version, 0);
	update_pipeline = update_shader_rd.is_valid() ? rd->compute_pipeline_create(update_shader_rd) : RID();

	// Mariner L1.
	Vector<String> screen_variants;
	screen_variants.push_back("\n#define MODE_PLACE\n");
	screen_variants.push_back("\n#define MODE_INTEGRATE\n");
	screen_variants.push_back("\n#define MODE_FILTER\n");
	screen_variants.push_back("\n#define MODE_STORE_COLOR\n");
	screen_variants.push_back("\n#define MODE_REFL_TEMPORAL\n"); // Mariner L2.
	screen_variants.push_back("\n#define MODE_REFL_RESOLVE\n");
	screen_variants.push_back("\n#define MODE_FILTER_RADIANCE\n");
	screen_shader.initialize(screen_variants);
	screen_version = screen_shader.version_create();
	for (int i = 0; i < SCREEN_MAX; i++) {
		screen_shader_rd[i] = screen_shader.version_get_shader(screen_version, i);
		screen_pipeline[i] = screen_shader_rd[i].is_valid() ? rd->compute_pipeline_create(screen_shader_rd[i]) : RID();
	}
}

RID RenderDDGI::get_inactive_params_buffer() {
	if (inactive_params_buffer.is_null()) {
		DDGI_ParamsBlock params = {};
		for (DDGI_Params &v : params.volumes) {
			v.probe_count[0] = v.probe_count[1] = v.probe_count[2] = 2;
			v.spacing[0] = v.spacing[1] = v.spacing[2] = 1.0f;
			v.channels[0] = v.channels[1] = 1.0f;
		}
		Vector<uint8_t> data;
		data.resize(sizeof(DDGI_ParamsBlock));
		memcpy(data.ptrw(), &params, sizeof(DDGI_ParamsBlock));
		inactive_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(DDGI_ParamsBlock), data);
		RD::get_singleton()->set_resource_name(inactive_params_buffer, "DDGI parameters (inactive)");
	}
	return inactive_params_buffer;
}

void RenderDDGI::_ensure_volume_resources(RenderSceneBuffersRD *p_rb, uint32_t p_volume, const VolumeSetup &p_setup) {
	RenderSceneBuffersRD::DDGIVolume &v = p_rb->ddgi.volumes[p_volume];
	if (v.irradiance.is_valid() && v.probe_count == p_setup.count && v.spacing.is_equal_approx(p_setup.spacing) && v.rays_per_probe == p_setup.rays) {
		return;
	}
	p_rb->free_ddgi_volume(p_volume);
	RD *rd = RD::get_singleton();
	const Vector3i count = p_setup.count;
	const uint32_t probes = uint32_t(count.x * count.y * count.z);
	const String prefix = p_volume == RenderSceneBuffersRD::DDGI_VOLUME_WORLD ? "DDGI world " : "DDGI local ";

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const Size2i irradiance_size = irradiance_atlas_size(count);
	tf.width = irradiance_size.width;
	tf.height = irradiance_size.height;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	v.irradiance = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(v.irradiance, prefix + "irradiance (sky)");
	rd->texture_clear(v.irradiance, Color(0, 0, 0, 0), 0, 1, 0, 1);
	v.irradiance_lights = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(v.irradiance_lights, prefix + "irradiance (lights)");
	rd->texture_clear(v.irradiance_lights, Color(0, 0, 0, 0), 0, 1, 0, 1);
	const Size2i distance_size = distance_atlas_size(count);
	tf.width = distance_size.width;
	tf.height = distance_size.height;
	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	v.distance = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(v.distance, prefix + "distance");
	rd->texture_clear(v.distance, Color(0, 0, 0, 0), 0, 1, 0, 1);
	tf.width = count.x * count.y;
	tf.height = count.z;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	v.probe_data = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(v.probe_data, prefix + "probe offsets");
	rd->texture_clear(v.probe_data, Color(0, 0, 0, 0), 0, 1, 0, 1);

	v.rays = rd->storage_buffer_create(probes * uint32_t(p_setup.rays) * 16u);
	rd->set_resource_name(v.rays, prefix + "rays");
	Vector<uint8_t> zeros;
	zeros.resize(probes * 32u);
	zeros.fill(0);
	v.probe_state = rd->storage_buffer_create(probes * 32u, zeros);
	rd->set_resource_name(v.probe_state, prefix + "probe state");
	v.slot_list = rd->storage_buffer_create(probes * 4u);
	rd->set_resource_name(v.slot_list, prefix + "slots to update");
	v.probe_count = count;
	v.spacing = p_setup.spacing;
	v.rays_per_probe = p_setup.rays;
}

bool RenderDDGI::update(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, RID p_scene_uniform_buffer, RID p_radiance_texture) {
	ERR_FAIL_NULL_V(p_owner, false);
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), false);
	RenderSceneBuffersRD::DDGI &dd = rb->ddgi;
	dd.active = false;
	for (RenderSceneBuffersRD::DDGIVolume &v : dd.volumes) {
		v.active = false;
	}
	if (!can_update(p_render_data)) {
		return false;
	}
	_initialize(p_owner);
	const TraceVariant variant = use_octmap_array ? TRACE_RADIANCE_ARRAY : TRACE_RADIANCE_2D;
	ERR_FAIL_COND_V(trace_pipeline[variant].is_null() || update_pipeline.is_null(), false);

	RD *rd = RD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
	const RID env = p_render_data->environment;

	// What to update: the world grid around the camera (Environment) and the grid of a DDGIVolume3D (the ship).
	VolumeSetup setups[RenderSceneBuffersRD::DDGI_VOLUME_MAX];
	{
		VolumeSetup &w = setups[RenderSceneBuffersRD::DDGI_VOLUME_WORLD];
		w.enabled = true;
		w.count = RendererEnvironmentStorage::ddgi_clamp_probe_count(env_storage->environment_get_ddgi_probe_count(env));
		const float spacing = env_storage->environment_get_ddgi_probe_spacing(env);
		w.spacing = Vector3(spacing, spacing, spacing);
		w.rays = env_storage->environment_get_ddgi_rays_per_probe(env);
		w.probes_per_frame = env_storage->environment_get_ddgi_probes_per_frame(env);
		w.follows_camera = true;
	}
	if (p_render_data->ddgi_volume && p_render_data->ddgi_volume->enabled) {
		const RendererSceneRender::DDGIVolumeData &src = *p_render_data->ddgi_volume;
		VolumeSetup &l = setups[RenderSceneBuffersRD::DDGI_VOLUME_LOCAL];
		l.enabled = true;
		local_volume_layout(src.transform, src.size, src.probe_spacing, l.count, l.spacing, l.grid_to_world);
		l.rays = src.rays_per_probe;
		l.probes_per_frame = src.probes_per_frame;
	}

	RD::get_singleton()->draw_command_begin_label("DDGI (Mariner E12)");

	if (dd.params_buffer.is_null()) {
		dd.params_buffer = rd->uniform_buffer_create(sizeof(DDGI_ParamsBlock));
		rd->set_resource_name(dd.params_buffer, "DDGI parameters");
		dd.light_buffer = rd->storage_buffer_create(RenderPathtracer::RT_LIGHTS_MAX * sizeof(RT_LightData));
		rd->set_resource_name(dd.light_buffer, "DDGI lights");
	}

	// Per volume: resources, grid placement, slots to update, parameters.
	DDGI_ParamsBlock block = {};
	uint32_t slot_counts[RenderSceneBuffersRD::DDGI_VOLUME_MAX] = {};
	const Basis rotation = ray_rotation(dd.frame_index);
	const bool sea = env_storage->environment_get_ddgi_sea(env);
	const int32_t common_flags = FLAG_ACTIVE | (env_storage->environment_get_ddgi_relocation(env) ? FLAG_RELOCATION : 0) | (sea ? FLAG_SEA : 0);
	const Color sea_color = env_storage->environment_get_ddgi_sea_color(env).srgb_to_linear();
	const float sea_level = env_storage->environment_get_ddgi_sea_level(env);
	for (uint32_t vi = 0; vi < RenderSceneBuffersRD::DDGI_VOLUME_MAX; vi++) {
		const VolumeSetup &setup = setups[vi];
		DDGI_Params &params = block.volumes[vi];
		params.probe_count[0] = params.probe_count[1] = params.probe_count[2] = 2;
		params.spacing[0] = params.spacing[1] = params.spacing[2] = 1.0f;
		params.channels[0] = env_storage->environment_get_ddgi_sky_energy(env);
		params.channels[1] = env_storage->environment_get_ddgi_lights_energy(env);
		params.sea[0] = sea_color.r;
		params.sea[1] = sea_color.g;
		params.sea[2] = sea_color.b;
		params.sea[3] = sea_level;
		if (!setup.enabled) {
			if (rb->ddgi.volumes[vi].irradiance.is_valid()) {
				rb->free_ddgi_volume(vi);
			}
			continue;
		}
		_ensure_volume_resources(rb.ptr(), vi, setup);
		RenderSceneBuffersRD::DDGIVolume &v = dd.volumes[vi];
		const Vector3i count = setup.count;

		const Vector3i old_grid_min = v.grid_min;
		const bool had_grid = v.has_grid;
		if (setup.follows_camera) {
			v.grid_min = grid_min_for(p_render_data->scene_data->cam_transform.origin, count, setup.spacing.x, v.grid_min, v.has_grid);
		} else {
			v.grid_min = Vector3i();
		}
		v.has_grid = true;
		build_slot_list(count, old_grid_min, had_grid, v.grid_min, uint32_t(setup.probes_per_frame), v.round_robin, slots);
		slot_counts[vi] = slots.size();
		rd->buffer_update(v.slot_list, 0, slots.size() * sizeof(uint32_t), slots.ptr());

		params.grid_min[0] = v.grid_min.x;
		params.grid_min[1] = v.grid_min.y;
		params.grid_min[2] = v.grid_min.z;
		params.grid_min[3] = common_flags;
		params.probe_count[0] = count.x;
		params.probe_count[1] = count.y;
		params.probe_count[2] = count.z;
		params.probe_count[3] = setup.rays;
		params.spacing[0] = setup.spacing.x;
		params.spacing[1] = setup.spacing.y;
		params.spacing[2] = setup.spacing.z;
		params.spacing[3] = env_storage->environment_get_ddgi_energy(env);
		const Size2i irradiance_size = irradiance_atlas_size(count);
		const Size2i distance_size = distance_atlas_size(count);
		params.atlas_inv_size[0] = 1.0f / irradiance_size.width;
		params.atlas_inv_size[1] = 1.0f / irradiance_size.height;
		params.atlas_inv_size[2] = 1.0f / distance_size.width;
		params.atlas_inv_size[3] = 1.0f / distance_size.height;
		params.misc[0] = env_storage->environment_get_ddgi_specular_occlusion(env);
		params.misc[1] = MAX(setup.spacing.x, MAX(setup.spacing.y, setup.spacing.z)) * 2.0f;
		params.misc[2] = env_storage->environment_get_ddgi_hysteresis(env);
		params.misc[3] = env_storage->environment_get_ddgi_bias(env);
		store_transform(Transform3D(rotation, Vector3()), params.ray_rotation);
		store_transform(setup.grid_to_world, params.grid_to_world);
		store_transform(setup.grid_to_world.affine_inverse(), params.world_to_grid);
	}
	rd->buffer_update(dd.params_buffer, 0, sizeof(DDGI_ParamsBlock), &block);

	// Lights (directional lights and the positional lights within the raytracing range).
	uint32_t sun_index = 0xFFFFFFFF;
	const uint32_t light_count = RenderPathtracer::gather_lights(p_owner, p_render_data, light_data, sun_index);
	if (light_count > 0) {
		rd->buffer_update(dd.light_buffer, 0, light_count * sizeof(RT_LightData), light_data.ptr());
	}

	struct TracePushConstant {
		uint32_t light_count;
		uint32_t flags;
		float ray_length;
		uint32_t slot_count;
		uint32_t volume;
		uint32_t pad[3];
	} push = {};
	push.light_count = light_count;
	RID radiance = p_radiance_texture;
	if (radiance.is_valid()) {
		push.flags |= 1; // DDGI_TRACE_FLAG_RADIANCE
	} else {
		radiance = texture_storage->texture_rd_get_default(use_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	push.flags |= 2; // DDGI_TRACE_FLAG_MULTI_BOUNCE: the atlases start cleared (no light), so always.
	push.ray_length = MAX(p_render_data->scene_data->z_far * 2.0f, 1000.0f);

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	frame_trace.scene_uniform_buffer = p_scene_uniform_buffer;
	frame_trace.radiance = radiance;
	frame_trace.light_count = light_count;
	frame_trace.flags = push.flags;
	frame_trace.ray_length = push.ray_length;
	RID trace_rd = trace_shader_rd[variant];
	RID trace_set1 = rb->get_raytracing_texture_uniform_set(trace_rd, 1);

	for (uint32_t vi = 0; vi < RenderSceneBuffersRD::DDGI_VOLUME_MAX; vi++) {
		if (!setups[vi].enabled) {
			continue;
		}
		const RenderSceneBuffersRD::DDGIVolume &v = dd.volumes[vi];
		RID trace_set0 = cache->get_cache_vec(trace_rd, 0, _trace_uniforms(rb.ptr(), v.rays, v.probe_state, v.slot_list, TRACE_SET_PROBES));
		RID update_set = cache->get_cache(update_shader_rd, 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, dd.params_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, v.rays),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, v.probe_state),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, v.irradiance),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, v.distance),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, v.probe_data),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, v.slot_list),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, v.irradiance_lights));
		if (trace_set0.is_null() || trace_set1.is_null() || update_set.is_null()) {
			RD::get_singleton()->draw_command_end_label();
			ERR_FAIL_V_MSG(false, "DDGI: could not create the uniform sets.");
		}
		push.slot_count = slot_counts[vi];
		push.volume = vi;

		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, trace_pipeline[variant]);
		rd->compute_list_bind_uniform_set(compute_list, trace_set0, 0);
		rd->compute_list_bind_uniform_set(compute_list, trace_set1, 1);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(TracePushConstant));
		rd->compute_list_dispatch_threads(compute_list, uint32_t(setups[vi].rays), slot_counts[vi], 1);
		rd->compute_list_end();

		uint32_t update_push[4] = { vi, 0, 0, 0 };
		compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, update_pipeline);
		rd->compute_list_bind_uniform_set(compute_list, update_set, 0);
		rd->compute_list_set_push_constant(compute_list, update_push, sizeof(update_push));
		rd->compute_list_dispatch(compute_list, slot_counts[vi], 1, 1);
		rd->compute_list_end();

		dd.volumes[vi].active = true;
	}

	RD::get_singleton()->draw_command_end_label();

	dd.frame_index++;
	dd.frame = RSG::rasterizer->get_frame_number();
	dd.active = true;
	return true;
}

LocalVector<RD::Uniform> RenderDDGI::_trace_uniforms(RenderSceneBuffersRD *p_rb, RID p_binding_10, RID p_binding_14, RID p_binding_15, TraceSet p_set, RID p_binding_21) const {
	const bool p_screen = p_set != TRACE_SET_PROBES;
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const RenderSceneBuffersRD::RaytracingScene &rts = p_rb->raytracing_scene;
	const RenderSceneBuffersRD::DDGI &dd = p_rb->ddgi;
	RID linear_clamp = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	RID nearest_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	const RID black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	auto tex = [&](RID p_texture) -> RID {
		return p_texture.is_valid() ? p_texture : black;
	};
	const RenderSceneBuffersRD::DDGIVolume &w = dd.volumes[RenderSceneBuffersRD::DDGI_VOLUME_WORLD];
	const RenderSceneBuffersRD::DDGIVolume &l = dd.volumes[RenderSceneBuffersRD::DDGI_VOLUME_LOCAL];
	LocalVector<RD::Uniform> u;
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, rts.tlas));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, rts.instance_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, rts.material_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, frame_trace.scene_uniform_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, dd.light_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, dd.params_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, frame_trace.radiance));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 7, linear_clamp));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 8, linear_repeat));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 9, nearest_repeat));
	u.push_back(RD::Uniform(p_screen ? RD::UNIFORM_TYPE_IMAGE : RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_binding_10));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, tex(w.irradiance)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 12, tex(w.distance)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 13, tex(w.probe_data)));
	u.push_back(RD::Uniform(p_set == TRACE_SET_REFLECTIONS ? RD::UNIFORM_TYPE_TEXTURE : RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, p_binding_14));
	u.push_back(RD::Uniform(p_screen ? RD::UNIFORM_TYPE_TEXTURE : RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, p_binding_15));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, tex(l.irradiance)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 17, tex(l.distance)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 18, tex(l.probe_data)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 19, tex(w.irradiance_lights)));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 20, tex(l.irradiance_lights)));
	if (p_set == TRACE_SET_REFLECTIONS) {
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 21, p_binding_21));
	}
	u.push_back(p_rb->get_raytracing_geometry_buffers_uniform(30)); // Mariner E5d: RT_GEOMETRY_BUFFERS_BINDING.
	return u;
}

// Mariner L1: screen space probes.

Size2i RenderDDGI::screen_probe_grid(const Size2i &p_internal_size, uint32_t p_tile) {
	const int tile = int(MAX(p_tile, 1u));
	return Size2i((p_internal_size.width + tile - 1) / tile, (p_internal_size.height + tile - 1) / tile);
}

RID RenderDDGI::get_inactive_screen_buffer() {
	if (inactive_screen_buffer.is_null()) {
		Vector<uint8_t> data;
		data.resize(64); // Header (active flag clear) + one SH / one probe.
		memset(data.ptrw(), 0, data.size());
		inactive_screen_buffer = RD::get_singleton()->storage_buffer_create(data.size(), data);
		RD::get_singleton()->set_resource_name(inactive_screen_buffer, "DDGI screen probes (inactive)");
	}
	return inactive_screen_buffer;
}

struct ScreenProbesPushConstant {
	uint32_t grid_x;
	uint32_t grid_y;
	uint32_t tile;
	uint32_t frame_index;
	uint32_t width;
	uint32_t height;
	float hysteresis;
	uint32_t has_history;
};

bool RenderDDGI::update_screen_probes(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, RID p_depth, RID p_normal_roughness) {
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), false);
	RenderSceneBuffersRD::DDGIScreenProbes &sp = rb->ddgi_screen;
	if (!initialized || !rb->ddgi.active || p_depth.is_null() || p_normal_roughness.is_null() || frame_trace.scene_uniform_buffer.is_null()) {
		return false;
	}
	const TraceVariant variant = use_octmap_array ? TRACE_SCREEN_RADIANCE_ARRAY : TRACE_SCREEN_RADIANCE_2D;
	for (int i = 0; i < SCREEN_MAX; i++) {
		ERR_FAIL_COND_V(screen_pipeline[i].is_null(), false);
	}
	ERR_FAIL_COND_V(trace_pipeline[variant].is_null(), false);

	RD *rd = RD::get_singleton();
	RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID env = p_render_data->environment;
	const uint32_t tile = uint32_t(env_storage->environment_get_ddgi_screen_probes_size(env));
	const Size2i internal_size = rb->get_internal_size();
	const Size2i grid = screen_probe_grid(internal_size, tile);
	const uint32_t probe_count = uint32_t(grid.width * grid.height);

	if (sp.trace.is_null() || sp.tile != tile || sp.internal_size != internal_size) {
		rb->free_ddgi_screen_probes();
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = grid.width * IRRADIANCE_TEXELS;
		tf.height = grid.height * IRRADIANCE_TEXELS;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		sp.trace = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.trace, "DDGI screen probes trace");
		for (int i = 0; i < 2; i++) {
			sp.radiance[i] = rd->texture_create(tf, RD::TextureView());
			rd->set_resource_name(sp.radiance[i], "DDGI screen probes radiance");
			rd->texture_clear(sp.radiance[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
			sp.probes[i] = rd->storage_buffer_create(probe_count * 32);
			rd->set_resource_name(sp.probes[i], "DDGI screen probes");
		}
		sp.radiance_filled = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.radiance_filled, "DDGI screen probes radiance (filled)");
		sp.radiance_filtered = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.radiance_filtered, "DDGI screen probes radiance (filtered)");
		sp.sh = rd->storage_buffer_create(16 + probe_count * 48);
		rd->set_resource_name(sp.sh, "DDGI screen probes SH");
		sp.sh_filtered = rd->storage_buffer_create(16 + probe_count * 48);
		rd->set_resource_name(sp.sh_filtered, "DDGI screen probes SH (filtered)");
		tf.width = internal_size.width;
		tf.height = internal_size.height;
		sp.last_color = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.last_color, "DDGI screen probes last color");
		rd->texture_clear(sp.last_color, Color(0, 0, 0, 0), 0, 1, 0, 1);
		sp.tile = tile;
		sp.internal_size = internal_size;
		sp.grid = grid;
		sp.current = 0;
		sp.has_history = false;
		sp.has_last_color = false;
	}
	// History only from the previous frame (a gap in updates drops it).
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	const bool history = sp.has_history && sp.frame + 1 == frame;
	if (!history) {
		sp.refl_has_history = false; // L2: a gap in the updates drops the reflection history too.
	}
	sp.current ^= 1u;
	const uint32_t cur = sp.current;
	const uint32_t prev = cur ^ 1u;

	RD::get_singleton()->draw_command_begin_label("DDGI screen probes (Mariner L1)");
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	ScreenProbesPushConstant push = {};
	push.grid_x = grid.width;
	push.grid_y = grid.height;
	push.tile = tile;
	push.frame_index = sp.frame_index;
	push.width = internal_size.width;
	push.height = internal_size.height;
	push.hysteresis = env_storage->environment_get_ddgi_screen_probes_hysteresis(env);
	// Mariner L1b: bit 0 - history; bit 1 - the probe on a random pixel of its tile each frame. Off by default: on
	// surfaces smaller than a few tiles (small stones) a jittered probe lands on another surface every frame, its
	// history is dropped and the probes that fit a pixel change - the light of a still view flickers.
	push.has_history = (history ? 1 : 0) | (GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/screen_probes_jitter") ? 2 : 0);
	const RID ubo = frame_trace.scene_uniform_buffer;

	// Placement.
	{
		RID set = cache->get_cache(screen_shader_rd[SCREEN_PLACE], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_depth),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_normal_roughness),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, nearest),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, sp.probes[cur]));
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_PLACE]);
		rd->compute_list_bind_uniform_set(cl, set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_threads(cl, grid.width, grid.height, 1);
		rd->compute_list_end();
	}

	// Rays.
	{
		RID trace_rd = trace_shader_rd[variant];
		const RID black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RID set0 = cache->get_cache_vec(trace_rd, 0, _trace_uniforms(rb.ptr(), sp.trace, sp.probes[cur], sp.has_last_color ? sp.last_color : black, TRACE_SET_SCREEN));
		RID set1 = rb->get_raytracing_texture_uniform_set(trace_rd, 1);
		if (set0.is_null() || set1.is_null()) {
			RD::get_singleton()->draw_command_end_label();
			ERR_FAIL_V_MSG(false, "DDGI screen probes: could not create the uniform sets.");
		}
		struct {
			uint32_t light_count;
			uint32_t flags;
			float ray_length;
			uint32_t grid_x;
			uint32_t grid_y;
			uint32_t frame_index;
			uint32_t pad[2];
		} trace_push = {};
		trace_push.light_count = frame_trace.light_count;
		// Mariner L1d: the lit color of the previous frame for the hits that were on screen can be turned off (a check
		// of where the light of the screen probes comes from: every hit is then shaded as the grids' rays shade it).
		const bool reuse_frame = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/screen_probes_reuse_frame");
		trace_push.flags = frame_trace.flags | (sp.has_last_color && reuse_frame ? 4u : 0u); // DDGI_TRACE_FLAG_LAST_COLOR.
		trace_push.ray_length = frame_trace.ray_length;
		trace_push.grid_x = grid.width;
		trace_push.grid_y = grid.height;
		trace_push.frame_index = sp.frame_index;
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, trace_pipeline[variant]);
		rd->compute_list_bind_uniform_set(cl, set0, 0);
		rd->compute_list_bind_uniform_set(cl, set1, 1);
		rd->compute_list_set_push_constant(cl, &trace_push, sizeof(trace_push));
		rd->compute_list_dispatch_threads(cl, grid.width * IRRADIANCE_TEXELS, grid.height * IRRADIANCE_TEXELS, 1);
		rd->compute_list_end();
	}

	// Temporal accumulation and SH, then the spatial filter.
	{
		sp.refl_active = false;
		uint32_t header[4] = { uint32_t(grid.width), uint32_t(grid.height), tile, 1 };
		if (env_storage->environment_get_ddgi_reflections(env)) {
			_update_reflections(rb.ptr(), p_render_data, p_depth, p_normal_roughness);
			header[3] |= 2u; // L2 on: probe reflections of rough surfaces; the traced ones where refl_active (bound then).
		} else if (sp.reflection.is_valid()) {
			RID *rids[] = { &sp.refl_trace, &sp.refl_accum[0], &sp.refl_accum[1], &sp.reflection };
			for (RID *r : rids) {
				rd->free_rid(*r);
				*r = RID();
			}
			sp.refl_has_history = false;
		}
		rd->buffer_update(sp.sh_filtered, 0, sizeof(header), header);
		RID set = cache->get_cache(screen_shader_rd[SCREEN_INTEGRATE], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, sp.trace),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, sp.radiance[prev]),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, sp.radiance[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, sp.probes[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, sp.probes[prev]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, sp.sh),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, sp.radiance_filled));
		RID filter_set = cache->get_cache(screen_shader_rd[SCREEN_FILTER], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, sp.probes[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, sp.sh),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, sp.sh_filtered));
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_INTEGRATE]);
		rd->compute_list_bind_uniform_set(cl, set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch(cl, grid.width, grid.height, 1);
		rd->compute_list_add_barrier(cl);
		rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_FILTER]);
		rd->compute_list_bind_uniform_set(cl, filter_set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_threads(cl, grid.width, grid.height, 1);
		if (header[3] & 2u) {
			// L2: the filtered radiance of the rough reflections.
			RID radiance_set = cache->get_cache(screen_shader_rd[SCREEN_FILTER_RADIANCE], 0,
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, sp.probes[cur]),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, sp.radiance_filled),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, sp.radiance_filtered));
			rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_FILTER_RADIANCE]);
			rd->compute_list_bind_uniform_set(cl, radiance_set, 0);
			rd->compute_list_set_push_constant(cl, &push, sizeof(push));
			rd->compute_list_dispatch(cl, grid.width, grid.height, 1);
		}
		rd->compute_list_end();
	}
	RD::get_singleton()->draw_command_end_label();

	sp.has_history = true;
	sp.frame = frame;
	sp.frame_index++;
	return true;
}

// Mariner L2: the ray traced glossy reflections of the frame (half resolution rays, accumulation, full resolution
// filter). Uses the trace state of the frame (update()) and the screen probes' last color.
void RenderDDGI::_update_reflections(RenderSceneBuffersRD *p_rb, const RenderDataRD *p_render_data, RID p_depth, RID p_normal_roughness) {
	RenderSceneBuffersRD::DDGIScreenProbes &sp = p_rb->ddgi_screen;
	RD *rd = RD::get_singleton();
	const TraceVariant variant = use_octmap_array ? TRACE_REFLECTIONS_ARRAY : TRACE_REFLECTIONS_2D;
	if (trace_pipeline[variant].is_null() || screen_pipeline[SCREEN_REFL_TEMPORAL].is_null() || screen_pipeline[SCREEN_REFL_RESOLVE].is_null()) {
		return;
	}
	const Size2i full = sp.internal_size;
	const Size2i half((full.width + 1) / 2, (full.height + 1) / 2);
	if (sp.reflection.is_null()) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = half.width;
		tf.height = half.height;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		sp.refl_trace = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.refl_trace, "DDGI reflections trace");
		for (int i = 0; i < 2; i++) {
			sp.refl_accum[i] = rd->texture_create(tf, RD::TextureView());
			rd->set_resource_name(sp.refl_accum[i], "DDGI reflections accumulation");
			rd->texture_clear(sp.refl_accum[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		tf.width = full.width;
		tf.height = full.height;
		sp.reflection = rd->texture_create(tf, RD::TextureView());
		rd->set_resource_name(sp.reflection, "DDGI reflections");
		sp.refl_has_history = false;
	}
	const uint32_t cur = sp.current;
	const uint32_t prev = cur ^ 1u;
	const float max_roughness = RendererEnvironmentStorage::get_singleton()->environment_get_ddgi_reflections_max_roughness(p_render_data->environment);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID nearest = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID black = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	RD::get_singleton()->draw_command_begin_label("DDGI reflections (Mariner L2)");
	{
		RID trace_rd = trace_shader_rd[variant];
		RID set0 = cache->get_cache_vec(trace_rd, 0, _trace_uniforms(p_rb, sp.refl_trace, p_depth, sp.has_last_color ? sp.last_color : black, TRACE_SET_REFLECTIONS, p_normal_roughness));
		RID set1 = p_rb->get_raytracing_texture_uniform_set(trace_rd, 1);
		if (set0.is_null() || set1.is_null()) {
			RD::get_singleton()->draw_command_end_label();
			ERR_FAIL_MSG("DDGI reflections: could not create the uniform sets.");
		}
		struct {
			uint32_t light_count;
			uint32_t flags;
			float ray_length;
			uint32_t grid_x;
			uint32_t grid_y;
			uint32_t frame_index;
			float max_roughness;
			uint32_t pad;
		} push = {};
		push.light_count = frame_trace.light_count;
		push.flags = frame_trace.flags | (sp.has_last_color ? 4u : 0u);
		push.ray_length = frame_trace.ray_length;
		push.grid_x = half.width;
		push.grid_y = half.height;
		push.frame_index = sp.frame_index;
		push.max_roughness = max_roughness;
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, trace_pipeline[variant]);
		rd->compute_list_bind_uniform_set(cl, set0, 0);
		rd->compute_list_bind_uniform_set(cl, set1, 1);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_threads(cl, half.width, half.height, 1);
		rd->compute_list_end();
	}
	{
		ScreenProbesPushConstant push = {};
		push.grid_x = half.width;
		push.grid_y = half.height;
		push.frame_index = sp.frame_index;
		push.width = full.width;
		push.height = full.height;
		push.hysteresis = max_roughness;
		push.has_history = sp.refl_has_history ? 1 : 0;
		const RID ubo = frame_trace.scene_uniform_buffer;
		RID temporal_set = cache->get_cache(screen_shader_rd[SCREEN_REFL_TEMPORAL], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, sp.refl_trace),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, sp.refl_accum[prev]),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, sp.refl_accum[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_depth),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_normal_roughness),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 6, nearest));
		RID resolve_set = cache->get_cache(screen_shader_rd[SCREEN_REFL_RESOLVE], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, sp.refl_accum[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_depth),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_normal_roughness),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 4, nearest),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, sp.reflection));
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_REFL_TEMPORAL]);
		rd->compute_list_bind_uniform_set(cl, temporal_set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_threads(cl, half.width, half.height, 1);
		rd->compute_list_add_barrier(cl);
		rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_REFL_RESOLVE]);
		rd->compute_list_bind_uniform_set(cl, resolve_set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_threads(cl, full.width, full.height, 1);
		rd->compute_list_end();
	}
	RD::get_singleton()->draw_command_end_label();
	sp.refl_has_history = true;
	sp.refl_active = true;
}

void RenderDDGI::store_screen_probes_color(const RenderDataRD *p_render_data, RID p_color, RID p_depth) {
	ERR_FAIL_NULL(p_render_data);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	RenderSceneBuffersRD::DDGIScreenProbes &sp = rb->ddgi_screen;
	if (sp.last_color.is_null() || sp.frame != RSG::rasterizer->get_frame_number() || p_color.is_null() || p_depth.is_null()) {
		return;
	}
	RD *rd = RD::get_singleton();
	RID nearest = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID set = UniformSetCacheRD::get_singleton()->get_cache(screen_shader_rd[SCREEN_STORE_COLOR], 0,
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, frame_trace.scene_uniform_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_color),
			RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_depth),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, nearest),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, sp.last_color));
	ScreenProbesPushConstant push = {};
	push.width = sp.internal_size.width;
	push.height = sp.internal_size.height;
	RD::get_singleton()->draw_command_begin_label("DDGI screen probes: store color");
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, screen_pipeline[SCREEN_STORE_COLOR]);
	rd->compute_list_bind_uniform_set(cl, set, 0);
	rd->compute_list_set_push_constant(cl, &push, sizeof(push));
	rd->compute_list_dispatch_threads(cl, push.width, push.height, 1);
	rd->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	sp.has_last_color = true;
}

RenderDDGI::~RenderDDGI() {
	if (inactive_params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(inactive_params_buffer);
	}
	if (inactive_screen_buffer.is_valid()) {
		RD::get_singleton()->free_rid(inactive_screen_buffer);
	}
	if (initialized) {
		trace_shader.version_free(trace_version);
		update_shader.version_free(update_version);
		screen_shader.version_free(screen_version);
	}
}
