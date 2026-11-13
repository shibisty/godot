/**************************************************************************/
/*  overhead_map.h                                                        */
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

#pragma once

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/overhead_map.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_device.h"

class RenderSceneBuffersRD;

namespace RendererRD {

// Mariner G1a: the overhead map (Environment.overhead_map_*) - the depth of the scene from above around the camera,
// as normalized heights with mipmaps (average and maximum), for the sky occlusion of the ambient light and the sky reflection in
// the scene shader (CryEngine HeightMapAO; own implementation). Forward+.
class OverheadMap {
public:
	// The uniform block of the scene shader (overhead_map_inc.glsl), std140.
	struct UBO {
		float box_min[4]; // xyz: the box's minimum (world), w: texel size (m).
		float box_size[4]; // x: size (m), y: height (m), z: 1 / size, w: 1 / height.
		float params[4]; // x: sky occlusion amount, y: radius (m), z: bias (m), w: active (1 / 0).
		float params2[4]; // x: mipmap count, yzw: unused.
	};
	static_assert(sizeof(UBO) == 64);

	enum Mode {
		MODE_COPY, // The depth (normalized height) into mipmap 0.
		MODE_DOWNSAMPLE, // Mipmap n + 1 from 2x2 of mipmap n: the average and the maximum.
		MODE_MAX
	};

	struct PushConstant {
		int32_t size[2]; // Of the destination.
		int32_t pad[2];
	};
	static_assert(sizeof(PushConstant) == 16);

	// Per viewport: the map's textures and uniform block (RenderSceneBuffersRD custom data).
	class Buffers : public RenderBufferCustomDataRD {
		GDCLASS(Buffers, RenderBufferCustomDataRD);

	public:
		int resolution = 0;
		RID depth; // D32, the depth pass target.
		RID framebuffer;
		RID height; // RG16 UNORM (average, maximum), mipmaps.
		Vector<RID> height_mips; // Views of single mipmaps.
		RID uniform_buffer;
		bool active = false;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;
		~Buffers() { free_data(); }
	};

private:
	OverheadMapShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];
	RID default_uniform_buffer; // Inactive.
	RID default_height; // 1x1, 0 (nothing above).

public:
	static const StringName &custom_data_name();

	OverheadMap();
	~OverheadMap();

	// The viewport's buffers for p_resolution (created or recreated as needed).
	Ref<Buffers> get_buffers(RenderSceneBuffersRD *p_render_buffers, int p_resolution);
	// After the depth pass into the buffers' framebuffer: the height mipmaps and the uniform block.
	void update(Ref<Buffers> p_buffers, const RendererSceneRender::OverheadMapData &p_data);

	// For the scene shader: the viewport's uniform block and heights, or inactive defaults.
	RID get_uniform_buffer(RenderSceneBuffersRD *p_render_buffers) const;
	RID get_height_texture(RenderSceneBuffersRD *p_render_buffers) const;
	void deactivate(RenderSceneBuffersRD *p_render_buffers);

	// The camera of the depth pass for the box (orthographic, looking down, the depth = normalized height).
	static void get_camera(const Vector3 &p_box_min, const Vector3 &p_box_max, Transform3D &r_transform, Projection &r_projection);
};

} // namespace RendererRD
