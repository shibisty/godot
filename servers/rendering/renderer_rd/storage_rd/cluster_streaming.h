/**************************************************************************/
/*  cluster_streaming.h                                                   */
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

// Mariner V4: streaming of the cluster LOD (V1-V3) of surfaces from memory into one index pool on the GPU, within the
// budget of rendering/mesh_lod/cluster_streaming/pool_size_mb (0: off, every surface's clusters stay resident).
//
// A page is the set of clusters one DAG group consumes (replaces with its coarser clusters); the roots are one page per
// surface, always resident. A resident page needs the pages of its group's coarser clusters resident (they cover the
// same surface when the error allows): loading a page loads them first, a page is evicted only when no finer resident
// page needs it. On the GPU (cluster_select.glsl) a cluster is drawn when its page is resident and its error fits - or
// when it is too coarse but the page that refines it is not resident: then that page is requested. The selection marks
// the pages it drew (used) and requested; update() reads these marks back (asynchronously, a frame or two late), loads
// requested pages within a per frame budget and evicts the least recently used.

#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

class ClusterStreaming {
public:
	static constexpr uint32_t NO_PAGE = 0xFFFFFFFF;
	static constexpr uint32_t CLUSTER_SIZE = 64; // Bytes of a ClusterLOD record.

private:
	static ClusterStreaming *singleton;

	struct Page {
		uint32_t surface = 0;
		uint32_t data_offset = 0; // Into its surface's page data (indices).
		uint32_t index_count = 0;
		LocalVector<uint32_t> needs; // Coarser pages that must be resident with it.
		uint32_t resident_dependents = 0; // Resident finer pages needing it.
		uint32_t pool_offset = NO_PAGE;
		uint64_t last_used = 0;
		bool root = false;
		bool alive = true;
	};
	struct Surface {
		LocalVector<uint32_t> page_data; // The indices of its pages, page by page.
		LocalVector<uint32_t> pages; // Global page ids.
		RID stream_buffer; // Per cluster: uvec4(own page, refining page, offset in the own page, 0).
		bool alive = true;
	};

	LocalVector<Page> pages;
	LocalVector<Surface> surfaces;

	RID pool; // Indices of the resident pages.
	uint32_t pool_capacity = 0; // Indices.
	struct FreeRange {
		uint32_t offset;
		uint32_t size;
	};
	LocalVector<FreeRange> free_ranges; // Sorted by offset.
	uint32_t pool_used = 0;

	RID residency; // Per page: its pool offset, NO_PAGE if not resident.
	RID feedback; // Requested pages (bits), then used pages (bits).
	uint32_t page_capacity = 0;
	bool residency_dirty = false;
	LocalVector<uint32_t> residency_cpu;

	bool readback_pending = false;
	PackedByteArray received; // The latest feedback read back.
	bool received_new = false;
	uint64_t last_update_frame = 0;

	// Stats.
	uint64_t pages_loaded = 0;
	uint64_t pages_evicted = 0;
	uint32_t requests_last = 0;
	uint32_t loads_failed = 0;

	uint32_t _alloc(uint32_t p_size);
	void _free(uint32_t p_offset, uint32_t p_size);
	void _grow_pages(uint32_t p_count);
	bool _load(uint32_t p_page, uint64_t p_frame, uint64_t &r_budget, uint32_t p_depth = 0);
	void _evict(uint32_t p_page);
	bool _make_room(uint32_t p_size, uint64_t p_frame);
	static void _feedback_received(const PackedByteArray &p_data);

public:
	static ClusterStreaming *get_singleton() { return singleton; }

	bool is_enabled() const { return pool_capacity > 0; }
	// A surface's clusters (ClusterLOD records) and their indices: its handle (NO_PAGE if it can't be streamed - the
	// caller keeps it resident as before) and the per cluster stream records.
	uint32_t surface_register(const Vector<uint8_t> &p_clusters, const Vector<uint8_t> &p_indices, RID &r_stream_buffer);
	void surface_unregister(uint32_t p_surface);

	// Once a frame, before the cluster selection: feedback, loads, evictions, residency.
	void update(uint64_t p_frame);

	RID get_pool() const { return pool; }
	RID get_residency() const { return residency; }
	RID get_feedback() const { return feedback; }
	uint32_t get_feedback_words() const { return page_capacity / 32; }
	Dictionary get_stats() const;

	ClusterStreaming(uint32_t p_pool_mb);
	~ClusterStreaming();
};

} // namespace RendererRD
