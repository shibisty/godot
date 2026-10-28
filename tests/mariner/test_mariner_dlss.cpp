/**************************************************************************/
/*  test_mariner_dlss.cpp                                                 */
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

TEST_FORCE_LINK(test_mariner_dlss)

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "drivers/aftermath/aftermath.h"
#include "drivers/streamline/streamline.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"

// Mariner engine patches E2 (DLSS via NVIDIA Streamline) and E2b (Aftermath, GPU markers): public API, enum values,
// project settings and the "DLSS not available" paths. DLSS itself needs Windows, an RTX GPU and the Streamline DLLs.

namespace TestMarinerDLSS {

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

static String class_property_hint(const StringName &p_class, const StringName &p_property) {
	PropertyInfo pi;
	if (!ClassDB::get_property_info(p_class, p_property, &pi)) {
		return String();
	}
	return pi.hint_string;
}

static const char *UPSTREAM_SCALING_HINTS[] = {
	"Nearest (Fastest):5",
	"Bilinear (Fastest):0",
	"FSR 1.0 (Fast):1",
	"FSR 2.2 (Slow):2",
};

// In test runs Main::setup() is not called, so the Streamline singleton does not exist yet. Create and register
// it the way main.cpp does, and undo the singleton registration afterwards.
struct StreamlineScope {
	Streamline *created = nullptr;

	StreamlineScope() {
		if (Streamline::get_singleton() == nullptr) {
			created = memnew(Streamline);
			Streamline::register_singleton();
		}
	}
	~StreamlineScope() {
		if (created) {
			Engine::get_singleton()->remove_singleton("Streamline");
			memdelete(created);
		}
	}
};

TEST_SUITE("[Mariner][DLSS]") {
	TEST_CASE("[Mariner][E2] Scaling 3D mode enums: DLSS appended, upstream values unchanged") {
		CHECK(int(Viewport::SCALING_3D_MODE_BILINEAR) == 0);
		CHECK(int(Viewport::SCALING_3D_MODE_FSR) == 1);
		CHECK(int(Viewport::SCALING_3D_MODE_FSR2) == 2);
		CHECK(int(Viewport::SCALING_3D_MODE_METALFX_SPATIAL) == 3);
		CHECK(int(Viewport::SCALING_3D_MODE_METALFX_TEMPORAL) == 4);
		CHECK(int(Viewport::SCALING_3D_MODE_NEAREST) == 5);
		CHECK(int(Viewport::SCALING_3D_MODE_DLSS) == 6);
		CHECK(int(Viewport::SCALING_3D_MODE_MAX) == 7);

		CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_FSR2) == 2);
		CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_NEAREST) == 5);
		CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == int(Viewport::SCALING_3D_MODE_DLSS));
		CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_MAX) == int(Viewport::SCALING_3D_MODE_MAX));
		CHECK(int(RSE::VIEWPORT_SCALING_3D_MODE_OFF) == 255);

		// DLSS is a temporal upscaler (TAA is disabled internally, like FSR2).
		CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);
		CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_FSR2) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);
		CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_FSR) == RSE::VIEWPORT_SCALING_3D_TYPE_SPATIAL);
		CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_OFF) == RSE::VIEWPORT_SCALING_3D_TYPE_NONE);

		// Bound to scripting.
		bool ok = false;
		CHECK(ClassDB::get_integer_constant("Viewport", "SCALING_3D_MODE_DLSS", &ok) == 6);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant_enum("Viewport", "SCALING_3D_MODE_DLSS") == StringName("Scaling3DMode"));
		CHECK(ClassDB::get_integer_constant("Viewport", "SCALING_3D_MODE_MAX", &ok) == 7);
		CHECK(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_SCALING_3D_MODE_DLSS", &ok) == 6);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_SCALING_3D_MODE_FSR2", &ok) == 2);
	}

	TEST_CASE("[Mariner][E2] DLSS in property hints and the rendering/scaling_3d/mode project setting") {
		const String viewport_hint = class_property_hint("Viewport", "scaling_3d_mode");
		CHECK(viewport_hint.contains("NVIDIA DLSS (Temporal):6"));
		for (const char *upstream : UPSTREAM_SCALING_HINTS) {
			CHECK_MESSAGE(viewport_hint.contains(upstream), upstream);
		}
		CHECK(viewport_hint.contains("MetalFX (Temporal - Slow):4"));

		const String config_hint = class_property_hint("RenderSceneBuffersConfiguration", "scaling_3d_mode");
		CHECK(config_hint.contains("NVIDIA DLSS (Temporal):6"));
		CHECK(config_hint.contains("FSR 2.2 (Slow):2"));

		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/scaling_3d/mode"));
		CHECK(int(ProjectSettings::get_singleton()->property_get_revert("rendering/scaling_3d/mode")) == 0);
		const PropertyInfo mode = find_project_setting("rendering/scaling_3d/mode");
		CHECK(mode.hint == PROPERTY_HINT_ENUM);
		CHECK(mode.hint_string.contains("NVIDIA DLSS (Temporal):6"));
		for (const char *upstream : UPSTREAM_SCALING_HINTS) {
			CHECK_MESSAGE(mode.hint_string.contains(upstream), upstream);
		}
		// Apple overrides keep their MetalFX list (DLSS is Windows-only).
		const PropertyInfo mode_macos = find_project_setting("rendering/scaling_3d/mode.macos");
		CHECK(mode_macos.hint_string.contains("MetalFX (Temporal - Slow):4"));
		CHECK_FALSE(mode_macos.hint_string.contains("DLSS"));
	}

	TEST_CASE("[Mariner][E2][SceneTree] Viewport keeps the DLSS mode and falls back without Streamline") {
		SubViewport *viewport = memnew(SubViewport);
		SceneTree::get_singleton()->get_root()->add_child(viewport);
		viewport->set_size(Size2i(64, 64));

		CHECK(viewport->get_scaling_3d_mode() == Viewport::SCALING_3D_MODE_BILINEAR);
		ERR_PRINT_OFF; // "DLSS is not available ... falling back" warning.
		viewport->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_DLSS);
		RS::get_singleton()->viewport_set_scaling_3d_mode(viewport->get_viewport_rid(), RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		ERR_PRINT_ON;
		CHECK(viewport->get_scaling_3d_mode() == Viewport::SCALING_3D_MODE_DLSS);
		CHECK(int(viewport->get("scaling_3d_mode")) == 6);

		ERR_PRINT_OFF; // FSR2 needs Forward+ (the test renderer is a dummy).
		viewport->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_FSR2);
		ERR_PRINT_ON;
		CHECK(viewport->get_scaling_3d_mode() == Viewport::SCALING_3D_MODE_FSR2);

		memdelete(viewport);
	}

	TEST_CASE("[Mariner][E2] Streamline singleton: API, project settings, no capabilities without DLSS") {
		StreamlineScope scope;
		Streamline *sl = Streamline::get_singleton();
		REQUIRE(sl != nullptr);

		CHECK(ClassDB::class_exists("Streamline"));
		CHECK(Engine::get_singleton()->has_singleton("Streamline"));
		CHECK(Engine::get_singleton()->get_singleton_object("Streamline") == sl);
		CHECK(ClassDB::has_method("Streamline", "get_capability"));
		CHECK(ClassDB::has_method("Streamline", "set_parameter"));

		bool ok = false;
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_CAPABILITY_DLSS", &ok) == 0);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_CAPABILITY_NIS", &ok) == 5);
		CHECK(ClassDB::get_integer_constant("Streamline", "STREAMLINE_PARAM_DLSS_PRESET", &ok) == 2);
		CHECK(ok);

		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/streamline/streamline_log"));
		CHECK(bool(ProjectSettings::get_singleton()->property_get_revert("rendering/streamline/streamline_log")) == false);
		REQUIRE(ProjectSettings::get_singleton()->has_setting("rendering/streamline/dlss_preset"));
		CHECK(String(ProjectSettings::get_singleton()->property_get_revert("rendering/streamline/dlss_preset")) == "?");
		const PropertyInfo preset = find_project_setting("rendering/streamline/dlss_preset");
		CHECK(preset.hint == PROPERTY_HINT_ENUM);
		CHECK(preset.hint_string.begins_with("?,"));
		CHECK(preset.hint_string.contains("K"));

		// No device was created through Streamline here (Linux, or Windows before slInit): nothing is available,
		// and the device lifecycle markers that run without an initialized Streamline must be harmless.
		sl->emit_marker(STREAMLINE_MARKER_AFTER_DEVICE_CREATION);
		sl->emit_marker(STREAMLINE_MARKER_BEFORE_MESSAGE_LOOP);
		sl->emit_marker(STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY);
		sl->set_internal_parameter("device_destroyed", nullptr);
		sl->set_parameter(STREAMLINE_PARAM_DLSS_PRESET, "K");
		sl->set_parameter(STREAMLINE_PARAM_DLSS_PRESET, "?");
		sl->set_parameter(STREAMLINE_PARAM_REFLEX_MODE, 1.0);

		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_DLSS));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_DLSS_G));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_DLSS_RR));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_REFLEX));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_PCL));
		CHECK_FALSE(sl->get_capability(STREAMLINE_CAPABILITY_NIS));
		CHECK(bool(sl->call("get_capability", STREAMLINE_CAPABILITY_DLSS)) == false);

		CHECK(sl->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_vkGetInstanceProcAddr) == nullptr);
		CHECK(sl->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_D3D12CreateDevice) == nullptr);
		CHECK(sl->get_internal_parameter(STREAMLINE_INTERNAL_PARAMETER_FUNC_CreateDXGIFactory2) == nullptr);
	}

	TEST_CASE("[Mariner][E2b] Aftermath stays inactive without --debug-shaders; GPU markers off by default") {
		CHECK_FALSE(Engine::get_singleton()->is_gpu_markers_enabled());
		REQUIRE_FALSE(Engine::get_singleton()->is_generate_spirv_debug_info_enabled());

		Aftermath *created = nullptr;
		if (Aftermath::get_singleton() == nullptr) {
			created = memnew(Aftermath);
		}
		Aftermath *am = Aftermath::get_singleton();
		REQUIRE(am != nullptr);

		CHECK_FALSE(Aftermath::is_active());
		// Without --debug-shaders nothing is loaded (no GFSDK_Aftermath_Lib.x64.dll needed) and shaders are not kept.
		am->emit_marker(AFTERMATH_MARKER_INITIALIZE_VULKAN);
		am->emit_marker(AFTERMATH_MARKER_INITIALIZE_D3D12);
		am->emit_marker(AFTERMATH_MARKER_AFTER_DEVICE_CREATION);
		am->emit_marker(AFTERMATH_MARKER_BEFORE_DEVICE_DESTROY);
		const uint8_t bytes[4] = { 0x03, 0x02, 0x23, 0x07 };
		am->register_shader(AFTERMATH_SHADER_SPIRV, bytes, sizeof(bytes));
		CHECK_FALSE(Aftermath::is_active());
#ifndef AFTERMATH_ENABLED
		CHECK(am->get_internal_parameter(AFTERMATH_INTERNAL_PARAM_VK_REQUIRED_EXTENSIONS) == nullptr);
		CHECK(am->get_internal_parameter(AFTERMATH_INTERNAL_PARAM_VK_DEVICE_DIAGNOSTICS_CONFIG) == nullptr);
#endif

		if (created) {
			memdelete(created);
			CHECK(Aftermath::get_singleton() == nullptr);
		}
	}
}

} // namespace TestMarinerDLSS
