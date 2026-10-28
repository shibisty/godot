/**************************************************************************/
/*  test_mariner_pathtracing.cpp                                          */
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

TEST_FORCE_LINK(test_mariner_pathtracing)

#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_pathtracer.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/environment_storage.h"
#include "servers/rendering/storage/render_scene_buffers.h"

// Mariner engine patch E5c (path tracer for photo mode): Environment / RenderingServer / Viewport API, defaults and
// clamping, values reaching the environment storage, the accumulation rules of the renderer and the fallback without
// ray tracing (the test renderer has no RenderingDevice). Tracing itself needs Vulkan with ray query.

namespace TestMarinerPathtracing {

using RendererSceneRenderImplementation::RenderPathtracer;

TEST_SUITE("[Mariner][Pathtracing]") {
	TEST_CASE("[Mariner][E5c] Environment pathtracing_* properties: types, hints and defaults") {
		struct Prop {
			const char *name;
			Variant::Type type;
			Variant value;
		};
		const Prop props[] = {
			{ "pathtracing_enabled", Variant::BOOL, false },
			{ "pathtracing_debug_mode", Variant::INT, 0 },
			{ "pathtracing_samples_per_pixel", Variant::INT, 1 },
			{ "pathtracing_max_bounces", Variant::INT, 3 },
			{ "pathtracing_denoiser", Variant::INT, 0 },
			{ "pathtracing_accumulate", Variant::BOOL, true },
			{ "pathtracing_max_samples", Variant::INT, 0 },
			{ "pathtracing_firefly_clamp", Variant::FLOAT, 0.0 },
		};
		for (const Prop &prop : props) {
			INFO(prop.name);
			PropertyInfo pi;
			REQUIRE(ClassDB::get_property_info("Environment", prop.name, &pi));
			CHECK(pi.type == prop.type);
			bool valid = false;
			CHECK(ClassDB::class_get_default_property_value("Environment", prop.name, &valid) == prop.value);
			CHECK(valid);
		}
		PropertyInfo pi;
		REQUIRE(ClassDB::get_property_info("Environment", "pathtracing_enabled", &pi));
		CHECK(pi.hint == PROPERTY_HINT_GROUP_ENABLE);
		REQUIRE(ClassDB::get_property_info("Environment", "pathtracing_denoiser", &pi));
		CHECK(pi.hint == PROPERTY_HINT_ENUM);
		CHECK(pi.hint_string == "None,DLSS Ray Reconstruction");
		REQUIRE(ClassDB::get_property_info("Environment", "pathtracing_debug_mode", &pi));
		CHECK(pi.hint == PROPERTY_HINT_ENUM);
		CHECK(pi.hint_string.get_slice_count(",") == int(Environment::RT_DEBUG_MAX));
		CHECK(pi.hint_string.begins_with("Disabled,Mirror Reflection,"));
		REQUIRE(ClassDB::get_property_info("Environment", "pathtracing_samples_per_pixel", &pi));
		CHECK(pi.hint_string == "1,64,1");
		REQUIRE(ClassDB::get_property_info("Environment", "pathtracing_max_bounces", &pi));
		CHECK(pi.hint_string == "1,8,1");
	}

	TEST_CASE("[Mariner][E5c] Environment debug mode and RenderingServer denoiser constants match the NVIDIA fork") {
		bool ok = false;
		CHECK(ClassDB::get_integer_constant("Environment", "RT_DEBUG_DISABLED", &ok) == 0);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("Environment", "RT_DEBUG_ALBEDO", &ok) == 8);
		CHECK(ClassDB::get_integer_constant("Environment", "RT_DEBUG_ROUGHNESS", &ok) == 15);
		CHECK(ClassDB::get_integer_constant("Environment", "RT_DEBUG_DEPTH", &ok) == 20);
		CHECK(ClassDB::get_integer_constant("Environment", "RT_DEBUG_BRDF_REJECTION", &ok) == 22);
		CHECK(ok);
		CHECK(int(Environment::RT_DEBUG_MAX) == 23);
		CHECK(ClassDB::get_integer_constant_enum("Environment", "RT_DEBUG_EMISSIVE") == StringName("PathtracingDebugMode"));

		CHECK(int(RSE::PT_DENOISER_NONE) == 0);
		CHECK(int(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION) == 1);
		CHECK(ClassDB::get_integer_constant("RenderingServer", "PT_DENOISER_NONE", &ok) == 0);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant("RenderingServer", "PT_DENOISER_DLSS_RAY_RECONSTRUCTION", &ok) == 1);
		CHECK(ok);
		CHECK(ClassDB::get_integer_constant_enum("RenderingServer", "PT_DENOISER_NONE") == StringName("PathtracingDenoiser"));

		// Material flag of the dielectric medium (path tracer water) does not collide with the E5a flags.
		using namespace RendererSceneRenderImplementation;
		CHECK(uint32_t(RT_MAT_FLAG_DIELECTRIC) == 64u);
		const uint32_t e5a_flags = RT_MAT_FLAG_HAS_NORMAL_MAP | RT_MAT_FLAG_HAS_EMISSION_TEX | RT_MAT_FLAG_POINT_FILTER | RT_MAT_FLAG_USE_VERTEX_COLOR | RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_UNSHADED;
		CHECK((e5a_flags & uint32_t(RT_MAT_FLAG_DIELECTRIC)) == 0u);
		CHECK(sizeof(RT_MaterialData) == 96); // Layout unchanged (the medium reuses fields).
		CHECK(sizeof(RendererSceneRenderImplementation::RT_LightData) == 80);
		CHECK(sizeof(RendererSceneRenderImplementation::PT_Params) == 80);
	}

	TEST_CASE("[Mariner][E5c] Dielectric detail: wave slope maps and foam pattern packed into the material") {
		using namespace RendererSceneRenderImplementation;
		const uint32_t used = RT_MAT_FLAG_HAS_NORMAL_MAP | RT_MAT_FLAG_HAS_EMISSION_TEX | RT_MAT_FLAG_POINT_FILTER | RT_MAT_FLAG_USE_VERTEX_COLOR | RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_UNSHADED | RT_MAT_FLAG_DIELECTRIC;
		CHECK(uint32_t(RT_MAT_FLAG_WAVE_DETAIL) == 128u);
		CHECK(uint32_t(RT_MAT_FLAG_FOAM_PATTERN) == 256u);
		CHECK((used & uint32_t(RT_MAT_FLAG_WAVE_DETAIL | RT_MAT_FLAG_FOAM_PATTERN)) == 0u);
		CHECK((RenderPathtracer::PT_FLAG_VOLUMETRIC_FOG & (RenderPathtracer::PT_FLAG_TRACE | RenderPathtracer::PT_FLAG_FOG | RenderPathtracer::PT_FLAG_DEPTH_FOG | RenderPathtracer::PT_FLAG_DIELECTRICS | RenderPathtracer::PT_FLAG_RADIANCE | RenderPathtracer::PT_FLAG_WRITE_DEPTH)) == 0);

		RT_MaterialData mat = {};
		mat.flags = RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_DIELECTRIC | RT_MAT_FLAG_HAS_NORMAL_MAP;
		mat.normal_texture_idx = 9; // A `texture_normal` of the shader is not used by the medium.
		mat.metallic = 0.7f;
		mat.uv1_scale[0] = 0.07f; // Scattering stays.
		mat.uv1_offset[0] = 0.09f;
		RenderRaytracing::apply_dielectric_detail(mat, 0, 0, Vector4(), 0, Vector2());
		CHECK(mat.flags == uint32_t(RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_DIELECTRIC));
		CHECK(mat.normal_texture_idx == 0);
		CHECK(mat.metallic == 0.0f);
		CHECK(mat.uv1_scale[0] == doctest::Approx(0.07f));
		CHECK(mat.uv1_offset[0] == doctest::Approx(0.09f));

		// Four wave maps in two textures: scales 1 / tile in metallic, ao_strength, normal_map_depth, emission_strength.
		const Vector4 scale(1.0 / 631.0, 1.0 / 113.0, 1.0 / 23.0, 1.0 / 5.3);
		RenderRaytracing::apply_dielectric_detail(mat, 3, 4, scale, 5, Vector2(1.0 / 17.0, 1.0 / 6.3));
		CHECK((mat.flags & RT_MAT_FLAG_WAVE_DETAIL) != 0u);
		CHECK((mat.flags & RT_MAT_FLAG_FOAM_PATTERN) != 0u);
		CHECK((mat.flags & RT_MAT_FLAG_HAS_NORMAL_MAP) == 0u);
		CHECK(mat.normal_texture_idx == 3);
		CHECK(mat.orm_texture_idx == 4);
		CHECK(mat.albedo_texture_idx == 5);
		CHECK(mat.metallic == doctest::Approx(1.0 / 631.0));
		CHECK(mat.ao_strength == doctest::Approx(1.0 / 113.0));
		CHECK(mat.normal_map_depth == doctest::Approx(1.0 / 23.0));
		CHECK(mat.emission_strength == doctest::Approx(1.0 / 5.3));
		uint32_t packed = 0;
		memcpy(&packed, &mat.uv1_offset[1], sizeof(uint32_t));
		CHECK(Math::half_to_float(uint16_t(packed & 0xFFFF)) == doctest::Approx(1.0 / 17.0).epsilon(0.002));
		CHECK(Math::half_to_float(uint16_t(packed >> 16)) == doctest::Approx(1.0 / 6.3).epsilon(0.002));
		CHECK(mat.uv1_offset[0] == doctest::Approx(0.09f)); // Scattering z untouched.

		// The second texture is optional (maps 2 and 3 off), without the first one there is no wave detail.
		RenderRaytracing::apply_dielectric_detail(mat, 3, 0, scale, 0, Vector2(0.1, 0.0));
		CHECK((mat.flags & RT_MAT_FLAG_WAVE_DETAIL) != 0u);
		CHECK((mat.flags & RT_MAT_FLAG_FOAM_PATTERN) == 0u);
		CHECK(mat.normal_map_depth == 0.0f);
		CHECK(mat.emission_strength == 0.0f);
		RenderRaytracing::apply_dielectric_detail(mat, 0, 4, scale, 0, Vector2());
		CHECK((mat.flags & RT_MAT_FLAG_WAVE_DETAIL) == 0u);
		CHECK(mat.orm_texture_idx == 0);
		RenderRaytracing::apply_dielectric_detail(mat, 3, 4, Vector4(), 0, Vector2());
		CHECK((mat.flags & RT_MAT_FLAG_WAVE_DETAIL) == 0u); // All scales 0.
	}

	TEST_CASE("[Mariner][E5c] Environment pathtracing setters clamp and reach the environment storage") {
		Ref<Environment> env;
		env.instantiate();
		RendererEnvironmentStorage *storage = RendererEnvironmentStorage::get_singleton();
		REQUIRE(storage != nullptr);
		const RID rid = env->get_rid();

		CHECK_FALSE(storage->environment_get_pathtracing_enabled(rid));
		CHECK(storage->environment_get_pathtracing_samples_per_pixel(rid) == 1);
		CHECK(storage->environment_get_pathtracing_max_bounces(rid) == 3);
		CHECK(storage->environment_get_pathtracing_denoiser(rid) == RSE::PT_DENOISER_NONE);
		CHECK(storage->environment_get_pathtracing_accumulate(rid));
		CHECK(storage->environment_get_pathtracing_max_samples(rid) == 0);
		CHECK(storage->environment_get_pathtracing_firefly_clamp(rid) == doctest::Approx(0.0));

		env->set_pathtracing_enabled(true);
		env->set_pathtracing_samples_per_pixel(4);
		env->set_pathtracing_max_bounces(6);
		env->set_pathtracing_debug_mode(Environment::RT_DEBUG_ALBEDO);
		env->set_pathtracing_denoiser(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);
		env->set_pathtracing_accumulate(false);
		env->set_pathtracing_max_samples(1024);
		env->set_pathtracing_firefly_clamp(50.0);
		CHECK(storage->environment_get_pathtracing_enabled(rid));
		CHECK(storage->environment_get_pathtracing_samples_per_pixel(rid) == 4);
		CHECK(storage->environment_get_pathtracing_max_bounces(rid) == 6);
		CHECK(storage->environment_get_pathtracing_debug_mode(rid) == int(Environment::RT_DEBUG_ALBEDO));
		CHECK(storage->environment_get_pathtracing_denoiser(rid) == RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);
		CHECK_FALSE(storage->environment_get_pathtracing_accumulate(rid));
		CHECK(storage->environment_get_pathtracing_max_samples(rid) == 1024);
		CHECK(storage->environment_get_pathtracing_firefly_clamp(rid) == doctest::Approx(50.0));

		// Clamping (resource and storage agree).
		env->set_pathtracing_samples_per_pixel(0);
		CHECK(env->get_pathtracing_samples_per_pixel() == 1);
		env->set_pathtracing_samples_per_pixel(1000);
		CHECK(env->get_pathtracing_samples_per_pixel() == 64);
		CHECK(storage->environment_get_pathtracing_samples_per_pixel(rid) == 64);
		env->set_pathtracing_max_bounces(0);
		CHECK(env->get_pathtracing_max_bounces() == 1);
		env->set_pathtracing_max_bounces(20);
		CHECK(env->get_pathtracing_max_bounces() == 8);
		env->set_pathtracing_max_samples(-5);
		CHECK(env->get_pathtracing_max_samples() == 0);
		env->set_pathtracing_firefly_clamp(-1.0);
		CHECK(env->get_pathtracing_firefly_clamp() == doctest::Approx(0.0));
		ERR_PRINT_OFF;
		env->set_pathtracing_debug_mode(Environment::PathtracingDebugMode(99));
		ERR_PRINT_ON;
		CHECK(env->get_pathtracing_debug_mode() == Environment::RT_DEBUG_ALBEDO);

		// Through the generic property API (scenes, scripts).
		env->set("pathtracing_max_bounces", 2);
		env->set("pathtracing_accumulate", true);
		CHECK(storage->environment_get_pathtracing_max_bounces(rid) == 2);
		CHECK(storage->environment_get_pathtracing_accumulate(rid));

		// Direct RenderingServer calls.
		RS::get_singleton()->environment_set_pathtracing(rid, false, 0, 2, 5, RSE::PT_DENOISER_NONE);
		RS::get_singleton()->environment_set_pathtracing_accumulation(rid, true, 256, 10.0);
		CHECK_FALSE(storage->environment_get_pathtracing_enabled(rid));
		CHECK(storage->environment_get_pathtracing_samples_per_pixel(rid) == 2);
		CHECK(storage->environment_get_pathtracing_max_bounces(rid) == 5);
		CHECK(storage->environment_get_pathtracing_max_samples(rid) == 256);
		CHECK(storage->environment_get_pathtracing_firefly_clamp(rid) == doctest::Approx(10.0));
	}

	TEST_CASE("[Mariner][E5c] RenderingServer and Viewport path tracing methods are bound") {
		struct Method {
			const char *cls;
			const char *name;
			int args;
		};
		const Method methods[] = {
			{ "RenderingServer", "environment_set_pathtracing", 6 },
			{ "RenderingServer", "environment_set_pathtracing_accumulation", 4 },
			{ "RenderingServer", "viewport_get_pathtracing_sample_count", 1 },
			{ "RenderingServer", "viewport_reset_pathtracing", 1 },
			{ "RenderingServer", "pathtracing_is_supported", 0 },
			{ "Viewport", "get_pathtracing_sample_count", 0 },
			{ "Viewport", "reset_pathtracing", 0 },
			{ "Environment", "set_pathtracing_accumulate", 1 },
			{ "Environment", "set_pathtracing_max_samples", 1 },
			{ "Environment", "set_pathtracing_firefly_clamp", 1 },
		};
		for (const Method &m : methods) {
			INFO(m.cls, ".", m.name);
			MethodBind *method = ClassDB::get_method(m.cls, m.name);
			REQUIRE(method != nullptr);
			CHECK(method->get_argument_count() == m.args);
		}
		// E5a API the path tracer builds on is still there.
		CHECK(ClassDB::has_method("Viewport", "set_raytracing_scene"));
		CHECK(ClassDB::has_method("RenderSceneBuffersRD", "get_raytracing_tlas"));
	}

	TEST_CASE("[Mariner][E5c][SceneTree] Without ray tracing the path tracer is unsupported and viewports report no samples") {
		CHECK_FALSE(RS::get_singleton()->pathtracing_is_supported());
		CHECK_FALSE(RenderPathtracer::is_supported()); // No RenderingDevice in the test renderer.

		SubViewport *viewport = memnew(SubViewport);
		SceneTree::get_singleton()->get_root()->add_child(viewport);
		Ref<Environment> env;
		env.instantiate();
		env->set_pathtracing_enabled(true);
		viewport->set_raytracing_scene(true);
		CHECK(viewport->get_pathtracing_sample_count() == 0);
		viewport->reset_pathtracing(); // Safe without render buffers / path tracer state.
		CHECK(viewport->get_pathtracing_sample_count() == 0);
		CHECK(RS::get_singleton()->viewport_get_pathtracing_sample_count(viewport->get_viewport_rid()) == 0);

		ERR_PRINT_OFF;
		CHECK(RS::get_singleton()->viewport_get_pathtracing_sample_count(RID()) == 0);
		RS::get_singleton()->viewport_reset_pathtracing(RID());
		ERR_PRINT_ON;

		memdelete(viewport);
	}

	TEST_CASE("[Mariner][E5c] Render scene buffers: path tracing defaults of the base class") {
		class TestBuffers : public RenderSceneBuffers {
		public:
			virtual void configure(const RenderSceneBuffersConfiguration *p_config) override {}
			virtual void set_fsr_sharpness(float p_fsr_sharpness) override {}
			virtual void set_texture_mipmap_bias(float p_texture_mipmap_bias) override {}
			virtual void set_anisotropic_filtering_level(RSE::ViewportAnisotropicFiltering p_anisotropic_filtering_level) override {}
			virtual void set_use_debanding(bool p_use_debanding) override {}
		};
		Ref<TestBuffers> buffers;
		buffers.instantiate();
		CHECK(buffers->get_pathtracing_sample_count() == 0);
		buffers->reset_pathtracing();
		CHECK(buffers->get_pathtracing_sample_count() == 0);
	}

	TEST_CASE("[Mariner][E5c] Accumulation rules: samples per frame, sample limit, camera changes, settings") {
		// Samples to trace this frame.
		CHECK(RenderPathtracer::samples_to_trace(0, 1, 0, true) == 1);
		CHECK(RenderPathtracer::samples_to_trace(500, 4, 0, true) == 4); // No limit.
		CHECK(RenderPathtracer::samples_to_trace(0, 4, 2, true) == 2); // Clipped to the limit.
		CHECK(RenderPathtracer::samples_to_trace(10, 4, 12, true) == 2);
		CHECK(RenderPathtracer::samples_to_trace(12, 4, 12, true) == 0); // Done: only composite.
		CHECK(RenderPathtracer::samples_to_trace(20, 4, 12, true) == 0);
		CHECK(RenderPathtracer::samples_to_trace(0, 4, 2, false) == 4); // No accumulation: no limit.
		CHECK(RenderPathtracer::samples_to_trace(0, 0, 0, true) == 1);
		CHECK(RenderPathtracer::samples_to_trace(0, 500, 0, true) == 64);

		// Camera changes restart the accumulation, the TAA jitter of the projection does not.
		Transform3D a;
		a.origin = Vector3(10, 2, -3);
		Projection p;
		p.set_perspective(70.0, 16.0 / 9.0, 0.05, 4000.0);
		CHECK_FALSE(RenderPathtracer::camera_changed(a, a, p, p));
		Projection jittered = p;
		jittered.add_jitter_offset(Vector2(0.3, -0.2) / 1080.0);
		CHECK_FALSE(RenderPathtracer::camera_changed(a, a, p, jittered));
		Transform3D moved = a;
		moved.origin.x += 0.01;
		CHECK(RenderPathtracer::camera_changed(a, moved, p, p));
		Transform3D turned = a.rotated_local(Vector3(0, 1, 0), 0.001);
		CHECK(RenderPathtracer::camera_changed(a, turned, p, p));
		Projection zoomed;
		zoomed.set_perspective(60.0, 16.0 / 9.0, 0.05, 4000.0);
		CHECK(RenderPathtracer::camera_changed(a, a, p, zoomed));

		// Settings that change the image restart it too (samples per pixel and the limit do not).
		RenderPathtracer::Settings s;
		const uint32_t h = s.hash();
		CHECK(s.hash() == h);
		RenderPathtracer::Settings s2 = s;
		s2.max_bounces = 4;
		CHECK(s2.hash() != h);
		s2 = s;
		s2.debug_mode = 8;
		CHECK(s2.hash() != h);
		s2 = s;
		s2.firefly_clamp = 20.0;
		CHECK(s2.hash() != h);
		s2 = s;
		s2.exposure_normalization = 0.5;
		CHECK(s2.hash() != h);
		s2 = s;
		s2.fog = true;
		CHECK(s2.hash() != h);
	}
}

} // namespace TestMarinerPathtracing
