/**************************************************************************/
/*  lens_flare.cpp                                                        */
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

#include "lens_flare.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

LensFlare::LensFlare() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	for (RID &buffer : params_buffers) {
		buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	}
	sun_buffer = RD::get_singleton()->storage_buffer_create(sizeof(float) * 4);
}

LensFlare::~LensFlare() {
	for (RID &buffer : params_buffers) {
		RD::get_singleton()->free_rid(buffer);
	}
	RD::get_singleton()->free_rid(sun_buffer);
	shader.version_free(shader_version);
}

void LensFlare::process(RenderSceneBuffersRD *p_buffers, RID p_color, const Size2i &p_color_size, RID p_depth, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	ERR_FAIL_NULL(texture_storage);
	RD *rd = RD::get_singleton();

	const Size2i quarter(MAX(1, p_color_size.width / 4), MAX(1, p_color_size.height / 4));
	const StringName scope = SNAME("mariner_lens_flare");
	const StringName n_a = SNAME("a");
	const StringName n_b = SNAME("b");
	if (p_buffers->has_texture(scope, n_a) && (p_buffers->get_texture_format(scope, n_a).width != uint32_t(quarter.width) || p_buffers->get_texture_format(scope, n_a).height != uint32_t(quarter.height))) {
		p_buffers->clear_context(scope);
	}
	if (!p_buffers->has_texture(scope, n_a)) {
		const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_buffers->create_texture(scope, n_a, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, quarter);
		p_buffers->create_texture(scope, n_b, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, quarter);
	}
	const RID tex_a = p_buffers->get_texture(scope, n_a);
	const RID tex_b = p_buffers->get_texture(scope, n_b);

	Params params = {};
	params.source_size[0] = p_color_size.width;
	params.source_size[1] = p_color_size.height;
	params.sun_uv[0] = p_settings.sun_uv.x;
	params.sun_uv[1] = p_settings.sun_uv.y;
	params.sun_in_front = p_settings.sun && p_settings.sun_in_front ? 1.0 : 0.0;
	params.sun_radius = MAX(p_settings.sun_radius, 0.004f);
	params.sun_color[0] = p_settings.sun_color.r;
	params.sun_color[1] = p_settings.sun_color.g;
	params.sun_color[2] = p_settings.sun_color.b;
	params.intensity = p_settings.intensity;
	params.threshold = p_settings.threshold;
	params.ghost_spacing = p_settings.ghost_spacing;
	params.halo_width = p_settings.halo_width;
	params.chromatic = p_settings.chromatic;
	params.dirt_intensity = p_settings.dirt_intensity;
	params.aspect = float(p_color_size.width) / float(MAX(1, p_color_size.height));
	params.streak_count = p_settings.streak_count;
	params.ghosts = uint32_t(CLAMP(p_settings.ghosts, 0, 8));

	const RID shader_rd = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rd.is_null());
	const RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID dirt = p_settings.dirt_texture;
	params.has_dirt = dirt.is_valid() ? 1 : 0;
	if (dirt.is_null()) {
		dirt = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_dirt(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ linear, dirt }));
	RD::Uniform u_sun(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, sun_buffer);
	const RID black = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	struct Pass {
		uint32_t mode;
		Size2i size;
		RID bright; // Binding 2.
		RID flare; // Binding 3.
		RID dest; // Binding 5.
		bool color_as_source; // Binding 0: the color, else a placeholder.
	};
	const Pass passes[6] = {
		{ 0, quarter, black, black, tex_a, true }, // Bright pass: color -> a.
		{ 1, quarter, tex_a, black, tex_b, false }, // Ghosts and halo: a -> b.
		{ 2, quarter, black, tex_b, tex_a, false }, // Blur: b -> a.
		{ 3, quarter, black, tex_a, tex_b, false }, // Blur: a -> b.
		{ 4, Size2i(1, 1), black, black, tex_a, false }, // Sun visibility (writes the buffer only).
		{ 5, p_color_size, black, tex_b, p_color, false }, // Composite: b -> color.
	};
	for (uint32_t i = 0; i < 6; i++) {
		const Pass &pass = passes[i];
		params.mode = pass.mode;
		params.dest_size[0] = pass.size.width;
		params.dest_size[1] = pass.size.height;
		rd->buffer_update(params_buffers[i], 0, sizeof(Params), &params);
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear, pass.color_as_source ? p_color : black }));
		RD::Uniform u_bright(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear, pass.bright }));
		RD::Uniform u_flare(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear, pass.flare }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 5, pass.dest);
		RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, params_buffers[i]);
		const RID uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_source, u_depth, u_bright, u_flare, u_dirt, u_dest, u_sun, u_params);
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_dispatch_threads(compute_list, pass.size.width, pass.size.height, 1);
		rd->compute_list_end();
	}
}
