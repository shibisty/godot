/**************************************************************************/
/*  test_mariner_fixes.cpp                                                */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mariner_fixes)

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/rendering_device.h"

#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif

// Mariner engine fixes found in the game (Terra flora, docs/veg_perf_plan.md):
// 1. Pending RenderingDevice readbacks (buffer_get_data_async / texture_get_data_async) kept their callbacks until the GPU
//    frame completed - on exit that is RenderingDevice::finalize(), after the script languages finished, and a GDScript
//    lambda destroyed or called then corrupted the heap. Main::cleanup() now calls
//    RenderingDevice::discard_pending_data_callbacks() before ScriptServer::finish_languages().
// 2. An indirect MultiMesh leaked its command buffer on every reallocation and on free (MeshStorage set the RID to null
//    without freeing it) and lost it until the mesh was set again. The MeshStorage path needs a full renderer; it is
//    checked by the exit leak report of RenderingDevice::finalize() (engine/README.md, "Engine fixes"), here only the new
//    RenderingDevice::buffer_is_valid() it relies on.
// 3. Directional soft shadows (PCSS, G0a in docs/engine_gap_g.md): the penumbra came from tan of the whole angular
//    diameter (twice the light's size), the blocker search counted every texel of its disk (a halo around thin objects:
//    the whole pole for a point at its foot) with the perspective divisor, its radii were all at the edge of the disk
//    (the search radius is negative there, the light direction points to the light) and the kernel compared one depth
//    on tilted receivers (self-shadowing). The shaders are checked by engine/tests/e20/sun_shadow.gd (maps against ray
//    traced shadows and the physics), here only the radius per unit of distance.
// 4. EditorDock::current_layout was uninitialized: test_mariner_fixes_editor.cpp (volk's X11 Window clashes with
//    Godot's Window in this file).
// The Vulkan cases run on any Vulkan driver (on the build server Mesa lavapipe); GODOT_MARINER_TEST_NO_VULKAN=1 skips them.

namespace TestMarinerFixes {

using RD = RenderingDevice;

#ifdef VULKAN_ENABLED

class ReadbackCounter : public Object {
public:
	int calls = 0;
	int last_size = -1;
	void on_data(const Vector<uint8_t> &p_data) {
		calls++;
		last_size = p_data.size();
	}
};

static RenderingContextDriverVulkan *create_vulkan_context() {
	if (OS::get_singleton()->get_environment("GODOT_MARINER_TEST_NO_VULKAN") == "1") {
		MESSAGE("GODOT_MARINER_TEST_NO_VULKAN=1: skipped.");
		return nullptr;
	}
	RenderingContextDriverVulkan *ctx = memnew(RenderingContextDriverVulkan);
	ERR_PRINT_OFF;
	const Error err = ctx->initialize();
	ERR_PRINT_ON;
	if (err != OK || ctx->device_get_count() == 0) {
		memdelete(ctx);
		MESSAGE("No Vulkan driver (ICD) here: skipped. On the build server Mesa lavapipe provides one.");
		return nullptr;
	}
	return ctx;
}

TEST_CASE("[Mariner][fixes] Pending readback callbacks: called normally, dropped by discard_pending_data_callbacks()") {
	RenderingContextDriverVulkan *ctx = create_vulkan_context();
	if (ctx == nullptr) {
		return;
	}
	REQUIRE(RenderingDevice::get_singleton() == nullptr);
	RenderingDevice *main_rd = memnew(RenderingDevice);
	REQUIRE(main_rd->initialize(ctx) == OK);
	RenderingDevice *rd = main_rd->create_local_device();
	REQUIRE(rd != nullptr);

	Vector<uint8_t> data;
	data.resize(256);
	for (int i = 0; i < data.size(); i++) {
		data.write[i] = uint8_t(i);
	}
	const RID buffer = rd->storage_buffer_create(data.size(), data);
	REQUIRE(buffer.is_valid());

	ReadbackCounter counter;

	// Without discarding: the callback gets the data once the frame completes.
	REQUIRE(rd->buffer_get_data_async(buffer, callable_mp(&counter, &ReadbackCounter::on_data)) == OK);
	rd->submit();
	rd->sync();
	CHECK(counter.calls == 1);
	CHECK(counter.last_size == 256);

	// Discarded while pending: the copy completes, the callback is never called (and no error is printed).
	REQUIRE(rd->buffer_get_data_async(buffer, callable_mp(&counter, &ReadbackCounter::on_data), 16, 64) == OK);
	REQUIRE(rd->buffer_get_data_async(buffer, callable_mp(&counter, &ReadbackCounter::on_data)) == OK);
	rd->discard_pending_data_callbacks();
	rd->submit();
	rd->sync();
	CHECK(counter.calls == 1);

	// The device keeps working: a new request after the discard is answered.
	REQUIRE(rd->buffer_get_data_async(buffer, callable_mp(&counter, &ReadbackCounter::on_data), 0, 32) == OK);
	rd->submit();
	rd->sync();
	CHECK(counter.calls == 2);
	CHECK(counter.last_size == 32);

	// A callback whose object is gone is skipped instead of being called (the request was made, the object freed).
	ReadbackCounter *gone = memnew(ReadbackCounter);
	REQUIRE(rd->buffer_get_data_async(buffer, callable_mp(gone, &ReadbackCounter::on_data)) == OK);
	memdelete(gone);
	rd->submit();
	rd->sync();

	// Discarding with nothing pending is harmless.
	rd->discard_pending_data_callbacks();

	rd->free_rid(buffer);
	memdelete(rd);
	memdelete(main_rd);
	memdelete(ctx);
}

TEST_CASE("[Mariner][fixes] RenderingDevice::buffer_is_valid()") {
	RenderingContextDriverVulkan *ctx = create_vulkan_context();
	if (ctx == nullptr) {
		return;
	}
	REQUIRE(RenderingDevice::get_singleton() == nullptr);
	RenderingDevice *main_rd = memnew(RenderingDevice);
	REQUIRE(main_rd->initialize(ctx) == OK);
	RenderingDevice *rd = main_rd->create_local_device();
	REQUIRE(rd != nullptr);

	CHECK_FALSE(rd->buffer_is_valid(RID()));
	const RID storage = rd->storage_buffer_create(64, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	const RID uniform = rd->uniform_buffer_create(64);
	CHECK(rd->buffer_is_valid(storage));
	CHECK(rd->buffer_is_valid(uniform));
	CHECK_FALSE(main_rd->buffer_is_valid(storage)); // Another device's buffer.
	rd->free_rid(storage);
	CHECK_FALSE(rd->buffer_is_valid(storage)); // Freed: not valid (RIDs are not reused).
	CHECK(rd->buffer_is_valid(uniform));
	rd->free_rid(uniform);
	CHECK_FALSE(rd->buffer_is_valid(uniform));

	memdelete(rd);
	memdelete(main_rd);
	memdelete(ctx);
}

#endif // VULKAN_ENABLED

TEST_CASE("[Mariner][fixes] Directional soft shadows: penumbra radius from half the angular diameter") {
	using RendererRD::LightStorage;
	CHECK(LightStorage::directional_penumbra_tan(0.0f) == 0.0f);
	// The sun (0.53 deg): 4.6 mm of penumbra radius per meter to the occluder.
	CHECK(LightStorage::directional_penumbra_tan(0.53f) == doctest::Approx(0.004625).epsilon(0.001));
	// 4 deg (engine/tests/e20/sun_shadow.gd): tan 2 deg.
	CHECK(LightStorage::directional_penumbra_tan(4.0f) == doctest::Approx(Math::tan(Math::deg_to_rad(2.0))).epsilon(1e-6));
	CHECK(LightStorage::directional_penumbra_tan(4.0f) < 0.5f * Math::tan(Math::deg_to_rad(4.0)) + 1e-4f);
}

} // namespace TestMarinerFixes
