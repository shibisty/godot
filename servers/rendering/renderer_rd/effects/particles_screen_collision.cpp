/**************************************************************************/
/*  particles_screen_collision.cpp                                        */
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

#pragma once

#include "particles_screen_collision.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

ParticlesScreenCollision::ParticlesScreenCollision() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

ParticlesScreenCollision::~ParticlesScreenCollision() {
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

void ParticlesScreenCollision::process(RID p_depth, const Size2i &p_size, const Projection &p_projection, const Basis &p_view_to_world, RID p_dest, const Size2i &p_dest_size) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	Params params = {};
	MaterialStorage::store_camera(p_projection.inverse(), params.inv_projection);
	MaterialStorage::store_transform(Transform3D(p_view_to_world, Vector3()), params.view_to_world);
	params.source_size[0] = p_size.width;
	params.source_size[1] = p_size.height;
	params.dest_size[0] = p_dest_size.width;
	params.dest_size[1] = p_dest_size.height;
	RD::get_singleton()->buffer_update(params_buffer, 0, sizeof(Params), &params);

	const RID shader_rd = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rd.is_null());
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, p_dest);
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, params_buffer);
	RID uniform_set = uniform_set_cache->get_cache(shader_rd, 0, u_depth, u_dest, u_params);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_dest_size.width, p_dest_size.height, 1);
	RD::get_singleton()->compute_list_end();
}
