/**************************************************************************/
/*  renderer_texture_streaming.cpp                                        */
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

#include "renderer_texture_streaming.h"

#include "core/math/vector2.h"

RendererTextureStreaming *RendererTextureStreaming::singleton = nullptr;

float RendererTextureStreaming::pixels_per_unit(const Projection &p_projection, float p_view_height, float p_distance) {
	// columns[1][1] maps a unit of view height to clip space (1 / tan(fov / 2) or 2 / size); half the view height is one clip unit.
	const float scale = Math::abs(p_projection.columns[1][1]) * 0.5f * p_view_height;
	if (p_projection.is_orthogonal()) {
		return scale;
	}
	return scale / MAX(p_distance, 0.01f);
}

float RendererTextureStreaming::distance_to_aabb(const Vector3 &p_point, const AABB &p_aabb) {
	const Vector3 end = p_aabb.position + p_aabb.size;
	const Vector3 nearest(CLAMP(p_point.x, p_aabb.position.x, end.x), CLAMP(p_point.y, p_aabb.position.y, end.y), CLAMP(p_point.z, p_aabb.position.z, end.z));
	return p_point.distance_to(nearest);
}

void RendererTextureStreaming::track(RID p_texture, bool p_enable) {
	MutexLock lock(mutex);
	if (p_enable) {
		if (!tracked.has(p_texture)) {
			tracked.insert(p_texture, Entry());
			tracked_count.increment();
		}
	} else if (tracked.erase(p_texture)) {
		tracked_count.decrement();
	}
}

void RendererTextureStreaming::begin_view() {
	view_materials.clear();
}

void RendererTextureStreaming::add_material(RID p_material, float p_demand) {
	if (p_material.is_null()) {
		return;
	}
	HashMap<RID, float>::Iterator E = view_materials.find(p_material);
	if (E) {
		E->value = MAX(E->value, p_demand);
	} else {
		view_materials.insert(p_material, p_demand);
	}
}

void RendererTextureStreaming::get_feedback(const RID *p_textures, uint32_t p_count, Vector2 *r_feedback) const {
	MutexLock lock(mutex);
	for (uint32_t i = 0; i < p_count; i++) {
		HashMap<RID, Entry>::ConstIterator E = tracked.find(p_textures[i]);
		if (!E || !E->value.seen) {
			r_feedback[i] = Vector2(0.0f, -1.0f);
		} else {
			r_feedback[i] = Vector2(E->value.demand, float(last_frame - E->value.frame));
		}
	}
}

RendererTextureStreaming::RendererTextureStreaming() {
	singleton = this;
}

RendererTextureStreaming::~RendererTextureStreaming() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
