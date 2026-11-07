/**************************************************************************/
/*  cas.cpp                                                               */
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

#include "cas.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

CAS::CAS() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline.setup(shader.version_get_shader(shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
}

CAS::~CAS() {
	pipeline.clear();
	shader.version_free(shader_version);
}

float CAS::peak_for_amount(float p_amount) {
	float amount = CLAMP(p_amount, 0.0f, 1.0f);
	return -amount / Math::lerp(8.0f, 5.0f, amount);
}

void CAS::process(RenderSceneBuffersRD *p_buffers, RID p_target, RID p_target_framebuffer, const Size2i &p_size, float p_amount, float p_max_value) {
	ERR_FAIL_NULL(p_buffers);
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	// The render target is read around each pixel while it is written: sharpen from a copy.
	const RD::DataFormat format = rd->texture_get_format(p_target).format;
	const StringName context = SNAME("Mariner_cas");
	const StringName name = SNAME("source");
	if (p_buffers->has_texture(context, name)) {
		RD::TextureFormat tf = p_buffers->get_texture_format(context, name);
		if (tf.format != format || int(tf.width) != p_size.x || int(tf.height) != p_size.y) {
			p_buffers->clear_context(context); // Output format or size changed (HDR toggled, resized target).
		}
	}
	RID source = p_buffers->has_texture(context, name) ? p_buffers->get_texture(context, name) : p_buffers->create_texture(context, name, format, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT, RD::TEXTURE_SAMPLES_1, p_size, 1, 1, true);
	rd->texture_copy(p_target, source, Vector3(), Vector3(), Vector3(p_size.x, p_size.y, 1), 0, 0, 0, 0);

	Params params;
	params.size[0] = p_size.x;
	params.size[1] = p_size.y;
	params.peak = peak_for_amount(p_amount);
	params.inv_max_value = 1.0f / MAX(p_max_value, 1.0f);

	RID shader_rid = shader.version_get_shader(shader_version, 0);
	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, source }));

	RD::DrawListID draw_list = rd->draw_list_begin(p_target_framebuffer);
	rd->draw_list_bind_render_pipeline(draw_list, pipeline.get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_target_framebuffer), false, rd->draw_list_get_current_pass()));
	rd->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader_rid, 0, u_source), 0);
	rd->draw_list_set_push_constant(draw_list, &params, sizeof(Params));
	rd->draw_list_draw(draw_list, false, 1u, 3u);
	rd->draw_list_end();
}
