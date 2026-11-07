/**************************************************************************/
/*  render_restir.cpp                                                     */
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

#include "render_restir.h"

#include "render_pathtracer.h"

#include "core/templates/hash_map.h"
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

namespace {

struct PushConstant {
	uint32_t pass;
	int32_t step;
	uint32_t pad[2];
};

void store_projection(const Projection &p_projection, float *r_mat4) {
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			r_mat4[i * 4 + j] = p_projection.columns[i][j];
		}
	}
}

} // namespace

bool RenderRestirDI::is_supported() {
	return RenderPathtracer::is_supported();
}

void RenderRestirDI::build_grid(const LocalVector<Light> &p_lights, const Vector3 &p_camera, float p_max_distance, Grid &r_grid) {
	r_grid.cells.clear();
	r_grid.indices.clear();
	r_grid.size = Vector3i();
	if (p_lights.is_empty()) {
		return;
	}
	// The grid covers the lit space: the union of the light spheres (clipped to the max distance around the camera).
	// Points outside it are lit by no light.
	AABB bounds;
	double mean_range = 0.0;
	for (uint32_t i = 0; i < p_lights.size(); i++) {
		const Light &l = p_lights[i];
		const float range = l.inv_range > 0.0f ? 1.0f / l.inv_range : 0.0f;
		const AABB sphere(Vector3(l.position[0], l.position[1], l.position[2]) - Vector3(range, range, range), Vector3(range, range, range) * 2.0f);
		bounds = i == 0 ? sphere : bounds.merge(sphere);
		mean_range += range;
	}
	mean_range /= p_lights.size();
	const AABB clip(p_camera - Vector3(p_max_distance, p_max_distance, p_max_distance), Vector3(p_max_distance, p_max_distance, p_max_distance) * 2.0f);
	bounds = bounds.intersection(clip);
	if (!bounds.has_volume()) {
		r_grid.size = Vector3i(1, 1, 1); // All lights are out of reach: one empty cell.
		r_grid.origin = p_camera;
		r_grid.cell_size = 1.0f;
		r_grid.cells.resize(2);
		r_grid.cells[0] = 0;
		r_grid.cells[1] = 0;
		r_grid.indices.resize(1);
		return;
	}

	// Cells about half the mean light range (a light lands in some 4^3 cells), coarser when there would be too many.
	float cell = CLAMP(float(mean_range) * 0.5f, 0.25f, 1024.0f);
	LocalVector<uint32_t> counts;
	for (int attempt = 0; attempt < 24; attempt++, cell *= 1.5f) {
		const Vector3i size = Vector3i(Math::ceil(bounds.size.x / cell), Math::ceil(bounds.size.y / cell), Math::ceil(bounds.size.z / cell)).maxi(1);
		if (size.x > int(MAX_GRID_AXIS) || size.y > int(MAX_GRID_AXIS) || size.z > int(MAX_GRID_AXIS) || uint64_t(size.x) * size.y * size.z > MAX_GRID_CELLS) {
			continue;
		}
		const Vector3 origin = bounds.position;
		const uint32_t cell_count = uint32_t(size.x) * uint32_t(size.y) * uint32_t(size.z);
		counts.resize(cell_count);
		memset(counts.ptr(), 0, cell_count * sizeof(uint32_t));
		uint64_t total = 0;
		auto cell_range = [&](const Light &l, Vector3i &r_from, Vector3i &r_to) -> bool {
			const float range = l.inv_range > 0.0f ? 1.0f / l.inv_range : 0.0f;
			const Vector3 p(l.position[0], l.position[1], l.position[2]);
			const Vector3 from = ((p - Vector3(range, range, range) - origin) / cell).floor();
			const Vector3 to = ((p + Vector3(range, range, range) - origin) / cell).floor();
			r_from = Vector3i(MAX(int(from.x), 0), MAX(int(from.y), 0), MAX(int(from.z), 0));
			r_to = Vector3i(MIN(int(to.x), size.x - 1), MIN(int(to.y), size.y - 1), MIN(int(to.z), size.z - 1));
			return r_from.x <= r_to.x && r_from.y <= r_to.y && r_from.z <= r_to.z;
		};
		for (const Light &l : p_lights) {
			Vector3i from, to;
			if (cell_range(l, from, to)) {
				for (int z = from.z; z <= to.z; z++) {
					for (int y = from.y; y <= to.y; y++) {
						for (int x = from.x; x <= to.x; x++) {
							counts[(z * size.y + y) * size.x + x]++;
						}
					}
				}
				total += uint64_t(to.x - from.x + 1) * (to.y - from.y + 1) * (to.z - from.z + 1);
			}
		}
		if (total > MAX_GRID_INDICES) {
			continue;
		}
		r_grid.origin = origin;
		r_grid.cell_size = cell;
		r_grid.size = size;
		r_grid.cells.resize(cell_count * 2);
		uint32_t offset = 0;
		for (uint32_t c = 0; c < cell_count; c++) {
			r_grid.cells[c * 2 + 0] = offset;
			r_grid.cells[c * 2 + 1] = 0;
			offset += counts[c];
		}
		r_grid.indices.resize(MAX(offset, 1u));
		for (uint32_t i = 0; i < p_lights.size(); i++) {
			Vector3i from, to;
			if (!cell_range(p_lights[i], from, to)) {
				continue;
			}
			for (int z = from.z; z <= to.z; z++) {
				for (int y = from.y; y <= to.y; y++) {
					for (int x = from.x; x <= to.x; x++) {
						const uint32_t c = (z * size.y + y) * size.x + x;
						r_grid.indices[r_grid.cells[c * 2] + r_grid.cells[c * 2 + 1]++] = i;
					}
				}
			}
		}
		return;
	}
	// No grid fits (lights everywhere at once): all points sample all lights.
	r_grid.size = Vector3i();
}

void RenderRestirDI::build_remap(const LocalVector<uint64_t> &p_previous, const LocalVector<uint64_t> &p_current, LocalVector<uint32_t> &r_remap) {
	HashMap<uint64_t, uint32_t> index;
	for (uint32_t i = 0; i < p_current.size(); i++) {
		index.insert(p_current[i], i);
	}
	r_remap.resize(p_previous.size());
	for (uint32_t i = 0; i < p_previous.size(); i++) {
		HashMap<uint64_t, uint32_t>::ConstIterator it = index.find(p_previous[i]);
		r_remap[i] = it ? it->value : uint32_t(INVALID_LIGHT);
	}
}

void RenderRestirDI::_initialize() {
	if (initialized) {
		return;
	}
	initialized = true;
	RD *rd = RD::get_singleton();
	Vector<String> variants;
	variants.push_back("");
	di_shader.initialize(variants);
	di_version = di_shader.version_create();
	di_shader_rd = di_shader.version_get_shader(di_version, 0);
	di_pipeline = di_shader_rd.is_valid() ? rd->compute_pipeline_create(di_shader_rd) : RID();
	denoise_shader.initialize(variants);
	denoise_version = denoise_shader.version_create();
	denoise_shader_rd = denoise_shader.version_get_shader(denoise_version, 0);
	denoise_pipeline = denoise_shader_rd.is_valid() ? rd->compute_pipeline_create(denoise_shader_rd) : RID();
	shadow_denoise_shader.initialize(variants);
	shadow_denoise_version = shadow_denoise_shader.version_create();
	shadow_denoise_shader_rd = shadow_denoise_shader.version_get_shader(shadow_denoise_version, 0);
	shadow_denoise_pipeline = shadow_denoise_shader_rd.is_valid() ? rd->compute_pipeline_create(shadow_denoise_shader_rd) : RID();
}

RenderRestirDI::~RenderRestirDI() {
	if (initialized) {
		di_shader.version_free(di_version);
		denoise_shader.version_free(denoise_version);
		shadow_denoise_shader.version_free(shadow_denoise_version);
	}
}

bool RenderRestirDI::update(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, RID p_normal_roughness) {
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null() || !rb->has_raytracing_scene(), false);
	_initialize();
	ERR_FAIL_COND_V(di_pipeline.is_null() || denoise_pipeline.is_null() || shadow_denoise_pipeline.is_null(), false);

	RD *rd = RD::get_singleton();
	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
	RenderSceneBuffersRD::Restir &st = rb->restir;
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const RID env = p_render_data->environment;
	const uint64_t frame = RSG::rasterizer->get_frame_number();

	// Lights of this frame (as the rasterizer's light buffer: energy × π or the physical units, exposure, fade).
	lights.clear();
	light_ids.clear();
	const Vector3 cam_pos = scene_data->cam_transform.origin;
	const float exposure = p_render_data->camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes) : 1.0f;
	if (p_render_data->lights) {
		const PagedArray<RID> &list = *p_render_data->lights;
		for (uint32_t i = 0; i < (uint32_t)list.size() && lights.size() < MAX_LIGHTS; i++) {
			const RID instance = list[i];
			const RID base = ls->light_instance_get_base_light(instance);
			if (base.is_null() || !ls->light_is_restir(base)) {
				continue;
			}
			const RSE::LightType type = ls->light_get_type(base);
			if (type != RSE::LIGHT_OMNI && type != RSE::LIGHT_SPOT) {
				continue;
			}
			const Transform3D xform = ls->light_instance_get_base_transform(instance);
			float fade = 1.0f;
			if (ls->light_is_distance_fade_enabled(base)) {
				const float distance = cam_pos.distance_to(xform.origin);
				const float begin = ls->light_get_distance_fade_begin(base);
				const float length = ls->light_get_distance_fade_length(base);
				if (distance > begin + length) {
					continue;
				}
				if (distance > begin) {
					fade = Math::smoothstep(0.0f, 1.0f, 1.0f - (distance - begin) / length);
				}
			}
			float energy = (ls->light_is_negative(base) ? -1.0f : 1.0f) * ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY) * fade;
			if (p_owner->is_using_physical_light_units()) {
				energy *= ls->light_get_param(base, RSE::LIGHT_PARAM_INTENSITY);
				energy *= type == RSE::LIGHT_OMNI ? 1.0f / (Math::PI * 4.0f) : 1.0f / Math::PI;
			} else {
				energy *= Math::PI;
			}
			energy *= exposure;
			if (energy <= 0.0f) {
				continue; // Negative lights are not resampled (the target must be positive).
			}
			const Color c = ls->light_get_color(base).srgb_to_linear();
			Light l = {};
			l.position[0] = xform.origin.x;
			l.position[1] = xform.origin.y;
			l.position[2] = xform.origin.z;
			l.inv_range = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE));
			l.color[0] = c.r * energy;
			l.color[1] = c.g * energy;
			l.color[2] = c.b * energy;
			l.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
			const Vector3 dir = -xform.basis.get_column(2).normalized();
			l.direction[0] = dir.x;
			l.direction[1] = dir.y;
			l.direction[2] = dir.z;
			l.type = type == RSE::LIGHT_SPOT ? LIGHT_SPOT : LIGHT_OMNI;
			l.cos_spot_angle = type == RSE::LIGHT_SPOT ? Math::cos(Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE))) : -1.0f;
			l.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			l.size = ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
			l.specular = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;
			lights.push_back(l);
			light_ids.push_back(instance.get_id());
		}
	}

	build_grid(lights, cam_pos, MAX(scene_data->z_far, 1.0f), grid);
	build_remap(st.light_ids, light_ids, remap);

	// Buffers (grow only).
	auto ensure_buffer = [&](RID &r_buffer, uint32_t &r_capacity, uint32_t p_bytes) {
		p_bytes = MAX(p_bytes, 64u);
		if (r_buffer.is_null() || r_capacity < p_bytes) {
			if (r_buffer.is_valid()) {
				rd->free_rid(r_buffer);
			}
			r_capacity = Math::next_power_of_2(p_bytes);
			r_buffer = rd->storage_buffer_create(r_capacity);
		}
	};
	ensure_buffer(st.light_buffer, st.light_capacity, lights.size() * sizeof(Light));
	ensure_buffer(st.cell_buffer, st.cell_capacity, grid.cells.size() * sizeof(uint32_t));
	ensure_buffer(st.index_buffer, st.index_capacity, grid.indices.size() * sizeof(uint32_t));
	ensure_buffer(st.remap_buffer, st.remap_capacity, remap.size() * sizeof(uint32_t));
	if (st.params_buffer.is_null()) {
		st.params_buffer = rd->uniform_buffer_create(sizeof(Params));
	}
	// Mariner E20b: directional lights with ray traced shadows (chosen by LightStorage::update_light_buffers()).
	const RendererRD::LightStorage::DirectionalRtShadow *rt_shadows = nullptr;
	const uint32_t rt_shadow_count = ls->get_directional_rt_shadows(&rt_shadows);
	if (st.shadow_params_buffer.is_null()) {
		st.shadow_params_buffer = rd->uniform_buffer_create(sizeof(ShadowParams));
	}
	{
		ShadowParams sp = {};
		for (uint32_t i = 0; i < rt_shadow_count; i++) {
			sp.lights[i][0] = rt_shadows[i].to_light.x;
			sp.lights[i][1] = rt_shadows[i].to_light.y;
			sp.lights[i][2] = rt_shadows[i].to_light.z;
			sp.lights[i][3] = rt_shadows[i].tan_radius;
		}
		sp.info[0] = rt_shadow_count;
		// History of the shadows: kept only while they are computed every frame (and the ReSTIR history is valid).
		const bool shadow_history = st.shadow_history_valid && st.history_valid && st.frame + 1 == frame;
		sp.info[1] = shadow_history ? 1 : 0;
		sp.range[0] = 10000.0f; // Occluders up to 10 km (mountains, a tall ship far away).
		// Size of a pixel in world units: range[1] × distance (perspective) or range[1] (orthogonal).
		const Projection proj = scene_data->get_cam_projection();
		const float height = MAX(1.0f, float(rb->get_internal_size().height));
		sp.range[1] = 2.0f / (MAX(Math::abs(proj.columns[1][1]), 1e-6f) * height);
		sp.range[2] = proj.is_orthogonal() ? 1.0f : 0.0f;
		rd->buffer_update(st.shadow_params_buffer, 0, sizeof(ShadowParams), &sp);
		// Mariner E20c: the same for the late opaque pass (update_late_shadows()), with its own history flag.
		if (st.late_shadow_params_buffer.is_null()) {
			st.late_shadow_params_buffer = rd->uniform_buffer_create(sizeof(ShadowParams));
		}
		sp.info[1] = (st.late_shadow_history_valid && st.late_frame + 1 == frame) ? 1 : 0;
		rd->buffer_update(st.late_shadow_params_buffer, 0, sizeof(ShadowParams), &sp);
	}
	if (!lights.is_empty()) {
		rd->buffer_update(st.light_buffer, 0, lights.size() * sizeof(Light), lights.ptr());
	}
	if (!grid.cells.is_empty()) {
		rd->buffer_update(st.cell_buffer, 0, grid.cells.size() * sizeof(uint32_t), grid.cells.ptr());
		rd->buffer_update(st.index_buffer, 0, grid.indices.size() * sizeof(uint32_t), grid.indices.ptr());
	}
	if (!remap.is_empty()) {
		rd->buffer_update(st.remap_buffer, 0, remap.size() * sizeof(uint32_t), remap.ptr());
	}

	// Textures.
	const Size2i size = rb->get_internal_size();
	const StringName ctx = SNAME("mariner_restir");
	if (st.size != size) {
		if (rb->has_texture(ctx, SNAME("surface0"))) {
			rb->clear_context(ctx);
		}
		st.size = size;
		st.history_valid = false;
	}
	auto tex = [&](const char *p_name, RD::DataFormat p_format) -> RID {
		const StringName name = StringName(p_name);
		if (!rb->has_texture(ctx, name)) {
			return rb->create_texture(ctx, name, p_format, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		return rb->get_texture(ctx, name);
	};
	const uint32_t cur = st.parity;
	const uint32_t prev = cur ^ 1u;
	const RD::DataFormat RES = RD::DATA_FORMAT_R32G32B32A32_UINT;
	const RD::DataFormat F32 = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	const RD::DataFormat F16 = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	// Temporal reservoirs (ping-pong): the history of a pixel holds only samples found at that surface, the spatial
	// reuse of a frame is not fed back (a neighbour's sample may not reach the pixel, which darkens many short-range
	// lights). The final (spatially reused) reservoir is shaded.
	const RID res_temporal[2] = { tex("reservoir_temporal0", RES), tex("reservoir_temporal1", RES) };
	const RID res_final = tex("reservoir_final", RES);
	const RID surface[2] = { tex("surface0", F32), tex("surface1", F32) };
	const RID noisy_d = tex("noisy_diffuse", F16);
	const RID noisy_s = tex("noisy_specular", F16);
	const RID shadow_raw = tex("shadow_raw", F16); // Mariner E20b.

	// Parameters.
	const bool history_valid = st.history_valid && st.frame + 1 == frame;
	Params params = {};
	const Projection projection = scene_data->get_cam_projection(); // As rasterized (depth correction, jitter).
	store_projection(projection.inverse(), params.inv_projection);
	store_projection(Projection(scene_data->cam_transform), params.view_to_world);
	store_projection(st.prev_world_to_clip, params.prev_world_to_clip);
	params.grid_origin[0] = grid.origin.x;
	params.grid_origin[1] = grid.origin.y;
	params.grid_origin[2] = grid.origin.z;
	params.grid_origin[3] = grid.cell_size;
	params.grid_size[0] = grid.size.x;
	params.grid_size[1] = grid.size.y;
	params.grid_size[2] = grid.size.z;
	params.grid_size[3] = int32_t(lights.size());
	params.screen_size[0] = size.width;
	params.screen_size[1] = size.height;
	params.frame = uint32_t(frame);
	params.flags = (env_storage->environment_get_restir_temporal(env) ? FLAG_TEMPORAL : 0) | (history_valid ? FLAG_HISTORY_VALID : 0);
	params.candidates = uint32_t(env_storage->environment_get_restir_candidates(env));
	params.spatial_samples = uint32_t(env_storage->environment_get_restir_spatial_samples(env));
	params.spatial_radius = MAX(8.0f, size.height * (30.0f / 1080.0f));
	params.prev_light_count = remap.size();
	rd->buffer_update(st.params_buffer, 0, sizeof(Params), &params);

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID depth = rb->get_depth_texture();
	const RenderSceneBuffersRD::RaytracingScene &rts = rb->raytracing_scene;

	auto di_set = [&](RID p_res_in, RID p_res_out, RID p_surface_in, RID p_surface_out) -> RID {
		return cache->get_cache(di_shader_rd, 0,
				RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, rts.tlas),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, st.light_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, st.cell_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, st.index_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest, depth })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest, p_normal_roughness })),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, p_res_in),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_res_out),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_surface_in),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_surface_out),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, noisy_d),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, noisy_s),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, st.params_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, st.remap_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, shadow_raw),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 15, st.shadow_params_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 16, Vector<RID>({ nearest, depth })));
	};

	RD::get_singleton()->draw_command_begin_label("ReSTIR DI");
	auto dispatch = [&](RID p_pipeline, RID p_set, uint32_t p_pass, int32_t p_step) {
		PushConstant push = {};
		push.pass = p_pass;
		push.step = p_step;
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, p_pipeline);
		rd->compute_list_bind_uniform_set(list, p_set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(list, size.width, size.height, 1);
		rd->compute_list_end();
	};
	// Initial candidates + temporal reuse (previous temporal reservoirs, previous surfaces).
	dispatch(di_pipeline, di_set(res_temporal[prev], res_temporal[cur], surface[prev], surface[cur]), 0, 0);
	// Spatial reuse of this frame's temporal reservoirs (this frame's surfaces).
	dispatch(di_pipeline, di_set(res_temporal[cur], res_final, surface[cur], surface[prev]), 1, 0);
	// Shading with the final sample.
	dispatch(di_pipeline, di_set(res_final, res_temporal[prev], surface[cur], surface[prev]), 2, 0);

	st.diffuse = noisy_d;
	st.specular = noisy_s;

	// Mariner E20b: ray traced shadows of directional lights - a ray per pixel and light, then the penumbra sized
	// denoiser (temporal accumulation, spatial filter as wide as the penumbra).
	if (rt_shadow_count > 0) {
		dispatch(di_pipeline, di_set(res_final, res_temporal[prev], surface[cur], surface[prev]), PASS_SHADOW, 0);
		const RID shadow_hist[2] = { tex("shadow_history0", F16), tex("shadow_history1", F16) };
		const RID shadow_len[2] = { tex("shadow_history_length0", F16), tex("shadow_history_length1", F16) };
		const RID shadow_mask = tex("shadow_mask", F16);
		auto sd_set = [&](RID p_hist_in, RID p_len_in, RID p_hist_out, RID p_len_out, RID p_accumulated) -> RID {
			return cache->get_cache(shadow_denoise_shader_rd, 0,
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, shadow_raw),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_hist_in),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_len_in),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_hist_out),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_len_out),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_accumulated),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, shadow_mask),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, surface[cur]),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, surface[prev]),
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 9, st.params_buffer),
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, st.shadow_params_buffer));
		};
		// Temporal: raw + previous history -> this frame's history (the accumulated input is unused here).
		dispatch(shadow_denoise_pipeline, sd_set(shadow_hist[prev], shadow_len[prev], shadow_hist[cur], shadow_len[cur], shadow_raw), 0, 0);
		// Spatial: this frame's history -> mask (the previous history images take the unused outputs).
		dispatch(shadow_denoise_pipeline, sd_set(shadow_raw, shadow_len[cur], shadow_hist[prev], shadow_len[prev], shadow_hist[cur]), 1, 0);
		st.shadow_mask = shadow_mask;
	}
	st.shadow_history_valid = rt_shadow_count > 0;

	if (env_storage->environment_get_restir_denoise(env)) {
		const RID hist_d[2] = { tex("history_diffuse0", F16), tex("history_diffuse1", F16) };
		const RID hist_s[2] = { tex("history_specular0", F16), tex("history_specular1", F16) };
		const RID moments[2] = { tex("moments0", F16), tex("moments1", F16) };
		const RID var_t = tex("variance_temporal", F16);
		const RID filt_d[2] = { tex("filtered_diffuse0", F16), tex("filtered_diffuse1", F16) };
		const RID filt_s[2] = { tex("filtered_specular0", F16), tex("filtered_specular1", F16) };
		const RID var[2] = { tex("variance0", F16), tex("variance1", F16) };
		auto dn_set = [&](RID p_in_d, RID p_in_s, RID p_hd, RID p_hs, RID p_hm, RID p_out_d, RID p_out_s, RID p_out_m, RID p_in_var, RID p_out_var) -> RID {
			return cache->get_cache(denoise_shader_rd, 0,
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_in_d),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_in_s),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_hd),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_hs),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_hm),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_out_d),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, p_out_s),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_out_m),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_in_var),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_out_var),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, surface[cur]),
					RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, surface[prev]),
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, st.params_buffer));
		};
		// Temporal accumulation into this frame's history.
		dispatch(denoise_pipeline, dn_set(noisy_d, noisy_s, hist_d[prev], hist_s[prev], moments[prev], hist_d[cur], hist_s[cur], moments[cur], var[1], var_t), 0, 0);
		// À-trous: history -> 0 -> 1 -> 0 -> 1.
		RID in_d = hist_d[cur];
		RID in_s = hist_s[cur];
		RID in_var = var_t;
		for (int i = 0; i < int(DENOISE_ITERATIONS); i++) {
			const int o = i & 1;
			dispatch(denoise_pipeline, dn_set(in_d, in_s, hist_d[prev], hist_s[prev], moments[prev], filt_d[o], filt_s[o], moments[cur], in_var, var[o]), 1, 1 << i);
			in_d = filt_d[o];
			in_s = filt_s[o];
			in_var = var[o];
		}
		st.diffuse = in_d;
		st.specular = in_s;
	}
	RD::get_singleton()->draw_command_end_label();

	st.shadow_count = rt_shadow_count;
	st.shadow_late_mask = RID();
	st.light_ids = light_ids;
	st.prev_world_to_clip = projection * Projection(scene_data->cam_transform.affine_inverse());
	st.frame = frame;
	st.parity = prev;
	st.history_valid = true;
	return true;
}

bool RenderRestirDI::update_late_shadows(const RenderDataRD *p_render_data, RID p_normal_roughness, RID p_opaque_depth) {
	ERR_FAIL_NULL_V(p_render_data, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null() || !rb->has_raytracing_scene(), false);
	RenderSceneBuffersRD::Restir &st = rb->restir;
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	// After update() of this frame, with ray traced directional shadows.
	if (!initialized || st.frame != frame || st.shadow_count == 0 || st.params_buffer.is_null() || st.late_shadow_params_buffer.is_null() || p_opaque_depth.is_null()) {
		return false;
	}
	RD *rd = RD::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const Size2i size = st.size;
	const StringName ctx = SNAME("mariner_restir");
	auto tex = [&](const char *p_name, RD::DataFormat p_format) -> RID {
		const StringName name = StringName(p_name);
		if (!rb->has_texture(ctx, name)) {
			return rb->create_texture(ctx, name, p_format, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		return rb->get_texture(ctx, name);
	};
	const RD::DataFormat RES = RD::DATA_FORMAT_R32G32B32A32_UINT;
	const RD::DataFormat F32 = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	const RD::DataFormat F16 = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	// Own ping-pong of the late surfaces and history.
	const uint32_t cur = st.late_parity;
	const uint32_t prev = cur ^ 1u;
	const RID surface[2] = { tex("late_surface0", F32), tex("late_surface1", F32) };
	const RID shadow_raw = tex("late_shadow_raw", F16);
	const RID hist[2] = { tex("late_shadow_history0", F16), tex("late_shadow_history1", F16) };
	const RID len[2] = { tex("late_shadow_history_length0", F16), tex("late_shadow_history_length1", F16) };
	const RID mask = tex("late_shadow_mask", F16);
	const RID reservoir = tex("reservoir_final", RES); // Bound, not used by PASS_SHADOW.
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID depth = rb->get_depth_texture();
	const RenderSceneBuffersRD::RaytracingScene &rts = rb->raytracing_scene;

	// Shadow parameters of the late pass (its own history flag), written by update().
	const RID shadow_params = st.late_shadow_params_buffer;

	auto dispatch = [&](RID p_pipeline, RID p_set, uint32_t p_pass, int32_t p_step) {
		PushConstant push = {};
		push.pass = p_pass;
		push.step = p_step;
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, p_pipeline);
		rd->compute_list_bind_uniform_set(list, p_set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(list, size.width, size.height, 1);
		rd->compute_list_end();
	};

	RD::get_singleton()->draw_command_begin_label("Ray Traced Shadows (late opaque)");
	// Rays for the pixels of the late surfaces; their surfaces into late_surface[cur].
	const RID di = cache->get_cache(di_shader_rd, 0,
			RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, rts.tlas),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, st.light_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, st.cell_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, st.index_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest, depth })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest, p_normal_roughness })),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, reservoir),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, tex("late_reservoir_unused", RES)),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, surface[prev]),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, surface[cur]),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, tex("late_unused_diffuse", F16)),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, tex("late_unused_specular", F16)),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, st.params_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, st.remap_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, shadow_raw),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 15, shadow_params),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 16, Vector<RID>({ nearest, p_opaque_depth })));
	dispatch(di_pipeline, di, PASS_SHADOW, 1);
	auto sd_set = [&](RID p_hist_in, RID p_len_in, RID p_hist_out, RID p_len_out, RID p_accumulated) -> RID {
		return cache->get_cache(shadow_denoise_shader_rd, 0,
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, shadow_raw),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_hist_in),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_len_in),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_hist_out),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_len_out),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_accumulated),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, mask),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, surface[cur]),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, surface[prev]),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 9, st.params_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, shadow_params));
	};
	dispatch(shadow_denoise_pipeline, sd_set(hist[prev], len[prev], hist[cur], len[cur], shadow_raw), 0, 0);
	dispatch(shadow_denoise_pipeline, sd_set(shadow_raw, len[cur], hist[prev], len[prev], hist[cur]), 1, 0);
	RD::get_singleton()->draw_command_end_label();

	st.shadow_late_mask = mask;
	st.late_parity = prev;
	st.late_frame = frame;
	st.late_shadow_history_valid = true;
	return true;
}
