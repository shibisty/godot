/**************************************************************************/
/*  texture_streamer.h                                                    */
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

#include "core/io/image.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"

// Mariner E17: texture mip streaming.
//
// A streamed CompressedTexture2D starts with only its small mips in memory. Every frame the streamer reads the
// screen demand that the scene cull measured for the texture, picks the mip level to keep, fits all streamed
// textures into the memory budget by coarsening them together, and reads the missing levels from the .ctex file on
// worker threads. Finished reads replace the texture data under the same RID, so materials keep working unchanged.
class TextureStreamer {
public:
	struct Settings {
		uint64_t budget_bytes = 1024ull << 20;
		float mip_bias = 0.0f;
		float drop_delay = 3.0f;
		int max_loads_in_flight = 4;
	};

	struct Info {
		String path;
		int width = 0;
		int height = 0;
		Image::Format format = Image::FORMAT_L8;
		int mip_count = 1; // Levels in the file, the base level included.
		int lowest = 0; // Level that always stays resident.
		int size_width = 0; // Size the texture reports (the size override of the file header).
		int size_height = 0;
	};

	// Pure helpers, shared with the tests.
	// Bytes of the mip chain starting at the given level.
	static uint64_t chain_bytes(int p_width, int p_height, Image::Format p_format, int p_first_mip);
	// Smallest extra bias that fits the wanted levels into the budget; levels are clamped to each texture's lowest level.
	static int fit_budget(const LocalVector<int> &p_wanted, const LocalVector<int> &p_lowest, const LocalVector<const uint64_t *> &p_chain_bytes, uint64_t p_budget, LocalVector<int> &r_target);
	// Mip level whose texels match the demand (screen pixels per UV unit) of a texture with the given largest side.
	static int wanted_mip(float p_demand, int p_size, int p_mip_count, float p_bias);

private:
	static TextureStreamer *singleton;
	static bool tick_connected;

	struct Job {
		String path;
		int mip = 0;
		Ref<Image> image;
	};

	struct Entry {
		Info info;
		LocalVector<uint64_t> chain; // Bytes resident when the given level is the top one.
		int resident = 0;
		int target = 0;
		float demand = 0.0f;
		double last_visible = -1.0;
		double drop_since = -1.0;
		Job *job = nullptr;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};

	BinaryMutex mutex;
	HashMap<RID, Entry> entries;
	double time = 0.0;
	uint64_t last_tick_usec = 0;

	static void _job_run(void *p_job);
	static void _tick();
	void _apply(RID p_texture, Entry &p_entry);
	void _start_job(RID p_texture, Entry &p_entry, int p_mip);

public:
	static Settings get_project_settings();
	static bool is_enabled();
	// Whether a texture with these properties should stream under the current project settings.
	static bool should_stream(int p_width, int p_height, int p_mip_count, int &r_lowest);

	static void register_texture(RID p_texture, const Info &p_info, int p_resident);
	static void unregister_texture(RID p_texture);
	static int get_resident_mip(RID p_texture);
	static uint64_t get_resident_bytes();
	static int get_texture_count();

	// One streaming step; p_delta is the time since the previous step. With p_wait set the reads it starts finish
	// before it returns and are applied right away (used by tests that run without a main loop).
	static void update(double p_delta, const Settings &p_settings, bool p_wait = false);

	// Reads the headers of a .ctex file; fails for data formats other than DATA_FORMAT_IMAGE.
	static Error probe(const String &p_path, Info &r_info);
	// Reads the mip chain of a .ctex file from the given level on (DATA_FORMAT_IMAGE only).
	static Ref<Image> load_mips(const String &p_path, int p_first_mip);
};
