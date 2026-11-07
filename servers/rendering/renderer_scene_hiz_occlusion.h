/**************************************************************************/
/*  renderer_scene_hiz_occlusion.h                                        */
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

#include "servers/rendering/renderer_scene_occlusion_cull.h"

// Mariner E13: occlusion culling of all instances by the depth of earlier frames (Viewport.use_hiz_occlusion_culling).
//
// The renderer reduces the depth buffer of a frame to a small grid of distances (the farthest surface in each cell,
// "no surface" where any pixel of the cell sees the sky) and reads it back without waiting (it arrives some frames
// later). Before culling, the grid of the newest frame that arrived is reprojected into the current camera: every
// cell is moved as a small quad, at its distance, from the camera it was rendered with to the current one, and the
// cells of the target grid whose centers it covers take its distance (the nearest, where several cover them). Cells
// nothing covers (the newly revealed parts of the view) stay "no surface", so a revealed object is never culled by
// them. The result fills the stock CPU depth pyramid (RendererSceneOcclusionCull::HZBuffer) that the instance culling
// already tests bounding boxes against.
//
// Geometry moving with the camera (the ship the camera is on) would be moved the wrong way by a reprojection that
// assumes a static world. The viewport may set a reference frame (the ship's transform, each frame) and a radius:
// cells nearer than the radius move with the reference frame, the farther ones with the world.
class RendererSceneHiZOcclusion {
public:
	// One read back grid, as the renderer produced it.
	struct Snapshot {
		LocalVector<float> distances; // Row 0 at the bottom of the view (as HZBuffer), meters from the camera; >= NO_SURFACE: none.
		Size2i size;
		Transform3D cam_transform; // The camera of that frame (world).
		Projection projection; // Its projection (as RendererSceneCull culls with).
		Transform3D reference; // The reference frame of that frame (world), see above.
		float reference_radius = 0.0; // 0: everything moves with the world.
		uint64_t frame = 0; // RSG::rasterizer->get_frame_number() of that frame.
		bool valid = false;
	};

	static constexpr float NO_SURFACE = 1e30f;

	class Buffer : public RendererSceneOcclusionCull::HZBuffer {
		LocalVector<Vector3> corner_dirs; // Unit view directions of the cell corners of the snapshot camera ((w + 1) x (h + 1)).
		Projection corner_projection;
		Size2i corner_size;
		LocalVector<Vector3> corner_view; // Per frame: the corner directions in the current view (world, reference frame).
		LocalVector<Vector4> corner_clip; // And in clip space.
		LocalVector<float> chunk_targets; // Targets of the worker threads but the first.

		static constexpr int MAX_CHUNKS = 16;
		struct SplatData {
			const float *src = nullptr;
			int w = 0;
			int h = 0;
			int corner_count = 0;
			int chunks = 1;
			bool use_reference = false;
			float radius = 0.0;
			float z_near = 0.0;
			Vector3 origin_view[2];
			Vector4 origin_clip[2];
			Basis basis[2];
			Projection projection;
			int frames = 1;
			float *targets[MAX_CHUNKS] = {};
			mutable uint32_t splats[MAX_CHUNKS] = {};
		};
		void _corner_chunk(uint32_t p_chunk, const SplatData *p_data);
		void _splat_chunk(uint32_t p_chunk, const SplatData *p_data);

	public:
		uint32_t last_splats = 0; // Cells moved in the last reprojection (statistics, tests).
		uint32_t last_covered = 0; // Target cells with a surface after it.

		// Fills the pyramid from p_snapshot as seen from the current camera. p_reference / p_reference_radius: the
		// reference frame now (radius 0: none). Returns false (and leaves the buffer empty) if there is nothing usable.
		bool reproject(const Snapshot &p_snapshot, const Transform3D &p_cam_transform, const Projection &p_projection, const Transform3D &p_reference, float p_reference_radius);
	};

	// The state kept per viewport, in its RenderSceneBuffers.
	struct State {
		// Set by the viewport before each frame.
		bool enabled = false;
		int buffer_width = 256; // Width of the grid (the height follows the aspect of the view).
		Transform3D reference;
		float reference_radius = 0.0;
		// Set by the renderer when a read back arrives.
		Snapshot snapshot;
		// Built by RendererSceneCull before culling the frame.
		Buffer buffer;
		bool ready = false; // The buffer holds this frame's reprojection.
	};

	static constexpr uint64_t MAX_SNAPSHOT_AGE = 8; // Frames; older snapshots are not used.
};
