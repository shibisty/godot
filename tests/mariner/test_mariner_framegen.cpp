/**************************************************************************/
/*  test_mariner_framegen.cpp                                             */
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

TEST_FORCE_LINK(test_mariner_framegen)

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "drivers/streamline/streamline.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/render_scene_buffers.h"

// Mariner engine patch E3 (DLSS Frame Generation + Reflex + PCL through NVIDIA Streamline): public API, enum values,
// project settings, the Reflex / frame count rules and the "Streamline not available" fallbacks. Frame generation
// itself (and the HUD-less colour) needs Windows, an RTX 40/50 GPU and the Streamline DLLs.

namespace TestMarinerFrameGeneration {

static PropertyInfo find_project_setting(const String &p_name) {
	List<PropertyInfo> list;
	ProjectSettings::get_singleton()->get_property_list(&list);
	for (const PropertyInfo &pi : list) {
		if (pi.name == p_name) {
			return pi;
		}
	}
	return PropertyInfo();
}

// In test runs Main::setup() is not called, so the Streamline singleton does not exist yet (see test_mariner_dlss.cpp).
struct StreamlineScope {
	Streamline *created = nullptr;

	StreamlineScope() {
		if (Streamline::get_singleton() == nullptr) {
			created = memnew(Streamline);
			Streamline::register_singleton();
		}
	}
	~StreamlineScope() {
		Streamline *sl = Streamline::get_singleton();
		if (sl) {
			// Back to the defaults for other tests.
			sl->set_parameter(STREAMLINE_PARAM_REFLEX_MODE, 0.0);
			sl->set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, 0.0);
			sl->set_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES, 1);
		}
		if (created) {
			Engine::get_singleton()->remove_singleton("Streamline");
			memdelete(created);
		}
	}
};

TEST_SUITE("[Mariner][FrameGeneration]") {
	TEST_CASE("[Mariner][E3] Reflex mode and generated frame rules") {
		// STREAMLINE_PARAM_REFLEX_MODE takes a double, like NVIDIA's fork: 0 off, 1 low latency, 2 boost.
		CHECK(streamline_reflex_mode_from_value(0.0) == STREAMLINE_REFLEX_MODE_OFF);
		CHECK(streamline_reflex_mode_from_value(-1.0) == STREAMLINE_REFLEX_MODE_OFF);
		CHECK(streamline_reflex_mode_from_value(0.5) == STREAMLINE_REFLEX_MODE_LOW_LATENCY);
		CHECK(streamline_reflex_mode_from_value(1.0) == STREAMLINE_REFLEX_MODE_LOW_LATENCY);
		CHECK(streamline_reflex_mode_from_value(1.5) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);
		CHECK(streamline_reflex_mode_from_value(2.0) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);
		CHECK(streamline_reflex_mode_from_value(9.0) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);

		// DLSS-G shows magenta frames without Reflex: "off" becomes low latency while frame generation is wanted,
		// explicit modes (boost included) are kept, and nothing is forced without frame generation.
		CHECK(streamline_effective_reflex_mode(STREAMLINE_REFLEX_MODE_OFF, true) == STREAMLINE_REFLEX_MODE_LOW_LATENCY);
		CHECK(streamline_effective_reflex_mode(STREAMLINE_REFLEX_MODE_LOW_LATENCY, true) == STREAMLINE_REFLEX_MODE_LOW_LATENCY);
		CHECK(streamline_effective_reflex_mode(STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST, true) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);
		CHECK(streamline_effective_reflex_mode(STREAMLINE_REFLEX_MODE_OFF, false) == STREAMLINE_REFLEX_MODE_OFF);
		CHECK(streamline_effective_reflex_mode(STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST, false) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);

		// Generated frames per rendered frame: 1 = 2x, 2 = 3x, 3 = 4x (multi frame generation, RTX 50).
		CHECK(STREAMLINE_DLSS_G_MAX_GENERATED_FRAMES == 3);
		CHECK(streamline_clamp_generated_frames(1, 0) == 1); // Max not known yet: 2x.
		CHECK(streamline_clamp_generated_frames(3, 0) == 1);
		CHECK(streamline_clamp_generated_frames(3, 1) == 1); // RTX 40: 2x only.
		CHECK(streamline_clamp_generated_frames(2, 3) == 2);
		CHECK(streamline_clamp_generated_frames(3, 3) == 3);
		CHECK(streamline_clamp_generated_frames(5, 3) == 3);
		CHECK(streamline_clamp_generated_frames(0, 3) == 1);
		CHECK(streamline_clamp_generated_frames(-2, 3) == 1);
		CHECK(streamline_clamp_generated_frames(3, 7) == 3); // Never above the SDK limit.
	}

	TEST_CASE("[Mariner][E3] Streamline singleton: frame generation / Reflex API and project settings") {
		StreamlineScope scope;
		Streamline *sl = Streamline::get_singleton();
		REQUIRE(sl != nullptr);

		for (const char *method : { "get_parameter", "get_reflex_mode", "is_frame_generation_active", "get_frame_generation_max_frames", "get_frame_generation_status" }) {
			CHECK_MESSAGE(ClassDB::has_method("Streamline", method), method);
		}

		bool ok = false;
		// E2 values unchanged, the frame count parameter is appended.
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_REFLEX_MODE", &ok) == 0);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US", &ok) == 1);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_DLSS_PRESET", &ok) == 2);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_DLSS_RR_PRESET", &ok) == 3);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_DLSS_G_FRAMES", &ok) == 4);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant_enum("Streamline", "STREAMLINE_PARAM_DLSS_G_FRAMES") == StringName("StreamlineParameterType"));
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_CAPABILITY_DLSS_G", &ok) == 1);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_CAPABILITY_REFLEX", &ok) == 3);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_CAPABILITY_PCL", &ok) == 4);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_REFLEX_MODE_OFF", &ok) == 0);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_REFLEX_MODE_LOW_LATENCY", &ok) == 1);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST", &ok) == 2);
		CHECK(ClassDB::get_integer_constant_enum("Streamline", "STREAMLINE_REFLEX_MODE_LOW_LATENCY") == StringName("StreamlineReflexMode"));

		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/streamline/reflex_mode"));
		CHECK(int(ProjectSettings::get_singleton()->property_get_revert("rendering/streamline/reflex_mode")) == 0);
		const PropertyInfo reflex_mode = find_project_setting("rendering/streamline/reflex_mode");
		CHECK(reflex_mode.type == Variant::INT);
		CHECK(reflex_mode.hint == PROPERTY_HINT_ENUM);
		CHECK(reflex_mode.hint_string == "Off,Low Latency,Low Latency + Boost");

		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/streamline/reflex_frame_limit_us"));
		CHECK(int(ProjectSettings::get_singleton()->property_get_revert("rendering/streamline/reflex_frame_limit_us")) == 0);
		const PropertyInfo frame_limit = find_project_setting("rendering/streamline/reflex_frame_limit_us");
		CHECK(frame_limit.hint == PROPERTY_HINT_RANGE);
		CHECK(frame_limit.hint_string.begins_with("0,1000000,1"));

		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/streamline/frame_generation_frames"));
		CHECK(int(ProjectSettings::get_singleton()->property_get_revert("rendering/streamline/frame_generation_frames")) == 1);
		const PropertyInfo frames = find_project_setting("rendering/streamline/frame_generation_frames");
		CHECK(frames.hint == PROPERTY_HINT_RANGE);
		CHECK(frames.hint_string == "1,3,1");
	}

	TEST_CASE("[Mariner][E3] Without Streamline: requests are kept, nothing runs, markers are harmless") {
		StreamlineScope scope;
		Streamline *sl = Streamline::get_singleton();
		REQUIRE(sl != nullptr);

		// Nothing is loaded here (Linux, or Windows without the DLLs / before the device exists).
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_DLSS_G));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_REFLEX));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_PCL));
		CHECK_FALSE(sl->is_frame_generation_active());
		CHECK(sl->get_frame_generation_max_frames() == 0);
		CHECK(sl->get_frame_generation_status() == 0);
		CHECK(sl->get_reflex_mode() == STREAMLINE_REFLEX_MODE_OFF);

		// Requested values are stored (and read back) even though Reflex does not run.
		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_REFLEX_MODE)) == 0);
		sl->set_parameter(STREAMLINE_PARAM_REFLEX_MODE, 2.0);
		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_REFLEX_MODE)) == STREAMLINE_REFLEX_MODE_LOW_LATENCY_WITH_BOOST);
		CHECK(sl->get_reflex_mode() == STREAMLINE_REFLEX_MODE_OFF); // Applied mode: off without Reflex.
		sl->set_parameter(STREAMLINE_PARAM_REFLEX_MODE, 1);
		CHECK(int(sl->call("get_parameter", STREAMLINE_PARAM_REFLEX_MODE)) == STREAMLINE_REFLEX_MODE_LOW_LATENCY);

		sl->set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, 16667.0);
		CHECK(int64_t(sl->get_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US)) == 16667);
		sl->set_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US, -5.0);
		CHECK(int64_t(sl->get_parameter(STREAMLINE_PARAM_REFLEX_FRAME_LIMIT_US)) == 0);

		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES)) == 1);
		sl->set_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES, 3);
		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES)) == 3);
		sl->set_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES, 7);
		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES)) == 3);
		sl->set_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES, 0);
		CHECK(int(sl->get_parameter(STREAMLINE_PARAM_DLSS_G_FRAMES)) == 1);

		sl->set_parameter(STREAMLINE_PARAM_DLSS_PRESET, "K");
		CHECK(String(sl->get_parameter(STREAMLINE_PARAM_DLSS_PRESET)) == "K");
		sl->set_parameter(STREAMLINE_PARAM_DLSS_PRESET, "?");
		CHECK(sl->get_parameter(STREAMLINE_PARAM_DLSS_RR_PRESET).get_type() == Variant::NIL);

		// Per-frame markers from Main::iteration, the compositor and the drivers, and the PCL ping: no-ops here.
		for (StreamlineMarkerType marker : { STREAMLINE_MARKER_BEGIN_SIMULATION, STREAMLINE_MARKER_END_SIMULATION, STREAMLINE_MARKER_BEGIN_RENDER, STREAMLINE_MARKER_END_RENDER, STREAMLINE_MARKER_BEGIN_PRESENT, STREAMLINE_MARKER_END_PRESENT, STREAMLINE_MARKER_MODIFY_SWAPCHAIN, STREAMLINE_MARKER_PC_PING, STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP }) {
			sl->emit_marker(marker);
		}
		CHECK_FALSE(sl->is_frame_generation_active());
		CHECK(sl->get_reflex_mode() == STREAMLINE_REFLEX_MODE_OFF);
	}

	TEST_CASE("[Mariner][E3][SceneTree] Viewport.frame_generation reaches the RenderingServer viewport") {
		PropertyInfo pi;
		REQUIRE(ClassDB::get_property_info("Viewport", "frame_generation", &pi));
		CHECK(pi.type == Variant::BOOL);
		CHECK(ClassDB::has_method("Viewport", "set_frame_generation"));
		CHECK(ClassDB::has_method("Viewport", "get_frame_generation"));

		CHECK_FALSE(SceneTree::get_singleton()->get_root()->get_frame_generation());

		SubViewport *viewport = memnew(SubViewport);
		SceneTree::get_singleton()->get_root()->add_child(viewport);
		viewport->set_size(Size2i(64, 64));
		const RID rid = viewport->get_viewport_rid();
		REQUIRE(RSG::viewport != nullptr);

		CHECK_FALSE(viewport->get_frame_generation());
		CHECK_FALSE(RSG::viewport->viewport_get_frame_generation(rid));

		viewport->set_frame_generation(true);
		CHECK(viewport->get_frame_generation());
		CHECK(RSG::viewport->viewport_get_frame_generation(rid));

		// Together with the DLSS mode, as the game sets it (no Streamline: DLSS falls back, nothing crashes).
		ERR_PRINT_OFF;
		viewport->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_DLSS);
		ERR_PRINT_ON;
		CHECK(RSG::viewport->viewport_get_frame_generation(rid));

		viewport->set("frame_generation", false);
		CHECK_FALSE(bool(viewport->get("frame_generation")));
		CHECK_FALSE(RSG::viewport->viewport_get_frame_generation(rid));

		RS::get_singleton()->viewport_set_frame_generation(rid, true);
		CHECK(RSG::viewport->viewport_get_frame_generation(rid));
		RS::get_singleton()->viewport_set_frame_generation(rid, false);
		CHECK_FALSE(RSG::viewport->viewport_get_frame_generation(rid));

		// Invalid RID: error, no crash.
		ERR_PRINT_OFF;
		RS::get_singleton()->viewport_set_frame_generation(RID(), true);
		ERR_PRINT_ON;
		CHECK_FALSE(RSG::viewport->viewport_get_frame_generation(RID()));

		memdelete(viewport);
	}

	TEST_CASE("[Mariner][E3] RenderingServer binding, render buffers flag, PCL ping window event, renderer fallback") {
		MethodBind *method = ClassDB::get_method("RenderingServer", "viewport_set_frame_generation");
		REQUIRE(method != nullptr);
		CHECK(method->get_argument_count() == 2);

		Ref<RenderSceneBuffersConfiguration> config;
		config.instantiate();
		CHECK_FALSE(config->get_use_frame_generation());
		config->set_use_frame_generation(true);
		CHECK(config->get_use_frame_generation());

		// Appended at the end of DisplayServer.WindowEvent; upstream values unchanged.
		CHECK(int(DisplayServerEnums::WINDOW_EVENT_FORCE_CLOSE) == 8);
		CHECK(int(DisplayServerEnums::WINDOW_EVENT_OUTPUT_MAX_LINEAR_VALUE_CHANGED) == 9);
		CHECK(int(DisplayServerEnums::WINDOW_EVENT_PCL_PING) == 10);
		bool ok = false;
		CHECK(ClassDB::get_integer_constant("DisplayServer", "WINDOW_EVENT_PCL_PING", &ok) == 10);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant_enum("DisplayServer", "WINDOW_EVENT_PCL_PING") == StringName("WindowEvent"));

		// Renderers without frame generation (here: the dummy one) ignore the HUD-less capture request.
		REQUIRE(RSG::rasterizer != nullptr);
		RenderingServerTypes::BlitToScreen blit;
		blit.dst_rect = Rect2i(0, 0, 64, 64);
		RSG::rasterizer->frame_generation_capture_hudless(DisplayServerEnums::MAIN_WINDOW_ID, blit);
	}
}

} // namespace TestMarinerFrameGeneration
