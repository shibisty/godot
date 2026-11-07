/**************************************************************************/
/*  renderer_scene_hiz_occlusion.cpp                                      */
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

#include "renderer_scene_hiz_occlusion.h"

#include "core/object/worker_thread_pool.h"

void RendererSceneHiZOcclusion::Buffer::_corner_chunk(uint32_t p_chunk, const SplatData *p_data) {
	const int row = p_data->w + 1;
	const int rows = p_data->h + 1;
	const int corner_count = row * rows;
	const int from = rows * p_chunk / p_data->chunks * row;
	const int to = rows * (p_chunk + 1) / p_data->chunks * row;
	for (int f = 0; f < p_data->frames; f++) {
		const Basis &basis = p_data->basis[f];
		for (int i = from; i < to; i++) {
			const Vector3 r = basis.xform(corner_dirs[i]);
			corner_view[f * corner_count + i] = r;
			corner_clip[f * corner_count + i] = p_data->projection.xform(Vector4(r.x, r.y, r.z, 0.0f));
		}
	}
}

void RendererSceneHiZOcclusion::Buffer::_splat_chunk(uint32_t p_chunk, const SplatData *p_data) {
	const int w = p_data->w;
	const int h = p_data->h;
	const int y_from = h * p_chunk / p_data->chunks;
	const int y_to = h * (p_chunk + 1) / p_data->chunks;
	float *dst = p_data->targets[p_chunk];
	if (p_chunk > 0) {
		for (int i = 0; i < w * h; i++) {
			dst[i] = FLT_MAX;
		}
	}
	const float *src = p_data->src;
	const int corner_count = p_data->corner_count;
	const int corner_offsets[4] = { 0, 1, w + 1, w + 2 };
	uint32_t splats = 0;

	for (int y = y_from; y < y_to; y++) {
		for (int x = 0; x < w; x++) {
			const float d = src[y * w + x];
			if (!(d > 0.0f) || d >= NO_SURFACE * 0.5f) {
				continue; // Sky (or a cell with sky in it): no occluder.
			}
			const int f = (p_data->use_reference && d < p_data->radius) ? 1 : 0;
			const Vector3 *views = &corner_view[f * corner_count + y * (w + 1) + x];
			const Vector4 *clips = &corner_clip[f * corner_count + y * (w + 1) + x];
			const Vector3 &origin_view = p_data->origin_view[f];
			const Vector4 &origin_clip = p_data->origin_clip[f];
			float min_u = FLT_MAX;
			float min_v = FLT_MAX;
			float max_u = -FLT_MAX;
			float max_v = -FLT_MAX;
			float far_distance_sq = 0.0f;
			bool in_front = true;
			for (int c = 0; c < 4; c++) {
				const Vector3 p = views[corner_offsets[c]] * d + origin_view;
				if (-p.z < p_data->z_near) {
					in_front = false; // Crosses the near plane: its extent on screen is unknown, skip it (conservative).
					break;
				}
				const Vector4 clip = clips[corner_offsets[c]] * d + origin_clip;
				const float inv_w = 1.0f / clip.w;
				const float u = clip.x * inv_w * 0.5f + 0.5f;
				const float v = clip.y * inv_w * 0.5f + 0.5f;
				min_u = MIN(min_u, u);
				max_u = MAX(max_u, u);
				min_v = MIN(min_v, v);
				max_v = MAX(max_v, v);
				far_distance_sq = MAX(far_distance_sq, p.length_squared());
			}
			if (!in_front) {
				continue;
			}
			// The target cells whose centers the moved cell covers.
			const int x0 = MAX(0, int(Math::ceil(min_u * w - 0.5f)));
			const int x1 = MIN(w - 1, int(Math::floor(max_u * w - 0.5f)));
			const int y0 = MAX(0, int(Math::ceil(min_v * h - 0.5f)));
			const int y1 = MIN(h - 1, int(Math::floor(max_v * h - 0.5f)));
			if (x0 > x1 || y0 > y1) {
				continue;
			}
			splats++;
			const float far_distance = Math::sqrt(far_distance_sq);
			for (int ty = y0; ty <= y1; ty++) {
				float *row = &dst[ty * w];
				for (int tx = x0; tx <= x1; tx++) {
					row[tx] = MIN(row[tx], far_distance);
				}
			}
		}
	}
	p_data->splats[p_chunk] = splats;
}

bool RendererSceneHiZOcclusion::Buffer::reproject(const Snapshot &p_snapshot, const Transform3D &p_cam_transform, const Projection &p_projection, const Transform3D &p_reference, float p_reference_radius) {
	last_splats = 0;
	last_covered = 0;
	const Size2i size = p_snapshot.size;
	if (!p_snapshot.valid || size.x <= 0 || size.y <= 0 || p_snapshot.distances.size() != uint32_t(size.x * size.y) || p_projection.is_orthogonal() || p_snapshot.projection.is_orthogonal()) {
		clear();
		return false;
	}
	resize(size);

	const int w = size.x;
	const int h = size.y;
	// Unit view directions of the cell corners of the snapshot's camera (row 0 at the bottom, as the pyramid).
	if (corner_size != size || !(corner_projection == p_snapshot.projection)) {
		corner_size = size;
		corner_projection = p_snapshot.projection;
		corner_dirs.resize((w + 1) * (h + 1));
		const Projection inv = p_snapshot.projection.inverse();
		for (int y = 0; y <= h; y++) {
			for (int x = 0; x <= w; x++) {
				const Vector3 near_point = inv.xform(Vector3(2.0f * x / w - 1.0f, 2.0f * y / h - 1.0f, -1.0f));
				corner_dirs[y * (w + 1) + x] = near_point.normalized();
			}
		}
	}

	float *dst = mips[0];
	for (int i = 0; i < w * h; i++) {
		dst[i] = FLT_MAX;
	}

	const Transform3D view_inv = p_cam_transform.affine_inverse();
	const bool use_reference = p_reference_radius > 0.0f && p_snapshot.reference_radius > 0.0f;
	const Transform3D to_view[2] = {
		view_inv * p_snapshot.cam_transform, // Static world.
		view_inv * p_reference * p_snapshot.reference.affine_inverse() * p_snapshot.cam_transform, // With the reference frame.
	};
	// A corner at distance d of a cell is d * (R * dir) + t in the new view and d * (P * R * dir) + P * t in clip space:
	// transform the corner directions once per frame, not every corner of every cell.
	const int corner_count = (w + 1) * (h + 1);
	const int frames = use_reference ? 2 : 1;
	corner_view.resize(corner_count * frames);
	corner_clip.resize(corner_count * frames);
	Vector3 origin_view[2];
	Vector4 origin_clip[2];
	for (int f = 0; f < frames; f++) {
		origin_view[f] = to_view[f].origin;
		origin_clip[f] = p_projection.xform(Vector4(origin_view[f].x, origin_view[f].y, origin_view[f].z, 1.0f));
	}

	// Both passes run in bands of rows on the worker threads: the corners, then the cells, each band of cells into its
	// own target; then the nearest wins.
	SplatData splat;
	splat.basis[0] = to_view[0].basis;
	splat.basis[1] = to_view[1].basis;
	splat.projection = p_projection;
	splat.frames = frames;
	splat.src = p_snapshot.distances.ptr();
	splat.w = w;
	splat.h = h;
	splat.corner_count = corner_count;
	splat.use_reference = use_reference;
	splat.radius = p_snapshot.reference_radius;
	splat.z_near = p_projection.get_z_near();
	for (int f = 0; f < 2; f++) {
		splat.origin_view[f] = origin_view[f];
		splat.origin_clip[f] = origin_clip[f];
	}
	const int chunks = CLAMP(MIN(WorkerThreadPool::get_singleton()->get_thread_count(), h / 8), 1, MAX_CHUNKS);
	splat.chunks = chunks;
	chunk_targets.resize((chunks - 1) * w * h);
	for (int c = 0; c < chunks; c++) {
		splat.targets[c] = c == 0 ? dst : &chunk_targets[(c - 1) * w * h];
		splat.splats[c] = 0;
	}
	if (chunks > 1) {
		WorkerThreadPool::GroupID corners = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Buffer::_corner_chunk, &splat, chunks, -1, true, SNAME("HiZOcclusionCorners"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(corners);
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Buffer::_splat_chunk, &splat, chunks, -1, true, SNAME("HiZOcclusionReproject"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		for (int c = 1; c < chunks; c++) {
			const float *other = splat.targets[c];
			for (int i = 0; i < w * h; i++) {
				dst[i] = MIN(dst[i], other[i]);
			}
		}
	} else {
		_corner_chunk(0, &splat);
		_splat_chunk(0, &splat);
	}
	for (int c = 0; c < chunks; c++) {
		last_splats += splat.splats[c];
	}

	for (int i = 0; i < w * h; i++) {
		if (dst[i] < FLT_MAX) {
			last_covered++;
		}
	}
	debug_tex_range = p_projection.get_z_far();
	update_mips();
	return true;
}
