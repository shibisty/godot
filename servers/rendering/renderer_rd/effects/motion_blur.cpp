/**************************************************************************/
/*  motion_blur.cpp                                                       */
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

#include "motion_blur.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

MotionBlur::MotionBlur() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	for (RID &buffer : params_buffers) {
		buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	}
}

MotionBlur::~MotionBlur() {
	for (RID &buffer : params_buffers) {
		RD::get_singleton()->free_rid(buffer);
	}
	shader.version_free(shader_version);
}

void MotionBlur::process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_velocity, RID p_depth, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	RD *rd = RD::get_singleton();

	// Longest blur in pixels; the tiles are as large (a pixel's blur reaches at most into the neighbor tiles).
	const float max_length = MAX(1.0f, p_settings.max_length * p_color_size.height);
	const int tile_size = CLAMP(int(Math::ceil(max_length)), 8, 128);
	const Size2i tiles((p_color_size.width + tile_size - 1) / tile_size, (p_color_size.height + tile_size - 1) / tile_size);

	// Working textures, sized with the view.
	const StringName scope = SNAME("mariner_motion_blur");
	const StringName n_prepared = SNAME("prepared");
	const StringName n_tile = SNAME("tile_max");
	const StringName n_neighbor = SNAME("neighbor_max");
	const StringName n_source = SNAME("source");
	if (p_buffers->has_texture(scope, n_prepared) && (p_buffers->get_texture_format(scope, n_prepared).width != uint32_t(p_color_size.width) || p_buffers->get_texture_format(scope, n_prepared).height != uint32_t(p_color_size.height) || p_buffers->get_texture_format(scope, n_tile).width != uint32_t(tiles.width) || p_buffers->get_texture_format(scope, n_tile).height != uint32_t(tiles.height))) {
		p_buffers->clear_context(scope);
	}
	if (!p_buffers->has_texture(scope, n_prepared)) {
		const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_buffers->create_texture(scope, n_prepared, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, p_color_size);
		p_buffers->create_texture(scope, n_source, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, p_color_size);
		p_buffers->create_texture(scope, n_tile, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, tiles);
		p_buffers->create_texture(scope, n_neighbor, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, tiles);
	}
	const RID prepared = p_buffers->get_texture(scope, n_prepared);
	const RID source = p_buffers->get_texture(scope, n_source);
	const RID tile_max = p_buffers->get_texture(scope, n_tile);
	const RID neighbor_max = p_buffers->get_texture(scope, n_neighbor);

	Params params = {};
	MaterialStorage::store_camera(p_settings.reprojection, params.reprojection);
	MaterialStorage::store_camera(p_settings.projection.inverse(), params.inv_projection);
	params.color_size[0] = p_color_size.width;
	params.color_size[1] = p_color_size.height;
	params.tile_count[0] = tiles.width;
	params.tile_count[1] = tiles.height;
	params.intensity = p_settings.intensity;
	params.camera_motion = p_settings.camera_motion;
	params.max_length = max_length;
	params.tile_size = tile_size;
	params.samples = CLAMP(p_settings.samples, 4, 32);
	params.frame = uint32_t(p_settings.frame);

	const RID shader_rd = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rd.is_null());
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_velocity }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_prepared(RD::UNIFORM_TYPE_IMAGE, 3, prepared);
	RD::Uniform u_tile(RD::UNIFORM_TYPE_IMAGE, 4, tile_max);
	RD::Uniform u_neighbor(RD::UNIFORM_TYPE_IMAGE, 5, neighbor_max);
	// Copy pass: color -> source; the others: source -> color.
	RD::Uniform u_color_as_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, p_color }));
	RD::Uniform u_source_as_dest(RD::UNIFORM_TYPE_IMAGE, 6, source);
	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, source }));
	RD::Uniform u_color_as_dest(RD::UNIFORM_TYPE_IMAGE, 6, p_color);

	struct Pass {
		uint32_t mode;
		Size2i size;
		bool copy_set;
	};
	const Pass passes[5] = {
		{ 4, p_color_size, true }, // Copy.
		{ 0, p_color_size, false }, // Velocity.
		{ 1, tiles, false }, // Tile max.
		{ 2, tiles, false }, // Neighbor max.
		{ 3, p_color_size, false }, // Gather.
	};
	for (uint32_t i = 0; i < 5; i++) {
		const Pass &pass = passes[i];
		params.mode = pass.mode;
		const RID buffer = params_buffers[i];
		rd->buffer_update(buffer, 0, sizeof(Params), &params);
		RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, buffer);
		RID uniform_set;
		if (pass.copy_set) {
			uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_velocity, u_depth, u_color_as_source, u_prepared, u_tile, u_neighbor, u_source_as_dest, u_params);
		} else {
			uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_velocity, u_depth, u_source, u_prepared, u_tile, u_neighbor, u_color_as_dest, u_params);
		}
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_dispatch_threads(compute_list, pass.size.width, pass.size.height, 1);
		rd->compute_list_end();
	}
}
