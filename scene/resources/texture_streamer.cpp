/**************************************************************************/
/*  texture_streamer.cpp                                                  */
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

#include "texture_streamer.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/resources/compressed_texture.h"
#include "servers/rendering/rendering_server.h"

TextureStreamer *TextureStreamer::singleton = nullptr;
bool TextureStreamer::tick_connected = false;

static constexpr uint32_t CTEX_HEADER_SIZE = 36; // "GST2", version, width, height, flags, mip limit, 3 reserved.
static constexpr uint32_t CTEX_IMAGE_HEADER_SIZE = 16; // Data format, width, height (16 bit each), mip count, image format.

uint64_t TextureStreamer::chain_bytes(int p_width, int p_height, Image::Format p_format, int p_first_mip) {
	const int64_t total = Image::get_image_data_size(p_width, p_height, p_format, true);
	const int64_t offset = Image::get_image_mipmap_offset(p_width, p_height, p_format, p_first_mip);
	return uint64_t(MAX(total - offset, int64_t(0)));
}

int TextureStreamer::fit_budget(const LocalVector<int> &p_wanted, const LocalVector<int> &p_lowest, const LocalVector<const uint64_t *> &p_chain_bytes, uint64_t p_budget, LocalVector<int> &r_target) {
	const uint32_t count = p_wanted.size();
	r_target.resize(count);
	int max_extra = 0;
	for (uint32_t i = 0; i < count; i++) {
		max_extra = MAX(max_extra, p_lowest[i] - p_wanted[i]);
	}
	// Coarsening every texture by the same number of levels keeps their relative sharpness, as a global mip bias would.
	for (int extra = 0; extra <= max_extra; extra++) {
		uint64_t total = 0;
		for (uint32_t i = 0; i < count; i++) {
			r_target[i] = MIN(p_wanted[i] + extra, p_lowest[i]);
			total += p_chain_bytes[i][r_target[i]];
		}
		if (total <= p_budget) {
			return extra;
		}
	}
	return max_extra; // Even the lowest levels exceed the budget; they stay anyway.
}

int TextureStreamer::wanted_mip(float p_demand, int p_size, int p_mip_count, float p_bias) {
	const int last = MAX(p_mip_count - 1, 0);
	if (p_demand <= 0.0f) {
		return last;
	}
	// Texels per screen pixel along the largest side; each level halves it. Rounding down keeps the sharper level.
	const float level = Math::log2(MAX(float(p_size) / p_demand, 1e-6f)) + p_bias;
	return CLAMP(int(Math::floor(level)), 0, last);
}

TextureStreamer::Settings TextureStreamer::get_project_settings() {
	Settings settings;
	settings.budget_bytes = uint64_t(MAX(int(GLOBAL_GET("rendering/textures/streaming/budget_mb")), 1)) << 20;
	settings.mip_bias = GLOBAL_GET("rendering/textures/streaming/mip_bias");
	settings.drop_delay = GLOBAL_GET("rendering/textures/streaming/drop_delay");
	settings.max_loads_in_flight = MAX(int(GLOBAL_GET("rendering/textures/streaming/max_loads_in_flight")), 1);
	return settings;
}

bool TextureStreamer::is_enabled() {
	return !Engine::get_singleton()->is_editor_hint() && GLOBAL_GET("rendering/textures/streaming/enabled").booleanize();
}

bool TextureStreamer::should_stream(int p_width, int p_height, int p_mip_count, int &r_lowest) {
	if (!is_enabled()) {
		return false;
	}
	const int size = MAX(p_width, p_height);
	const int min_size = GLOBAL_GET("rendering/textures/streaming/min_texture_size");
	if (p_mip_count < 2 || size < min_size) {
		return false;
	}
	const int resident_size = MAX(int(GLOBAL_GET("rendering/textures/streaming/resident_size")), 1);
	int lowest = 0;
	while (lowest < p_mip_count - 1 && (size >> (lowest + 1)) >= resident_size) {
		lowest++;
	}
	r_lowest = lowest;
	return lowest > 0;
}

Error TextureStreamer::probe(const String &p_path, Info &r_info) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return ERR_CANT_OPEN;
	}
	uint8_t magic[4];
	f->get_buffer(magic, 4);
	if (magic[0] != 'G' || magic[1] != 'S' || magic[2] != 'T' || magic[3] != '2') {
		return ERR_FILE_CORRUPT;
	}
	if (f->get_32() > CompressedTexture2D::FORMAT_VERSION) {
		return ERR_FILE_CORRUPT;
	}
	r_info.size_width = f->get_32();
	r_info.size_height = f->get_32();
	f->seek(CTEX_HEADER_SIZE);
	const uint32_t data_format = f->get_32();
	if (data_format != CompressedTexture2D::DATA_FORMAT_IMAGE) {
		return ERR_UNAVAILABLE;
	}
	r_info.width = f->get_16();
	r_info.height = f->get_16();
	const uint32_t mipmaps = f->get_32();
	const uint32_t format = f->get_32();
	if (format >= Image::FORMAT_MAX || r_info.width <= 0 || r_info.height <= 0) {
		return ERR_FILE_CORRUPT;
	}
	r_info.format = Image::Format(format);
	r_info.mip_count = int(mipmaps) + 1;
	r_info.path = p_path;
	const uint64_t expected = CTEX_HEADER_SIZE + CTEX_IMAGE_HEADER_SIZE + Image::get_image_data_size(r_info.width, r_info.height, r_info.format, mipmaps > 0);
	if (f->get_length() < expected) {
		return ERR_FILE_CORRUPT;
	}
	return OK;
}

Ref<Image> TextureStreamer::load_mips(const String &p_path, int p_first_mip) {
	Info info;
	ERR_FAIL_COND_V(probe(p_path, info) != OK, Ref<Image>());
	const int first = CLAMP(p_first_mip, 0, info.mip_count - 1);
	int w = 0;
	int h = 0;
	const int64_t offset = Image::get_image_mipmap_offset_and_dimensions(info.width, info.height, info.format, first, w, h);
	const bool mipmaps = first < info.mip_count - 1;
	const int64_t size = Image::get_image_data_size(w, h, info.format, mipmaps);
	const int64_t available = Image::get_image_data_size(info.width, info.height, info.format, info.mip_count > 1) - offset;
	ERR_FAIL_COND_V_MSG(size > available, Ref<Image>(), vformat("Mip chain of %s does not match its header.", p_path));

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V(f.is_null(), Ref<Image>());
	f->seek(CTEX_HEADER_SIZE + CTEX_IMAGE_HEADER_SIZE + offset);
	Vector<uint8_t> data;
	data.resize(size);
	ERR_FAIL_COND_V(f->get_buffer(data.ptrw(), size) != uint64_t(size), Ref<Image>());
	return Image::create_from_data(w, h, mipmaps, info.format, data);
}

void TextureStreamer::register_texture(RID p_texture, const Info &p_info, int p_resident) {
	ERR_FAIL_COND(p_texture.is_null());
	if (!singleton) {
		singleton = memnew(TextureStreamer);
	}
	{
		MutexLock lock(singleton->mutex);
		Entry entry;
		entry.info = p_info;
		entry.chain.resize(p_info.mip_count);
		for (int i = 0; i < p_info.mip_count; i++) {
			entry.chain[i] = chain_bytes(p_info.width, p_info.height, p_info.format, i);
		}
		entry.resident = p_resident;
		entry.target = p_resident;
		singleton->entries.insert(p_texture, entry);
	}
	RS::get_singleton()->texture_streaming_track(p_texture, true);
	if (!tick_connected && RS::get_singleton()->has_signal("frame_pre_draw")) {
		tick_connected = true;
		RS::get_singleton()->connect("frame_pre_draw", callable_mp_static(&TextureStreamer::_tick));
	}
}

void TextureStreamer::unregister_texture(RID p_texture) {
	if (!singleton) {
		return;
	}
	Job *job = nullptr;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	bool empty = false;
	{
		MutexLock lock(singleton->mutex);
		HashMap<RID, Entry>::Iterator E = singleton->entries.find(p_texture);
		if (!E) {
			return;
		}
		job = E->value.job;
		task = E->value.task;
		singleton->entries.remove(E);
		empty = singleton->entries.is_empty();
	}
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
	}
	if (job) {
		memdelete(job);
	}
	if (RS::get_singleton()) {
		RS::get_singleton()->texture_streaming_track(p_texture, false);
	}
	if (empty && Thread::is_main_thread()) {
		// Only the main thread runs the per-frame step, so only it may free the streamer under it.
		memdelete(singleton);
		singleton = nullptr;
	}
}

int TextureStreamer::get_resident_mip(RID p_texture) {
	if (!singleton) {
		return -1;
	}
	MutexLock lock(singleton->mutex);
	HashMap<RID, Entry>::ConstIterator E = singleton->entries.find(p_texture);
	return E ? E->value.resident : -1;
}

uint64_t TextureStreamer::get_resident_bytes() {
	if (!singleton) {
		return 0;
	}
	MutexLock lock(singleton->mutex);
	uint64_t total = 0;
	for (const KeyValue<RID, Entry> &E : singleton->entries) {
		total += E.value.chain[E.value.resident];
	}
	return total;
}

int TextureStreamer::get_texture_count() {
	if (!singleton) {
		return 0;
	}
	MutexLock lock(singleton->mutex);
	return singleton->entries.size();
}

void TextureStreamer::_job_run(void *p_job) {
	Job *job = static_cast<Job *>(p_job);
	job->image = load_mips(job->path, job->mip);
}

void TextureStreamer::_start_job(RID p_texture, Entry &p_entry, int p_mip) {
	p_entry.job = memnew(Job);
	p_entry.job->path = p_entry.info.path;
	p_entry.job->mip = p_mip;
	p_entry.task = WorkerThreadPool::get_singleton()->add_native_task(&TextureStreamer::_job_run, p_entry.job, false, SNAME("TextureStreaming"));
}

void TextureStreamer::_apply(RID p_texture, Entry &p_entry) {
	Job *job = p_entry.job;
	WorkerThreadPool::get_singleton()->wait_for_task_completion(p_entry.task);
	p_entry.job = nullptr;
	p_entry.task = WorkerThreadPool::INVALID_TASK_ID;
	if (job->image.is_valid() && !job->image->is_empty()) {
		RenderingServer *rs = RS::get_singleton();
		RID by_texture = rs->texture_2d_create(job->image);
		rs->texture_replace(p_texture, by_texture);
		// The replacement takes the size and path of the new data; the texture keeps reporting the size of the file.
		rs->texture_set_size_override(p_texture, p_entry.info.size_width, p_entry.info.size_height);
		rs->texture_set_path(p_texture, p_entry.info.path);
		p_entry.resident = job->mip;
	}
	memdelete(job);
}

void TextureStreamer::_tick() {
	if (!singleton) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const double delta = singleton->last_tick_usec ? double(now - singleton->last_tick_usec) / 1000000.0 : 0.0;
	singleton->last_tick_usec = now;
	update(delta, get_project_settings());
}

void TextureStreamer::update(double p_delta, const Settings &p_settings, bool p_wait) {
	if (!singleton) {
		return;
	}
	MutexLock lock(singleton->mutex);
	TextureStreamer &s = *singleton;
	s.time += p_delta;
	const uint32_t count = s.entries.size();
	if (count == 0) {
		return;
	}

	LocalVector<RID> rids;
	LocalVector<Entry *> list;
	rids.reserve(count);
	list.reserve(count);
	for (KeyValue<RID, Entry> &E : s.entries) {
		rids.push_back(E.key);
		list.push_back(&E.value);
	}

	// Finished reads first, so this step plans from the levels actually resident.
	int in_flight = 0;
	for (uint32_t i = 0; i < count; i++) {
		Entry &e = *list[i];
		if (e.job && (p_wait || WorkerThreadPool::get_singleton()->is_task_completed(e.task))) {
			s._apply(rids[i], e);
		}
		in_flight += e.job ? 1 : 0;
	}

	LocalVector<Vector2> feedback;
	feedback.resize(count);
	RS::get_singleton()->texture_streaming_get_feedback(rids.ptr(), count, feedback.ptr());

	LocalVector<int> wanted;
	LocalVector<int> lowest;
	LocalVector<uint8_t> visible;
	visible.resize(count);
	LocalVector<const uint64_t *> chains;
	wanted.resize(count);
	lowest.resize(count);
	chains.resize(count);
	uint64_t resident_bytes = 0;
	for (uint32_t i = 0; i < count; i++) {
		Entry &e = *list[i];
		const float age = feedback[i].y;
		if (age >= 0.0f && age < 1.5f) {
			e.last_visible = s.time;
			e.demand = feedback[i].x;
		}
		const bool recent = e.last_visible >= 0.0 && s.time - e.last_visible <= p_settings.drop_delay;
		visible[i] = recent ? 1 : 0;
		const int size = MAX(e.info.width, e.info.height);
		wanted[i] = recent ? MIN(wanted_mip(e.demand, size, e.info.mip_count, p_settings.mip_bias), e.info.lowest) : e.info.lowest;
		lowest[i] = e.info.lowest;
		chains[i] = e.chain.ptr();
		resident_bytes += e.chain[e.resident];
	}

	LocalVector<int> target;
	fit_budget(wanted, lowest, chains, p_settings.budget_bytes, target);
	const bool over_budget = resident_bytes > p_settings.budget_bytes;

	// Sharper levels first where the gap is largest, then where the screen demand is highest.
	struct Candidate {
		uint32_t index;
		int gap;
		float demand;
		bool operator<(const Candidate &p_other) const {
			return gap != p_other.gap ? gap > p_other.gap : demand > p_other.demand;
		}
	};
	LocalVector<Candidate> upgrades;
	for (uint32_t i = 0; i < count; i++) {
		Entry &e = *list[i];
		e.target = target[i];
		if (e.target < e.resident) {
			e.drop_since = -1.0;
			if (!e.job) {
				upgrades.push_back({ i, e.resident - e.target, e.demand });
			}
		} else if (e.target > e.resident) {
			if (e.drop_since < 0.0) {
				e.drop_since = s.time;
			}
			// A visible texture that needs less detail waits for the drop delay, so moving back and forth does not reload it;
			// one out of view already waited that long, and the budget does not wait.
			if (!e.job && in_flight < p_settings.max_loads_in_flight && (over_budget || !visible[i] || s.time - e.drop_since >= p_settings.drop_delay)) {
				s._start_job(rids[i], e, e.target);
				in_flight++;
			}
		} else {
			e.drop_since = -1.0;
		}
	}
	upgrades.sort();
	for (const Candidate &c : upgrades) {
		if (in_flight >= p_settings.max_loads_in_flight) {
			break;
		}
		Entry &e = *list[c.index];
		s._start_job(rids[c.index], e, e.target);
		in_flight++;
	}

	if (p_wait) {
		for (uint32_t i = 0; i < count; i++) {
			if (list[i]->job) {
				s._apply(rids[i], *list[i]);
			}
		}
	}
}
