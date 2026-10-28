/**************************************************************************/
/*  test_mariner_raytracing.cpp                                           */
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

TEST_FORCE_LINK(test_mariner_raytracing)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

// Mariner engine patch E5a (ray tracing scene for compositor effects): Viewport / RenderingServer API and state,
// RenderSceneBuffersRD and RenderingDevice bindings, project settings and the RenderingDevice enums the patches
// touched. Building acceleration structures needs Vulkan with ray query and is not covered here.

namespace TestMarinerRaytracing {

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

TEST_SUITE("[Mariner][Raytracing]") {
	TEST_CASE("[Mariner][E5a][SceneTree] Viewport raytracing_scene_* properties are bound with the documented defaults") {
		const char *properties[] = { "raytracing_scene_enabled", "raytracing_scene_cull_mask", "raytracing_scene_distance" };
		for (const char *property : properties) {
			CHECK_MESSAGE(ClassDB::has_property("Viewport", property), property);
		}
		CHECK(ClassDB::has_method("Viewport", "set_raytracing_scene"));
		CHECK(ClassDB::has_method("Viewport", "is_raytracing_scene_enabled"));
		CHECK(ClassDB::has_method("Viewport", "set_raytracing_scene_cull_mask"));
		CHECK(ClassDB::has_method("Viewport", "get_raytracing_scene_cull_mask"));
		CHECK(ClassDB::has_method("Viewport", "set_raytracing_scene_distance"));
		CHECK(ClassDB::has_method("Viewport", "get_raytracing_scene_distance"));

		PropertyInfo pi;
		REQUIRE(ClassDB::get_property_info("Viewport", "raytracing_scene_enabled", &pi));
		CHECK(pi.type == Variant::BOOL);
		REQUIRE(ClassDB::get_property_info("Viewport", "raytracing_scene_cull_mask", &pi));
		CHECK(pi.type == Variant::INT);
		CHECK(pi.hint == PROPERTY_HINT_LAYERS_3D_RENDER);
		REQUIRE(ClassDB::get_property_info("Viewport", "raytracing_scene_distance", &pi));
		CHECK(pi.type == Variant::FLOAT);
		CHECK(pi.hint == PROPERTY_HINT_RANGE);
		CHECK(pi.hint_string.begins_with("0,"));

		bool valid = false;
		CHECK(bool(ClassDB::class_get_default_property_value("SubViewport", "raytracing_scene_enabled", &valid)) == false);
		CHECK(valid);
		CHECK(int64_t(ClassDB::class_get_default_property_value("SubViewport", "raytracing_scene_cull_mask", &valid)) == 0xFFFFF);
		CHECK(valid);
		CHECK(double(ClassDB::class_get_default_property_value("SubViewport", "raytracing_scene_distance", &valid)) == doctest::Approx(0.0));
		CHECK(valid);
	}

	TEST_CASE("[Mariner][E5a][SceneTree] Viewport raytracing scene settings reach the RenderingServer viewport") {
		SubViewport *viewport = memnew(SubViewport);
		SceneTree::get_singleton()->get_root()->add_child(viewport);
		const RID rid = viewport->get_viewport_rid();
		REQUIRE(RSG::viewport != nullptr);

		uint32_t mask = 0;
		float distance = -1.0f;
		CHECK_FALSE(viewport->is_raytracing_scene_enabled());
		CHECK(viewport->get_raytracing_scene_cull_mask() == 0xFFFFF);
		CHECK(viewport->get_raytracing_scene_distance() == doctest::Approx(0.0));
		CHECK_FALSE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));

		viewport->set_raytracing_scene_cull_mask(0b101);
		viewport->set_raytracing_scene_distance(250.0f);
		// Mask and distance are kept while disabled, but the server reports no raytracing scene.
		CHECK_FALSE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));

		viewport->set_raytracing_scene(true);
		CHECK(viewport->is_raytracing_scene_enabled());
		REQUIRE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));
		CHECK(mask == 0b101);
		CHECK(distance == doctest::Approx(250.0));

		// Negative distances are clamped to 0 (= camera far plane) on both sides.
		viewport->set_raytracing_scene_distance(-10.0f);
		CHECK(viewport->get_raytracing_scene_distance() == doctest::Approx(0.0));
		REQUIRE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));
		CHECK(distance == doctest::Approx(0.0));

		// Through the generic property API as scripts and scenes use it.
		viewport->set("raytracing_scene_cull_mask", 0xFF);
		viewport->set("raytracing_scene_distance", 32.0);
		CHECK(int64_t(viewport->get("raytracing_scene_cull_mask")) == 0xFF);
		REQUIRE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));
		CHECK(mask == 0xFF);
		CHECK(distance == doctest::Approx(32.0));

		viewport->set_raytracing_scene(false);
		CHECK_FALSE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));

		// Direct RenderingServer call; the server clamps the distance as well.
		RS::get_singleton()->viewport_set_raytracing_scene(rid, true, 0x3, -5.0f);
		REQUIRE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));
		CHECK(mask == 0x3);
		CHECK(distance == doctest::Approx(0.0));
		RS::get_singleton()->viewport_set_raytracing_scene(rid, false);
		CHECK_FALSE(RSG::viewport->viewport_get_raytracing_scene(rid, mask, distance));

		// Invalid RID: error, no crash.
		ERR_PRINT_OFF;
		RS::get_singleton()->viewport_set_raytracing_scene(RID(), true);
		ERR_PRINT_ON;
		CHECK_FALSE(RSG::viewport->viewport_get_raytracing_scene(RID(), mask, distance));

		memdelete(viewport);
	}

	TEST_CASE("[Mariner][E5a] RenderingServer.viewport_set_raytracing_scene binding and project settings") {
		MethodBind *method = ClassDB::get_method("RenderingServer", "viewport_set_raytracing_scene");
		REQUIRE(method != nullptr);
		CHECK(method->get_argument_count() == 4);
		CHECK(method->get_default_argument_count() == 2);
		CHECK(int64_t(method->get_default_argument(2)) == 0xFFFFF);
		CHECK(double(method->get_default_argument(3)) == doctest::Approx(0.0));

		struct Setting {
			const char *name;
			Variant value;
		};
		const Setting settings[] = {
			{ "rendering/raytracing/scene_acceleration_structures", false },
			{ "rendering/raytracing/multimesh_merged_blas_max_triangles", 1048576 },
			{ "rendering/raytracing/deformed_mesh_cache_ttl_frames", 60 },
			{ "rendering/raytracing/multimesh_blas_cache_ttl_frames", 3600 },
		};
		for (const Setting &setting : settings) {
			INFO(setting.name);
			REQUIRE(ProjectSettings::get_singleton()->has_setting(setting.name));
			CHECK(ProjectSettings::get_singleton()->property_get_revert(setting.name) == setting.value);
			const PropertyInfo pi = find_project_setting(setting.name);
			CHECK((pi.usage & PROPERTY_USAGE_RESTART_IF_CHANGED) != 0);
		}
		CHECK(bool(GLOBAL_GET("rendering/raytracing/scene_acceleration_structures")) == false);
	}

	TEST_CASE("[Mariner][E5a][E4] RenderSceneBuffersRD methods for compositor effects are bound") {
		REQUIRE(ClassDB::class_exists("RenderSceneBuffersRD"));
		const char *methods[] = {
			// E5a
			"has_raytracing_scene",
			"get_raytracing_tlas",
			"get_raytracing_instance_buffer",
			"get_raytracing_material_buffer",
			"get_raytracing_instance_count",
			"get_raytracing_textures",
			"get_raytracing_texture_uniform_set",
			// E4
			"has_opaque_late_textures",
			"get_opaque_late_color",
			"get_opaque_late_depth",
		};
		for (const char *name : methods) {
			CHECK_MESSAGE(ClassDB::has_method("RenderSceneBuffersRD", name), name);
		}
		CHECK(ClassDB::get_method("RenderSceneBuffersRD", "get_raytracing_texture_uniform_set")->get_argument_count() == 2);
		CHECK(ClassDB::get_method("RenderSceneBuffersRD", "get_opaque_late_color")->get_argument_count() == 1);
		CHECK(ClassDB::get_method("RenderSceneBuffersRD", "get_opaque_late_depth")->get_argument_count() == 1);
		// Upstream methods still there.
		CHECK(ClassDB::has_method("RenderSceneBuffersRD", "get_color_texture"));
		CHECK(ClassDB::has_method("RenderSceneBuffersRD", "get_velocity_texture"));
	}

	TEST_CASE("[Mariner][E5a][E2] RenderingDevice: blas_update bound, enums appended, ray tracing stage names") {
		CHECK(ClassDB::has_method("RenderingDevice", "blas_update"));
		CHECK(ClassDB::has_method("RenderingDevice", "blas_build"));
		CHECK(ClassDB::has_method("RenderingDevice", "tlas_build"));

		// E2: driver resources appended after the upstream ones (values used by GDExtensions must not move).
		CHECK(int(RD::DRIVER_RESOURCE_LOGICAL_DEVICE) == 0);
		CHECK(int(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT) == 7);
		CHECK(int(RD::DRIVER_RESOURCE_RENDER_PIPELINE) == 12);
		CHECK(int(RD::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY) == 13);
		CHECK(int(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS) == 14);
		bool ok = false;
		CHECK(ClassDB::get_integer_constant("RenderingDevice", "DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY", &ok) == 13);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("RenderingDevice", "DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS", &ok) == 14);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("RenderingDevice", "DRIVER_RESOURCE_RENDER_PIPELINE", &ok) == 12);

		// Upstream ray tracing enums are unchanged.
		CHECK(int(RD::SHADER_STAGE_COMPUTE) == 4);
		CHECK(int(RD::SHADER_STAGE_RAYGEN) == 5);
		CHECK(int(RD::SHADER_STAGE_INTERSECTION) == 9);
		CHECK(int(RD::SHADER_STAGE_MAX) == 12); // E7 appended the task and mesh stages.
		CHECK(int(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE) == 12);
		CHECK(int(RD::UNIFORM_TYPE_MAX) == 13);
		CHECK(ClassDB::get_integer_constant("RenderingDevice", "SUPPORTS_RAY_QUERY", &ok) == int64_t(RD::SUPPORTS_RAY_QUERY));
		CHECK(ok);

		// Every shader stage has a name (the ray tracing ones were missing upstream: nullptr in error messages).
		const char *expected[RD::SHADER_STAGE_MAX] = { "Vertex", "Fragment", "TesselationControl", "TesselationEvaluation", "Compute", "RayGen", "AnyHit", "ClosestHit", "Miss", "Intersection", "Task", "Mesh" };
		for (int i = 0; i < RD::SHADER_STAGE_MAX; i++) {
			REQUIRE_MESSAGE(RD::SHADER_STAGE_NAMES[i] != nullptr, i);
			CHECK(String(RD::SHADER_STAGE_NAMES[i]) == String(expected[i]));
		}
	}

	TEST_CASE("[Mariner][E5a] Unbounded (runtime-sized) shader uniforms") {
		using SU = RD::ShaderUniform;
		CHECK(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_TEXTURE));
		CHECK(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_SAMPLER));
		CHECK(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE));
		CHECK(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_IMAGE));
		CHECK_FALSE(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_UNIFORM_BUFFER));
		CHECK_FALSE(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_STORAGE_BUFFER));
		CHECK_FALSE(SU::type_can_be_unbounded(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE));

		SU bounded;
		bounded.type = RD::UNIFORM_TYPE_TEXTURE;
		bounded.binding = 0;
		bounded.length = 0;
		SU unbounded = bounded;
		unbounded.unbounded = true;

		CHECK_FALSE(bounded.unbounded); // Default.
		CHECK(bounded != unbounded);
		CHECK_FALSE(bounded != bounded);
		// Strict weak ordering: exactly one of a < b, b < a.
		CHECK((bounded < unbounded) != (unbounded < bounded));
		CHECK_FALSE(bounded < bounded);
		// Binding still dominates the order.
		SU later = bounded;
		later.binding = 1;
		CHECK(unbounded < later);
	}
}

} // namespace TestMarinerRaytracing
