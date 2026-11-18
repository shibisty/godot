/**************************************************************************/
/*  cluster_streaming.cpp                                                 */
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


#include "cluster_streaming.h"

#include "core/object/callable_mp.h"
#include "core/templates/hash_map.h"

using namespace RendererRD;

ClusterStreaming *ClusterStreaming::singleton = nullptr;

namespace {
uint32_t _lowest_bit(uint32_t p_value) {
	uint32_t bit = 0;
	while (!(p_value & 1u)) {
		p_value >>= 1u;
		bit++;
	}
	return bit;
}

// A DAG group, by its LOD sphere and error (what its consumed clusters carry as parent, its produced ones as self).
struct GroupKey {
	uint32_t words[5] = {};
	bool operator==(const GroupKey &p_other) const { return memcmp(words, p_other.words, sizeof(words)) == 0; }
};
struct GroupKeyHasher {
	static uint32_t hash(const GroupKey &p_key) { return hash_murmur3_buffer(p_key.words, sizeof(p_key.words)); }
};
GroupKey group_key(const uint8_t *p_sphere, const uint8_t *p_error) {
	GroupKey key;
	memcpy(key.words, p_sphere, 16);
	memcpy(key.words + 4, p_error, 4);
	return key;
}
} // namespace

uint32_t ClusterStreaming::_alloc(uint32_t p_size) {
	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		FreeRange &range = free_ranges[i];
		if (range.size >= p_size) {
			const uint32_t offset = range.offset;
			range.offset += p_size;
			range.size -= p_size;
			if (range.size == 0) {
				free_ranges.remove_at(i);
			}
			pool_used += p_size;
			return offset;
		}
	}
	return NO_PAGE;
}

void ClusterStreaming::_free(uint32_t p_offset, uint32_t p_size) {
	pool_used -= p_size;
	uint32_t at = 0;
	while (at < free_ranges.size() && free_ranges[at].offset < p_offset) {
		at++;
	}
	free_ranges.insert(at, { p_offset, p_size });
	// Merge with the neighbours.
	if (at + 1 < free_ranges.size() && free_ranges[at].offset + free_ranges[at].size == free_ranges[at + 1].offset) {
		free_ranges[at].size += free_ranges[at + 1].size;
		free_ranges.remove_at(at + 1);
	}
	if (at > 0 && free_ranges[at - 1].offset + free_ranges[at - 1].size == free_ranges[at].offset) {
		free_ranges[at - 1].size += free_ranges[at].size;
		free_ranges.remove_at(at);
	}
}

void ClusterStreaming::_grow_pages(uint32_t p_count) {
	if (p_count <= page_capacity) {
		return;
	}
	RD *rd = RD::get_singleton();
	uint32_t capacity = MAX(page_capacity * 2u, 1024u);
	while (capacity < p_count) {
		capacity *= 2u;
	}
	page_capacity = capacity; // A multiple of 32 (bit words).
	const uint32_t old_size = residency_cpu.size();
	residency_cpu.resize(page_capacity);
	for (uint32_t i = old_size; i < page_capacity; i++) {
		residency_cpu[i] = NO_PAGE;
	}
	if (residency.is_valid()) {
		rd->free_rid(residency);
		rd->free_rid(feedback);
	}
	residency = rd->storage_buffer_create(page_capacity * sizeof(uint32_t));
	Vector<uint8_t> zero;
	zero.resize_initialized(page_capacity / 32 * 2 * sizeof(uint32_t));
	feedback = rd->storage_buffer_create(zero.size(), zero);
	residency_dirty = true;
}

uint32_t ClusterStreaming::surface_register(const Vector<uint8_t> &p_clusters, const Vector<uint8_t> &p_indices, RID &r_stream_buffer) {
	const uint32_t cluster_count = p_clusters.size() / CLUSTER_SIZE;
	const uint32_t index_count = p_indices.size() / 4;
	if (!is_enabled() || cluster_count == 0) {
		return NO_PAGE;
	}
	const uint8_t *records = p_clusters.ptr();
	const uint32_t *indices = reinterpret_cast<const uint32_t *>(p_indices.ptr());

	// Local pages: 0 the roots, then one per consuming group.
	HashMap<GroupKey, uint32_t, GroupKeyHasher> consumer_pages;
	LocalVector<uint32_t> own;
	own.resize(cluster_count);
	uint32_t local_pages = 1;
	for (uint32_t c = 0; c < cluster_count; c++) {
		const uint8_t *r = records + c * CLUSTER_SIZE;
		float parent_error;
		memcpy(&parent_error, r + 52, 4);
		if (parent_error >= 3.0e38f) {
			own[c] = 0;
			continue;
		}
		const GroupKey key = group_key(r + 32, r + 52);
		const uint32_t *found = consumer_pages.getptr(key);
		if (found) {
			own[c] = *found;
		} else {
			own[c] = local_pages;
			consumer_pages[key] = local_pages++;
		}
	}
	// The page refining each cluster (the group that produced it consumes it), and what each page needs: the pages of
	// its group's produced clusters.
	LocalVector<uint32_t> refining;
	refining.resize(cluster_count);
	LocalVector<LocalVector<uint32_t>> needs;
	needs.resize(local_pages);
	for (uint32_t c = 0; c < cluster_count; c++) {
		const uint8_t *r = records + c * CLUSTER_SIZE;
		float self_error;
		memcpy(&self_error, r + 48, 4);
		refining[c] = NO_PAGE;
		if (self_error <= 0.0f) {
			continue; // Full detail: nothing finer.
		}
		const uint32_t *found = consumer_pages.getptr(group_key(r + 16, r + 48));
		if (!found) {
			continue;
		}
		refining[c] = *found;
		LocalVector<uint32_t> &n = needs[*found];
		if (own[c] != *found && n.find(own[c]) < 0) {
			n.push_back(own[c]);
		}
	}

	// The page data: each page's clusters' indices together.
	Surface surface;
	LocalVector<uint32_t> page_offset;
	page_offset.resize(local_pages);
	LocalVector<uint32_t> page_count;
	page_count.resize(local_pages);
	LocalVector<uint32_t> local_offset;
	local_offset.resize(cluster_count);
	for (uint32_t p = 0; p < local_pages; p++) {
		page_count[p] = 0;
	}
	for (uint32_t c = 0; c < cluster_count; c++) {
		uint32_t count;
		memcpy(&count, records + c * CLUSTER_SIZE + 60, 4);
		page_count[own[c]] += count;
	}
	uint32_t total = 0;
	for (uint32_t p = 0; p < local_pages; p++) {
		page_offset[p] = total;
		total += page_count[p];
	}
	surface.page_data.resize(total);
	LocalVector<uint32_t> fill;
	fill.resize(local_pages);
	for (uint32_t p = 0; p < local_pages; p++) {
		fill[p] = 0;
	}
	for (uint32_t c = 0; c < cluster_count; c++) {
		uint32_t offset;
		uint32_t count;
		memcpy(&offset, records + c * CLUSTER_SIZE + 56, 4);
		memcpy(&count, records + c * CLUSTER_SIZE + 60, 4);
		ERR_FAIL_COND_V(offset + count > index_count, NO_PAGE);
		const uint32_t p = own[c];
		local_offset[c] = fill[p];
		memcpy(surface.page_data.ptr() + page_offset[p] + fill[p], indices + offset, count * sizeof(uint32_t));
		fill[p] += count;
	}

	const uint32_t surface_id = surfaces.size();
	const uint32_t base = pages.size();
	_grow_pages(base + local_pages);
	for (uint32_t p = 0; p < local_pages; p++) {
		Page page;
		page.surface = surface_id;
		page.data_offset = page_offset[p];
		page.index_count = page_count[p];
		page.root = p == 0;
		for (uint32_t n : needs[p]) {
			page.needs.push_back(base + n);
		}
		pages.push_back(page);
		surface.pages.push_back(base + p);
	}

	// The roots stay resident.
	uint64_t budget = UINT64_MAX;
	surfaces.push_back(surface);
	if (pages[base].index_count == 0 || !_load(base, last_update_frame, budget)) {
		ERR_PRINT_ONCE("Cluster streaming: no room for a surface's root clusters in the pool; the surface is not streamed.");
		surface_unregister(surface_id);
		return NO_PAGE;
	}

	LocalVector<uint32_t> stream;
	stream.resize(cluster_count * 4);
	for (uint32_t c = 0; c < cluster_count; c++) {
		stream[c * 4 + 0] = base + own[c];
		stream[c * 4 + 1] = refining[c] == NO_PAGE ? NO_PAGE : base + refining[c];
		stream[c * 4 + 2] = local_offset[c];
		stream[c * 4 + 3] = 0;
	}
	Vector<uint8_t> stream_bytes;
	stream_bytes.resize(stream.size() * sizeof(uint32_t));
	memcpy(stream_bytes.ptrw(), stream.ptr(), stream_bytes.size());
	surfaces[surface_id].stream_buffer = RD::get_singleton()->storage_buffer_create(stream_bytes.size(), stream_bytes);
	r_stream_buffer = surfaces[surface_id].stream_buffer;
	return surface_id;
}

void ClusterStreaming::surface_unregister(uint32_t p_surface) {
	ERR_FAIL_UNSIGNED_INDEX(p_surface, surfaces.size());
	Surface &surface = surfaces[p_surface];
	if (!surface.alive) {
		return;
	}
	for (uint32_t p : surface.pages) {
		Page &page = pages[p];
		if (page.pool_offset != NO_PAGE) {
			_free(page.pool_offset, page.index_count);
			page.pool_offset = NO_PAGE;
			residency_cpu[p] = NO_PAGE;
		}
		page.alive = false;
		page.needs.clear();
	}
	residency_dirty = true;
	if (surface.stream_buffer.is_valid()) {
		RD::get_singleton()->free_rid(surface.stream_buffer);
	}
	surface.stream_buffer = RID();
	surface.page_data.reset();
	surface.alive = false;
}

bool ClusterStreaming::_load(uint32_t p_page, uint64_t p_frame, uint64_t &r_budget, uint32_t p_depth) {
	Page &page = pages[p_page];
	if (!page.alive) {
		return false;
	}
	if (page.pool_offset != NO_PAGE) {
		page.last_used = MAX(page.last_used, p_frame);
		return true;
	}
	ERR_FAIL_COND_V(p_depth > 64, false);
	for (uint32_t need : page.needs) {
		if (!_load(need, p_frame, r_budget, p_depth + 1)) {
			return false;
		}
	}
	const uint64_t bytes = uint64_t(page.index_count) * sizeof(uint32_t);
	if (bytes > r_budget) {
		return false;
	}
	uint32_t offset = _alloc(page.index_count);
	if (offset == NO_PAGE && _make_room(page.index_count, p_frame)) {
		offset = _alloc(page.index_count);
	}
	if (offset == NO_PAGE) {
		loads_failed++;
		return false;
	}
	const Surface &surface = surfaces[page.surface];
	RD::get_singleton()->buffer_update(pool, offset * sizeof(uint32_t), bytes, surface.page_data.ptr() + page.data_offset);
	page.pool_offset = offset;
	page.last_used = p_frame;
	residency_cpu[p_page] = offset;
	residency_dirty = true;
	for (uint32_t need : page.needs) {
		pages[need].resident_dependents++;
	}
	r_budget -= bytes;
	pages_loaded++;
	return true;
}

void ClusterStreaming::_evict(uint32_t p_page) {
	Page &page = pages[p_page];
	_free(page.pool_offset, page.index_count);
	page.pool_offset = NO_PAGE;
	residency_cpu[p_page] = NO_PAGE;
	residency_dirty = true;
	for (uint32_t need : page.needs) {
		pages[need].resident_dependents--;
	}
	pages_evicted++;
}

bool ClusterStreaming::_make_room(uint32_t p_size, uint64_t p_frame) {
	// Evict the least recently used pages that no resident page needs and that were not used in the last frames,
	// until a free range is large enough.
	while (true) {
		for (const FreeRange &range : free_ranges) {
			if (range.size >= p_size) {
				return true;
			}
		}
		uint32_t victim = NO_PAGE;
		uint64_t oldest = UINT64_MAX;
		for (uint32_t p = 0; p < pages.size(); p++) {
			const Page &page = pages[p];
			if (page.pool_offset == NO_PAGE || page.root || page.resident_dependents > 0 || page.last_used + 2 >= p_frame) {
				continue;
			}
			if (page.last_used < oldest) {
				oldest = page.last_used;
				victim = p;
			}
		}
		if (victim == NO_PAGE) {
			return false;
		}
		_evict(victim);
	}
}

void ClusterStreaming::_feedback_received(const PackedByteArray &p_data) {
	if (!singleton) {
		return;
	}
	singleton->received = p_data;
	singleton->received_new = true;
	singleton->readback_pending = false;
}

void ClusterStreaming::update(uint64_t p_frame) {
	if (!is_enabled() || p_frame == last_update_frame) {
		return;
	}
	last_update_frame = p_frame;
	RD *rd = RD::get_singleton();

	LocalVector<uint32_t> requested;
	if (received_new) {
		received_new = false;
		const uint32_t words = MIN(uint32_t(received.size() / 8), page_capacity / 32);
		const uint32_t *bits = reinterpret_cast<const uint32_t *>(received.ptr());
		for (uint32_t w = 0; w < words; w++) {
			uint32_t used = bits[words + w];
			while (used) {
				const uint32_t b = _lowest_bit(used);
				used &= used - 1;
				const uint32_t p = w * 32 + b;
				if (p < pages.size()) {
					pages[p].last_used = p_frame;
				}
			}
			uint32_t request = bits[w];
			while (request) {
				const uint32_t b = _lowest_bit(request);
				request &= request - 1;
				const uint32_t p = w * 32 + b;
				if (p < pages.size() && pages[p].alive && pages[p].pool_offset == NO_PAGE) {
					requested.push_back(p);
				}
			}
		}
		requests_last = requested.size();
	}
	// Up to 8 MB of pages a frame.
	uint64_t budget = 8u << 20;
	for (uint32_t p : requested) {
		if (!_load(p, p_frame, budget)) {
			if (budget < (64u << 10)) {
				break;
			}
		}
	}
	if (residency_dirty && residency.is_valid()) {
		rd->buffer_update(residency, 0, page_capacity * sizeof(uint32_t), residency_cpu.ptr());
		residency_dirty = false;
	}
	if (!readback_pending && feedback.is_valid()) {
		// This frame's selection writes the marks after this: read back the last frame's, then clear them.
		readback_pending = true;
		rd->buffer_get_data_async(feedback, callable_mp_static(&ClusterStreaming::_feedback_received));
		rd->buffer_clear(feedback, 0, page_capacity / 32 * 2 * sizeof(uint32_t));
	}
}

Dictionary ClusterStreaming::get_stats() const {
	Dictionary stats;
	uint32_t resident = 0;
	uint32_t alive = 0;
	for (const Page &page : pages) {
		alive += page.alive ? 1 : 0;
		resident += page.pool_offset != NO_PAGE ? 1 : 0;
	}
	stats["enabled"] = is_enabled();
	stats["pool_indices"] = pool_capacity;
	stats["pool_used_indices"] = pool_used;
	stats["pages"] = alive;
	stats["resident_pages"] = resident;
	stats["pages_loaded"] = pages_loaded;
	stats["pages_evicted"] = pages_evicted;
	stats["requests"] = requests_last;
	stats["loads_failed"] = loads_failed;
	return stats;
}

ClusterStreaming::ClusterStreaming(uint32_t p_pool_mb) {
	singleton = this;
	if (p_pool_mb == 0) {
		return;
	}
	pool_capacity = uint32_t(MIN(uint64_t(p_pool_mb), uint64_t(2047)) * 1024 * 1024 / sizeof(uint32_t));
	pool = RD::get_singleton()->storage_buffer_create(pool_capacity * sizeof(uint32_t));
	free_ranges.push_back({ 0, pool_capacity });
	_grow_pages(1024);
}

ClusterStreaming::~ClusterStreaming() {
	RD *rd = RD::get_singleton();
	for (Surface &surface : surfaces) {
		if (surface.stream_buffer.is_valid()) {
			rd->free_rid(surface.stream_buffer);
		}
	}
	for (RID rid : { pool, residency, feedback }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	singleton = nullptr;
}
