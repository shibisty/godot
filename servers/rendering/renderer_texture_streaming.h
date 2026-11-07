/**************************************************************************/
/*  renderer_texture_streaming.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
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

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"

// Mariner E17: screen demand of streamed textures.
//
// After culling a camera view, the scene cull reports every visible mesh surface with the number of screen
// pixels that one unit of its UV channel covers at the nearest point of the instance bounds. The demand of
// a material is the largest value among its surfaces; each texture of the material inherits it. The texture
// streamer on the main thread reads the demand back and turns it into the mip level to keep resident.
class RendererTextureStreaming {
	static RendererTextureStreaming *singleton;

	struct Entry {
		float demand = 0.0f;
		uint64_t frame = 0;
		bool seen = false;
	};

	mutable BinaryMutex mutex;
	HashMap<RID, Entry> tracked;
	SafeNumeric<uint32_t> tracked_count;
	uint64_t last_frame = 0;

	// Render thread only: material demand of the view being collected.
	HashMap<RID, float> view_materials;
	LocalVector<RID> view_textures;

public:
	static RendererTextureStreaming *get_singleton() { return singleton; }

	// Screen pixels covered by one world unit at the given distance (vertical axis of the projection).
	static float pixels_per_unit(const Projection &p_projection, float p_view_height, float p_distance);
	// Distance from a point to the nearest point of a box, 0 inside.
	static float distance_to_aabb(const Vector3 &p_point, const AABB &p_aabb);

	void track(RID p_texture, bool p_enable);
	bool is_active() const { return tracked_count.get() > 0; }

	// Render thread: one view.
	void begin_view();
	void add_material(RID p_material, float p_demand);
	template <typename F>
	void end_view(uint64_t p_frame, F &&p_get_textures) {
		MutexLock lock(mutex);
		// The frame counts even when nothing streamed is in view: that is how textures age out of it.
		last_frame = MAX(last_frame, p_frame);
		for (const KeyValue<RID, float> &E : view_materials) {
			view_textures.clear();
			float scale = 1.0f;
			p_get_textures(E.key, view_textures, scale);
			for (const RID &texture : view_textures) {
				HashMap<RID, Entry>::Iterator T = tracked.find(texture);
				if (!T) {
					continue;
				}
				const float demand = E.value * scale;
				if (T->value.frame != p_frame || !T->value.seen) {
					T->value.demand = demand;
				} else {
					T->value.demand = MAX(T->value.demand, demand);
				}
				T->value.frame = p_frame;
				T->value.seen = true;
			}
		}
		view_materials.clear();
	}

	// Main thread: x = demand in screen pixels per UV unit, y = frames since the texture was last seen (-1 if never).
	void get_feedback(const RID *p_textures, uint32_t p_count, Vector2 *r_feedback) const;

	RendererTextureStreaming();
	~RendererTextureStreaming();
};
