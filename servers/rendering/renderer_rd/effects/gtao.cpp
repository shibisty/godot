/**************************************************************************/
/*  gtao.cpp                                                              */
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

#include "gtao.h"

#include "motion_blur.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

void GTAO::quality_samples(int p_quality, uint32_t &r_directions, uint32_t &r_steps) {
	// Spartan's own 2 x 3 is the low level; the temporal accumulation, the filter and the TAA / upscaler add the rest.
	static const uint32_t directions[5] = { 1, 2, 2, 3, 4 };
	static const uint32_t steps[5] = { 3, 3, 4, 6, 8 };
	const int q = CLAMP(p_quality, 0, 4);
	r_directions = directions[q];
	r_steps = steps[q];
}

GTAO::GTAO() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	for (RID &buffer : params_buffers) {
		buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	}
}

GTAO::~GTAO() {
	for (RID &buffer : params_buffers) {
		RD::get_singleton()->free_rid(buffer);
	}
	shader.version_free(shader_version);
}

void GTAO::process(RenderSceneBuffersRD *p_buffers, RID p_depth, RID p_normal_roughness, RID p_velocity, RID p_final, const Size2i &p_size, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	RD *rd = RD::get_singleton();

	// History, ping-ponged by frame.
	const StringName scope = SNAME("mariner_gtao");
	const StringName n_history[2] = { SNAME("history_0"), SNAME("history_1") };
	const StringName n_depth[2] = { SNAME("depth_0"), SNAME("depth_1") };
	bool fresh = false;
	if (p_buffers->has_texture(scope, n_history[0]) && (p_buffers->get_texture_format(scope, n_history[0]).width != uint32_t(p_size.width) || p_buffers->get_texture_format(scope, n_history[0]).height != uint32_t(p_size.height))) {
		p_buffers->clear_context(scope);
	}
	if (!p_buffers->has_texture(scope, n_history[0])) {
		const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		for (int i = 0; i < 2; i++) {
			p_buffers->create_texture(scope, n_history[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, p_size);
			p_buffers->create_texture(scope, n_depth[i], RD::DATA_FORMAT_R32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, p_size);
		}
		fresh = true;
	}
	const uint32_t write = uint32_t(p_settings.frame & 1);
	const uint32_t read = write ^ 1;
	const RID history_read = p_buffers->get_texture(scope, n_history[read]);
	const RID history_write = p_buffers->get_texture(scope, n_history[write]);
	const RID depth_read = p_buffers->get_texture(scope, n_depth[read]);
	const RID depth_write = p_buffers->get_texture(scope, n_depth[write]);

	Params params = {};
	MaterialStorage::store_camera(p_settings.projection.inverse(), params.inv_projection);
	MaterialStorage::store_transform(Transform3D(p_settings.view_to_world, Vector3()), params.view_to_world);
	params.size[0] = p_size.width;
	params.size[1] = p_size.height;
	params.radius = MAX(p_settings.radius, 0.01f);
	params.intensity = MAX(p_settings.intensity, 0.0f);
	quality_samples(p_settings.quality, params.directions, params.steps);
	params.frame = uint32_t(p_settings.frame);
	params.history_valid = (fresh || p_settings.camera_cut) ? 0 : 1;
	params.use_velocity = p_velocity.is_valid() ? 1 : 0;
	params.sharpness = CLAMP(p_settings.sharpness, 0.0f, 1.0f);
	params.thickness = MAX(p_settings.thickness, 0.0f);

	const RID shader_rd = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rd.is_null());
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID velocity = p_velocity.is_valid() ? p_velocity : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, velocity }));
	RD::Uniform u_final(RD::UNIFORM_TYPE_IMAGE, 7, p_final);

	for (uint32_t mode = 0; mode < 2; mode++) {
		params.mode = mode;
		rd->buffer_update(params_buffers[mode], 0, sizeof(Params), &params);
		RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, params_buffers[mode]);
		RID uniform_set;
		if (mode == 0) {
			// Main pass: reads last frame's history, writes this frame's.
			RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear, history_read }));
			RD::Uniform u_history_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest, depth_read }));
			RD::Uniform u_history_out(RD::UNIFORM_TYPE_IMAGE, 5, history_write);
			RD::Uniform u_depth_out(RD::UNIFORM_TYPE_IMAGE, 6, depth_write);
			uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_depth, u_normal, u_velocity, u_history, u_history_depth, u_history_out, u_depth_out, u_final, u_params);
		} else {
			// Filter: reads this frame's history; the history images stay bound to the (unused) read targets.
			RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest, history_write }));
			RD::Uniform u_history_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest, depth_write }));
			RD::Uniform u_history_out(RD::UNIFORM_TYPE_IMAGE, 5, history_read);
			RD::Uniform u_depth_out(RD::UNIFORM_TYPE_IMAGE, 6, depth_read);
			uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_depth, u_normal, u_velocity, u_history, u_history_depth, u_history_out, u_depth_out, u_final, u_params);
		}
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_dispatch_threads(compute_list, p_size.width, p_size.height, 1);
		rd->compute_list_end();
	}
}
