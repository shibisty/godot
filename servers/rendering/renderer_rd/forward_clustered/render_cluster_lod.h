/**************************************************************************/
/*  render_cluster_lod.h                                                  */
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

// Mariner V2: GPU cluster LOD of single meshes (ArrayMesh.build_cluster_lod(), V1) in Forward+. For each surface with a
// cluster DAG drawn by the camera of a scene render, a compute pass picks the cut of the DAG by screen error
// (screen_mesh_lod_threshold pixels, 0: full detail) and writes its indices into the surface's own index buffers: the
// camera list (clusters inside the view) and the shadow list (the whole cut; shadow passes draw it, so that the shadow
// matches the surface). The passes draw the surface as usual (its material, vertex buffers) with these indices and an
// indirect command. Stats of the render: the named texture "mariner_cluster_lod" / "stats" (R32UI 8x1: camera
// triangles, shadow triangles, camera clusters, shadow clusters of the camera's cut; then V3: shadow pass triangles,
// shadow pass clusters, shadow requests that did not fit, shadow requests).
//
// V3: each shadow pass picks its own cut of each surface it draws (shadow_add(), error below a texel of its shadow map,
// its side planes), all into one shared index buffer (shadow_process(), before the shadow passes are drawn).

#include "core/math/projection.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/cluster_select.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;

namespace RendererSceneRenderImplementation {

class RenderClusterLOD {
public:
	struct Cut {
		RID camera_buffer; // Index buffers (uint32, storage) and their index arrays.
		RID shadow_buffer;
		RID camera_array;
		RID shadow_array;
		RID commands; // 2 x 5 uint: camera, shadow indexed indirect draws.
		uint32_t capacity = 0; // Indices.
		uint64_t frame = 0; // Last frame used.
	};
	static constexpr uint32_t CAMERA_COMMAND_OFFSET = 0;
	static constexpr uint32_t SHADOW_COMMAND_OFFSET = 5 * sizeof(uint32_t);

private:
	enum Mode {
		MODE_CAMERA,
		MODE_SHADOW_COUNT,
		MODE_SHADOW_PREFIX,
		MODE_SHADOW_WRITE,
		MODE_MAX
	};
	ClusterSelectShaderRD shader;
	RID version;
	RID shader_rd[MODE_MAX];
	RID pipeline[MODE_MAX];
	// V3: the shadow requests of the frame's shadow passes.
	struct ShadowRequest {
		RID clusters;
		RID indices;
		RID stream[3]; // V4.
		uint8_t push[112];
		uint32_t cluster_count = 0;
	};
	LocalVector<ShadowRequest> shadow_requests;
	uint64_t shadow_index_total = 0; // Full detail indices of the requests (bound of the shared buffer's need).
	RID shared_buffer; // Indices of all shadow cuts.
	RID shared_array;
	uint32_t shared_capacity = 0;
	RID request_buffer; // 3 uint per request.
	RID shadow_commands; // 5 uint per request.
	uint32_t request_capacity = 0;
	bool initialized = false;
	HashMap<uint64_t, Cut> cuts; // By surface cache.
	RID stats; // The render's stats texture (when selecting).
	RID private_stats; // Renders without render buffers.
	RID dummy_buffer; // V4: the stream bindings of surfaces that are not streamed.
	// V4: the stream bindings (6: stream records, 7: residency, 8: feedback) and the push constant's word of a surface.
	uint32_t _stream_bindings(void *p_mesh_surface, RID r_bindings[3]);

	void _initialize();
	void _free_cut(Cut &r_cut);

public:
	// Starts the selection of a scene render (clears the render's stats); end() closes it.
	void begin(const RenderDataRD *p_render_data);
	// The cut of a surface (p_mesh_surface: the mesh storage's) of a geometry instance with p_transform; nullptr if the
	// surface has no cluster DAG.
	const Cut *select(uint64_t p_key, void *p_mesh_surface, const Transform3D &p_transform, const RenderDataRD *p_render_data);
	void end();

	// V3: a shadow pass's cut of a surface (p_mesh_surface) of a geometry instance with p_transform, for a pass with
	// p_projection from p_light_transform onto p_size texels. Returns the request (its indirect command at
	// request * SHADOW_REQUEST_STRIDE of get_shadow_commands()), -1 if the surface has no cluster DAG.
	int shadow_add(void *p_mesh_surface, const Transform3D &p_transform, const Projection &p_projection, const Transform3D &p_light_transform, const Size2i &p_size);
	// Selects the cuts of the requests added since the last call (before the shadow passes draw them).
	void shadow_process();
	RID get_shadow_index_array() const { return shared_array; }
	RID get_shadow_commands() const { return shadow_commands; }
	static constexpr uint32_t SHADOW_REQUEST_STRIDE = 5 * sizeof(uint32_t);
	// Frees the cuts not used for some frames (surfaces gone).
	void purge(uint64_t p_frame);

	~RenderClusterLOD();
};

} // namespace RendererSceneRenderImplementation
