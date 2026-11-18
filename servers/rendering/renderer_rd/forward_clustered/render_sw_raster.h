/**************************************************************************/
/*  render_sw_raster.h                                                    */
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

// Mariner V5: software rasterizer of small triangles (MultiMesh.software_raster) for Forward+. The multimeshes
// flagged for it are left out of the hardware passes of the camera; their triangles are rasterized by compute into
// the visibility buffer of the viewport (named textures "mariner_swraster": "depth" R32UI (reverse Z float bits, 0
// empty), "payload" RGBA32UI (object << 24 | instance (objects from 1), triangle, vertex colour RGBA8, bit 0: negative
// binormal), "attributes" RGBA32UI (normal, tangent: octahedral snorm16 x 2 in object space; UV as float bits),
// "stats" R32UI 4x1 (triangles, pixels, too large, crossing the near plane)). A material with render_mode
// visibility_resolve shades it (V5-T2): its surface is drawn as one full screen triangle in the depth prepass and the
// opaque pass, the fragment function gets VERTEX, NORMAL, TANGENT, UV, COLOR of the pixel from these textures.
// Mariner V5a: the depth of orthogonal shadow passes (directional cascades, the object map E10b) - the instances of a
// pass's software bucket (at most MAX_TEXELS_SHADOW texels across in it) into a scratch R32UI, merged into the shadow
// map after the pass's hardware draws.

#include "core/math/projection.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/sw_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/sw_raster_clear.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/sw_raster_shadow.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderSoftwareRaster {
public:
	struct Item {
		RID multimesh;
		Transform3D transform; // The multimesh node's.
		uint64_t owner = 0; // The geometry instance.
		// Mariner V5-T3: the instances from the camera's cull (the software bucket): data, its first instance, room;
		// the instance count at command_word + 1 of command_buffer. Else the multimesh's own.
		RID source_buffer;
		uint32_t source_offset = 0;
		uint32_t source_capacity = 0;
		RID command_buffer;
		uint32_t command_word = 0;
	};

private:
	enum {
		MODE_DEPTH,
		MODE_PAYLOAD,
		MODE_SHADOW, // Mariner V5a.
		MODE_MAX,
	};
	SwRasterShaderRD shader;
	RID version;
	RID shader_rd[MODE_MAX];
	RID pipeline[MODE_MAX];
	RID dummy_buffer; // Commands of a non indirect multimesh.
	// The screen rectangle of each object (see sw_raster.glsl), for the resolve draws: the viewport's own buffer (render()),
	// as the visibility buffer - one shared by the viewports of a frame raced the next viewport's clear on the GPU.
	RID bounds_buffer;
	bool initialized = false;
	LocalVector<Item> items;

	// Mariner V5a: shadow passes.
	SwRasterShadowShaderRD shadow_merge_shader;
	RID shadow_merge_version;
	PipelineCacheRD shadow_merge_pipeline;
	SwRasterClearShaderRD shadow_clear_shader;
	RID shadow_clear_version;
	RID shadow_clear_pipeline;
	RID shadow_scratch; // R32UI, grown to the largest pass.
	Size2i shadow_scratch_size;
	RID shadow_stats; // R32UI 4x1: the counters when the render has no buffers to keep them in.
	uint64_t shadow_stats_pass = UINT64_MAX; // The scene pass whose counters were cleared last.

	void _initialize();
	// Dispatches the triangles of an item (one invocation each) into p_depth (MODE_DEPTH / MODE_PAYLOAD: the visibility
	// buffer; MODE_SHADOW: the scratch, p_size its used part). False if the item can't be drawn.
	bool _dispatch(RD::ComputeListID p_list, int p_mode, const Item &p_item, uint32_t p_object_id, const Projection &p_view_projection, const Size2i &p_size, uint32_t p_extra_flags, RID p_depth, RID p_payload, RID p_stats, RID p_attributes);

public:
	enum {
		MAX_OBJECTS = 127, // Object ids 1-127: 7 bits of the instance flags of the resolve draw (8 in the payload).
		MAX_PIXELS_SOFTWARE = 32, // V5-T3: instances at most this many pixels across (bounding sphere) are drawn here.
		MAX_TEXELS_SHADOW = 32, // V5a: the same in the texels of an orthogonal shadow pass.
		BOUNDS_BUFFER_SIZE = (MAX_OBJECTS + 1) * 4 * 4, // Bytes of a bounds buffer (render()).
	};

	void clear() { items.clear(); }
	// Returns the object id of the multimesh in the payload (1 and up), 0 when it is not drawn.
	uint32_t add(RID p_multimesh, const Transform3D &p_transform, uint64_t p_owner = 0);
	const Item &get_item(uint32_t p_index) const { return items[p_index]; }
	void set_item_source(uint32_t p_index, RID p_buffer, uint32_t p_offset, uint32_t p_capacity, RID p_commands, uint32_t p_command_word);
	bool has_items() const { return !items.is_empty(); }
	RID get_bounds_buffer() const { return bounds_buffer; }
	uint32_t get_item_count() const { return items.size(); }
	// Rasterizes the items of the frame into the visibility buffer of the viewport (cleared first); p_bounds: the
	// viewport's bounds buffer (BOUNDS_BUFFER_SIZE bytes; cleared first).
	void render(const RenderDataRD *p_render_data, RID p_bounds);

	// Mariner V5a: rasterizes p_items (their sources: the pass's software bucket) with p_view_projection (clip from
	// world, depth corrected as the pass's) into the scratch (p_size: the pass's rectangle), then merges it into the
	// shadow map p_framebuffer at p_rect with the depth test. The counters (triangles, texels, too large, near) are
	// summed into p_stats (R32UI 4x1, cleared at the first pass of scene pass p_scene_pass; RID(): not kept).
	void render_shadow(const LocalVector<Item> &p_items, const Projection &p_view_projection, bool p_pancake, RID p_framebuffer, const Rect2i &p_rect, RID p_stats, uint64_t p_scene_pass);

	~RenderSoftwareRaster();
};

} // namespace RendererSceneRenderImplementation
