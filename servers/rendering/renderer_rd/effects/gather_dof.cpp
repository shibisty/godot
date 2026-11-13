/**************************************************************************/
/*  gather_dof.cpp                                                        */
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

#include "gather_dof.h"

#include "core/math/random_pcg.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

using namespace RendererRD;

uint32_t GatherDOF::rings_for_quality(int p_quality) {
	static const uint32_t rings[4] = { 2, 3, 4, 5 }; // 25, 49, 81, 121 samples
	return rings[CLAMP(p_quality, 0, 3)];
}

uint32_t GatherDOF::sides_for_shape(int p_shape) {
	switch (p_shape) {
		case RSE::DOF_BOKEH_BOX:
			return 4;
		case RSE::DOF_BOKEH_HEXAGON:
			return 6;
		default:
			return 0;
	}
}

GatherDOF::GatherDOF() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SETUP\n");
	modes.push_back("\n#define MODE_TILES\n");
	modes.push_back("\n#define MODE_DILATE\n");
	modes.push_back("\n#define MODE_GATHER\n");
	modes.push_back("\n#define MODE_POSTFILTER\n");
	modes.push_back("\n#define MODE_COMPOSITE\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}
}

GatherDOF::~GatherDOF() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	shader.version_free(shader_version);
}

void GatherDOF::process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_depth, RID p_camera_attributes, float p_z_near, float p_z_far, bool p_orthogonal, uint64_t p_frame) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	RD *rd = RD::get_singleton();
	RendererCameraAttributes *ca = RSG::camera_attributes;

	const Size2i half_size((p_color_size.width + 1) / 2, (p_color_size.height + 1) / 2);
	const Size2i tiles((half_size.width + TILE - 1) / TILE, (half_size.height + TILE - 1) / TILE);

	// Working textures, sized with the view.
	const StringName scope = SNAME("mariner_gather_dof");
	const StringName n_half = SNAME("half");
	const StringName n_tiles = SNAME("tiles");
	const StringName n_dilated = SNAME("dilated");
	const StringName n_bg = SNAME("background");
	const StringName n_fg = SNAME("foreground");
	const StringName n_bg_f = SNAME("background_filtered");
	const StringName n_fg_f = SNAME("foreground_filtered");
	if (p_buffers->has_texture(scope, n_half) && (p_buffers->get_texture_format(scope, n_half).width != uint32_t(half_size.width) || p_buffers->get_texture_format(scope, n_half).height != uint32_t(half_size.height))) {
		p_buffers->clear_context(scope);
	}
	if (!p_buffers->has_texture(scope, n_half)) {
		const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_buffers->create_texture(scope, n_half, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, half_size);
		p_buffers->create_texture(scope, n_tiles, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, tiles);
		p_buffers->create_texture(scope, n_dilated, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, tiles);
		p_buffers->create_texture(scope, n_bg, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, half_size);
		p_buffers->create_texture(scope, n_fg, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, half_size);
		p_buffers->create_texture(scope, n_bg_f, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, half_size);
		p_buffers->create_texture(scope, n_fg_f, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, half_size);
	}
	const RID half = p_buffers->get_texture(scope, n_half);
	const RID tile_max = p_buffers->get_texture(scope, n_tiles);
	const RID dilated = p_buffers->get_texture(scope, n_dilated);
	const RID bg = p_buffers->get_texture(scope, n_bg);
	const RID fg = p_buffers->get_texture(scope, n_fg);
	const RID bg_f = p_buffers->get_texture(scope, n_bg_f);
	const RID fg_f = p_buffers->get_texture(scope, n_fg_f);

	// Circle of confusion as the classic path (BokehDOF::bokeh_dof_compute()).
	const bool dof_far = ca->camera_attributes_get_dof_far_enabled(p_camera_attributes);
	const float far_begin = ca->camera_attributes_get_dof_far_distance(p_camera_attributes);
	const float far_size = ca->camera_attributes_get_dof_far_transition(p_camera_attributes);
	const bool dof_near = ca->camera_attributes_get_dof_near_enabled(p_camera_attributes);
	const float near_begin = ca->camera_attributes_get_dof_near_distance(p_camera_attributes);
	const float near_size = ca->camera_attributes_get_dof_near_transition(p_camera_attributes);
	const float bokeh_size = ca->camera_attributes_get_dof_blur_amount(p_camera_attributes) * 64.0f; // Base 64 pixel radius.

	PushConstant pc = {};
	pc.full_size[0] = p_color_size.width;
	pc.full_size[1] = p_color_size.height;
	pc.half_size[0] = half_size.width;
	pc.half_size[1] = half_size.height;
	pc.tile_count[0] = tiles.width;
	pc.tile_count[1] = tiles.height;
	pc.z_near = p_z_near;
	pc.z_far = p_z_far;
	pc.flags = (p_orthogonal ? FLAG_ORTHOGONAL : 0) | (dof_near ? FLAG_NEAR : 0) | (dof_far ? FLAG_FAR : 0) | (near_size < 0.0f ? FLAG_PHYSICAL_NEAR : 0) | (far_size < 0.0f ? FLAG_PHYSICAL_FAR : 0);
	pc.near_begin = near_begin;
	pc.near_end = near_begin - near_size;
	pc.far_begin = far_begin;
	pc.far_end = far_begin + far_size;
	pc.blur_size = (near_size < 0.0f && far_size < 0.0f) ? 32.0f : bokeh_size;
	pc.blur_size_near = bokeh_size;
	pc.blur_size_far = bokeh_size;
	// Largest blur radius, half pixels: as large as the classic path reaches (64 px x amount; physical capped at 32).
	pc.max_radius = CLAMP(MAX(pc.blur_size, (near_size < 0.0f || far_size < 0.0f) ? 32.0f : 0.0f) * 0.5f, 1.0f, 48.0f);
	pc.rings = rings_for_quality(int(ca->camera_attributes_get_dof_blur_quality()));
	pc.sides = sides_for_shape(int(ca->camera_attributes_get_dof_blur_bokeh_shape()));
	if (ca->camera_attributes_get_dof_blur_use_jitter()) {
		// The rings turn each frame (the temporal antialiasing averages them).
		RandomPCG rng(p_frame * 2654435761u + 1);
		pc.jitter = rng.randf();
	}

	const RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	auto shader_of = [&](Mode p_mode) {
		return shader.version_get_shader(shader_version, p_mode);
	};
	auto dispatch = [&](RD::ComputeListID p_list, Mode p_mode, RID p_set, const Size2i &p_size) {
		rd->compute_list_bind_compute_pipeline(p_list, pipelines[p_mode].get_rid());
		rd->compute_list_bind_uniform_set(p_list, p_set, 0);
		rd->compute_list_set_push_constant(p_list, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(p_list, p_size.width, p_size.height, 1);
		rd->compute_list_add_barrier(p_list);
	};

	RD::ComputeListID list = rd->compute_list_begin();
	{
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_color }));
		RD::Uniform u_half(RD::UNIFORM_TYPE_IMAGE, 2, half);
		dispatch(list, MODE_SETUP, uniform_set_cache->get_cache(shader_of(MODE_SETUP), 0, u_depth, u_color, u_half), half_size);
	}
	{
		RD::Uniform u_half(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, half }));
		RD::Uniform u_tiles(RD::UNIFORM_TYPE_IMAGE, 1, tile_max);
		dispatch(list, MODE_TILES, uniform_set_cache->get_cache(shader_of(MODE_TILES), 0, u_half, u_tiles), tiles);
	}
	{
		RD::Uniform u_tiles(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, tile_max }));
		RD::Uniform u_dilated(RD::UNIFORM_TYPE_IMAGE, 1, dilated);
		dispatch(list, MODE_DILATE, uniform_set_cache->get_cache(shader_of(MODE_DILATE), 0, u_tiles, u_dilated), tiles);
	}
	{
		RD::Uniform u_half(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, half }));
		RD::Uniform u_dilated(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, dilated }));
		RD::Uniform u_bg(RD::UNIFORM_TYPE_IMAGE, 2, bg);
		RD::Uniform u_fg(RD::UNIFORM_TYPE_IMAGE, 3, fg);
		dispatch(list, MODE_GATHER, uniform_set_cache->get_cache(shader_of(MODE_GATHER), 0, u_half, u_dilated, u_bg, u_fg), half_size);
	}
	{
		RD::Uniform u_bg(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, bg }));
		RD::Uniform u_fg(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, fg }));
		RD::Uniform u_bg_f(RD::UNIFORM_TYPE_IMAGE, 2, bg_f);
		RD::Uniform u_fg_f(RD::UNIFORM_TYPE_IMAGE, 3, fg_f);
		dispatch(list, MODE_POSTFILTER, uniform_set_cache->get_cache(shader_of(MODE_POSTFILTER), 0, u_bg, u_fg, u_bg_f, u_fg_f), half_size);
	}
	{
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
		RD::Uniform u_bg(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear, bg_f }));
		RD::Uniform u_fg(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear, fg_f }));
		RD::Uniform u_color(RD::UNIFORM_TYPE_IMAGE, 3, p_color);
		dispatch(list, MODE_COMPOSITE, uniform_set_cache->get_cache(shader_of(MODE_COMPOSITE), 0, u_depth, u_bg, u_fg, u_color), p_color_size);
	}
	rd->compute_list_end();
}
