/**************************************************************************/
/*  hiz_pyramid.cpp                                                       */
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

#include "hiz_pyramid.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

HiZPyramid *HiZPyramid::singleton = nullptr;

HiZPyramid::HiZPyramid() {
	singleton = this;
	Vector<String> modes;
	modes.push_back("\n#define MODE_FROM_DEPTH\n");
	modes.push_back("\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

HiZPyramid::~HiZPyramid() {
	shader.version_free(shader_version);
	singleton = nullptr;
}

void HiZPyramid::process(RID p_depth, const Vector<RID> &p_levels, const Vector<Size2i> &p_sizes) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	ERR_FAIL_COND(p_levels.is_empty() || p_levels.size() != p_sizes.size());

	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	for (int i = 0; i < p_levels.size(); i++) {
		const Mode mode = i == 0 ? MODE_FROM_DEPTH : MODE_REDUCE;
		const RID shader_rd = shader.version_get_shader(shader_version, mode);
		ERR_BREAK(shader_rd.is_null());
		PushConstant push_constant = {};
		const Size2i source = i == 0 ? p_sizes[0] : p_sizes[i - 1];
		push_constant.source_size[0] = source.width;
		push_constant.source_size[1] = source.height;
		push_constant.dest_size[0] = p_sizes[i].width;
		push_constant.dest_size[1] = p_sizes[i].height;

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[mode]);
		if (i == 0) {
			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_depth }));
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_depth), 0);
		} else {
			RD::Uniform u_source(RD::UNIFORM_TYPE_IMAGE, 0, p_levels[i - 1]);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_source), 0);
		}
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_levels[i]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_sizes[i].width, p_sizes[i].height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}
	RD::get_singleton()->compute_list_end();
}
