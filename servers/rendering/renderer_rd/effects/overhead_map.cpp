/**************************************************************************/
/*  overhead_map.cpp                                                      */
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

#include "overhead_map.h"

#include "core/io/image.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

void OverheadMap::Buffers::free_data() {
	RD *rd = RD::get_singleton();
	if (!rd) {
		return;
	}
	for (const RID &mip : height_mips) {
		if (mip.is_valid() && rd->texture_is_valid(mip)) {
			rd->free_rid(mip);
		}
	}
	height_mips.clear();
	if (height.is_valid()) {
		rd->free_rid(height);
		height = RID();
	}
	framebuffer = RID(); // From the framebuffer cache: freed with the depth texture.
	if (depth.is_valid()) {
		rd->free_rid(depth);
		depth = RID();
	}
	if (uniform_buffer.is_valid()) {
		rd->free_rid(uniform_buffer);
		uniform_buffer = RID();
	}
	resolution = 0;
	active = false;
}

const StringName &OverheadMap::custom_data_name() {
	static const StringName name = SNAME("mariner_overhead_map");
	return name;
}

OverheadMap::OverheadMap() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_COPY\n");
	modes.push_back("\n#define MODE_DOWNSAMPLE\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}

	RD *rd = RD::get_singleton();
	UBO ubo = {};
	default_uniform_buffer = rd->uniform_buffer_create(sizeof(UBO), Span<uint8_t>((uint8_t *)&ubo, sizeof(UBO)));

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16_UNORM;
	tf.width = 1;
	tf.height = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> zero;
	zero.resize(4);
	zero.fill(0);
	default_height = rd->texture_create(tf, RD::TextureView(), Vector<Vector<uint8_t>>({ zero }));
}

OverheadMap::~OverheadMap() {
	RD *rd = RD::get_singleton();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	shader.version_free(shader_version);
	rd->free_rid(default_uniform_buffer);
	rd->free_rid(default_height);
}

void OverheadMap::get_camera(const Vector3 &p_box_min, const Vector3 &p_box_max, Transform3D &r_transform, Projection &r_projection) {
	// As the height field of GPUParticlesCollisionHeightField3D: texture x along +X, y along +Z, depth 1 at the top.
	const Vector3 extents = (p_box_max - p_box_min) * 0.5f;
	const Vector3 center = (p_box_max + p_box_min) * 0.5f;
	r_projection.set_orthogonal(-extents.x, extents.x, -extents.z, extents.z, 0, extents.y * 2.0f);
	const Vector3 cam_pos = center + Vector3(0, extents.y, 0);
	r_transform = Transform3D();
	r_transform.set_look_at(cam_pos, cam_pos - Vector3(0, 1, 0), Vector3(0, 0, -1));
}

Ref<OverheadMap::Buffers> OverheadMap::get_buffers(RenderSceneBuffersRD *p_render_buffers, int p_resolution) {
	Ref<Buffers> b;
	if (p_render_buffers->has_custom_data(custom_data_name())) {
		b = p_render_buffers->get_custom_data(custom_data_name());
	}
	if (b.is_null()) {
		b.instantiate();
		p_render_buffers->set_custom_data(custom_data_name(), b);
	}
	if (b->resolution == p_resolution && b->depth.is_valid()) {
		return b;
	}
	b->free_data();

	RD *rd = RD::get_singleton();
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_D32_SFLOAT;
	tf.width = p_resolution;
	tf.height = p_resolution;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	b->depth = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(b->depth, "Mariner overhead map depth");
	b->framebuffer = FramebufferCacheRD::get_singleton()->get_cache(b->depth);

	const uint32_t mips = Image::get_image_required_mipmaps(p_resolution, p_resolution, Image::FORMAT_L8) + 1;
	tf.format = RD::DATA_FORMAT_R16G16_UNORM;
	tf.mipmaps = mips;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	b->height = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(b->height, "Mariner overhead map heights");
	for (uint32_t i = 0; i < mips; i++) {
		b->height_mips.push_back(rd->texture_create_shared_from_slice(RD::TextureView(), b->height, 0, i));
	}

	UBO ubo = {};
	b->uniform_buffer = rd->uniform_buffer_create(sizeof(UBO), Span<uint8_t>((uint8_t *)&ubo, sizeof(UBO)));
	b->resolution = p_resolution;
	return b;
}

void OverheadMap::update(Ref<Buffers> p_buffers, const RendererSceneRender::OverheadMapData &p_data) {
	ERR_FAIL_COND(p_buffers.is_null() || p_buffers->depth.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	const RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::get_singleton()->draw_command_begin_label("Overhead Map Heights");
	RD::ComputeListID list = rd->compute_list_begin();
	PushConstant pc = {};
	const int mips = p_buffers->height_mips.size();
	for (int i = 0; i < mips; i++) {
		const int size = MAX(p_buffers->resolution >> i, 1);
		pc.size[0] = size;
		pc.size[1] = size;
		RID set;
		if (i == 0) {
			RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_buffers->depth }));
			RD::Uniform u_dst(RD::UNIFORM_TYPE_IMAGE, 1, p_buffers->height_mips[0]);
			set = uniform_set_cache->get_cache(shader.version_get_shader(shader_version, MODE_COPY), 0, u_src, u_dst);
		} else {
			RD::Uniform u_src(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->height_mips[i - 1]);
			RD::Uniform u_dst(RD::UNIFORM_TYPE_IMAGE, 1, p_buffers->height_mips[i]);
			set = uniform_set_cache->get_cache(shader.version_get_shader(shader_version, MODE_DOWNSAMPLE), 0, u_src, u_dst);
		}
		rd->compute_list_bind_compute_pipeline(list, pipelines[i == 0 ? MODE_COPY : MODE_DOWNSAMPLE].get_rid());
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(list, size, size, 1);
		rd->compute_list_add_barrier(list);
	}
	rd->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	const RendererEnvironmentStorage::OverheadMapSettings &s = p_data.settings;
	const Vector3 size = p_data.box_max - p_data.box_min;
	UBO ubo = {};
	ubo.box_min[0] = p_data.box_min.x;
	ubo.box_min[1] = p_data.box_min.y;
	ubo.box_min[2] = p_data.box_min.z;
	ubo.box_min[3] = size.x / float(p_buffers->resolution);
	ubo.box_size[0] = size.x;
	ubo.box_size[1] = size.y;
	ubo.box_size[2] = 1.0f / size.x;
	ubo.box_size[3] = 1.0f / size.y;
	ubo.params[0] = s.sky_occlusion;
	ubo.params[1] = s.sky_occlusion_radius;
	ubo.params[2] = s.bias;
	ubo.params[3] = 1.0f;
	ubo.params2[0] = float(mips);
	rd->buffer_update(p_buffers->uniform_buffer, 0, sizeof(UBO), &ubo);
	p_buffers->active = true;
}

void OverheadMap::deactivate(RenderSceneBuffersRD *p_render_buffers) {
	if (p_render_buffers && p_render_buffers->has_custom_data(custom_data_name())) {
		Ref<Buffers> b = p_render_buffers->get_custom_data(custom_data_name());
		if (b.is_valid() && b->depth.is_valid()) {
			b->free_data(); // Disabled: free the textures.
		}
	}
}

RID OverheadMap::get_uniform_buffer(RenderSceneBuffersRD *p_render_buffers) const {
	if (p_render_buffers && p_render_buffers->has_custom_data(custom_data_name())) {
		Ref<Buffers> b = p_render_buffers->get_custom_data(custom_data_name());
		if (b.is_valid() && b->active && b->uniform_buffer.is_valid()) {
			return b->uniform_buffer;
		}
	}
	return default_uniform_buffer;
}

RID OverheadMap::get_height_texture(RenderSceneBuffersRD *p_render_buffers) const {
	if (p_render_buffers && p_render_buffers->has_custom_data(custom_data_name())) {
		Ref<Buffers> b = p_render_buffers->get_custom_data(custom_data_name());
		if (b.is_valid() && b->active && b->height.is_valid()) {
			return b->height;
		}
	}
	return default_height;
}
